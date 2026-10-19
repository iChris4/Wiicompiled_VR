#include "frame_interpolation.hpp"

#include "../internal.hpp"
#include "aurora/gfx.h"
#include "../gfx/stereo_replay.hpp"

// Guest matrices really do carry NaN/Inf, and the isfinite guards here keep them
// out of the MatchEdge sort. Needs -fno-finite-math-only (see runtime/CMakeLists.txt).
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "frame_interpolation.cpp must be compiled with -fno-finite-math-only; its NaN filtering is load-bearing for memory safety (see comment above)."
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aurora::gx {
namespace detail {
std::atomic_uint32_t g_frameInterpolationFps{0};
std::atomic_bool g_stereoFrameInterpolation{false};
} // namespace detail

namespace {
static Module Log("aurora::gx::interp");

// Diagnostics for aurora_get_frame_interpolation_diagnostics. Individually atomic:
// one writer, and a torn read is harmless for an overlay.
std::atomic_uint32_t s_diagCandidates{0};
std::atomic_uint32_t s_diagMatchable{0};
std::atomic_uint32_t s_diagMatches{0};
std::atomic_bool s_diagEligible{false};
std::atomic_bool s_diagReplaySafe{true};
std::atomic_uint64_t s_diagFramesSealed{0};
std::atomic_uint64_t s_diagFramesLowMatch{0};
std::atomic_uint64_t s_diagFramesReplayUnsafe{0};
std::atomic_uint64_t s_diagSlotReductions{0};
std::atomic_uint64_t s_diagLateSealDrops{0};
std::atomic_uint32_t s_diagPreparedDraws{0};
std::atomic_uint32_t s_diagRejectedDraws{0};
std::atomic_uint32_t s_diagVertexMotionDraws{0};
std::atomic_uint32_t s_diagVertexMotionHeld{0};
std::atomic_uint64_t s_diagAnimationWrapCuts{0};
// Persistent worker pool for the per-sample interpolation tasks. libc++ has no
// parallel execution policies, so without it the seal loop runs serially. Leaked.
class InterpolationWorkerPool {
public:
  static InterpolationWorkerPool& instance() {
    static InterpolationWorkerPool* pool = new InterpolationWorkerPool();
    return *pool;
  }

  // Runs fn(index) for every index in [0, count). Returns once every index has
  // been processed. Not reentrant; only the producer/seal thread dispatches.
  template <typename Fn>
  void run(size_t count, const Fn& fn) {
    if (count == 0) {
      return;
    }
    if (m_workers.empty()) {
      for (size_t i = 0; i < count; ++i) {
        fn(i);
      }
      return;
    }
    {
      // Job state and the generation bump publish under one lock, so a late worker
      // can never observe a half-written job.
      std::lock_guard lock(m_mutex);
      m_invoke = [&fn](size_t index) { fn(index); };
      m_count.store(count, std::memory_order_relaxed);
      m_next.store(0, std::memory_order_relaxed);
      m_remaining.store(count, std::memory_order_relaxed);
      ++m_generation;
    }
    m_wake.notify_all();
    consume();
    // Wait for stragglers to leave consume() entirely, not just finish their chunks,
    // so the next dispatch can safely reset the shared counters.
    while (m_remaining.load(std::memory_order_acquire) != 0 ||
           m_active.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }
    m_invoke = nullptr;
  }

private:
  static constexpr size_t kChunk = 16;

  InterpolationWorkerPool() {
    const unsigned hardware = std::thread::hardware_concurrency();
    // The caller helps too. Cap the helpers: tasks are short memcpy+math, so dispatch
    // overhead and memory bandwidth dominate past a few threads.
    const unsigned helpers = hardware > 2 ? std::min(hardware - 1, 6u) : 0;
    m_workers.reserve(helpers);
    for (unsigned i = 0; i < helpers; ++i) {
      m_workers.emplace_back([this] { worker_loop(); });
    }
  }

  void consume() {
    const size_t count = m_count.load(std::memory_order_relaxed);
    while (true) {
      const size_t begin = m_next.fetch_add(kChunk, std::memory_order_relaxed);
      if (begin >= count) {
        return;
      }
      const size_t end = std::min(begin + kChunk, count);
      for (size_t index = begin; index < end; ++index) {
        m_invoke(index);
      }
      m_remaining.fetch_sub(end - begin, std::memory_order_release);
    }
  }

  void worker_loop() {
    uint64_t seenGeneration = 0;
    while (true) {
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [&] { return m_generation != seenGeneration; });
        seenGeneration = m_generation;
        // Counted under the mutex: when the dispatcher sees m_active == 0 every worker is
        // parked or has not read the current generation, so a counter reset is safe.
        m_active.fetch_add(1, std::memory_order_relaxed);
      }
      consume();
      m_active.fetch_sub(1, std::memory_order_release);
    }
  }

  std::vector<std::thread> m_workers;
  std::mutex m_mutex;
  std::condition_variable m_wake;
  uint64_t m_generation = 0;
  std::function<void(size_t)> m_invoke;
  std::atomic_size_t m_count{0};
  std::atomic_size_t m_next{0};
  std::atomic_size_t m_remaining{0};
  std::atomic_size_t m_active{0};
};
struct FrameTransformSnapshot {
  Mat4x4<float> projection{};
  HashType viewportIdentity = 0;
  Mat3x4<float> position{};
  Mat3x4<float> normal{};
  uint16_t usedMatrixMask = 1;
  // Direct particle vertices can already be in camera space with identity XF.
  // Their retained vertex buffer belongs to the current frame, so only the
  // sampled camera should move them; an old matrix cannot animate those vertices.
  bool viewSpaceVertices = false;
  DrawVertexMotion vertexMotion{};
  // Vertex-motion quads only: centre and edges in the recording camera's space.
  // The seal rebases `position` as if the quad were world-fixed, but emitters
  // that follow the kart keep their particles near their old camera-space place.
  std::array<float, 3> quadCenter{};
  std::array<std::array<float, 3>, 2> quadEdges{};
  // Left unmatched in a group whose particles follow the camera: hold it there,
  // not at a world position the sampled camera would sweep past.
  bool holdInCamera = false;
  struct IndexedMatrices {
    std::array<Mat3x4<float>, MaxPnMtx> position{};
    std::array<Mat3x4<float>, MaxPnMtx> normal{};
    // Content hash of each used slot's position/normal pair. A mesh split across draws
    // repeats bone matrices byte for byte, so the seal can pair slots across draws.
    std::array<HashType, MaxPnMtx> slotHash{};
  };
  std::unique_ptr<IndexedMatrices> indexedMatrices;
};

struct FrameTransformEntry {
  FrameInterpolationDrawIdentity identity;
  FrameTransformSnapshot transform;
  // Constant-velocity prediction of a non-indexed transform, so repeated meshes match
  // on where they will be. Not for indexed draws: PNMTXIDX slot identity pairs those.
  Mat3x4<float> predictedPosition{};
  bool hasPrediction = false;
};

struct Quaternion {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

struct PendingUniformInterpolation {
  size_t currentTransformIndex = 0;
  const uint8_t* sourceUniformData = nullptr;
  uint8_t* uniformData = nullptr;
  size_t uniformSize = 0;
  size_t projectionOffset = 0;
  size_t positionOffset = 0;
  size_t normalOffset = 0;
  size_t currentMatrix = 0;
  uint32_t numerator = 0;
  uint32_t denominator = 1;
  bool indexedMatrices = false;
};

std::vector<FrameTransformEntry> s_previousFrameTransforms;
std::vector<FrameTransformEntry> s_currentFrameTransforms;
Mat3x4<float> s_currentFromPreviousView{}, s_previousFromCurrentView{};
bool s_rebaseView = false;
std::unordered_map<HashType, std::vector<size_t>> s_previousTransformIndices;
std::unordered_map<HashType, std::vector<size_t>> s_currentTransformIndices;
std::unordered_map<HashType, std::vector<size_t>> s_previousStableTransformIndices;
std::unordered_map<HashType, std::vector<size_t>> s_currentStableTransformIndices;
std::unordered_map<HashType, std::vector<size_t>> s_previousGeometryTransformIndices;
std::unordered_map<HashType, std::vector<size_t>> s_currentGeometryTransformIndices;

// Free list for the indexed-matrix snapshots; per-draw heap allocation was the
// hottest cost in this path. Unused slots keep stale data, consumers mask first.
std::vector<std::unique_ptr<FrameTransformSnapshot::IndexedMatrices>> s_indexedMatricesPool;
constexpr size_t kMaximumPooledIndexedMatrices = 4096;

std::unique_ptr<FrameTransformSnapshot::IndexedMatrices> acquire_indexed_matrices() {
  if (s_indexedMatricesPool.empty()) {
    return std::make_unique<FrameTransformSnapshot::IndexedMatrices>();
  }
  auto block = std::move(s_indexedMatricesPool.back());
  s_indexedMatricesPool.pop_back();
  return block;
}

void recycle_transform_entries(std::vector<FrameTransformEntry>& entries) noexcept {
  for (auto& entry : entries) {
    if (entry.transform.indexedMatrices &&
        s_indexedMatricesPool.size() < kMaximumPooledIndexedMatrices) {
      s_indexedMatricesPool.push_back(std::move(entry.transform.indexedMatrices));
    }
  }
  entries.clear();
}

// Whether the frame in s_previousFrameTransforms recorded any palette draw;
// record_interpolation_draw uses it to decide whether staging can pay off.
bool s_previousFrameHasIndexedMatrices = false;

// Hands the frame that just sealed to the next frame's matching and returns
// the retiring one's matrix blocks to the pool.
void retire_frame_transforms() noexcept {
  s_previousFrameHasIndexedMatrices =
      std::any_of(s_currentFrameTransforms.begin(), s_currentFrameTransforms.end(),
                  [](const FrameTransformEntry& entry) noexcept {
                    return static_cast<bool>(entry.transform.indexedMatrices);
                  });
  s_previousFrameTransforms.swap(s_currentFrameTransforms);
  recycle_transform_entries(s_currentFrameTransforms);
}

// clear() destroys every node, so empty the vectors in place and let a stable scene
// reuse them. Fall back to a real clear once the map outgrows the live set.
void clear_index_map_keep_nodes(std::unordered_map<HashType, std::vector<size_t>>& map,
                                size_t liveEntries) {
  if (map.size() > liveEntries + 256) {
    map.clear();
    return;
  }
  for (auto& entry : map) {
    entry.second.clear();
  }
}
uint32_t s_perspectiveCandidates = 0;
// Candidates that also existed last frame. Freshly spawned effects have no partner
// by definition, so counting them measured spawn churn instead of matcher health.
uint32_t s_perspectiveMatchable = 0;
uint32_t s_perspectiveMatches = 0;
std::vector<PendingUniformInterpolation> s_pendingUniformInterpolations;
std::atomic_bool s_hasInterpolatedFrame{false};
std::atomic_bool s_frameInterpolationReplaySafe{true};
uint32_t s_previousInterpolationFps = 0;

// Adaptive slot count: the target moves at most one step per pacing window, while
// s_activeInterpolationSamples is latched per frame and must not move mid-frame.
std::atomic_uint32_t s_interpolationSampleTarget{0};
std::atomic_uint32_t s_activeInterpolationSamples{0};

// Set when the producer already overran its retrace budget; the next seal then skips
// inserted slots for that one frame, which helps the late frame catch back up.
std::atomic_bool s_dropInterpolationAtSeal{false};

// Windowed backstop for sustained overload. The wide reduce/restore gap is the
// hysteresis, so a scene that can sustain N slots settles there instead of flapping.
std::atomic_uint32_t s_pacingWindowFrames{0};
std::atomic_uint32_t s_pacingWindowMisses{0};
constexpr uint32_t kPacingWindowFrames = 60; // ~1 second of guest frames
constexpr uint32_t kPacingReducePercent = 75;
constexpr uint32_t kPacingRestorePercent = 25;

uint32_t maximum_interpolation_samples() noexcept {
  const uint32_t targetFps = frame_interpolation_fps();
  if (targetFps == 0) {
    return 0;
  }
  return std::min(targetFps / 60 - 1, MaxInterpolatedFrames);
}

constexpr float kMinimumScale = 1.0e-5f;
constexpr float kMaximumTranslationPerFrame = 1500.0f;
constexpr float kMinimumQuaternionDot = 0.70710678f;
constexpr size_t kNoPreparedPair = std::numeric_limits<size_t>::max();
// Vertex-motion quad pairing (matchQuadGroup). Costs are squared distances.
// A pair must undercut the runner-up among other particles by this factor.
constexpr float kQuadAmbiguity = 1.5f;
// A particle's edges change by at most 30% of its size per frame.
constexpr float kQuadShapeChange = 0.3f * 0.3f;
// A particle with a path lands within a quarter of its last step of the
// prediction, or within a tenth of its short edge when it barely moves.
constexpr float kQuadPathTolerance = 0.25f * 0.25f;
constexpr float kQuadPathFloor = 0.1f * 0.1f;
// Centres this close belong to one particle: a cross draws two quads per streak.
constexpr float kQuadSamePlace = 1.0e-4f;
// All-pairs bound per group. A denser swarm stays where the game drew it.
constexpr size_t kMaximumQuadPairs = 4096;

HashType combine_identity(HashType first, HashType second) noexcept {
  return xxh3_hash(second, first);
}

HashType stable_identity(const FrameInterpolationDrawIdentity& identity) noexcept {
  return combine_identity(combine_identity(identity.pipeline, identity.texture),
                          identity.matrixTopology);
}

HashType geometry_identity(const FrameInterpolationDrawIdentity& identity) noexcept {
  return combine_identity(combine_identity(identity.pipeline, identity.geometry), identity.matrixTopology);
}

float dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> cross3(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
  return {
      a[1] * b[2] - a[2] * b[1],
      a[2] * b[0] - a[0] * b[2],
      a[0] * b[1] - a[1] * b[0],
  };
}

Quaternion quaternion_from_rotation(const std::array<std::array<float, 3>, 3>& m) noexcept {
  Quaternion q;
  const float trace = m[0][0] + m[1][1] + m[2][2];
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (m[2][1] - m[1][2]) / s;
    q.y = (m[0][2] - m[2][0]) / s;
    q.z = (m[1][0] - m[0][1]) / s;
  } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
    q.w = (m[2][1] - m[1][2]) / s;
    q.x = 0.25f * s;
    q.y = (m[0][1] + m[1][0]) / s;
    q.z = (m[0][2] + m[2][0]) / s;
  } else if (m[1][1] > m[2][2]) {
    const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
    q.w = (m[0][2] - m[2][0]) / s;
    q.x = (m[0][1] + m[1][0]) / s;
    q.y = 0.25f * s;
    q.z = (m[1][2] + m[2][1]) / s;
  } else {
    const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
    q.w = (m[1][0] - m[0][1]) / s;
    q.x = (m[0][2] + m[2][0]) / s;
    q.y = (m[1][2] + m[2][1]) / s;
    q.z = 0.25f * s;
  }
  return q;
}

