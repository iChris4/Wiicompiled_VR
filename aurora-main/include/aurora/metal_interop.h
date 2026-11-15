#ifndef AURORA_METAL_INTEROP_H
#define AURORA_METAL_INTEROP_H

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#else
#include "stdbool.h"
#include "stdint.h"
#endif

/**
 * Apple/Metal counterpart of aurora/vulkan_interop.h, for the Apple Vision Pro
 * build (docs/visionos-port.md).
 *
 * CompositorServices has no notion of an application-owned swapchain, and
 * Dawn's Metal device is never handed to the compositor. So the headset side
 * (runtime/src/vr/visionos) owns IOSurface-backed images: Aurora imports each
 * IOSurface as Dawn shared texture memory and copies an eye into it inside the
 * frame worker's command buffer, and the compositor side wraps the same
 * IOSurface in an MTLTexture on its own command queue and copies it into the
 * drawable CompositorServices hands out for the frame. The two queues live on
 * the one GPU and are ordered with MTLSharedEvents (Dawn's
 * SharedFenceMTLSharedEvent), the Metal equivalent of the sync file
 * descriptors the Quest build uses.
 *
 * Every handle in this API is a plain C value so the runtime never includes
 * Dawn's C++ headers: an IOSurfaceRef or an id<MTLSharedEvent> travels as a
 * void*. Metal pixel formats travel as their MTLPixelFormat enum values, spelled
 * out below so a translation unit without <Metal/Metal.h> can name them.
 */

enum { AURORA_METAL_STEREO_MAX_TARGETS = 2 };
// The eyes, then the settings panel's quad-layer image when one was given.
enum { AURORA_METAL_STEREO_MAX_RELEASES = AURORA_METAL_STEREO_MAX_TARGETS + 1 };

// MTLPixelFormat values (Metal/MTLPixelFormat.h); Aurora's eye output is one of the UNORM ones.
enum {
  AURORA_MTL_PIXEL_FORMAT_INVALID = 0,
  AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM = 70,
  AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM_SRGB = 71,
  AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM = 80,
  AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM_SRGB = 81,
  AURORA_MTL_PIXEL_FORMAT_RGBA16_FLOAT = 115,
};

/**
 * Borrowed facts about Aurora's Dawn Metal device. colorMetalPixelFormat is
 * the MTLPixelFormat matching Aurora's single-sample eye output, and the two
 * flags say whether the device was created with the Dawn features the bridge
 * needs (gpu.cpp requests them under AuroraConfig::xrInterop).
 */
typedef struct {
  int64_t colorMetalPixelFormat;
  bool sharedTextureMemoryIOSurface;
  bool sharedFenceMTLSharedEvent;
} AuroraMetalNativeHandles;

/**
 * One IOSurface the next Aurora stereo sink must copy an eye into. The surface
 * is imported into Dawn on first use and the import is cached, keyed by the
 * IOSurfaceRef, for as long as the bridge lives; callers should recycle a small
 * ring of surfaces rather than allocating per frame. Aurora retains the
 * IOSurfaceRef while the import lives.
 *
 * acquireEvent/acquireValue name the MTLSharedEvent value Dawn waits for
 * before writing (the compositor side's previous copy out of this surface), or
 * NULL when the surface has no pending reader. The event is only borrowed for
 * the call: Dawn retains what it needs.
 */
typedef struct {
  void* ioSurface;
  uint32_t width;
  uint32_t height;
  int64_t metalPixelFormat;
  void* acquireEvent;
  uint64_t acquireValue;
} AuroraMetalStereoTarget;

/**
 * Per-target result handed to the submitted callback. releaseEvent is an
 * id<MTLSharedEvent> that reaches releaseValue once Aurora's copy into the
 * surface has completed on Dawn's queue (NULL if Dawn reported no fence). The
 * event is handed over retained (+1); the callee releases it with CFRelease
 * once it has encoded its wait.
 */
typedef struct {
  void* releaseEvent;
  uint64_t releaseValue;
} AuroraMetalStereoRelease;

/**
 * Fired when Aurora either finishes or abandons the stereo sink. `success`
 * guarantees that the copies were submitted and that every release entry is
 * valid. Otherwise `gpuWorkQueued` says whether the copies may have reached
 * Dawn's queue before the failure (the shared surfaces may then be written with
 * no fence to wait on) or nothing was recorded at all. The callback runs on
 * Aurora's frame worker while its queue-submit mutex is held: it may encode and
 * commit work on the compositor side's own Metal queue, but must not wait for
 * the GPU or re-enter Aurora.
 */
typedef void (*AuroraMetalStereoSubmittedCallback)(uint64_t frameToken, bool success, bool gpuWorkQueued,
                                                   const AuroraMetalStereoRelease* releases,
                                                   uint32_t releaseCount, void* userdata);

/** Returns false unless the active Aurora backend is Dawn Metal. */
bool aurora_metal_get_native_handles(AuroraMetalNativeHandles* handles);

/**
 * Installs the internal IOSurface stereo sink. Call while Aurora's frame worker
 * is idle, after aurora_initialize(). Fails when the Dawn device was not created
 * with the IOSurface shared-memory and MTLSharedEvent fence features.
 */
bool aurora_metal_enable_stereo_bridge(AuroraMetalStereoSubmittedCallback submitted, void* userdata);

/**
 * Publishes the surface(s) for frameToken. Immersive projection frames supply
 * two targets; virtual-screen quad frames supply one. Exactly one frame may be
 * pending at a time.
 */
bool aurora_metal_set_stereo_targets(uint64_t frameToken, const AuroraMetalStereoTarget* targets,
                                     uint32_t targetCount);

/**
 * The same, plus the headset settings panel's quad-layer surface when `panel` is
 * not null (aurora_set_stereo_panel_layer): Aurora copies the panel into it, or
 * a transparent image while the panel is not showing, with the eyes. Its
 * release entry follows the eyes' in the submitted callback, whose
 * releaseCount then counts it too.
 */
bool aurora_metal_set_stereo_targets_with_panel(uint64_t frameToken, const AuroraMetalStereoTarget* targets,
                                                uint32_t targetCount, const AuroraMetalStereoTarget* panel);

/**
 * Withdraws frameToken only while its targets have not been encoded. Semantics
 * match aurora_vulkan_cancel_stereo_targets: false means the worker already
 * owns encoded work and the submitted callback remains the completion
 * authority. A successful cancellation fires no callback.
 */
bool aurora_metal_cancel_stereo_targets(uint64_t frameToken);

/**
 * The host is about to destroy these target IOSurfaces (the eye swapchains, when
 * the render resolution changes): the bridge drops its Dawn imports of them.
 * Dawn keeps a texture a queued copy still uses alive until that copy is done,
 * and the host orders the compositor's reads with the release fences, so no
 * wait is needed. Call with no frame token pending; false when one is (keep the
 * surfaces and try again later).
 */
bool aurora_metal_forget_stereo_targets(void* const* ioSurfaces, uint32_t count);

/**
 * Removes the sink and releases the cached Dawn imports. The worker must be
 * idle. Returns false only when Dawn could not be drained, in which case the
 * bridge is retained for the process lifetime.
 */
bool aurora_metal_disable_stereo_bridge();

#ifdef __cplusplus
}
#endif

#endif
