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

using Clock = std::chrono::steady_clock;
static std::mutex packetMutex;
static std::condition_variable packetCv;
static AuroraStereoFrame packet{};
static bool available = false;
static bool stop = false;
static uint64_t completed = 0;
static std::atomic_uint32_t submitted{0};

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
  const bool interpolate = argc < 3 || std::atoi(argv[2]) != 0;
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
  aurora_set_stereo_frame_interpolation(interpolate);
  aurora_set_stereo_frame_provider(Provide, nullptr);
  aurora::stereo::set_sink(Encode, Submitted, nullptr);

  std::thread compositor([] {
    const auto start = Clock::now();
    for (uint64_t token = 1;; ++token) {
      const auto deadline = start + std::chrono::nanoseconds(token * 1'000'000'000 / 90);
      std::this_thread::sleep_until(deadline);
      {
        std::lock_guard lock(packetMutex);
        if (stop)
          return;
        packet = {};
        packet.frameToken = token;
        packet.contentTag = 42;
        packet.displayTimeNanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
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
    }
  });

  const auto start = Clock::now();
  for (uint64_t frame = 0; frame < 240; ++frame) {
    const auto boundary = start + std::chrono::nanoseconds((frame + 1) * 1'000'000'000 / 60);
    std::this_thread::sleep_until(boundary);
    aurora_update();
    if (!aurora_begin_frame())
      continue;
    aurora_set_present_schedule(
        std::chrono::duration_cast<std::chrono::nanoseconds>(boundary.time_since_epoch()).count(), 16'666'667);
    Mtx44 projection{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, -1, -1}, {0, 0, -1, 0}};
    Mtx transform{{1, 0, 0, static_cast<float>(frame % 60) * 0.01f}, {0, 1, 0, 0}, {0, 0, 1, -3}};
    GXSetProjection(projection, GX_PERSPECTIVE);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetViewport(0, 0, 160, 120, 0, 1);
    GXSetScissor(0, 0, 160, 120);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetNumTexGens(0);
    GXSetNumChans(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    for (unsigned draw = 0; draw < drawCount; ++draw) {
      transform[1][3] = static_cast<float>(draw % 20) * 0.01f;
      GXLoadPosMtxImm(transform, GX_PNMTX0);
      GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
      GXPosition3f32(-1 + static_cast<float>(draw) * 0.0001f, -1, 0);
      GXPosition3f32(1, -1, 0);
      GXPosition3f32(0, 1, 0);
      GXEnd();
    }
    aurora_end_frame_tagged(42);
    if (drawCount > 1 && (frame + 1) % 60 == 0) {
      std::printf("Completed %llu producer frames in %.2f s\n", static_cast<unsigned long long>(frame + 1),
                  std::chrono::duration<double>(Clock::now() - start).count());
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
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  const double fps = submitted.load() / elapsed;
  std::printf("%u draws: producer %.1f FPS; %u stereo submissions in %.2f s (%.1f FPS)\n", drawCount, 240 / elapsed,
              submitted.load(), elapsed, fps);
  aurora_shutdown();
  // More headset submissions must not come at the expense of simulation speed.
  return 240 / elapsed > 55 && fps > (interpolate ? 85 : 55) && fps < (interpolate ? 100 : 65) ? 0 : 1;
}
