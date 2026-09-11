// Optional GPU smoke test: a 60 Hz GX producer with an independent 90 Hz
// compositor. Exercises the real frame worker, eye replay and submission sink.
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <aurora/main.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include "stereo.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using Clock = std::chrono::steady_clock;
static void WaitUntil(Clock::time_point deadline) {
#if defined(_WIN32)
  // Match the runtime's high-resolution pacing. Sleep's coarse Windows timer
  // would otherwise turn an offscreen 90 Hz test into a roughly 60 Hz test.
  struct Timer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_MODIFY_STATE | SYNCHRONIZE);
    ~Timer() {
      if (handle != nullptr)
        CloseHandle(handle);
    }
  };
  static thread_local Timer timer;
  const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
  if (remaining <= 0)
    return;
  LARGE_INTEGER due{};
  due.QuadPart = -std::max<int64_t>(remaining / 100, 1);
  if (timer.handle != nullptr && SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0)) {
    WaitForSingleObject(timer.handle, INFINITE);
    return;
  }
#endif
  std::this_thread::sleep_until(deadline);
}
static std::mutex packetMutex;
static std::condition_variable packetCv;
static AuroraStereoFrame packet{};
static bool available = false;
static bool stop = false;
static uint64_t completed = 0;
static std::atomic_uint32_t submitted{0};
static uint64_t wakeLateness = 0, submitTime = 0, skippedTicks = 0, compositorFrames = 0;

static bool Provide(uint32_t, AuroraStereoFrame* output, void*) {
  std::lock_guard lock(packetMutex);
  if (!available)
    return false;
  *output = packet;
  available = false;
  return true;
}
static bool Encode(wgpu::CommandEncoder&, const aurora::stereo::SinkFrame&, void*) noexcept { return true; }
static void Submitted(const aurora::stereo::SinkFrame& frame, void*) noexcept {
  std::lock_guard lock(packetMutex);
  completed = frame.frameToken;
  ++submitted;
  packetCv.notify_all();
}
static void Log(AuroraLogLevel level, const char* module, const char* message, unsigned int length) {
  if (level >= LOG_WARNING)
    std::fprintf(stderr, "%s: %.*s\n", module, static_cast<int>(length), message);
}

