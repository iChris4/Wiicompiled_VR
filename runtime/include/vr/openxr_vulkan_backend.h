// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR)

#include "vr/openxr_runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mkw::vr {

// Keep Vulkan implementation types out of the runtime/Aurora boundary. Vulkan
// dispatchable and non-dispatchable handles both fit in 64 bits on every target
// supported by this project. The backend converts these values to the Vulkan
// header's actual handle types only in its implementation file.
using OpenXRVulkanHandle = uint64_t;

inline constexpr uint32_t kOpenXRVulkanNativeContextVersion = 1;

enum OpenXRVulkanNativeContextFlagBits : uint32_t {
    // The handles are the exact objects used by Aurora's Dawn device. A second
    // Vulkan device is not a valid substitute: it cannot present Dawn's work to
    // an OpenXR swapchain without an explicit external-memory bridge.
    OpenXRVulkanNativeContextDawnOwnedBit = 1u << 0,

    // The provider has queried the queue from device/queue_family_index/
    // queue_index and verified that it equals queue. Dawn's package API does not
    // currently expose enough information for this backend to do that itself.
    OpenXRVulkanNativeContextQueueVerifiedBit = 1u << 1,
};

using OpenXRVulkanQueueLockCallback = bool (*)(void* userdata);
using OpenXRVulkanQueueUnlockCallback = void (*)(void* userdata);

// ABI-stable context a Dawn-native shim must provide. All handles are borrowed
// and must outlive OpenXRVulkanBackend. The paired callbacks externally
// synchronize Vulkan queue access with Dawn; they are mandatory because a
// Vulkan queue is not internally synchronized and XR may use the graphics queue
// during image hand-off.
struct OpenXRVulkanNativeContext {
    uint32_t version = kOpenXRVulkanNativeContextVersion;
    uint32_t struct_size = sizeof(OpenXRVulkanNativeContext);
    uint32_t flags = 0;

    OpenXRVulkanHandle instance = 0;
    OpenXRVulkanHandle physical_device = 0;
    OpenXRVulkanHandle device = 0;
    OpenXRVulkanHandle queue = 0;
    uint32_t queue_family_index = 0;
    uint32_t queue_index = 0;

    // A VK_MAKE_API_VERSION encoded value for the Vulkan instance/device.
    uint32_t api_version = 0;

    OpenXRVulkanQueueLockCallback lock_queue = nullptr;
    OpenXRVulkanQueueUnlockCallback unlock_queue = nullptr;
    void* queue_userdata = nullptr;
};

enum class OpenXRVulkanCapability {
    Available,
    VulkanHeadersUnavailable,
    DawnNativeHandlesUnavailable,
    InvalidNativeContext,
    MissingEnable2Extension,
    RuntimeRejectedGraphicsDevice,
    RuntimeApiVersionMismatch,
};

struct OpenXRVulkanCapabilityInfo {
    OpenXRVulkanCapability capability = OpenXRVulkanCapability::Available;
    std::string reason;

    explicit operator bool() const {
        return capability == OpenXRVulkanCapability::Available;
    }
};

struct OpenXRVulkanBackendConfig {
    // Rendering directly into an XR image needs COLOR_ATTACHMENT. Aurora's
    // intended zero-copy bridge renders to an imported/application-owned image
    // and performs a GPU copy, which additionally requires TRANSFER_DST.
    bool require_transfer_destination = true;
    XrDuration image_wait_timeout = XR_INFINITE_DURATION;
    XrCompositionLayerFlags projection_layer_flags = 0;
};

struct OpenXRVulkanEyeImage {
    uint32_t eye = 0;
    uint32_t image_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t format = 0;
    OpenXRVulkanHandle image = 0;
};

// Owns the Vulkan-bound session and its per-eye OpenXR swapchains. It does not
// record Vulkan commands: Aurora's backend bridge is responsible for rendering
// or copying into the borrowed VkImage on the supplied Dawn queue, submitting
// that GPU work, then calling ReleaseEyeImage(). No CPU/readback fallback exists.
//
// All methods must be called on the XR owner thread. Shutdown() must run before
// the native Vulkan device is destroyed.
class OpenXRVulkanBackend final {
public:
    explicit OpenXRVulkanBackend(OpenXRLogCallback logger = {});
    ~OpenXRVulkanBackend();

    OpenXRVulkanBackend(const OpenXRVulkanBackend&) = delete;
    OpenXRVulkanBackend& operator=(const OpenXRVulkanBackend&) = delete;
    OpenXRVulkanBackend(OpenXRVulkanBackend&&) = delete;
    OpenXRVulkanBackend& operator=(OpenXRVulkanBackend&&) = delete;

    // Reports whether this build's Aurora/Dawn provider has the native Vulkan
    // handle shim required by the backend. The pinned prebuilt Dawn package does
    // not expose VkPhysicalDevice, VkDevice, VkQueue, or the queue-family index,
    // so this intentionally reports DawnNativeHandlesUnavailable until such a
    // shim is compiled in.
    static OpenXRVulkanCapabilityInfo DawnInteropCapability();

    // runtime must already be initialized with XR_KHR_vulkan_enable2 in its
    // required extension list and must not yet own a session.
    bool Initialize(OpenXRRuntime& runtime,
                    const OpenXRVulkanNativeContext& native_context,
                    const OpenXRVulkanBackendConfig& config = {});
    void Shutdown();

    bool AcquireEyeImage(uint32_t eye, OpenXRVulkanEyeImage& image);
    bool ReleaseEyeImage(uint32_t eye);

    // Ends a begun OpenXR frame with a stereo projection layer. The pose/FOV
    // come from the same OpenXRFrame token used for rendering. When
    // should_render is false or views are invalid, the frame is ended without a
    // layer as required by the OpenXR frame protocol.
    bool SubmitProjection(const OpenXRFrame& frame);

    bool IsInitialized() const { return m_runtime != nullptr; }
    int64_t SwapchainFormat() const { return m_swapchain_format; }
    const std::array<OpenXRViewConfiguration, kOpenXREyeCount>&
    ViewConfiguration() const;
    const OpenXRError& LastError() const { return m_last_error; }

private:
    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<OpenXRVulkanHandle> images;
        uint32_t acquired_index = 0;
        bool acquired = false;
        bool waited = false;
    };

    bool ValidateNativeContext(const OpenXRVulkanNativeContext& native_context);
    bool ValidateRuntimeDevice();
    bool CreateSwapchains();
    bool CreateEyeSwapchain(uint32_t eye);
    bool SelectSwapchainFormat();
    void DestroySwapchains();

    bool LockQueue();
    void UnlockQueue();
    bool Check(XrResult result, std::string_view operation);
    bool Fail(XrResult result, std::string_view operation, std::string_view detail);
    void Log(OpenXRLogLevel level, std::string_view message) const noexcept;
    std::string ResultString(XrResult result) const;

    OpenXRRuntime* m_runtime = nullptr;
    OpenXRLogCallback m_logger;
    OpenXRError m_last_error;
    OpenXRVulkanNativeContext m_native_context{};
    OpenXRVulkanBackendConfig m_config{};
    std::array<EyeSwapchain, kOpenXREyeCount> m_eye_swapchains{};
    int64_t m_swapchain_format = 0;
    bool m_owns_session = false;
};

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
