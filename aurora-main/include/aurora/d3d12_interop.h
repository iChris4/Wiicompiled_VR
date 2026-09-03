#ifndef AURORA_D3D12_INTEROP_H
#define AURORA_D3D12_INTEROP_H

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#else
#include "stdbool.h"
#include "stdint.h"
#endif

enum { AURORA_D3D12_STEREO_MAX_TARGETS = 2 };

/**
 * Borrowed native objects owned by Aurora/Dawn. They remain valid until
 * aurora_shutdown() and must not be released by the caller.
 *
 * colorDxgiFormat is the DXGI format matching Aurora's single-sample eye
 * output. adapterLuid is returned in the same split representation used by
 * OpenXR's XrGraphicsRequirementsD3D12KHR.
 */
typedef struct {
  void* device;
  void* queue;
  int64_t colorDxgiFormat;
  uint32_t adapterLuidLow;
  int32_t adapterLuidHigh;
} AuroraD3D12NativeHandles;

/** One acquired OpenXR swapchain image for the next Aurora stereo sink. */
typedef struct {
  void* resource;
  uint32_t width;
  uint32_t height;
  int64_t dxgiFormat;
} AuroraD3D12StereoTarget;

/**
 * Fired when Aurora either finishes or abandons the stereo sink. `success`
 * guarantees that the final same-queue D3D12 copy and its completion fence were
 * enqueued. A false result may occur after ExecuteCommandLists, so callers must
 * conservatively retain externally owned targets until graphics/session
 * teardown. The callback must not wait for the GPU or re-enter Aurora.
 */
typedef void (*AuroraD3D12StereoSubmittedCallback)(uint64_t frameToken, bool success,
                                                   void* userdata);

/** Returns false unless the active Aurora backend is Dawn D3D12. */
bool aurora_d3d12_get_native_handles(AuroraD3D12NativeHandles* handles);

/**
 * Installs the internal zero-readback stereo sink. Call while Aurora's frame
 * worker is idle, after aurora_initialize().
 */
bool aurora_d3d12_enable_stereo_bridge(AuroraD3D12StereoSubmittedCallback submitted,
                                       void* userdata);

/**
 * Publishes the acquired OpenXR image(s) for frameToken. Immersive projection
 * frames supply two targets; virtual-screen quad frames supply one. Exactly
 * one frame may be pending at a time.
 */
bool aurora_d3d12_set_stereo_targets(uint64_t frameToken,
                                     const AuroraD3D12StereoTarget* targets,
                                     uint32_t targetCount);

/**
 * Withdraws frameToken only while its target has not been encoded. This is
 * safe to race with Aurora's frame worker: false means the worker already owns
 * encoded work (or the token is no longer pending), so the submitted callback
 * remains the only completion authority. A successful cancellation performs
 * no GPU work and deliberately does not fire the callback.
 */
bool aurora_d3d12_cancel_stereo_targets(uint64_t frameToken);

/**
 * Removes the sink and drains bridge resources. The worker must be idle. Returns
 * false when queued work cannot be fenced; in that case the bridge is retained
 * for the process lifetime and the caller must likewise retain its graphics/XR
 * owners rather than destroying resources with unknown GPU use.
 */
bool aurora_d3d12_disable_stereo_bridge();

#ifdef __cplusplus
}
#endif

#endif
