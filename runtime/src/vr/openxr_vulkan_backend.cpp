// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR)

#include "vr/openxr_vulkan_backend.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <type_traits>
#include <utility>

#if __has_include(<vulkan/vulkan.h>)
#define MKW_OPENXR_VULKAN_HEADERS_AVAILABLE 1
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>
#else
#define MKW_OPENXR_VULKAN_HEADERS_AVAILABLE 0
#endif

namespace mkw::vr {
namespace {

constexpr std::array<OpenXRViewConfiguration, kOpenXREyeCount>
    kEmptyViewConfiguration{};
constexpr std::string_view kVulkanEnable2Extension =
    "XR_KHR_vulkan_enable2";

bool HasExtension(const OpenXRRuntime& runtime, std::string_view extension) {
    const auto& extensions = runtime.EnabledExtensions();
    return std::find(extensions.begin(), extensions.end(), extension) !=
           extensions.end();
}

#if MKW_OPENXR_VULKAN_HEADERS_AVAILABLE
template <typename VulkanHandle>
VulkanHandle FromOpaqueHandle(OpenXRVulkanHandle handle) {
    if constexpr (std::is_pointer_v<VulkanHandle>) {
        return reinterpret_cast<VulkanHandle>(static_cast<uintptr_t>(handle));
    } else {
        return static_cast<VulkanHandle>(handle);
    }
}

template <typename VulkanHandle>
OpenXRVulkanHandle ToOpaqueHandle(VulkanHandle handle) {
    if constexpr (std::is_pointer_v<VulkanHandle>) {
        return static_cast<OpenXRVulkanHandle>(reinterpret_cast<uintptr_t>(handle));
    } else {
        return static_cast<OpenXRVulkanHandle>(handle);
    }
}

XrVersion VulkanApiVersionAsXrVersion(uint32_t version) {
    return XR_MAKE_VERSION(VK_API_VERSION_MAJOR(version),
                           VK_API_VERSION_MINOR(version),
                           VK_API_VERSION_PATCH(version));
}
#endif

} // namespace

OpenXRVulkanBackend::OpenXRVulkanBackend(OpenXRLogCallback logger)
    : m_logger(std::move(logger)) {}

OpenXRVulkanBackend::~OpenXRVulkanBackend() {
    Shutdown();
}

OpenXRVulkanCapabilityInfo OpenXRVulkanBackend::DawnInteropCapability() {
#if !MKW_OPENXR_VULKAN_HEADERS_AVAILABLE
    return {OpenXRVulkanCapability::VulkanHeadersUnavailable,
            "Vulkan headers were not available when the runtime was built"};
#elif !defined(MKW_AURORA_DAWN_VULKAN_NATIVE_HANDLES)
    return {
        OpenXRVulkanCapability::DawnNativeHandlesUnavailable,
        "the pinned Dawn package exposes VkInstance only; same-device OpenXR "
        "also requires Aurora to expose Dawn's VkPhysicalDevice, VkDevice, "
        "VkQueue, queue-family index, and queue synchronization",
    };
#else
    return {OpenXRVulkanCapability::Available, {}};
#endif
}

bool OpenXRVulkanBackend::Initialize(
    OpenXRRuntime& runtime,
    const OpenXRVulkanNativeContext& native_context,
    const OpenXRVulkanBackendConfig& config) {
    m_last_error = {};
    if (IsInitialized()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "Initialize",
                    "the Vulkan OpenXR backend is already initialized");
    }
    if (!runtime.IsInitialized()) {
        return Fail(XR_ERROR_HANDLE_INVALID, "Initialize",
                    "OpenXRRuntime must be initialized first");
    }
    if (runtime.HasSession()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "Initialize",
                    "OpenXRRuntime already owns a graphics session");
    }
    if (!HasExtension(runtime, kVulkanEnable2Extension)) {
        return Fail(XR_ERROR_EXTENSION_NOT_PRESENT, "Initialize",
                    "XR_KHR_vulkan_enable2 was not enabled on the instance");
    }
#if !MKW_OPENXR_VULKAN_HEADERS_AVAILABLE
    (void)native_context;
    (void)config;
    return Fail(XR_ERROR_GRAPHICS_DEVICE_INVALID, "Initialize",
                "this build has no Vulkan headers");