int main(int argc, char** argv) {
  // Extra distinct draws expose CPU uniform/replay costs that a single triangle
  // cannot exercise. Keep the eye targets small to isolate that regression.
  const unsigned drawCount = argc > 1 ? std::max(1, std::atoi(argv[1])) : 1;
  const int mode = argc < 3 ? 1 : std::clamp(std::atoi(argv[2]), 0, 2);
  const bool indexed = argc > 3 && std::atoi(argv[3]) != 0;
  std::filesystem::create_directories("stereo-smoke-cache");
  AuroraConfig config{};
  config.appName = "Aurora VR interpolation smoke";
  config.userPath = ".";
  config.cachePath = "stereo-smoke-cache";
  config.desiredBackend = BACKEND_D3D12;
  config.windowWidth = 160;
  config.windowHeight = 120;
  config.hasWindowPosition = true;
  config.windowPosX = -30000;
  config.windowPosY = -30000;
  config.logCallback = Log;
  config.logLevel = LOG_WARNING;
  config.xrInterop = true;
  aurora_initialize(argc, argv, &config);
  aurora_set_frame_interpolation_fps(0);
  aurora_set_stereo_frame_interpolation(mode != 0);
  aurora_set_stereo_frame_provider(Provide, nullptr);
  aurora::stereo::set_sink(Encode, Submitted, nullptr);

  std::thread compositor([] {
    const auto start = Clock::now();
    uint64_t slot = 1;
    for (uint64_t token = 1;; ++token) {
      const auto deadline = start + std::chrono::nanoseconds(slot * 1'000'000'000 / 90);
      WaitUntil(deadline);
      const auto woke = Clock::now();
      wakeLateness +=
          std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(woke - deadline).count());
      {
        std::lock_guard lock(packetMutex);
        if (stop)
          return;
        packet = {};
        packet.frameToken = token;
        packet.contentTag = 42;
        // Wake to render the next display tick, as xrWaitFrame does, rather
        // than announcing an image whose display deadline has already passed.
        packet.displayTimeNanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count() +
            1'000'000'000 / 90;
        for (auto& eye : packet.eyes) {
          eye.width = 160;
          eye.height = 120;
          eye.projection[0] = eye.projection[5] = 1;
          eye.projection[10] = -1;
          eye.projection[11] = -1;
          eye.projection[14] = -1;
          eye.viewFromCenter[0] = eye.viewFromCenter[5] = eye.viewFromCenter[10] = 1;
        }
        available = true;
      }
      aurora_notify_stereo_frame();
      std::unique_lock lock(packetMutex);
      packetCv.wait(lock, [&] { return stop || completed == token; });
      if (stop)
        return;
      // Retain the current render tick (its display deadline is still ahead),
      // but discard older ticks. Skipping even the current tick would insert
      // an idle period whenever rendering overruns a wakeup by a fraction.
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
      submitTime += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - woke).count();
      ++compositorFrames;
      const auto nextSlot = std::max(slot + 1, static_cast<uint64_t>(elapsed) * 90 / 1'000'000'000);
      skippedTicks += nextSlot - slot - 1;
      slot = nextSlot;
    }
  });

  constexpr uint64_t warmupFrames = 60;
  auto start = Clock::now();
  auto measuredStart = start;
  uint32_t initialSubmissions = 0;
  for (uint64_t frame = 0; frame < warmupFrames + 240; ++frame) {
    if (frame == warmupFrames) {
      // Measure a running scene after shader compilation and initial resource
      // creation, and rebase the producer so it owes no catch-up frames.
      aurora_wait_for_frame_worker();
      measuredStart = Clock::now();
      start = measuredStart - std::chrono::nanoseconds(frame * 1'000'000'000 / 60);
      initialSubmissions = submitted.load();
    }
    const auto boundary = start + std::chrono::nanoseconds((frame + 1) * 1'000'000'000 / 60);
    WaitUntil(boundary);
    aurora_update();
    if (!aurora_begin_frame())
      continue;
    // A live setting change can arrive after this batch's backing memory was
    // selected. Exercise both directions without restarting the renderer.
    if (mode == 2 && frame % 60 == 0)
      aurora_set_stereo_frame_interpolation((frame / 60) % 2 == 0);
    aurora_set_present_schedule(
        std::chrono::duration_cast<std::chrono::nanoseconds>(boundary.time_since_epoch()).count(), 16'666'667);
    Mtx44 projection{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, -1, -1}, {0, 0, -1, 0}};
    Mtx transform{{1, 0, 0, static_cast<float>(frame % 60) * 0.01f}, {0, 1, 0, 0}, {0, 0, 1, -3}};
    GXSetProjection(projection, GX_PERSPECTIVE);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetViewport(0, 0, 160, 120, 0, 1);
    GXSetScissor(0, 0, 160, 120);
    GXClearVtxDesc();
    if (indexed)
      GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetNumTexGens(0);
    GXSetNumChans(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    // Keep palette triangles small to limit fill cost during uniform stress.
    const float extent = indexed ? 0.02f : 1.0f;
    if (indexed) {
      for (unsigned matrix = 0; matrix < 10; ++matrix)
        GXLoadPosMtxImm(transform, matrix * 3);
    }
    for (unsigned draw = 0; draw < drawCount; ++draw) {
      transform[1][3] = static_cast<float>(draw % 20) * 0.01f;
      GXLoadPosMtxImm(transform, GX_PNMTX0);
      GXBegin(GX_TRIANGLES, GX_VTXFMT0, indexed ? 30 : 3);
      for (unsigned matrix = 0; matrix < (indexed ? 10u : 1u); ++matrix) {
        if (indexed)
          GXMatrixIndex1u8(GX_VA_PNMTXIDX, matrix * 3);
        GXPosition3f32((-1 + static_cast<float>(draw) * 0.0001f) * extent, -extent, 0);
        if (indexed)
          GXMatrixIndex1u8(GX_VA_PNMTXIDX, matrix * 3);
        GXPosition3f32(extent, -extent, 0);
        if (indexed)
          GXMatrixIndex1u8(GX_VA_PNMTXIDX, matrix * 3);
        GXPosition3f32(0, extent, 0);
      }
      GXEnd();
    }
    aurora_end_frame_tagged(42);
    // The runtime pre-warms immediately after its asynchronous seal. The
    // worker needs this permit before publishing SEALED or encoding XR eyes.
    // Waiting until the next 60 Hz tick would strand it for a whole interval.
    aurora_begin_frame();
    if (drawCount > 1 && frame >= warmupFrames && (frame + 1) % 60 == 0) {
      std::printf("Completed %llu producer frames in %.2f s\n",
                  static_cast<unsigned long long>(frame + 1 - warmupFrames),
                  std::chrono::duration<double>(Clock::now() - measuredStart).count());
      std::fflush(stdout);
    }
  }
  {
    std::lock_guard lock(packetMutex);
    stop = true;
    packetCv.notify_all();
  }
  compositor.join();
  aurora_set_stereo_frame_interpolation(false);
  aurora_quiesce_frame_worker();
  aurora_set_stereo_frame_provider(nullptr, nullptr);
  aurora::stereo::set_sink(nullptr, nullptr);
  const double elapsed = std::chrono::duration<double>(Clock::now() - measuredStart).count();
  const auto measuredSubmissions = submitted.load() - initialSubmissions;
  const double fps = measuredSubmissions / elapsed;
  if (compositorFrames != 0)
    std::printf("Compositor (including warm-up): wake late %.2f ms; submit %.2f ms; skipped %llu ticks\n",
                wakeLateness / (1.0e6 * compositorFrames), submitTime / (1.0e6 * compositorFrames),
                static_cast<unsigned long long>(skippedTicks));
  std::printf("%u draws: producer %.1f FPS; %u stereo submissions in %.2f s (%.1f FPS)\n", drawCount, 240 / elapsed,
              measuredSubmissions, elapsed, fps);
  aurora_shutdown();
  // More headset submissions must not come at the expense of simulation speed.
  const double target = mode == 2 ? 75 : mode == 1 ? 90 : 60;
  return 240 / elapsed > 55 && fps > target - 5 && fps < target + 5 ? 0 : 1;
}