// Splits the 3x3 part into a rotation R and an upper-triangular stretch U with M = R*U
// (Gram-Schmidt on the columns): scale on U's diagonal, shear above it, a mirror as a
// negative last diagonal. V*T*R*S puts non-uniform model scale on the columns, so a plain
// R*S comes back with a diagonal U. A sheared matrix keeps its tilt in U instead of
// losing it to the nearest rotation. `skew` receives the largest |cosine| between the
// columns: zero for rotation times scale.
bool decompose_affine(const Mat3x4<float>& matrix, std::array<std::array<float, 3>, 3>& rotation,
                      std::array<float, 6>& stretch, std::array<float, 3>& translation,
                      Quaternion& quaternion, float& skew) noexcept {
  const std::array<Vec4<float>, 3> rows{matrix.m0, matrix.m1, matrix.m2};
  for (size_t row = 0; row < 3; ++row) {
    translation[row] = rows[row].w();
    if (!std::isfinite(translation[row])) {
      return false;
    }
  }
  const auto column = [&rows](size_t index) noexcept {
    return std::array<float, 3>{rows[0][index], rows[1][index], rows[2][index]};
  };
  const auto c0 = column(0), c1 = column(1), c2 = column(2);
  const float u00 = std::sqrt(dot3(c0, c0));
  if (!std::isfinite(u00) || u00 < kMinimumScale) {
    return false;
  }
  const std::array<float, 3> r0{c0[0] / u00, c0[1] / u00, c0[2] / u00};
  const float u01 = dot3(r0, c1);
  const std::array<float, 3> c1Rest{c1[0] - u01 * r0[0], c1[1] - u01 * r0[1], c1[2] - u01 * r0[2]};
  const float u11 = std::sqrt(dot3(c1Rest, c1Rest));
  if (!std::isfinite(u11) || u11 < kMinimumScale) {
    return false;
  }
  const std::array<float, 3> r1{c1Rest[0] / u11, c1Rest[1] / u11, c1Rest[2] / u11};
  const auto r2 = cross3(r0, r1);
  const float u02 = dot3(r0, c2);
  const float u12 = dot3(r1, c2);
  const float u22 = dot3(r2, c2);
  if (!std::isfinite(u02) || !std::isfinite(u12) || !std::isfinite(u22) || std::abs(u22) < kMinimumScale) {
    return false;
  }
  const float length1 = std::sqrt(dot3(c1, c1));
  const float length2 = std::sqrt(dot3(c2, c2));
  skew = std::max({std::abs(u01) / length1, std::abs(u02) / length2,
                   std::abs(dot3(c1, c2)) / (length1 * length2)});
  for (size_t row = 0; row < 3; ++row) {
    rotation[row] = {r0[row], r1[row], r2[row]};
  }
  stretch = {u00, u01, u02, u11, u12, u22};

  quaternion = quaternion_from_rotation(rotation);
  return std::isfinite(quaternion.x) && std::isfinite(quaternion.y) && std::isfinite(quaternion.z) &&
         std::isfinite(quaternion.w);
}

std::array<std::array<float, 3>, 3> rotation_from_quaternion(const Quaternion& q) noexcept {
  const float xx = q.x * q.x;
  const float yy = q.y * q.y;
  const float zz = q.z * q.z;
  const float xy = q.x * q.y;
  const float xz = q.x * q.z;
  const float yz = q.y * q.z;
  const float wx = q.w * q.x;
  const float wy = q.w * q.y;
  const float wz = q.w * q.z;
  return {{
      {{1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy)}},
      {{2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx)}},
      {{2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy)}},
  }};
}

struct PreparedAffinePair {
  Mat3x4<float> previous{};
  Mat3x4<float> current{};
  // Upper-triangular stretch from decompose_affine: u00 u01 u02 u11 u12 u22.
  std::array<float, 6> previousStretch{};
  std::array<float, 6> currentStretch{};
  std::array<float, 3> previousTranslation{};
  std::array<float, 3> currentTranslation{};
  Quaternion previousQuaternion{};
  Quaternion currentQuaternion{};
  float rotationAngle = 0.0f;
  float inverseRotationSin = 0.0f;
  bool linear = false;
  bool identical = false;
  bool valid = false;
};

struct PreparedTransformInterpolation {
  size_t nonIndexedPairOffset = kNoPreparedPair;
  std::array<size_t, MaxPnMtx> indexedPairOffsets{};
  // Previous-frame entry the projection interpolates from. For a draw that borrowed
  // its matrices from a sibling, that is the sibling's partner.
  size_t previousProjectionEntry = kNoPreparedPair;
  bool indexedValid = false;
  bool desktopIndexedValid = false;
  // Unpaired quad held in camera space: its own transform is both endpoints.
  bool cameraHold = false;

  PreparedTransformInterpolation() {
    indexedPairOffsets.fill(kNoPreparedPair);
  }
};

PreparedAffinePair prepare_affine_pair(const Mat3x4<float>& previous,
                                       const Mat3x4<float>& current, bool rotatingDraw = false) noexcept {
  PreparedAffinePair pair{.previous = previous, .current = current};
  if (std::memcmp(&previous, &current, sizeof(current)) == 0) {
    pair.identical = true;
    pair.valid = true;
    return pair;
  }

  std::array<std::array<float, 3>, 3> previousRotation{};
  std::array<std::array<float, 3>, 3> currentRotation{};
  float previousSkew = 0.0f, currentSkew = 0.0f;
  if (!decompose_affine(previous, previousRotation, pair.previousStretch,
                        pair.previousTranslation, pair.previousQuaternion, previousSkew) ||
      !decompose_affine(current, currentRotation, pair.currentStretch,
                        pair.currentTranslation, pair.currentQuaternion, currentSkew)) {
    return pair;
  }
  // Camera and seat anchors are rigid; a sheared one is garbage. Draws may shear:
  // MKW's Lakitu sways by tilting his body's Y axis, and dropping that tilt from his
  // rigid goggles (or holding them at the game frame) sank them into his skinned head.
  if (!rotatingDraw && std::max(previousSkew, currentSkew) > 0.05f) {
    return pair;
  }

  float translationDeltaSquared = 0.0f;
  for (size_t i = 0; i < 3; ++i) {
    const float delta = pair.currentTranslation[i] - pair.previousTranslation[i];
    translationDeltaSquared += delta * delta;
  }
  if (!std::isfinite(translationDeltaSquared) ||
      translationDeltaSquared > kMaximumTranslationPerFrame * kMaximumTranslationPerFrame) {
    return pair;
  }

  float quaternionDot = pair.previousQuaternion.x * pair.currentQuaternion.x +
                        pair.previousQuaternion.y * pair.currentQuaternion.y +
                        pair.previousQuaternion.z * pair.currentQuaternion.z +
                        pair.previousQuaternion.w * pair.currentQuaternion.w;
  if (quaternionDot < 0.0f) {
    pair.currentQuaternion.x = -pair.currentQuaternion.x;
    pair.currentQuaternion.y = -pair.currentQuaternion.y;
    pair.currentQuaternion.z = -pair.currentQuaternion.z;
    pair.currentQuaternion.w = -pair.currentQuaternion.w;
    quaternionDot = -quaternionDot;
  }
  if (!std::isfinite(quaternionDot) || (!rotatingDraw && quaternionDot < kMinimumQuaternionDot)) {
    return pair;
  }
  if (quaternionDot < kMinimumQuaternionDot) {
    // A fast wheel is not a camera cut. Slerp its shortest arc so arbitrary VR
    // sample weights keep angular speed constant without shrinking the wheel.
    pair.rotationAngle = std::acos(std::clamp(quaternionDot, 0.0f, 1.0f));
    pair.inverseRotationSin = 1.0f / std::sin(pair.rotationAngle);
  }
  pair.valid = true;
  return pair;
}

// Indexed matrices are final palette transforms, so interpolate coefficients instead
// of decomposing: ((1-t)M0 + tM1)v keeps a shared mesh boundary shared.
PreparedAffinePair prepare_indexed_pair(const Mat3x4<float>& previous,
                                        const Mat3x4<float>& current) noexcept {
  PreparedAffinePair pair{
      .previous = previous,
      .current = current,
      .linear = true,
  };
  if (std::memcmp(&previous, &current, sizeof(current)) == 0) {
    pair.identical = true;
    pair.valid = true;
    return pair;
  }

  const std::array<Vec4<float>, 3> previousRows{previous.m0, previous.m1, previous.m2};
  const std::array<Vec4<float>, 3> currentRows{current.m0, current.m1, current.m2};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      if (!std::isfinite(previousRows[row][column]) ||
          !std::isfinite(currentRows[row][column])) {
        return pair;
      }
    }
  }

  const float dx = current.m0.w() - previous.m0.w();
  const float dy = current.m1.w() - previous.m1.w();
  const float dz = current.m2.w() - previous.m2.w();
  const float translationDeltaSquared = dx * dx + dy * dy + dz * dz;
  if (!(translationDeltaSquared <=
        kMaximumTranslationPerFrame * kMaximumTranslationPerFrame)) {
    return pair;
  }
  pair.valid = true;
  return pair;
}