#else
    m_runtime = &runtime;
    m_config = config;
    m_native_context = native_context;

    if (!ValidateNativeContext(native_context) || !ValidateRuntimeDevice()) {
        m_runtime = nullptr;
        return false;
    }

    const XrGraphicsBindingVulkan2KHR graphics_binding{
        XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        nullptr,
        FromOpaqueHandle<VkInstance>(native_context.instance),
        FromOpaqueHandle<VkPhysicalDevice>(native_context.physical_device),
        FromOpaqueHandle<VkDevice>(native_context.device),
        native_context.queue_family_index,
        native_context.queue_index,
    };
    if (!runtime.CreateSession(&graphics_binding)) {
        m_last_error = runtime.LastError();
        m_runtime = nullptr;
        return false;
    }
    m_owns_session = true;

    if (!SelectSwapchainFormat() || !CreateSwapchains()) {
        DestroySwapchains();
        runtime.DestroySession();
        m_owns_session = false;
        m_runtime = nullptr;
        return false;
    }

    std::ostringstream message;
    message << "OpenXR Vulkan swapchains ready: format " << m_swapchain_format
            << ", left " << m_eye_swapchains[0].width << 'x'
            << m_eye_swapchains[0].height << ", right "
            << m_eye_swapchains[1].width << 'x'
            << m_eye_swapchains[1].height;
    Log(OpenXRLogLevel::Info, message.str());
    return true;
#endif
}

bool OpenXRVulkanBackend::ValidateNativeContext(
    const OpenXRVulkanNativeContext& native_context) {
    if (native_context.version != kOpenXRVulkanNativeContextVersion ||
        native_context.struct_size < sizeof(OpenXRVulkanNativeContext)) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "ValidateNativeContext",
                    "unsupported native context ABI version or size");
    }
    if (native_context.instance == 0 || native_context.physical_device == 0 ||
        native_context.device == 0 || native_context.queue == 0 ||
        native_context.api_version == 0) {
        return Fail(XR_ERROR_GRAPHICS_DEVICE_INVALID, "ValidateNativeContext",
                    "instance, physical device, device, queue, and API version "
                    "must all be supplied");
    }
    constexpr uint32_t required_flags =
        OpenXRVulkanNativeContextDawnOwnedBit |
        OpenXRVulkanNativeContextQueueVerifiedBit;
    if ((native_context.flags & required_flags) != required_flags) {
        return Fail(XR_ERROR_GRAPHICS_DEVICE_INVALID, "ValidateNativeContext",
                    "native handles must be the verified Dawn device and queue");
    }
    if (native_context.lock_queue == nullptr ||
        native_context.unlock_queue == nullptr) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "ValidateNativeContext",
                    "paired Dawn graphics-queue lock callbacks are required");
    }
    return true;
}

