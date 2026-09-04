#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef enum {
  AURORA_DISPLAY_MODE_WINDOWED,
  AURORA_DISPLAY_MODE_BORDERLESS,
  AURORA_DISPLAY_MODE_EXCLUSIVE,
} AuroraDisplayMode;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

enum { AURORA_STEREO_EYE_COUNT = 2 };

/**
 * One eye of a stereo frame supplied by the host application.
 *
 * projection is row-major and supplies the OpenXR frustum's X/Y scale and
 * asymmetric-center terms at [0][0], [0][2], [1][1], and [1][2]. Aurora
 * applies those four values to each perspective GX draw while preserving the
 * draw's own depth mapping and renderer depth-range adjustment.
 *
 * viewFromCenter is a row-major affine 3x4 transform from the center-eye view
 * space into this eye's view space. Identity keeps the recorded view and is
 * useful when the game has already applied the eye transform before issuing GX
 * commands. That center-eye space is the game's recorded view space unless
 * aurora_set_stereo_scene_anchor() relocated the camera for the sealed frame,
 * in which case the anchor is composed in for world draws only.
 *
 * Both transforms are ignored in AURORA_STEREO_FRAME_VIRTUAL_SCREEN mode.
 */
typedef struct {
  uint32_t width;
  uint32_t height;
  float projection[16];
  float viewFromCenter[12];
} AuroraStereoEye;

typedef enum {
  // Replay perspective GX draws with the supplied per-eye transforms.
  AURORA_STEREO_FRAME_IMMERSIVE_REPLAY = 0,
  // Copy the completed mono present source to both eye outputs. The OpenXR
  // backend can present these images as a compositor quad layer.
  AURORA_STEREO_FRAME_VIRTUAL_SCREEN = 1,
} AuroraStereoFrameMode;

// aurora_end_frame() uses this sentinel when its caller cannot associate a
// sealed frame with an application safety state. Immersive providers are only
// accepted through aurora_end_frame_tagged() with an exact matching tag.
#define AURORA_STEREO_CONTENT_TAG_UNKNOWN UINT64_MAX

/**
 * Stereo data for one sealed GX frame. frameToken is opaque to Aurora and is
 * forwarded unchanged to the internal stereo output sink. contentTag must
 * match the tag latched by aurora_end_frame_tagged() for immersive replay.
 */
typedef struct {
  uint64_t frameToken;
  AuroraStereoEye eyes[AURORA_STEREO_EYE_COUNT];
  // Appended to preserve the frameToken/eyes prefix used by older providers.
  AuroraStereoFrameMode mode;
  uint64_t contentTag;
} AuroraStereoFrame;

/**
 * Called on Aurora's frame worker immediately before a GX frame is sealed.
 * Return false to render that logical frame in mono only. The callback must
 * be non-blocking and must not call back into Aurora.
 */
typedef bool (*AuroraStereoFrameProvider)(uint32_t logicalFrame, AuroraStereoFrame* frame, void* userdata);

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  // Read-only application resources. Defaults to SDL_GetBasePath(), which is
  // where release builds place initial_pipeline_cache.db.
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  uint16_t maxTextureAnisotropy;
  // No vsync knob exists: the swapchain is always configured for a
  // non-blocking present mode (Immediate, else Mailbox). See best_present_mode.
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureReplacements;
  bool allowTextureDumps;
  bool disableCopyFilter;
  // When false, Aurora centers the first window. When true, windowPosX/Y are restored verbatim,
  // including negative coordinates on monitors left of or above the primary display.
  bool hasWindowPosition;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;

  // Optional directory for the portable GX pipeline database. When null, the
  // database is stored in cachePath with Dawn's machine-specific cache.
  const char* pipelineCachePath;

  // Enables renderer features needed by an external XR compositor. The normal
  // desktop path is unchanged when false.
  bool xrInterop;
  // Optional OpenXR-selected D3D adapter. Supplying the runtime's LUID before
  // device creation keeps Dawn and the compositor on the same physical GPU.
  bool hasD3D12AdapterLuid;
  uint32_t d3d12AdapterLuidLow;
  int32_t d3d12AdapterLuidHigh;
} AuroraConfig;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
const AuroraEvent* aurora_update();
bool aurora_begin_frame();
void aurora_end_frame();
// Seal the current frame with an opaque application safety tag. Aurora rejects
// an immersive provider packet unless its contentTag matches this exact frame.
void aurora_end_frame_tagged(uint64_t contentTag);
/**
 * Relocates the immersive camera for the frame about to be sealed.
 *
 * anchorFromScene is a row-major affine 3x4 transform from the game's recorded
 * view space into the view space the headset should render from, in world
 * units. Identity keeps the recorded camera, which is the default and the
 * behaviour of every frame that does not call this.
 *
 * Perspective draws carry the recorded camera in their own position matrices,
 * so they are replayed through viewFromCenter * anchorFromScene. The 2D virtual
 * screen is defined in the relocated camera's space and keeps viewFromCenter.
 *
 * This is latched by the next aurora_end_frame*(), then cleared: the anchor
 * belongs to the guest frame that produced the GX content, so it must be
 * published per frame from the producer thread rather than by the stereo
 * provider, which cannot know which frame will consume its packet.
 */
void aurora_set_stereo_scene_anchor(const float anchorFromScene[12]);
typedef void (*AuroraFrameWorkerWaitCallback)();
// Called from the producer thread at bounded intervals while Aurora waits for
// the asynchronous frame worker. The callback must not enter Aurora.
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback);
// Registering nullptr restores the ordinary mono-only render path. Replace or
// unregister a provider only while Aurora's frame worker is idle.
void aurora_set_stereo_frame_provider(AuroraStereoFrameProvider provider, void* userdata);
void aurora_wait_for_frame_worker();
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros);
// Producer-thread shutdown barrier. If an asynchronous cycle is waiting for
// the next begin-frame permission, grant that permission and wait until the
// worker is fully done. Unlike aurora_wait_for_frame_worker(), this is safe in
// the gap between aurora_end_frame() and aurora_begin_frame().
void aurora_quiesce_frame_worker();
// Absolute schedule for the next sealed frame, on steady_clock: baseNanos anchors the group and
// intervalNanos is the period, so slot k of N+1 fires at base + k*interval/(N+1). Zeros clear it.
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos);
// Reports whether the frame about to be sealed met its display boundary. Interpolation sizes its
// slot group from this, backing off after misses. Only paced presents may report.
void aurora_report_producer_paced(bool paced);
void aurora_request_frame_capture(uint32_t frame, const char* outputPath);
bool aurora_flush_efb_copies_to_ram();
bool aurora_flush_efb_copy_to_ram(void* dest);

void aurora_set_log_level(AuroraLogLevel level);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_display_mode(AuroraDisplayMode mode);
AuroraDisplayMode aurora_get_display_mode();

AuroraBackend aurora_get_backend();
const AuroraBackend* aurora_get_available_backends(size_t* count);

#ifdef __cplusplus
}
#endif

#endif