bool evaluate_affine_pair(const PreparedAffinePair& pair, float weight,
                          Mat3x4<float>& output) noexcept {
  if (!pair.valid || pair.identical) {
    output = pair.current;
    return pair.valid;
  }

  if (pair.linear) {
    const std::array<const Vec4<float>*, 3> previousRows{
        &pair.previous.m0, &pair.previous.m1, &pair.previous.m2};
    const std::array<const Vec4<float>*, 3> currentRows{
        &pair.current.m0, &pair.current.m1, &pair.current.m2};
    const std::array<Vec4<float>*, 3> outputRows{&output.m0, &output.m1, &output.m2};
    for (size_t row = 0; row < 3; ++row) {
      for (size_t column = 0; column < 4; ++column) {
        const float before = (*previousRows[row])[column];
        (*outputRows[row])[column] =
            before + ((*currentRows[row])[column] - before) * weight;
      }
    }
    return true;
  }

  // Most transforms use the cheaper normalized lerp. Fast rigid spins need
  // constant angular speed across arbitrary display samples, so use slerp there.
  const float previousWeight = pair.rotationAngle > 0.0f
                                   ? std::sin((1.0f - weight) * pair.rotationAngle) * pair.inverseRotationSin
                                   : 1.0f - weight;
  const float currentWeight = pair.rotationAngle > 0.0f
                                  ? std::sin(weight * pair.rotationAngle) * pair.inverseRotationSin
                                  : weight;
  Quaternion interpolated{
      pair.previousQuaternion.x * previousWeight + pair.currentQuaternion.x * currentWeight,
      pair.previousQuaternion.y * previousWeight + pair.currentQuaternion.y * currentWeight,
      pair.previousQuaternion.z * previousWeight + pair.currentQuaternion.z * currentWeight,
      pair.previousQuaternion.w * previousWeight + pair.currentQuaternion.w * currentWeight,
  };
  const float quaternionLength = std::sqrt(interpolated.x * interpolated.x +
                                           interpolated.y * interpolated.y +
                                           interpolated.z * interpolated.z +
                                           interpolated.w * interpolated.w);
  if (!std::isfinite(quaternionLength) || quaternionLength < kMinimumScale) {
    output = pair.current;
    return false;
  }
  interpolated.x /= quaternionLength;
  interpolated.y /= quaternionLength;
  interpolated.z /= quaternionLength;
  interpolated.w /= quaternionLength;

  const auto interpolatedRotation = rotation_from_quaternion(interpolated);
  std::array<float, 6> u{};
  for (size_t i = 0; i < u.size(); ++i) {
    u[i] = pair.previousStretch[i] + (pair.currentStretch[i] - pair.previousStretch[i]) * weight;
  }
  std::array<float, 3> interpolatedTranslation{};
  for (size_t i = 0; i < 3; ++i) {
    interpolatedTranslation[i] =
        pair.previousTranslation[i] +
        (pair.currentTranslation[i] - pair.previousTranslation[i]) * weight;
  }
  // Mirrors decompose_affine: the reconstruction is R*U, U upper-triangular.
  const std::array<Vec4<float>*, 3> outputRows{&output.m0, &output.m1, &output.m2};
  for (size_t row = 0; row < 3; ++row) {
    const auto& r = interpolatedRotation[row];
    *outputRows[row] = {r[0] * u[0], r[0] * u[1] + r[1] * u[3], r[0] * u[2] + r[1] * u[4] + r[2] * u[5],
                        interpolatedTranslation[row]};
  }
  return true;
}

bool interpolate_affine_impl(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                             float weight, Mat3x4<float>& output) noexcept {
  return evaluate_affine_pair(prepare_affine_pair(previous, current), weight, output);
}

Mat4x4<float> interpolate_projection(const Mat4x4<float>& previous, const Mat4x4<float>& current,
                                     float weight) noexcept {
  Mat4x4<float> result = current;
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      const float before = previous[row][column];
      const float after = current[row][column];
      if (std::isfinite(before) && std::isfinite(after)) {
        result[row][column] = before + (after - before) * weight;
      }
    }
  }
  return result;
}
} // namespace

bool interpolate_transform(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                           float weight, Mat3x4<float>& output) noexcept {
  return interpolate_affine_impl(previous, current, std::clamp(weight, 0.0f, 1.0f), output);
}

bool interpolate_transform_midpoint(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                                    Mat3x4<float>& output) noexcept {
  return interpolate_transform(previous, current, 0.5f, output);
}

bool interpolate_draw_transform(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                                float weight, Mat3x4<float>& output) noexcept {
  return evaluate_affine_pair(prepare_affine_pair(previous, current, true), std::clamp(weight, 0.0f, 1.0f), output);
}

bool interpolate_indexed_transform(const Mat3x4<float>& previous,
                                   const Mat3x4<float>& current, float weight,
                                   Mat3x4<float>& output) noexcept {
  return evaluate_affine_pair(prepare_indexed_pair(previous, current),
                              std::clamp(weight, 0.0f, 1.0f), output);
}

float transform_match_distance_squared(const Mat3x4<float>& previous,
                                       const Mat3x4<float>& current) noexcept {
  // Translation dominates in game units; the 3x3 part only breaks ties between
  // repeated meshes at the same origin.
  double distance = 0.0;
  const std::array<Vec4<float>, 3> previousRows{previous.m0, previous.m1, previous.m2};
  const std::array<Vec4<float>, 3> currentRows{current.m0, current.m1, current.m2};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      const double before = previousRows[row][column];
      const double after = currentRows[row][column];
      if (!std::isfinite(before) || !std::isfinite(after)) {
        return std::numeric_limits<float>::infinity();
      }
      const double delta = after - before;
      distance += delta * delta;
    }
  }
  if (!std::isfinite(distance) || distance > std::numeric_limits<float>::max()) {
    return std::numeric_limits<float>::infinity();
  }
  return static_cast<float>(distance);
}

namespace {
// Translation-only delta, matching the kMaximumTranslationPerFrame gate. Non-finite
// inputs propagate NaN; callers compare with `<=` so NaN fails the gate.
float translation_delta_squared(const Mat3x4<float>& previous,
                                const Mat3x4<float>& current) noexcept {
  const float dx = current.m0.w() - previous.m0.w();
  const float dy = current.m1.w() - previous.m1.w();
  const float dz = current.m2.w() - previous.m2.w();
  return dx * dx + dy * dy + dz * dz;
}

// 2*current - previous. Not a valid rigid transform, but it is only a matching
// reference and its one-frame error stays far below instance spacing.
Mat3x4<float> extrapolate_transform(const Mat3x4<float>& previous,
                                    const Mat3x4<float>& current) noexcept {
  Mat3x4<float> predicted{};
  const std::array<const Vec4<float>*, 3> previousRows{&previous.m0, &previous.m1, &previous.m2};
  const std::array<const Vec4<float>*, 3> currentRows{&current.m0, &current.m1, &current.m2};
  const std::array<Vec4<float>*, 3> predictedRows{&predicted.m0, &predicted.m1, &predicted.m2};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      (*predictedRows[row])[column] =
          2.0f * (*currentRows[row])[column] - (*previousRows[row])[column];
    }
  }
  return predicted;
}

std::array<float, 3> translation_of(const Mat3x4<float>& matrix) noexcept {
  return {matrix.m0.w(), matrix.m1.w(), matrix.m2.w()};
}

std::array<float, 3> rotate_vector(const Mat3x4<float>& matrix, const std::array<float, 3>& vector) noexcept {
  const Vec4<float>* rows[] = {&matrix.m0, &matrix.m1, &matrix.m2};
  std::array<float, 3> result{};
  for (size_t row = 0; row < 3; ++row)
    result[row] = (*rows[row])[0] * vector[0] + (*rows[row])[1] * vector[1] + (*rows[row])[2] * vector[2];
  return result;
}

float distance_squared(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
  const std::array<float, 3> delta{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return dot3(delta, delta);
}
} // namespace

float snapshot_match_distance_squared(const FrameTransformEntry& previousEntry,
                                      const FrameTransformSnapshot& current) noexcept {
  const FrameTransformSnapshot& previous = previousEntry.transform;
  if (static_cast<bool>(previous.indexedMatrices) != static_cast<bool>(current.indexedMatrices)) {
    return std::numeric_limits<float>::infinity();
  }
  if (!current.indexedMatrices) {
    // Match against the predicted position, falling back to the last known one for
    // entries with no motion history.
    const Mat3x4<float>& reference =
        previousEntry.hasPrediction ? previousEntry.predictedPosition : previous.position;
    return transform_match_distance_squared(reference, current.position);
  }

  if (previous.usedMatrixMask == 0 || current.usedMatrixMask == 0) {
    return std::numeric_limits<float>::infinity();
  }

  // PNMTXIDX is an absolute palette slot baked into the vertices, so nearest matching
  // swaps joint histories. A changed mask has no safe correspondence: leave the draw.
  if (previous.usedMatrixMask != current.usedMatrixMask) {
    return std::numeric_limits<float>::infinity();
  }

  double distance = 0.0;
  size_t matchedMatrices = 0;
  for (size_t currentIndex = 0; currentIndex < MaxPnMtx; ++currentIndex) {
    if ((current.usedMatrixMask & (1u << currentIndex)) == 0) {
      continue;
    }
    const float matrixDistance = transform_match_distance_squared(
        previous.indexedMatrices->position[currentIndex],
        current.indexedMatrices->position[currentIndex]);
    if (!std::isfinite(matrixDistance)) {
      return std::numeric_limits<float>::infinity();
    }
    distance += matrixDistance;
    ++matchedMatrices;
  }
  if (matchedMatrices == 0 || !std::isfinite(distance) ||
      distance > std::numeric_limits<float>::max()) {
    return std::numeric_limits<float>::infinity();
  }
  return static_cast<float>(distance / static_cast<double>(matchedMatrices));
}

void set_frame_interpolation_fps(uint32_t targetFps) noexcept {
  if (targetFps != 120 && targetFps != 180 && targetFps != 240) {
    targetFps = 0;
  }
  detail::g_frameInterpolationFps.store(targetFps, std::memory_order_release);
  // Start at the configured quality; the controller only ever backs off from here.
  s_interpolationSampleTarget.store(targetFps == 0 ? 0u : std::min(targetFps / 60 - 1, MaxInterpolatedFrames),
                                    std::memory_order_release);
  s_dropInterpolationAtSeal.store(false, std::memory_order_release);
  s_pacingWindowFrames.store(0, std::memory_order_release);
  s_pacingWindowMisses.store(0, std::memory_order_release);
  // A reconfiguration starts a fresh diagnostic window.
  s_diagFramesSealed.store(0, std::memory_order_relaxed);
  s_diagFramesLowMatch.store(0, std::memory_order_relaxed);
  s_diagFramesReplayUnsafe.store(0, std::memory_order_relaxed);
  s_diagSlotReductions.store(0, std::memory_order_relaxed);
  s_diagLateSealDrops.store(0, std::memory_order_relaxed);
}