bool OpenXRVulkanBackend::ValidateRuntimeDevice() {
#if !MKW_OPENXR_VULKAN_HEADERS_AVAILABLE
    return false;
#else
    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_graphics_device = nullptr;
    if (!m_runtime->LoadFunction("xrGetVulkanGraphicsRequirements2KHR",
                                 &get_requirements) ||
        !m_runtime->LoadFunction("xrGetVulkanGraphicsDevice2KHR",
                                 &get_graphics_device)) {
        m_last_error = m_runtime->LastError();
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!Check(get_requirements(m_runtime->Instance(), m_runtime->SystemId(),
                                &requirements),
               "xrGetVulkanGraphicsRequirements2KHR")) {
        return false;
    }

    const XrVersion supplied_version =
        VulkanApiVersionAsXrVersion(m_native_context.api_version);
    if (supplied_version < requirements.minApiVersionSupported ||
        supplied_version > requirements.maxApiVersionSupported) {
        std::ostringstream detail;
        detail << "Dawn Vulkan version "
               << VK_API_VERSION_MAJOR(m_native_context.api_version) << '.'
               << VK_API_VERSION_MINOR(m_native_context.api_version) << '.'
               << VK_API_VERSION_PATCH(m_native_context.api_version)
               << " is outside the OpenXR runtime range "
               << XR_VERSION_MAJOR(requirements.minApiVersionSupported) << '.'
               << XR_VERSION_MINOR(requirements.minApiVersionSupported) << '-'
               << XR_VERSION_MAJOR(requirements.maxApiVersionSupported) << '.'
               << XR_VERSION_MINOR(requirements.maxApiVersionSupported);
        return Fail(XR_ERROR_GRAPHICS_DEVICE_INVALID,
                    "xrGetVulkanGraphicsRequirements2KHR", detail.str());
    }

    XrVulkanGraphicsDeviceGetInfoKHR device_get_info{
        XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    device_get_info.systemId = m_runtime->SystemId();
    device_get_info.vulkanInstance =
        FromOpaqueHandle<VkInstance>(m_native_context.instance);

    VkPhysicalDevice runtime_physical_device = VK_NULL_HANDLE;
    if (!Check(get_graphics_device(m_runtime->Instance(), &device_get_info,
                                   &runtime_physical_device),
               "xrGetVulkanGraphicsDevice2KHR")) {
        return false;
    }
    if (ToOpaqueHandle(runtime_physical_device) !=
        m_native_context.physical_device) {
        return Fail(XR_ERROR_GRAPHICS_DEVICE_INVALID,
                    "xrGetVulkanGraphicsDevice2KHR",
                    "OpenXR requires a different physical device than Dawn selected");
    }
    return true;
#endif
}

bool OpenXRVulkanBackend::SelectSwapchainFormat() {
#if !MKW_OPENXR_VULKAN_HEADERS_AVAILABLE
    return false;
#else
    const auto& formats = m_runtime->SwapchainFormats();
    if (formats.empty()) {
        return Fail(XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED,
                    "xrEnumerateSwapchainFormats",
                    "the runtime reported no Vulkan swapchain formats");
    }

    constexpr std::array<VkFormat, 6> preferred_formats{
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_R16G16B16A16_SFLOAT,
    };
    for (VkFormat preferred : preferred_formats) {
        const int64_t candidate = static_cast<int64_t>(preferred);
        if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
            m_swapchain_format = candidate;
            return true;
        }
    }

    // A caller-provided GPU bridge can still support a format outside our
    // preference list, so keep the runtime's first valid choice visible.
    m_swapchain_format = formats.front();
    Log(OpenXRLogLevel::Warning,
        "OpenXR Vulkan runtime offered no preferred RGBA/BGRA format; using "
        "its first advertised format");
    return true;
#endif
}

bool OpenXRVulkanBackend::CreateSwapchains() {
    for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
        if (!CreateEyeSwapchain(eye)) {
            return false;
        }
    }
    return true;
}

bool OpenXRVulkanBackend::CreateEyeSwapchain(uint32_t eye) {
#if !MKW_OPENXR_VULKAN_HEADERS_AVAILABLE
    (void)eye;
    return false;
#else
    const auto& view_configuration = m_runtime->ViewConfiguration();
    EyeSwapchain& swapchain = m_eye_swapchains[eye];
    swapchain.width = view_configuration[eye].render_width;
    swapchain.height = view_configuration[eye].render_height;

    XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    if (m_config.require_transfer_destination) {
        usage |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    }
    const XrSwapchainCreateInfo create_info{
        XR_TYPE_SWAPCHAIN_CREATE_INFO,
        nullptr,
        0,
        usage,
        m_swapchain_format,
        1,
        swapchain.width,
        swapchain.height,
        1,
        1,
        1,
    };
    if (!Check(xrCreateSwapchain(m_runtime->Session(), &create_info,
                                 &swapchain.handle),
               eye == 0 ? "xrCreateSwapchain(left)"
                        : "xrCreateSwapchain(right)")) {
        return false;
    }

    uint32_t image_count = 0;
    if (!Check(xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count,
                                          nullptr),
               "xrEnumerateSwapchainImages(count)")) {
        return false;
    }
    if (image_count == 0) {
        return Fail(XR_ERROR_RUNTIME_FAILURE, "xrEnumerateSwapchainImages",
                    "the runtime returned an empty Vulkan swapchain");
    }

    std::vector<XrSwapchainImageVulkan2KHR> images(
        image_count,
        XrSwapchainImageVulkan2KHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    if (!Check(xrEnumerateSwapchainImages(
                   swapchain.handle, image_count, &image_count,
                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
               "xrEnumerateSwapchainImages")) {
        return false;
    }

    swapchain.images.reserve(image_count);
    for (const XrSwapchainImageVulkan2KHR& image : images) {
        swapchain.images.push_back(ToOpaqueHandle(image.image));
    }
    return true;
#endif
}

bool OpenXRVulkanBackend::AcquireEyeImage(uint32_t eye,
                                         OpenXRVulkanEyeImage& image) {
    if (!IsInitialized() || eye >= kOpenXREyeCount) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "AcquireEyeImage",
                    "backend is not initialized or eye index is invalid");
    }
    EyeSwapchain& swapchain = m_eye_swapchains[eye];
    if (swapchain.acquired && swapchain.waited) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "AcquireEyeImage",
                    "this eye already has an acquired image");
    }

    if (!swapchain.acquired) {
        XrSwapchainImageAcquireInfo acquire_info{
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!LockQueue()) {
            return false;
        }
        const XrResult acquire_result =
            xrAcquireSwapchainImage(swapchain.handle, &acquire_info,
                                    &swapchain.acquired_index);
        UnlockQueue();
        if (!Check(acquire_result, "xrAcquireSwapchainImage")) {
            return false;
        }
        swapchain.acquired = true;
    }

    const XrSwapchainImageWaitInfo wait_info{
        XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr,
        m_config.image_wait_timeout};
    const XrResult wait_result = xrWaitSwapchainImage(swapchain.handle, &wait_info);
    if (wait_result == XR_TIMEOUT_EXPIRED) {
        // XR_TIMEOUT_EXPIRED is a positive XrResult, but the image has not
        // completed its wait and therefore cannot be used or released yet.
        return Fail(wait_result, "xrWaitSwapchainImage",
                    "the image wait timed out; retry acquisition to resume the wait");
    }
    if (!Check(wait_result, "xrWaitSwapchainImage")) {
        // A timeout leaves the image acquired but not waited. The caller may
        // retry AcquireEyeImage(), which resumes at xrWaitSwapchainImage rather
        // than illegally acquiring a second image or releasing an unwaited one.
        return false;
    }
    swapchain.waited = true;
    if (swapchain.acquired_index >= swapchain.images.size()) {
        ReleaseEyeImage(eye);
        return Fail(XR_ERROR_RUNTIME_FAILURE, "xrAcquireSwapchainImage",
                    "the runtime returned an out-of-range image index");
    }

    image = {
        eye,
        swapchain.acquired_index,
        swapchain.width,
        swapchain.height,
        m_swapchain_format,
        swapchain.images[swapchain.acquired_index],
    };
    return true;
}

