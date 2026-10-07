#ifndef AURORA_VULKAN_DIRECT_INTEROP_H
#define AURORA_VULKAN_DIRECT_INTEROP_H

#include <aurora/dawn_vulkan_abi.h>

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#else
#include "stdbool.h"
#include "stdint.h"
#endif

/**
 * Direct presentation on the Quest: the alternative to aurora/vulkan_interop.h's two-device
 * AHardwareBuffer bridge when Dawn is the build carrying Aurora's patches (aurora-main/patches/dawn,
 * android/Build-QuestDawn.ps1), whose AuroraDawnVulkan* entry points the Windows Vulkan binding
 * (aurora/vulkan_win32_interop.h) also uses.
 *
 * The OpenXR runtime creates Dawn's VkInstance and VkDevice (XR_KHR_vulkan_enable2, through hooks
 * Dawn calls while they are configured), the session is bound to Dawn's own device and queue, and
 * Aurora draws each eye straight into the acquired XrSwapchain image on Dawn's queue: one pass per
 * eye, and nothing to order across devices.
 *
 * The contract of the targets, the cancellation and the submitted callback is the one of
 * aurora/d3d12_interop.h, whose images live on Dawn's device too.
 */

/** One acquired swapchain image: a VkImage of Dawn's device, its size and its VkFormat. */
typedef struct {
  uint64_t image;
  uint32_t width;
  uint32_t height;
  int64_t vkFormat;
} AuroraVulkanDirectTarget;

/**
 * Fired when Aurora either finishes or abandons the stereo sink. `success` guarantees that the
 * eyes' passes, and the images' return to the colour-attachment layout, were submitted to Dawn's
 * queue, so the images may be released to the runtime. A false result may come after work was
 * queued: the caller must then keep the images until the session ends. The callback runs on
 * Aurora's frame worker and must neither wait for the GPU nor re-enter Aurora.
 */
typedef void (*AuroraVulkanDirectSubmittedCallback)(uint64_t frameToken, bool success, void* userdata);

/** Whether Aurora was built against the patched Dawn and its entry points match this ABI. */
bool aurora_vulkan_direct_available(void);

/**
 * Before aurora_initialize(): Dawn then creates its Vulkan instance and device and picks its
 * GPU through `hooks` (null removes them). The hooks' userdata must outlive Dawn's device.
 */
bool aurora_vulkan_direct_configure(const AuroraDawnVulkanHooks* hooks);

/**
 * After aurora_initialize(): Dawn's instance, physical device, device and queue, for the
 * session's graphics binding, and the VkFormat of Aurora's eye output. False unless the active
 * backend is the patched Dawn's Vulkan with implicit device synchronization.
 */
bool aurora_vulkan_direct_get_handles(AuroraDawnVulkanHandles* handles, int64_t* colorVkFormat);

/** Installs the stereo sink. Call while Aurora's frame worker is idle. */
bool aurora_vulkan_direct_enable(AuroraVulkanDirectSubmittedCallback submitted, void* userdata);

/**
 * Publishes the acquired image(s) for frameToken: two for immersive projection frames, one for
 * the virtual screen, plus the settings panel's quad-layer image when `panel` is not null.
 * Exactly one frame may be pending at a time.
 */
bool aurora_vulkan_direct_set_targets(uint64_t frameToken, const AuroraVulkanDirectTarget* targets,
                                      uint32_t targetCount, const AuroraVulkanDirectTarget* panel);

/**
 * Withdraws frameToken only while it has not been encoded. False means the worker owns encoded
 * work and the submitted callback remains the completion authority.
 */
bool aurora_vulkan_direct_cancel(uint64_t frameToken);

/**
 * Before the runtime destroys swapchain images (a replaced pair): waits for Dawn's queue, then
 * drops the textures wrapping them, since the runtime may hand the same handles out again. Call
 * with no frame pending; false if one is, or if the queue could not be drained.
 */
bool aurora_vulkan_direct_forget_targets(const uint64_t* images, uint32_t count);

/**
 * Removes the sink, drains Dawn's queue and drops every wrapped image. False only when the queue
 * could not be drained; the bridge is then retained for the process lifetime.
 */
bool aurora_vulkan_direct_disable(void);

/**
 * Dawn's device lock, which serializes the runtime's calls that use the VkQueue (xrBeginFrame,
 * xrEndFrame, swapchain image acquire and release) with Dawn's own submissions. Null when the
 * bridge is unavailable; unlock accepts null.
 */
void* aurora_vulkan_direct_lock_queue(void);
void aurora_vulkan_direct_unlock_queue(void* guard);

#ifdef __cplusplus
}
#endif

#endif