void report_producer_paced(bool paced) noexcept {
  const uint32_t maximumSamples = maximum_interpolation_samples();
  if (maximumSamples == 0) {
    s_dropInterpolationAtSeal.store(false, std::memory_order_release);
    s_pacingWindowFrames.store(0, std::memory_order_release);
    s_pacingWindowMisses.store(0, std::memory_order_release);
    s_interpolationSampleTarget.store(0, std::memory_order_release);
    return;
  }

  // Per-frame decision, no streaks: a late frame seals without its inserted slots and
  // the next one is back at full count. See s_dropInterpolationAtSeal.
  s_dropInterpolationAtSeal.store(!paced, std::memory_order_release);

  const uint32_t frames = s_pacingWindowFrames.load(std::memory_order_acquire) + 1;
  const uint32_t misses =
      s_pacingWindowMisses.load(std::memory_order_acquire) + (paced ? 0u : 1u);
  if (frames < kPacingWindowFrames) {
    s_pacingWindowFrames.store(frames, std::memory_order_release);
    s_pacingWindowMisses.store(misses, std::memory_order_release);
    return;
  }
  // Window complete: adjust the target one step at most, then start over. At
  // one decision per window this cannot spam the log or flap within a second.
  uint32_t target =
      std::min(s_interpolationSampleTarget.load(std::memory_order_acquire), maximumSamples);
  if (misses * 100u >= frames * kPacingReducePercent) {
    if (target > 0) {
      --target;
      s_diagSlotReductions.fetch_add(1, std::memory_order_relaxed);
      Log.info("interpolation slots reduced to {}: {}/{} frames overran their retrace budget",
               target, misses, frames);
    }
  } else if (misses * 100u <= frames * kPacingRestorePercent) {
    if (target < maximumSamples) {
      ++target;
      Log.info("interpolation slots restored to {}: {}/{} frames overran their retrace budget",
               target, misses, frames);
    }
  }
  s_interpolationSampleTarget.store(target, std::memory_order_release);
  s_pacingWindowFrames.store(0, std::memory_order_release);
  s_pacingWindowMisses.store(0, std::memory_order_release);
}

void set_frame_interpolation_view_rebase(const Mat3x4<float>* currentFromPrevious,
                                         const Mat3x4<float>* previousFromCurrent) noexcept {
  s_rebaseView = currentFromPrevious && previousFromCurrent && stereo_frame_interpolation_active();
  if (s_rebaseView) {
    s_currentFromPreviousView = *currentFromPrevious;
    s_previousFromCurrentView = *previousFromCurrent;
  }
}

void begin_frame_interpolation() noexcept {
  s_rebaseView = false;
  const uint32_t targetFps = frame_interpolation_fps();
  static bool previousStereo = false;
  const bool stereo = stereo_frame_interpolation_active();
  if (targetFps != s_previousInterpolationFps || stereo != previousStereo) {
    recycle_transform_entries(s_previousFrameTransforms);
    s_previousInterpolationFps = targetFps;
    previousStereo = stereo;
  }
  // Latch the slot count for the frame starting here; see s_activeInterpolationSamples
  // for why it cannot move again until the seal.
  s_activeInterpolationSamples.store(
      std::min(s_interpolationSampleTarget.load(std::memory_order_acquire),
               maximum_interpolation_samples()),
      std::memory_order_release);
  // Normally empty (finalize swapped and recycled); an aborted frame can leave
  // entries behind, whose matrix blocks go back to the pool here.
  recycle_transform_entries(s_currentFrameTransforms);
  // Keep buckets, nodes and capacities across frames so a stable race scene does not
  // pay thousands of small allocations every frame.
  clear_index_map_keep_nodes(s_previousTransformIndices, s_previousFrameTransforms.size());
  clear_index_map_keep_nodes(s_previousStableTransformIndices, s_previousFrameTransforms.size());
  clear_index_map_keep_nodes(s_previousGeometryTransformIndices, s_previousFrameTransforms.size());
  for (size_t i = 0; i < s_previousFrameTransforms.size(); ++i) {
    const auto& identity = s_previousFrameTransforms[i].identity;
    s_previousTransformIndices[identity.combined].push_back(i);
    s_previousStableTransformIndices[stable_identity(identity)].push_back(i);
    if (identity.geometry != 0)
      s_previousGeometryTransformIndices[geometry_identity(identity)].push_back(i);
  }
  clear_index_map_keep_nodes(s_currentTransformIndices, s_previousFrameTransforms.size());
  clear_index_map_keep_nodes(s_currentStableTransformIndices, s_previousFrameTransforms.size());
  clear_index_map_keep_nodes(s_currentGeometryTransformIndices, s_previousFrameTransforms.size());
  s_pendingUniformInterpolations.clear();
  s_perspectiveCandidates = 0;
  s_perspectiveMatchable = 0;
  s_perspectiveMatches = 0;
  s_hasInterpolatedFrame.store(false, std::memory_order_release);
  s_frameInterpolationReplaySafe.store(true, std::memory_order_release);
}