bool OpenXRVulkanBackend::ReleaseEyeImage(uint32_t eye) {
    if (!IsInitialized() || eye >= kOpenXREyeCount) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "ReleaseEyeImage",
                    "backend is not initialized or eye index is invalid");
    }
    EyeSwapchain& swapchain = m_eye_swapchains[eye];
    if (!swapchain.acquired) {
        return true;
    }
    if (!swapchain.waited) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "ReleaseEyeImage",
                    "xrWaitSwapchainImage has not completed for this image");
    }

    XrSwapchainImageReleaseInfo release_info{
        XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (!LockQueue()) {
        return false;
    }
    const XrResult release_result =
        xrReleaseSwapchainImage(swapchain.handle, &release_info);
    UnlockQueue();
    if (!Check(release_result, "xrReleaseSwapchainImage")) {
        return false;
    }
    swapchain.acquired = false;
    swapchain.waited = false;
    return true;
}

bool OpenXRVulkanBackend::SubmitProjection(const OpenXRFrame& frame) {
    if (!IsInitialized()) {
        return Fail(XR_ERROR_HANDLE_INVALID, "SubmitProjection",
                    "backend is not initialized");
    }
    for (const EyeSwapchain& swapchain : m_eye_swapchains) {
        if (swapchain.acquired) {
            return Fail(XR_ERROR_CALL_ORDER_INVALID, "SubmitProjection",
                        "all eye images must be released before xrEndFrame");
        }
    }
    const bool position_valid =
        (frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    if (!frame.should_render || !frame.views_valid || !position_valid) {
        return m_runtime->EndFrameWithoutLayers(frame);
    }

    std::array<XrCompositionLayerProjectionView, kOpenXREyeCount> views{};
    for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = frame.views[eye].pose;
        views[eye].fov = frame.views[eye].fov;
        views[eye].subImage.swapchain = m_eye_swapchains[eye].handle;
        views[eye].subImage.imageRect = {
            {0, 0},
            {static_cast<int32_t>(m_eye_swapchains[eye].width),
             static_cast<int32_t>(m_eye_swapchains[eye].height)},
        };
        views[eye].subImage.imageArrayIndex = 0;
    }

    const XrCompositionLayerProjection layer{
        XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        nullptr,
        m_config.projection_layer_flags,
        m_runtime->AppSpace(),
        kOpenXREyeCount,
        views.data(),
    };
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer),
    };
    if (!m_runtime->EndFrame(frame, layers, 1)) {
        m_last_error = m_runtime->LastError();
        return false;
    }
    return true;
}

void OpenXRVulkanBackend::DestroySwapchains() {
    for (EyeSwapchain& swapchain : m_eye_swapchains) {
        bool may_destroy_swapchain = true;
        if (swapchain.acquired && swapchain.waited &&
            swapchain.handle != XR_NULL_HANDLE) {
            XrSwapchainImageReleaseInfo release_info{
                XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if (LockQueue()) {
                const XrResult result =
                    xrReleaseSwapchainImage(swapchain.handle, &release_info);
                UnlockQueue();
                m_runtime->ObserveResult(result);
                if (XR_FAILED(result)) {
                    may_destroy_swapchain = false;
                    Log(OpenXRLogLevel::Warning,
                        "xrReleaseSwapchainImage failed during Vulkan shutdown");
                } else {
                    swapchain.acquired = false;
                    swapchain.waited = false;
                }
            } else {
                may_destroy_swapchain = false;
            }
        } else if (swapchain.acquired) {
            // OpenXR does not permit releasing before a successful wait. Leave
            // this child to xrDestroySession instead of violating the image
            // call order with a best-effort release or destroy.
            may_destroy_swapchain = false;
            Log(OpenXRLogLevel::Warning,
                "Vulkan swapchain still has an unwaited image; deferring its "
                "destruction to xrDestroySession");
        }
        if (swapchain.handle != XR_NULL_HANDLE && m_runtime != nullptr &&
            m_runtime->HasSession() && may_destroy_swapchain) {
            const XrResult result = xrDestroySwapchain(swapchain.handle);
            m_runtime->ObserveResult(result);
            if (XR_FAILED(result)) {
                Log(OpenXRLogLevel::Warning,
                    "xrDestroySwapchain failed during Vulkan shutdown");
            }
        }
        swapchain = {};
    }
    m_swapchain_format = 0;
}

void OpenXRVulkanBackend::Shutdown() {
    if (m_runtime == nullptr) {
        return;
    }
    DestroySwapchains();
    if (m_owns_session && m_runtime->HasSession()) {
        m_runtime->DestroySession();
    }
    m_owns_session = false;
    m_runtime = nullptr;
    m_native_context = {};
}

const std::array<OpenXRViewConfiguration, kOpenXREyeCount>&
OpenXRVulkanBackend::ViewConfiguration() const {
    return m_runtime != nullptr ? m_runtime->ViewConfiguration()
                                : kEmptyViewConfiguration;
}

bool OpenXRVulkanBackend::LockQueue() {
    if (m_native_context.lock_queue == nullptr) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "LockQueue",
                    "no Vulkan queue lock callback is installed");
    }
    if (!m_native_context.lock_queue(m_native_context.queue_userdata)) {
        return Fail(XR_ERROR_RUNTIME_FAILURE, "LockQueue",
                    "Aurora refused the Vulkan graphics queue lock");
    }
    return true;
}

void OpenXRVulkanBackend::UnlockQueue() {
    m_native_context.unlock_queue(m_native_context.queue_userdata);
}

bool OpenXRVulkanBackend::Check(XrResult result, std::string_view operation) {
    if (m_runtime != nullptr) {
        m_runtime->ObserveResult(result);
    }
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    return Fail(result, operation, {});
}

bool OpenXRVulkanBackend::Fail(XrResult result, std::string_view operation,
                               std::string_view detail) {
    m_last_error.result = result;
    m_last_error.operation.assign(operation);
    m_last_error.message = ResultString(result);
    if (!detail.empty()) {
        m_last_error.message.append(": ");
        m_last_error.message.append(detail);
    }
    std::string message = m_last_error.operation;
    message.append(" failed: ");
    message.append(m_last_error.message);
    Log(OpenXRLogLevel::Error, message);
    return false;
}

void OpenXRVulkanBackend::Log(OpenXRLogLevel level,
                              std::string_view message) const noexcept {
    if (!m_logger) {
        return;
    }
    try {
        m_logger(level, message);
    } catch (...) {
        // Diagnostics must not escape a graphics/session teardown path.
    }
}

std::string OpenXRVulkanBackend::ResultString(XrResult result) const {
    char buffer[XR_MAX_RESULT_STRING_SIZE]{};
    if (m_runtime != nullptr && m_runtime->IsInitialized() &&
        XR_SUCCEEDED(xrResultToString(m_runtime->Instance(), result, buffer))) {
        return buffer;
    }
    return std::to_string(static_cast<int32_t>(result));
}

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