void finalize_frame_interpolation() noexcept {
  s_diagPreparedDraws.store(0, std::memory_order_relaxed);
  s_diagRejectedDraws.store(0, std::memory_order_relaxed);
  s_diagVertexMotionDraws.store(0, std::memory_order_relaxed);
  s_diagVertexMotionHeld.store(0, std::memory_order_relaxed);
  if (s_rebaseView) {
    // The old history retires at the end of this seal. Rebase it once, before
    // instance matching and motion gates, rather than treating camera rotation
    // around a distant object as an object teleport.
    for (auto& entry : s_previousFrameTransforms) {
      auto& transform = entry.transform;
      if (transform.viewSpaceVertices) continue;
      if (transform.indexedMatrices) {
        for (size_t slot = 0; slot < MaxPnMtx; ++slot) {
          if ((transform.usedMatrixMask & (1u << slot)) == 0) continue;
          auto& matrices = *transform.indexedMatrices;
          matrices.position[slot] = gfx::stereo_replay::compose_affine(s_currentFromPreviousView, matrices.position[slot]);
          matrices.normal[slot] = gfx::stereo_replay::compose_normal(s_currentFromPreviousView, matrices.normal[slot]);
          matrices.slotHash[slot] = xxh3_hash_s(&matrices.position[slot], sizeof(Mat3x4<float>),
                                               xxh3_hash_s(&matrices.normal[slot], sizeof(Mat3x4<float>)));
        }
      } else {
        transform.position = gfx::stereo_replay::compose_affine(s_currentFromPreviousView, transform.position);
        transform.normal = gfx::stereo_replay::compose_normal(s_currentFromPreviousView, transform.normal);
        if (entry.hasPrediction)
          entry.predictedPosition = gfx::stereo_replay::compose_affine(s_currentFromPreviousView, entry.predictedPosition);
      }
    }
  }
  // A frame reported late seals without inserted slots, so the encode phase renders
  // the native frame only. Its transforms still seed the next frame's matching.
  const bool late = s_dropInterpolationAtSeal.exchange(false, std::memory_order_acq_rel);
  if (late) {
    s_diagLateSealDrops.fetch_add(1, std::memory_order_relaxed);
  }
  if (late && !stereo_frame_interpolation_active()) {
    s_hasInterpolatedFrame.store(false, std::memory_order_release);
    s_pendingUniformInterpolations.clear();
    retire_frame_transforms();
    return;
  }
  // Below this bound a direct all-pairs build is cheaper than setting up the
  // spatial grid; the produced edge set is identical either way.
  constexpr size_t kAllPairsEdgeLimit = 1024;
  // Bounds the all-pairs cost for indexed groups, which have no single translation to
  // bucket on. Non-indexed groups of any size use the grid instead.
  constexpr size_t kMaximumAssignmentEdges = 16384;
  // Hard bound for the grid path; only a swarm co-located within one cell reaches it,
  // which is the CPU-deformed shape the ordered fallback exists for.
  constexpr size_t kMaximumGridEdges = 65536;

  struct MatchEdge {
    float distance = std::numeric_limits<float>::infinity();
    size_t previous = SIZE_MAX;
    size_t current = SIZE_MAX;
  };

  // Frame-persistent matching scratch: sized to the frame's draw count, so a
  // stable scene performs no matching allocations at all after warm-up.
  static std::vector<size_t> currentToPrevious;
  static std::vector<uint8_t> previousMatched;
  static std::vector<uint8_t> currentMatched;
  static std::vector<size_t> groupCurrentIndices;
  static std::vector<size_t> groupPreviousIndices;
  static std::vector<MatchEdge> edges;
  static std::unordered_map<uint64_t, std::vector<uint32_t>> gridCells;
  currentToPrevious.assign(s_currentFrameTransforms.size(), SIZE_MAX);
  previousMatched.assign(s_previousFrameTransforms.size(), 0);
  currentMatched.assign(s_currentFrameTransforms.size(), 0);

  // Quantizes a translation onto the matching grid. Cell size equals the gate distance,
  // so every acceptable pair lies within one cell along every axis.
  constexpr uint64_t kInvalidCell = std::numeric_limits<uint64_t>::max();
  const auto translation_cell = [](const Mat3x4<float>& m) noexcept -> uint64_t {
    const float x = m.m0.w();
    const float y = m.m1.w();
    const float z = m.m2.w();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
      return kInvalidCell;
    }
    constexpr float kInverseCell = 1.0f / kMaximumTranslationPerFrame;
    constexpr int64_t kBias = int64_t{1} << 20;
    const int64_t cx = static_cast<int64_t>(std::floor(x * kInverseCell)) + kBias;
    const int64_t cy = static_cast<int64_t>(std::floor(y * kInverseCell)) + kBias;
    const int64_t cz = static_cast<int64_t>(std::floor(z * kInverseCell)) + kBias;
    // One cell of margin at each field edge so the +-1 neighbor arithmetic cannot borrow
    // into the next packed coordinate.
    if (cx < 1 || cy < 1 || cz < 1 || cx >= (kBias << 1) - 1 || cy >= (kBias << 1) - 1 ||
        cz >= (kBias << 1) - 1) {
      return kInvalidCell;
    }
    return (static_cast<uint64_t>(cx) << 42) | (static_cast<uint64_t>(cy) << 21) |
           static_cast<uint64_t>(cz);
  };

  // Vertex-motion quads come from particle emitters, which draw many look-alike
  // quads: a speed line is two crossed quads, two new ones start on the same ring
  // every frame, and each moves further per frame than the gap to its neighbours.
  // Nearest-centre pairing swaps them and every swap sweeps a quad across the view,
  // so these groups keep only pairs that are unambiguous in centre and shape.
  struct QuadPrevious {
    std::array<float, 3> world{}, camera{}, predicted{};
    std::array<std::array<float, 3>, 2> worldEdges{}, cameraEdges{};
    float size2 = 0.f, step2 = 0.f;
    bool tracked = false;
  };
  struct QuadCurrent {
    std::array<float, 3> center{};
    std::array<std::array<float, 3>, 2> edges{};
    float size2 = 0.f, shortEdge2 = 0.f;
  };
  struct QuadCandidate {
    float cost = 0.f;
    uint32_t previous = 0, current = 0;
  };
  struct QuadBest {
    float cost = std::numeric_limits<float>::infinity();
    float runnerUp = std::numeric_limits<float>::infinity();
    uint32_t index = UINT32_MAX;
  };
  static std::vector<QuadPrevious> quadPrevious;
  static std::vector<QuadCurrent> quadCurrent;
  static std::vector<QuadCandidate> quadCandidates;
  static std::vector<QuadBest> quadBestForCurrent, quadBestForPrevious;
  static std::array<std::vector<uint32_t>, 2> quadPairs;
  // A first pair that only one reading supports seeds the particle's path but is
  // drawn held; it moves once the next frame lands on the prediction.
  static std::vector<uint8_t> quadSeedOnly;
  quadSeedOnly.assign(s_currentFrameTransforms.size(), 0);

  const auto samePlace = [](const std::array<float, 3>& a, const std::array<float, 3>& b, float size2) noexcept {
    return distance_squared(a, b) <= kQuadSamePlace * (size2 + 1.f);
  };
  const auto buildQuadCandidates = [&](bool followsCamera) {
    quadCandidates.clear();
    for (uint32_t current = 0; current < quadCurrent.size(); ++current) {
      const auto& quad = quadCurrent[current];
      for (uint32_t previous = 0; previous < quadPrevious.size(); ++previous) {
        const auto& before = quadPrevious[previous];
        // `<=` also drops a NaN delta.
        if (!(distance_squared(before.world, quad.center) <=
              kMaximumTranslationPerFrame * kMaximumTranslationPerFrame)) {
          continue;
        }
        const auto shapeDistance = [&](const std::array<std::array<float, 3>, 2>& edges) noexcept {
          return distance_squared(edges[0], quad.edges[0]) + distance_squared(edges[1], quad.edges[1]);
        };
        float centerCost, shapeCost;
        if (before.tracked) {
          centerCost = distance_squared(before.predicted, quad.center);
          if (!(centerCost <= std::max(kQuadPathTolerance * before.step2, kQuadPathFloor * quad.shortEdge2))) {
            continue;
          }
          shapeCost = std::min(shapeDistance(before.worldEdges), shapeDistance(before.cameraEdges));
        } else {
          centerCost = distance_squared(followsCamera ? before.camera : before.world, quad.center);
          shapeCost = shapeDistance(followsCamera ? before.cameraEdges : before.worldEdges);
        }
        const float cost = centerCost + shapeCost;
        if (!(shapeCost <= kQuadShapeChange * quad.size2) || !std::isfinite(cost)) {
          continue;
        }
        quadCandidates.push_back({cost, previous, current});
      }
    }
  };
  // Keeps a pair only when each side prefers the other and beats its runner-up
  // among other particles; the two quads of one cross are not each other's rivals.
  const auto selectQuadPairs = [&](std::vector<uint32_t>& pairs) {
    quadBestForCurrent.assign(quadCurrent.size(), {});
    quadBestForPrevious.assign(quadPrevious.size(), {});
    for (const auto& candidate : quadCandidates) {
      auto& forCurrent = quadBestForCurrent[candidate.current];
      if (candidate.cost < forCurrent.cost) {
        forCurrent.cost = candidate.cost;
        forCurrent.index = candidate.previous;
      }
      auto& forPrevious = quadBestForPrevious[candidate.previous];
      if (candidate.cost < forPrevious.cost) {
        forPrevious.cost = candidate.cost;
        forPrevious.index = candidate.current;
      }
    }
    for (const auto& candidate : quadCandidates) {
      auto& forCurrent = quadBestForCurrent[candidate.current];
      const auto& rival = quadPrevious[candidate.previous];
      if (!samePlace(rival.world, quadPrevious[forCurrent.index].world, rival.size2)) {
        forCurrent.runnerUp = std::min(forCurrent.runnerUp, candidate.cost);
      }
      auto& forPrevious = quadBestForPrevious[candidate.previous];
      const auto& other = quadCurrent[candidate.current];
      if (!samePlace(other.center, quadCurrent[forPrevious.index].center, other.size2)) {
        forPrevious.runnerUp = std::min(forPrevious.runnerUp, candidate.cost);
      }
    }
    pairs.assign(quadCurrent.size(), UINT32_MAX);
    for (uint32_t current = 0; current < quadCurrent.size(); ++current) {
      const auto& forCurrent = quadBestForCurrent[current];
      if (forCurrent.index == UINT32_MAX) {
        continue;
      }
      const auto& forPrevious = quadBestForPrevious[forCurrent.index];
      if (samePlace(quadCurrent[current].center, quadCurrent[forPrevious.index].center,
                    quadCurrent[current].size2) &&
          forCurrent.cost * kQuadAmbiguity < forCurrent.runnerUp &&
          forPrevious.cost * kQuadAmbiguity < forPrevious.runnerUp) {
        pairs[current] = forCurrent.index;
      }
    }
  };
  const auto matchQuadGroup = [&] {
    const size_t previousCount = groupPreviousIndices.size();
    const size_t currentCount = groupCurrentIndices.size();
    if (previousCount * currentCount > kMaximumQuadPairs) {
      return;
    }
    quadPrevious.resize(previousCount);
    for (size_t index = 0; index < previousCount; ++index) {
      const auto& entry = s_previousFrameTransforms[groupPreviousIndices[index]];
      auto& quad = quadPrevious[index];
      quad.world = translation_of(entry.transform.position);
      quad.camera = entry.transform.quadCenter;
      quad.cameraEdges = entry.transform.quadEdges;
      for (size_t edge = 0; edge < 2; ++edge) {
        quad.worldEdges[edge] =
            s_rebaseView ? rotate_vector(s_currentFromPreviousView, quad.cameraEdges[edge]) : quad.cameraEdges[edge];
      }
      quad.size2 = dot3(quad.cameraEdges[0], quad.cameraEdges[0]) + dot3(quad.cameraEdges[1], quad.cameraEdges[1]);
      quad.tracked = entry.hasPrediction;
      if (quad.tracked) {
        quad.predicted = translation_of(entry.predictedPosition);
        quad.step2 = distance_squared(quad.predicted, quad.world);
      }
    }
    quadCurrent.resize(currentCount);
    for (size_t index = 0; index < currentCount; ++index) {
      const auto& transform = s_currentFrameTransforms[groupCurrentIndices[index]].transform;
      auto& quad = quadCurrent[index];
      quad.center = translation_of(transform.position);
      quad.edges = transform.quadEdges;
      const float edge0 = dot3(quad.edges[0], quad.edges[0]);
      const float edge1 = dot3(quad.edges[1], quad.edges[1]);
      quad.size2 = edge0 + edge1;
      quad.shortEdge2 = std::min(edge0, edge1);
    }
    // A particle without a path is read two ways: fixed in the world (smoke left
    // behind) or carried with the camera (speed lines follow the kart). Without a
    // camera rebase the two readings coincide.
    const size_t readings = s_rebaseView ? 2 : 1;
    for (size_t reading = 0; reading < readings; ++reading) {
      buildQuadCandidates(reading == 1);
      selectQuadPairs(quadPairs[reading]);
    }
    // One emitter's particles move alike. Particles with a path show which reading
    // fits: carried ones step less in camera space than in the world.
    bool followsCamera = false;
    if (readings == 2) {
      double worldSteps = 0.0, cameraSteps = 0.0;
      uint32_t tracked = 0, worldFirstSteps = 0, cameraFirstSteps = 0;
      for (size_t current = 0; current < currentCount; ++current) {
        if (const uint32_t previous = quadPairs[0][current]; previous != UINT32_MAX) {
          const auto& before = quadPrevious[previous];
          if (before.tracked) {
            worldSteps += distance_squared(before.world, quadCurrent[current].center);
            cameraSteps += distance_squared(before.camera, quadCurrent[current].center);
            ++tracked;
          } else {
            ++worldFirstSteps;
          }
        }
        if (const uint32_t previous = quadPairs[1][current];
            previous != UINT32_MAX && !quadPrevious[previous].tracked) {
          ++cameraFirstSteps;
        }
      }
      followsCamera = tracked != 0 ? cameraSteps < worldSteps : cameraFirstSteps > worldFirstSteps;
    }
    const auto& pairs = quadPairs[followsCamera ? 1 : 0];
    const auto& otherPairs = quadPairs[followsCamera ? 0 : 1];
    for (size_t current = 0; current < currentCount; ++current) {
      const size_t currentIndex = groupCurrentIndices[current];
      auto& transform = s_currentFrameTransforms[currentIndex].transform;
      const uint32_t previous = pairs[current];
      // A first step shows only when both readings choose it. A newborn that spawns
      // where the last one did fools one reading, rarely both.
      const bool seedOnly = previous != UINT32_MAX && !quadPrevious[previous].tracked &&
                            !(readings == 2 && otherPairs[current] == previous);
      transform.holdInCamera = followsCamera && (previous == UINT32_MAX || seedOnly);
      if (previous == UINT32_MAX) {
        continue;
      }
      const size_t previousIndex = groupPreviousIndices[previous];
      currentToPrevious[currentIndex] = previousIndex;
      currentMatched[currentIndex] = 1;
      previousMatched[previousIndex] = 1;
      quadSeedOnly[currentIndex] = seedOnly;
    }
  };

  const auto matchGroups =
      [&](const auto& currentGroups, const auto& previousGroups,
          bool allowOrderedFallback) {
    for (const auto& [signature, allCurrentIndices] : currentGroups) {
      if (allCurrentIndices.empty()) {
        continue;
      }
      const auto previousIt = previousGroups.find(signature);
      if (previousIt == previousGroups.end() || previousIt->second.empty()) {
        continue;
      }
      groupCurrentIndices.clear();
      groupPreviousIndices.clear();
      for (const size_t index : allCurrentIndices) {
        if (!currentMatched[index]) {
          groupCurrentIndices.push_back(index);
        }
      }
      for (const size_t index : previousIt->second) {
        if (!previousMatched[index]) {
          groupPreviousIndices.push_back(index);
        }
      }
      if (groupCurrentIndices.empty() || groupPreviousIndices.empty()) {
        continue;
      }
      // Draw identity includes the vertex-motion flag, so a group is all quads or none.
      // One quad on each side is no proof either: a particle died and another spawned.
      if (s_currentFrameTransforms[groupCurrentIndices.front()].transform.vertexMotion.enabled) {
        matchQuadGroup();
        continue;
      }

      // A unique draw has no identity ambiguity. Keep the conservative
      // interpolation fallback for malformed/non-finite matrices.
      if (groupPreviousIndices.size() == 1 && groupCurrentIndices.size() == 1) {
        currentToPrevious[groupCurrentIndices.front()] = groupPreviousIndices.front();
        previousMatched[groupPreviousIndices.front()] = 1;
        currentMatched[groupCurrentIndices.front()] = 1;
        continue;
      }

      // Drop pairs past the translation gate (`<=` also drops a NaN delta): they can only
      // render as a snap, and matching them steals a neighbor's true partner.
      const auto appendEdge = [&](size_t previousIndex, size_t currentIndex) {
        const auto& previousEntry = s_previousFrameTransforms[previousIndex];
        const auto& currentSnapshot = s_currentFrameTransforms[currentIndex].transform;
        if (!previousEntry.transform.indexedMatrices && !currentSnapshot.indexedMatrices &&
            !(translation_delta_squared(previousEntry.transform.position,
                                        currentSnapshot.position) <=
              kMaximumTranslationPerFrame * kMaximumTranslationPerFrame)) {
          return;
        }
        const float distance = snapshot_match_distance_squared(previousEntry, currentSnapshot);
        if (std::isfinite(distance)) {
          edges.push_back({
              .distance = distance,
              .previous = previousIndex,
              .current = currentIndex,
          });
        }
      };

      const auto orderedFallback = [&] {
        // GX preserves submission order for these draws, so pair the leftovers in order
        // once exact identities have claimed their instances.
        if (!allowOrderedFallback) {
          return;
        }
        const size_t pairCount = std::min(groupPreviousIndices.size(), groupCurrentIndices.size());
        for (size_t i = 0; i < pairCount; ++i) {
          // Submission order shifts when the transparent sort reorders or culling drops an
          // instance; leave over-gate pairs unmatched so they snap instead of sweeping.
          const auto& previousEntry = s_previousFrameTransforms[groupPreviousIndices[i]];
          const auto& currentEntry = s_currentFrameTransforms[groupCurrentIndices[i]];
          if (!previousEntry.transform.indexedMatrices &&
              !currentEntry.transform.indexedMatrices &&
              !(translation_delta_squared(previousEntry.transform.position,
                                          currentEntry.transform.position) <=
                kMaximumTranslationPerFrame * kMaximumTranslationPerFrame)) {
            continue;
          }
          currentToPrevious[groupCurrentIndices[i]] = groupPreviousIndices[i];
          previousMatched[groupPreviousIndices[i]] = 1;
          currentMatched[groupCurrentIndices[i]] = 1;
        }
      };

      edges.clear();
      const size_t allPairsEdges = groupPreviousIndices.size() * groupCurrentIndices.size();
      if (allPairsEdges <= kAllPairsEdgeLimit) {
        for (const size_t currentIndex : groupCurrentIndices) {
          for (const size_t previousIndex : groupPreviousIndices) {
            appendEdge(previousIndex, currentIndex);
          }
        }
      } else {
        // Grid buckets need one translation per entry, which mixed or skinned groups lack,
        // so those stay on the bounded all-pairs path.
        bool gridable = true;
        for (const size_t index : groupCurrentIndices) {
          if (s_currentFrameTransforms[index].transform.indexedMatrices) {
            gridable = false;
            break;
          }
        }
        if (gridable) {
          for (const size_t index : groupPreviousIndices) {
            if (s_previousFrameTransforms[index].transform.indexedMatrices) {
              gridable = false;
              break;
            }
          }
        }
        if (!gridable) {
          if (groupPreviousIndices.size() > kMaximumAssignmentEdges / groupCurrentIndices.size()) {
            orderedFallback();
            continue;
          }
          for (const size_t currentIndex : groupCurrentIndices) {
            for (const size_t previousIndex : groupPreviousIndices) {
              appendEdge(previousIndex, currentIndex);
            }
          }
        } else {
          // Previous entries are matched against their predicted position, so bucket them by
          // that same reference (see snapshot_match_distance_squared).
          gridCells.clear();
          for (const size_t previousIndex : groupPreviousIndices) {
            const auto& previousEntry = s_previousFrameTransforms[previousIndex];
            const Mat3x4<float>& reference = previousEntry.hasPrediction
                                                 ? previousEntry.predictedPosition
                                                 : previousEntry.transform.position;
            const uint64_t cell = translation_cell(reference);
            if (cell == kInvalidCell) {
              continue;
            }
            gridCells[cell].push_back(static_cast<uint32_t>(previousIndex));
          }
          bool overflowed = false;
          for (const size_t currentIndex : groupCurrentIndices) {
            const auto& position = s_currentFrameTransforms[currentIndex].transform.position;
            const uint64_t cell = translation_cell(position);
            if (cell == kInvalidCell) {
              continue;
            }
            for (int64_t dx = -1; dx <= 1 && !overflowed; ++dx) {
              for (int64_t dy = -1; dy <= 1 && !overflowed; ++dy) {
                for (int64_t dz = -1; dz <= 1 && !overflowed; ++dz) {
                  const uint64_t neighbor = cell + (static_cast<uint64_t>(dx) << 42) +
                                            (static_cast<uint64_t>(dy) << 21) +
                                            static_cast<uint64_t>(dz);
                  const auto cellIt = gridCells.find(neighbor);
                  if (cellIt == gridCells.end()) {
                    continue;
                  }
                  for (const uint32_t previousIndex : cellIt->second) {
                    appendEdge(previousIndex, currentIndex);
                    if (edges.size() > kMaximumGridEdges) {
                      overflowed = true;
                      break;
                    }
                  }
                }
              }
            }
            if (overflowed) {
              break;
            }
          }
          if (overflowed) {
            orderedFallback();
            continue;
          }
        }
      }

      std::sort(edges.begin(), edges.end(), [](const MatchEdge& lhs, const MatchEdge& rhs) {
        return lhs.distance < rhs.distance;
      });
      for (const auto& edge : edges) {
        if (previousMatched[edge.previous] || currentMatched[edge.current]) {
          continue;
        }
        currentToPrevious[edge.current] = edge.previous;
        previousMatched[edge.previous] = 1;
        currentMatched[edge.current] = 1;
      }
    }
  };
  matchGroups(s_currentTransformIndices, s_previousTransformIndices, false);
  // Prefer unchanged meshes across a texture flip before the material-only
  // fallback for deforming geometry. Both passes keep palette topology strict.
  matchGroups(s_currentGeometryTransformIndices, s_previousGeometryTransformIndices, false);
  matchGroups(s_currentStableTransformIndices, s_previousStableTransformIndices, true);

  s_perspectiveMatches = static_cast<uint32_t>(std::count_if(
      currentToPrevious.begin(), currentToPrevious.end(),
      [](size_t previousIndex) { return previousIndex != SIZE_MAX; }));
  // Interpolation never pauses on match quality: an unmatched draw just renders its
  // end-frame state, while a ratio gate flapped the whole output cadence instead.
  const bool eligible = frame_interpolation_fps() != 0 && !late;

  // Overlay observability: the live match ratio, and how often the scene sits in
  // low-match territory where inserted slots mostly duplicate draws.
  {
    constexpr uint32_t kLowMatchPercent = 55;
    const bool replaySafe = frame_interpolation_replay_safe();
    s_diagCandidates.store(s_perspectiveCandidates, std::memory_order_relaxed);
    s_diagMatchable.store(s_perspectiveMatchable, std::memory_order_relaxed);
    s_diagMatches.store(s_perspectiveMatches, std::memory_order_relaxed);
    s_diagEligible.store(eligible, std::memory_order_relaxed);
    s_diagReplaySafe.store(replaySafe, std::memory_order_relaxed);
    if (eligible || stereo_frame_interpolation_active()) {
      s_diagFramesSealed.fetch_add(1, std::memory_order_relaxed);
      if (s_perspectiveMatchable != 0 &&
          s_perspectiveMatches * 100 < s_perspectiveMatchable * kLowMatchPercent) {
        s_diagFramesLowMatch.fetch_add(1, std::memory_order_relaxed);
      }
      if (!replaySafe) {
        s_diagFramesReplayUnsafe.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  // Only ordinary one-matrix draws use motion prediction. Indexed draws keep the slot
  // identity encoded by PNMTXIDX and are prepared as one coherent draw below.
  constexpr float kMaximumPredictionSeedDeltaSquared =
      kMaximumTranslationPerFrame * kMaximumTranslationPerFrame;
  uint32_t quadsHeld = 0;
  for (size_t currentIndex = 0; currentIndex < currentToPrevious.size(); ++currentIndex) {
    const size_t previousIndex = currentToPrevious[currentIndex];
    if (previousIndex == SIZE_MAX) {
      quadsHeld += s_currentFrameTransforms[currentIndex].transform.vertexMotion.enabled;
      continue;
    }
    auto& currentEntry = s_currentFrameTransforms[currentIndex];
    const auto& previousEntry = s_previousFrameTransforms[previousIndex];
    if (currentEntry.transform.indexedMatrices || previousEntry.transform.indexedMatrices) {
      continue;
    }
    // A looping rigid animation can wrap by far less than the teleport limit
    // (Coconut Mall resets its escalator phase every 20 local units). Blending
    // that jump produces a brief reverse sweep. Only cut a large, nearly
    // opposite jump after a measured velocity, with the same mesh and basis.
    // Camera motion has already been removed above. Particle births are not
    // cyclic rigid animations and must not use this test.
    if (s_rebaseView && previousEntry.hasPrediction &&
        !currentEntry.transform.vertexMotion.enabled && currentEntry.identity.geometry != 0 &&
        currentEntry.identity.geometry == previousEntry.identity.geometry) {
      const auto& before = previousEntry.transform.position;
      const auto& now = currentEntry.transform.position;
      const auto& predicted = previousEntry.predictedPosition;
      const Vec4<float>* oldRows[] = {&before.m0, &before.m1, &before.m2};
      const Vec4<float>* newRows[] = {&now.m0, &now.m1, &now.m2};
      const Vec4<float>* predictedRows[] = {&predicted.m0, &predicted.m1, &predicted.m2};
      float speed2 = 0.f, jump2 = 0.f, dot = 0.f, basisDelta = 0.f, basisSize = 0.f;
      for (size_t row = 0; row < 3; ++row) {
        const float velocity = (*predictedRows[row])[3] - (*oldRows[row])[3];
        const float delta = (*newRows[row])[3] - (*oldRows[row])[3];
        speed2 += velocity * velocity;
        jump2 += delta * delta;
        dot += velocity * delta;
        for (size_t col = 0; col < 3; ++col) {
          const float difference = (*newRows[row])[col] - (*oldRows[row])[col];
          basisDelta += difference * difference;
          basisSize += (*newRows[row])[col] * (*newRows[row])[col];
        }
      }
      if (speed2 > 0.0001f && jump2 > std::max(1.f, 16.f * speed2) && dot < 0.f &&
          dot * dot > 0.9f * speed2 * jump2 && basisDelta < 0.0001f * basisSize) {
        currentToPrevious[currentIndex] = SIZE_MAX;
        s_diagAnimationWrapCuts.fetch_add(1, std::memory_order_relaxed);
        continue; // Do not seed the next frame with the reset's apparent velocity.
      }
    }
    // Seed the next frame's matching with a constant-velocity reference, but never from
    // a pair the interpolator would reject as a teleport (`<=` so NaN fails too).
    if (translation_delta_squared(previousEntry.transform.position,
                                  currentEntry.transform.position) <=
        kMaximumPredictionSeedDeltaSquared) {
      currentEntry.predictedPosition = extrapolate_transform(previousEntry.transform.position,
                                                             currentEntry.transform.position);
      currentEntry.hasPrediction = true;
    }
    if (quadSeedOnly[currentIndex]) {
      // Seeded above, drawn held: it moves once the next frame lands on the path.
      currentToPrevious[currentIndex] = SIZE_MAX;
      ++quadsHeld;
    }
  }
  s_diagVertexMotionHeld.store(quadsHeld, std::memory_order_relaxed);

  if (eligible || stereo_frame_interpolation_active()) {
    // Prepare each matched pair once: every sample of a draw shares the same
    // previous/current matrices. A flat vector keeps the sample tasks parallel.
    std::vector<PreparedTransformInterpolation> preparedTransforms(s_currentFrameTransforms.size());
    std::vector<uint8_t> preparedTransformState(s_currentFrameTransforms.size(), 0);
    std::vector<PreparedAffinePair> preparedPairs;
    std::vector<PreparedAffinePair> desktopPairs;
    const bool separateDesktopPairs = s_rebaseView && eligible;
    preparedPairs.reserve(s_pendingUniformInterpolations.size() * 2);

    const auto appendPreparedPair = [&](const Mat3x4<float>& previousPosition,
                                        const Mat3x4<float>& currentPosition,
                                        const Mat3x4<float>& previousNormal,
                                        const Mat3x4<float>& currentNormal,
                                        bool indexed, bool rebased = true, bool vertexMotion = false) {
      const size_t pairOffset = preparedPairs.size();
      const auto retainCurrentBasis = [&](const Mat3x4<float>& position) {
        if (!vertexMotion) return position;
        auto result = currentPosition;
        result.m0[3] = position.m0[3];
        result.m1[3] = position.m1[3];
        result.m2[3] = position.m2[3];
        return result;
      };
      if (separateDesktopPairs) {
        const auto originalPosition = rebased ? gfx::stereo_replay::compose_affine(s_previousFromCurrentView, previousPosition)
                                              : previousPosition;
        const auto originalNormal = rebased ? gfx::stereo_replay::compose_normal(s_previousFromCurrentView, previousNormal)
                                            : previousNormal;
        desktopPairs.push_back(indexed ? prepare_indexed_pair(originalPosition, currentPosition)
                                       : prepare_affine_pair(retainCurrentBasis(originalPosition), currentPosition, true));
        desktopPairs.push_back(indexed ? prepare_indexed_pair(originalNormal, currentNormal)
                                       : prepare_affine_pair(vertexMotion ? currentNormal : originalNormal, currentNormal, true));
      }
      preparedPairs.push_back(indexed ? prepare_indexed_pair(previousPosition, currentPosition)
                                      : prepare_affine_pair(retainCurrentBasis(previousPosition), currentPosition, true));
      preparedPairs.push_back(indexed ? prepare_indexed_pair(previousNormal, currentNormal)
                                      : prepare_affine_pair(vertexMotion ? currentNormal : previousNormal, currentNormal, true));
      return pairOffset;
    };

    // Content-keyed palette pairing: a mesh split across draws repeats bone matrices byte
    // for byte, so a chunk with no partner of its own can borrow a sibling's.
    static std::vector<int32_t> paletteDrawIndex; // draw -> compact palette index
    static std::vector<uint32_t> paletteDraws;    // compact palette index -> draw
    paletteDrawIndex.assign(s_currentFrameTransforms.size(), -1);
    paletteDraws.clear();
    for (size_t drawIndex = 0; drawIndex < s_currentFrameTransforms.size(); ++drawIndex) {
      const auto& transform = s_currentFrameTransforms[drawIndex].transform;
      if (!transform.indexedMatrices || transform.usedMatrixMask == 0) {
        continue;
      }
      paletteDrawIndex[drawIndex] = static_cast<int32_t>(paletteDraws.size());
      paletteDraws.push_back(static_cast<uint32_t>(drawIndex));
    }

    struct PaletteSlotKey {
      HashType hash = 0;
      uint32_t palette = 0;
      uint32_t slot = 0;
    };
    struct ResolvedSlot {
      const Mat3x4<float>* position = nullptr;
      const Mat3x4<float>* normal = nullptr;
    };
    static std::vector<PaletteSlotKey> paletteSlotKeys;
    static std::vector<ResolvedSlot> resolvedSlots;
    static std::vector<size_t> resolvedProjectionEntry;
    paletteSlotKeys.clear();
    resolvedSlots.assign(paletteDraws.size() * MaxPnMtx, ResolvedSlot{});
    resolvedProjectionEntry.assign(paletteDraws.size(), kNoPreparedPair);

    for (uint32_t palette = 0; palette < paletteDraws.size(); ++palette) {
      const auto& transform = s_currentFrameTransforms[paletteDraws[palette]].transform;
      for (uint32_t slot = 0; slot < MaxPnMtx; ++slot) {
        if ((transform.usedMatrixMask & (1u << slot)) == 0) {
          continue;
        }
        paletteSlotKeys.push_back({combine_identity(transform.indexedMatrices->slotHash[slot],
                                                   transform.viewportIdentity), palette, slot});
      }
    }
    std::sort(paletteSlotKeys.begin(), paletteSlotKeys.end(),
              [](const PaletteSlotKey& lhs, const PaletteSlotKey& rhs) {
                if (lhs.hash != rhs.hash) {
                  return lhs.hash < rhs.hash;
                }
                if (lhs.palette != rhs.palette) {
                  return lhs.palette < rhs.palette;
                }
                return lhs.slot < rhs.slot;
              });

    for (size_t runStart = 0; runStart < paletteSlotKeys.size();) {
      size_t runEnd = runStart + 1;
      while (runEnd < paletteSlotKeys.size() &&
             paletteSlotKeys[runEnd].hash == paletteSlotKeys[runStart].hash) {
        ++runEnd;
      }
      // Where this matrix was last frame, per the draws that did match. Partners that
      // disagree mean no single previous pose, so the coupled unit duplicates.
      const Mat3x4<float>* sourcePosition = nullptr;
      const Mat3x4<float>* sourceNormal = nullptr;
      size_t sourceEntry = kNoPreparedPair;
      HashType sourceHash = 0;
      bool ambiguous = false;
      for (size_t keyIndex = runStart; keyIndex < runEnd && !ambiguous; ++keyIndex) {
        const uint32_t drawIndex = paletteDraws[paletteSlotKeys[keyIndex].palette];
        const size_t previousIndex = currentToPrevious[drawIndex];
        if (previousIndex >= s_previousFrameTransforms.size()) {
          continue;
        }
        const auto& current = s_currentFrameTransforms[drawIndex].transform;
        const auto& previous = s_previousFrameTransforms[previousIndex].transform;
        // A slot index is an absolute palette address, which is why it pairs matched draws
        // and why a changed layout invalidates every slot as a source.
        if (!previous.indexedMatrices || previous.usedMatrixMask != current.usedMatrixMask) {
          continue;
        }
        const uint32_t slot = paletteSlotKeys[keyIndex].slot;
        const HashType candidateHash = previous.indexedMatrices->slotHash[slot];
        if (sourcePosition == nullptr) {
          sourcePosition = &previous.indexedMatrices->position[slot];
          sourceNormal = &previous.indexedMatrices->normal[slot];
          sourceHash = candidateHash;
          sourceEntry = previousIndex;
        } else if (candidateHash != sourceHash) {
          ambiguous = true;
        }
      }
      if (ambiguous || sourcePosition == nullptr) {
        runStart = runEnd;
        continue;
      }
      for (size_t keyIndex = runStart; keyIndex < runEnd; ++keyIndex) {
        const auto& key = paletteSlotKeys[keyIndex];
        resolvedSlots[static_cast<size_t>(key.palette) * MaxPnMtx + key.slot] = {sourcePosition,
                                                                                sourceNormal};
        if (resolvedProjectionEntry[key.palette] == kNoPreparedPair) {
          resolvedProjectionEntry[key.palette] = sourceEntry;
        }
      }
      runStart = runEnd;
    }

    for (const auto& task : s_pendingUniformInterpolations) {
      if (task.currentTransformIndex >= preparedTransformState.size() ||
          preparedTransformState[task.currentTransformIndex] != 0) {
        continue;
      }
      preparedTransformState[task.currentTransformIndex] = 1;

      const auto& current = s_currentFrameTransforms[task.currentTransformIndex].transform;
      auto& prepared = preparedTransforms[task.currentTransformIndex];
      if (task.indexedMatrices) {
        const int32_t palette = paletteDrawIndex[task.currentTransformIndex];
        if (palette < 0) {
          continue;
        }
        prepared.previousProjectionEntry = resolvedProjectionEntry[palette];
        // A palette is one deformation unit: interpolating only the resolved slots cracks
        // the mesh, so any unresolved slot duplicates the whole current draw.
        bool allSlotsValid = true;
        bool desktopSlotsValid = true;
        for (size_t slot = 0; slot < MaxPnMtx; ++slot) {
          if ((current.usedMatrixMask & (1u << slot)) == 0) {
            continue;
          }
          const auto& resolved = resolvedSlots[static_cast<size_t>(palette) * MaxPnMtx + slot];
          if (resolved.position == nullptr) {
            allSlotsValid = false;
            desktopSlotsValid = false;
            break;
          }
          const size_t pairOffset =
              appendPreparedPair(*resolved.position, current.indexedMatrices->position[slot],
                                 *resolved.normal, current.indexedMatrices->normal[slot], true);
          prepared.indexedPairOffsets[slot] = pairOffset;
          if (!preparedPairs[pairOffset].valid || !preparedPairs[pairOffset + 1].valid) {
            allSlotsValid = false;
          }
          const auto& desktop = separateDesktopPairs ? desktopPairs : preparedPairs;
          desktopSlotsValid &= desktop[pairOffset].valid && desktop[pairOffset + 1].valid;
        }
        prepared.indexedValid = allSlotsValid;
        prepared.desktopIndexedValid = desktopSlotsValid;
      } else {
        const size_t previousTransformIndex = currentToPrevious[task.currentTransformIndex];
        if (previousTransformIndex >= s_previousFrameTransforms.size()) {
          if (current.holdInCamera && s_rebaseView) {
            // As if it sat at the same camera-space place last frame: the sampled
            // camera then carries it, like the emitter it follows.
            prepared.cameraHold = true;
            prepared.nonIndexedPairOffset = appendPreparedPair(
                gfx::stereo_replay::compose_affine(s_currentFromPreviousView, current.position), current.position,
                current.normal, current.normal, false, true, true);
          }
          continue;
        }
        const auto& previous = s_previousFrameTransforms[previousTransformIndex].transform;
        prepared.previousProjectionEntry = previousTransformIndex;
        prepared.nonIndexedPairOffset = appendPreparedPair(
            previous.position, current.position, previous.normal, current.normal, false, !previous.viewSpaceVertices,
            current.vertexMotion.enabled);
      }
    }

    uint32_t preparedDraws = 0;
    uint32_t rejectedDraws = 0;
    uint32_t vertexMotionDraws = 0;
    for (size_t i = 0; i < preparedTransforms.size(); ++i) {
      if (preparedTransformState[i] == 0)
        continue;
      const auto& prepared = preparedTransforms[i];
      if (prepared.previousProjectionEntry == kNoPreparedPair)
        continue;
      const bool valid = s_currentFrameTransforms[i].transform.indexedMatrices
                             ? prepared.indexedValid
                             : prepared.nonIndexedPairOffset != kNoPreparedPair &&
                                   preparedPairs[prepared.nonIndexedPairOffset].valid &&
                                   preparedPairs[prepared.nonIndexedPairOffset + 1].valid;
      if (valid)
        ++preparedDraws;
      else
        ++rejectedDraws;
      if (valid && s_currentFrameTransforms[i].transform.vertexMotion.enabled)
        ++vertexMotionDraws;
    }
    s_diagPreparedDraws.store(preparedDraws, std::memory_order_relaxed);
    s_diagRejectedDraws.store(rejectedDraws, std::memory_order_relaxed);
    s_diagVertexMotionDraws.store(vertexMotionDraws, std::memory_order_relaxed);

    const auto interpolatePendingUniform = [&](const auto& task) {
      if (task.currentTransformIndex >= s_currentFrameTransforms.size()) {
        return;
      }

      const auto& current = s_currentFrameTransforms[task.currentTransformIndex].transform;
      std::memcpy(task.uniformData, task.sourceUniformData, task.uniformSize);
      const auto& prepared = preparedTransforms[task.currentTransformIndex];
      const bool desktopSample = task.numerator != 0;
      const auto& pairs = desktopSample && separateDesktopPairs ? desktopPairs : preparedPairs;
      if (task.indexedMatrices && !(desktopSample ? prepared.desktopIndexedValid : prepared.indexedValid)) {
        return;
      }
      // The projection comes from whichever previous entry supplied the transforms, which
      // for a borrowed palette is a sibling's partner. A camera hold has no partner.
      const size_t previousTransformIndex = prepared.previousProjectionEntry;
      if (previousTransformIndex >= s_previousFrameTransforms.size() && !prepared.cameraHold) {
        return;
      }
      const auto& previousProjection = prepared.cameraHold
                                           ? current.projection
                                           : s_previousFrameTransforms[previousTransformIndex].transform.projection;
      const float weight =
          static_cast<float>(task.numerator) / static_cast<float>(task.denominator);
      const auto interpolatedProjection =
          interpolate_projection(previousProjection, current.projection, weight);
      std::memcpy(task.uniformData + task.projectionOffset, &interpolatedProjection,
                  sizeof(interpolatedProjection));

      const auto interpolateMatrixSlot = [&](size_t currentIndex, size_t pairOffset) {
        if (pairOffset == kNoPreparedPair) {
          return;
        }
        Mat3x4<float> interpolatedPosition{};
        Mat3x4<float> interpolatedNormal{};
        evaluate_affine_pair(pairs[pairOffset], weight, interpolatedPosition);
        evaluate_affine_pair(pairs[pairOffset + 1], weight, interpolatedNormal);
        if (current.vertexMotion.enabled && !task.indexedMatrices)
          interpolatedPosition = offset_transform_origin(interpolatedPosition, current.vertexMotion.center, -1.f);
        std::memcpy(task.uniformData + task.positionOffset + currentIndex * sizeof(Mat3x4<float>),
                    &interpolatedPosition, sizeof(interpolatedPosition));
        std::memcpy(task.uniformData + task.normalOffset + currentIndex * sizeof(Mat3x4<float>),
                    &interpolatedNormal, sizeof(interpolatedNormal));
      };
      if (task.indexedMatrices) {
        if (!current.indexedMatrices) {
          return;
        }
        for (size_t currentIndex = 0; currentIndex < MaxPnMtx; ++currentIndex) {
          if ((current.usedMatrixMask & (1u << currentIndex)) == 0) {
            continue;
          }
          interpolateMatrixSlot(currentIndex, prepared.indexedPairOffsets[currentIndex]);
        }
      } else {
        interpolateMatrixSlot(task.currentMatrix, prepared.nonIndexedPairOffset);
      }
    };
    // A handful of tasks costs more to schedule than to run. The pool stands in for
    // std::execution::par, which libc++ does not provide at all.
    constexpr size_t kMinimumParallelInterpolationTasks = 64;
    if (s_pendingUniformInterpolations.size() < kMinimumParallelInterpolationTasks) {
      std::for_each(s_pendingUniformInterpolations.begin(), s_pendingUniformInterpolations.end(),
                    interpolatePendingUniform);
    } else {
      InterpolationWorkerPool::instance().run(
          s_pendingUniformInterpolations.size(),
          [&](size_t index) { interpolatePendingUniform(s_pendingUniformInterpolations[index]); });
    }
  }
  s_hasInterpolatedFrame.store(eligible, std::memory_order_release);
  s_pendingUniformInterpolations.clear();
  // The index maps are emptied node-preservingly by the next
  // begin_frame_interpolation.
  retire_frame_transforms();
}

void get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics& diagnostics) noexcept {
  diagnostics.targetFps = frame_interpolation_fps();
  diagnostics.targetSamples = std::min(s_interpolationSampleTarget.load(std::memory_order_acquire),
                                       maximum_interpolation_samples());
  diagnostics.activeSamples = s_activeInterpolationSamples.load(std::memory_order_acquire);
  diagnostics.candidates = s_diagCandidates.load(std::memory_order_relaxed);
  diagnostics.matchable = s_diagMatchable.load(std::memory_order_relaxed);
  diagnostics.matches = s_diagMatches.load(std::memory_order_relaxed);
  diagnostics.eligible = s_diagEligible.load(std::memory_order_relaxed) ? 1 : 0;
  diagnostics.replaySafe = s_diagReplaySafe.load(std::memory_order_relaxed) ? 1 : 0;
  diagnostics.framesSealed = s_diagFramesSealed.load(std::memory_order_relaxed);
  diagnostics.framesLowMatch = s_diagFramesLowMatch.load(std::memory_order_relaxed);
  diagnostics.framesReplayUnsafe = s_diagFramesReplayUnsafe.load(std::memory_order_relaxed);
  diagnostics.slotReductions = s_diagSlotReductions.load(std::memory_order_relaxed);
  diagnostics.lateSealDrops = s_diagLateSealDrops.load(std::memory_order_relaxed);
  diagnostics.preparedDraws = s_diagPreparedDraws.load(std::memory_order_relaxed);
  diagnostics.rejectedDraws = s_diagRejectedDraws.load(std::memory_order_relaxed);
  diagnostics.vertexMotionDraws = s_diagVertexMotionDraws.load(std::memory_order_relaxed);
  diagnostics.vertexMotionHeld = s_diagVertexMotionHeld.load(std::memory_order_relaxed);
  diagnostics.animationWrapCuts = s_diagAnimationWrapCuts.load(std::memory_order_relaxed);
}

bool has_interpolated_frame() noexcept {
  return s_hasInterpolatedFrame.load(std::memory_order_acquire);
}

uint32_t interpolated_frame_count() noexcept {
  if (!has_interpolated_frame()) {
    return 0;
  }
  // The count latched when this frame began recording, not the configured maximum:
  // build_uniform staged exactly this many ranges for every draw.
  return s_activeInterpolationSamples.load(std::memory_order_acquire);
}

void drop_pending_frame_interpolation_uniforms() noexcept {
  // Pending tasks hold raw pointers into the mapped uniform staging range, so anything
  // that unmaps or rotates that buffer first has to drop the tasks.
  s_pendingUniformInterpolations.clear();
}

void mark_frame_interpolation_replay_unsafe() noexcept {
  s_frameInterpolationReplaySafe.store(false, std::memory_order_release);
  // Marking the frame unsafe rotates the staging buffer mid-frame, and the seal then
  // duplicates slots instead of replaying, so drop the copies staged so far.
  drop_pending_frame_interpolation_uniforms();
}

bool frame_interpolation_replay_safe() noexcept {
  return s_frameInterpolationReplaySafe.load(std::memory_order_acquire);
}

void extend_interpolation_draw(uint16_t usedPnMtxMask) noexcept {
  if (s_currentFrameTransforms.empty()) {
    return;
  }
  auto& snapshot = s_currentFrameTransforms.back().transform;
  if (!snapshot.indexedMatrices) {
    // One-matrix draws all read the current matrix index, which a merge cannot
    // have changed either.
    return;
  }
  const uint16_t addedSlots = static_cast<uint16_t>(usedPnMtxMask & ~snapshot.usedMatrixMask);
  if (addedSlots == 0) {
    return;
  }
  for (size_t slot = 0; slot < MaxPnMtx; ++slot) {
    if ((addedSlots & (1u << slot)) == 0) {
      continue;
    }
    snapshot.indexedMatrices->position[slot] = g_gxState.pnMtx[slot].pos;
    snapshot.indexedMatrices->normal[slot] = g_gxState.pnMtx[slot].nrm;
    snapshot.indexedMatrices->slotHash[slot] =
        xxh3_hash_s(&g_gxState.pnMtx[slot].pos, sizeof(Mat3x4<float>),
                    xxh3_hash_s(&g_gxState.pnMtx[slot].nrm, sizeof(Mat3x4<float>)));
  }
  snapshot.usedMatrixMask |= addedSlots;
}

std::array<gfx::Range, MaxInterpolatedFrames> record_interpolation_draw(const FrameInterpolationDrawIdentity& drawIdentity,
                                                                        const Mat4x4<float>& projection,
                                                                        uint16_t usedPnMtxMask,
                                                                        const InterpolatedUniformLayout& uniformLayout,
                                                                        gfx::Range* previousUniform) noexcept {
  if (previousUniform != nullptr) {
    *previousUniform = {};
  }
  // Split-screen cameras can draw identical meshes in unrelated view spaces.
  // Scope exact, material-only and sibling-palette history to the guest viewport.
  // Logical coordinates keep render-scale changes out of the camera identity.
  const auto& viewport = g_gxState.logicalViewport;
  const std::array viewportValues{viewport.left, viewport.top, viewport.width,
                                  viewport.height, viewport.znear, viewport.zfar};
  const HashType viewportIdentity = xxh3_hash_s(viewportValues.data(), sizeof(viewportValues));
  auto identity = drawIdentity;
  identity.combined = combine_identity(identity.combined, viewportIdentity);
  identity.pipeline = combine_identity(identity.pipeline, viewportIdentity);
  FrameTransformSnapshot snapshot{
      .projection = projection,
      .viewportIdentity = viewportIdentity,
      .usedMatrixMask = usedPnMtxMask,
      .vertexMotion = uniformLayout.vertexMotion,
  };
  if (uniformLayout.indexedMatrices) {
    snapshot.indexedMatrices = acquire_indexed_matrices();
    // Only the used slots are copied; consumers mask with usedMatrixMask, so the stale
    // pool contents of unused slots are never read.
    for (size_t i = 0; i < MaxPnMtx; ++i) {
      if ((usedPnMtxMask & (1u << i)) == 0) {
        continue;
      }
      snapshot.indexedMatrices->position[i] = g_gxState.pnMtx[i].pos;
      snapshot.indexedMatrices->normal[i] = g_gxState.pnMtx[i].nrm;
      snapshot.indexedMatrices->slotHash[i] =
          xxh3_hash_s(&g_gxState.pnMtx[i].pos, sizeof(Mat3x4<float>),
                      xxh3_hash_s(&g_gxState.pnMtx[i].nrm, sizeof(Mat3x4<float>)));
    }
  } else {
    const size_t currentMatrix = std::min<size_t>(g_gxState.currentPnMtx, MaxPnMtx - 1);
    snapshot.position = g_gxState.pnMtx[currentMatrix].pos;
    snapshot.normal = g_gxState.pnMtx[currentMatrix].nrm;
    if (uniformLayout.vertexMotion.enabled) {
      snapshot.position = offset_transform_origin(snapshot.position, uniformLayout.vertexMotion.center);
      const Vec4<float>* rows[] = {&snapshot.position.m0, &snapshot.position.m1, &snapshot.position.m2};
      const auto& shape = uniformLayout.vertexShape;
      for (size_t row = 0; row < 3; ++row) {
        const auto& basis = *rows[row];
        snapshot.quadCenter[row] = basis[3];
        snapshot.quadEdges[0][row] = basis[0] * shape.edge0[0] + basis[1] * shape.edge0[1] + basis[2] * shape.edge0[2];
        snapshot.quadEdges[1][row] = basis[0] * shape.edge1[0] + basis[1] * shape.edge1[1] + basis[2] * shape.edge1[2];
      }
    } else if (g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT) {
      const Vec4<float>* rows[] = {&snapshot.position.m0, &snapshot.position.m1, &snapshot.position.m2};
      snapshot.viewSpaceVertices = true;
      for (size_t row = 0; row < 3; ++row)
        for (size_t col = 0; col < 4; ++col)
          snapshot.viewSpaceVertices &= (*rows[row])[col] == (row == col ? 1.f : 0.f);
    }
  }

  const size_t currentTransformIndex = s_currentFrameTransforms.size();
  s_currentFrameTransforms.emplace_back(FrameTransformEntry{
      .identity = identity,
      .transform = std::move(snapshot),
  });
  s_currentTransformIndices[identity.combined].push_back(currentTransformIndex);
  const HashType stableIdentity = stable_identity(identity);
  s_currentStableTransformIndices[stableIdentity].push_back(currentTransformIndex);
  if (identity.geometry != 0)
    s_currentGeometryTransformIndices[geometry_identity(identity)].push_back(currentTransformIndex);

  std::array<gfx::Range, MaxInterpolatedFrames> interpolatedRanges{};
  ++s_perspectiveCandidates;
  const auto exactPrevious = s_previousTransformIndices.find(identity.combined);
  const auto stablePrevious = s_previousStableTransformIndices.find(stableIdentity);
  const auto geometryPrevious = identity.geometry != 0
                                    ? s_previousGeometryTransformIndices.find(geometry_identity(identity))
                                    : s_previousGeometryTransformIndices.end();
  const bool hasPreviousPartner =
      (exactPrevious != s_previousTransformIndices.end() && !exactPrevious->second.empty()) ||
      (stablePrevious != s_previousStableTransformIndices.end() && !stablePrevious->second.empty()) ||
      (geometryPrevious != s_previousGeometryTransformIndices.end() && !geometryPrevious->second.empty());
  if (hasPreviousPartner) {
    ++s_perspectiveMatchable;
  }
  // A skinned draw with no identity partner can still borrow sibling transforms at
  // seal time, so stage copies whenever the previous frame held any palette.
  const bool stageInterpolation =
      hasPreviousPartner ||
      (uniformLayout.indexedMatrices && s_previousFrameHasIndexedMatrices);
  // A frame already split by a submitted prefix duplicates its slots instead of
  // replaying, so staging copies for the resumed suffix would only waste space.
  if (frame_interpolation_replay_safe() && stageInterpolation) {
    const uint32_t sampleCount = s_activeInterpolationSamples.load(std::memory_order_acquire);
    for (uint32_t sample = 0; sample < sampleCount; ++sample) {
      auto [interpolatedBuffer, interpolatedRange] = gfx::map_uniform(uniformLayout.uniformSize);
      s_pendingUniformInterpolations.push_back({
          .currentTransformIndex = currentTransformIndex,
          .sourceUniformData = uniformLayout.sourceUniformData,
          .uniformData = interpolatedBuffer.data(),
          .uniformSize = uniformLayout.uniformSize,
          .projectionOffset = uniformLayout.projectionOffset,
          .positionOffset = uniformLayout.positionOffset,
          .normalOffset = uniformLayout.normalOffset,
          .currentMatrix = uniformLayout.currentMatrix,
          .numerator = sample + 1,
          .denominator = sampleCount + 1,
          .indexedMatrices = uniformLayout.indexedMatrices,
      });
      interpolatedRanges[sample] = interpolatedRange;
    }
    // Keep the matched previous endpoint, in the current palette's layout, for
    // arbitrary headset display times. It shares desktop matching and cut guards.
    if (previousUniform != nullptr && stereo_frame_interpolation_active()) {
      auto [buffer, range] = gfx::map_uniform(uniformLayout.uniformSize);
      std::memcpy(buffer.data(), uniformLayout.sourceUniformData, uniformLayout.uniformSize);
      s_pendingUniformInterpolations.push_back({
          .currentTransformIndex = currentTransformIndex,
          .sourceUniformData = uniformLayout.sourceUniformData,
          .uniformData = buffer.data(),
          .uniformSize = uniformLayout.uniformSize,
          .projectionOffset = uniformLayout.projectionOffset,
          .positionOffset = uniformLayout.positionOffset,
          .normalOffset = uniformLayout.normalOffset,
          .currentMatrix = uniformLayout.currentMatrix,
          .numerator = 0,
          .denominator = 1,
          .indexedMatrices = uniformLayout.indexedMatrices,
      });
      *previousUniform = range;
    }
  }
  return interpolatedRanges;
}
} // namespace aurora::gx
