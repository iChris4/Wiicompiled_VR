// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR) && defined(__ANDROID__)

// OpenXR's Vulkan structures are selected when openxr_platform.h is parsed.
#define VK_USE_PLATFORM_ANDROID_KHR
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_ANDROID

#include "vr/openxr_vulkan_direct.h"
#include "vr/openxr_diagnostics.h"
#include "vr/openxr_passthrough.h"
#include "runtime_config.h"

#include <aurora/vulkan_direct_interop.h>

#include <jni.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mkw::vr {
namespace {

bool SameVkCopyFamily(VkFormat a, VkFormat b) noexcept {
    return a == b ||
           ((a == VK_FORMAT_R8G8B8A8_UNORM || a == VK_FORMAT_R8G8B8A8_SRGB) &&
            (b == VK_FORMAT_R8G8B8A8_UNORM || b == VK_FORMAT_R8G8B8A8_SRGB)) ||
           ((a == VK_FORMAT_B8G8R8A8_UNORM || a == VK_FORMAT_B8G8R8A8_SRGB) &&
            (b == VK_FORMAT_B8G8R8A8_UNORM || b == VK_FORMAT_B8G8R8A8_SRGB));
}

VkFormat SrgbSibling(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

const char* BeginStatusOperation(OpenXRFrameStatus status) noexcept {
    switch (status) {
    case OpenXRFrameStatus::Ready:
        return "ready";
    case OpenXRFrameStatus::SessionNotRunning:
        return "session is not running";
    case OpenXRFrameStatus::ExitRequested:
        return "runtime requested exit";
    case OpenXRFrameStatus::Error:
        return "xrWaitFrame failed";
    }
    return "unknown frame status";
}

OpenXRBeginStatus BeginStatusFor(OpenXRFrameStatus status) noexcept {
    return status == OpenXRFrameStatus::SessionNotRunning ? OpenXRBeginStatus::SessionNotRunning
           : status == OpenXRFrameStatus::ExitRequested  ? OpenXRBeginStatus::ExitRequested
                                                          : OpenXRBeginStatus::Error;
}

uint32_t PresentationTargetCount(const OpenXRPresentation& presentation) noexcept {
    return presentation.mode == OpenXRFrameMode::VirtualScreen ? 1u : kOpenXREyeCount;
}

const OpenXRVulkanGraphicsRequirements kNoRequirements{};
const std::string kNoBackend = "the Quest's OpenXR Vulkan backend was not set up";

} // namespace

class OpenXRVulkanDirectBackend::Impl final {
public:
    explicit Impl(OpenXRLogCallback logger) : logger_(std::move(logger)) {}

    ~Impl() { Shutdown(); }

    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<XrSwapchainImageVulkan2KHR> images;
        uint32_t acquired_index = 0;
        bool acquired = false;
        bool waited = false;
        bool release_forbidden = false;
    };

    static uint32_t VkVersion(XrVersion version) noexcept {
        return VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version), XR_VERSION_PATCH(version));
    }

    // ---- Dawn's hooks (aurora/dawn_vulkan_abi.h): its Vulkan objects come from the runtime ----

    static int32_t CreateInstance(void* self, void* proc, const void* info, const void* allocator, void** out) {
        auto& owner = *static_cast<Impl*>(self);
        const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc);
        auto vk_info = *static_cast<const VkInstanceCreateInfo*>(info);
        if (vk_info.pApplicationInfo == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto app = *vk_info.pApplicationInfo;
        // Dawn asks for Vulkan 1.1; take 1.2 where the loader and the runtime allow it, so the
        // device can enable timeline semaphores (core there) should the runtime use them.
        uint32_t loader_version = VK_API_VERSION_1_0;
        const auto enumerate = get_proc != nullptr ? reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                                                         get_proc(nullptr, "vkEnumerateInstanceVersion"))
                                                   : nullptr;
        if (enumerate == nullptr || enumerate(&loader_version) != VK_SUCCESS) {
            loader_version = VK_API_VERSION_1_0;
        }
        const uint32_t preferred = std::min({static_cast<uint32_t>(VK_API_VERSION_1_2), loader_version,
                                             VkVersion(owner.requirements_.max_api_version)});
        app.apiVersion = std::max({app.apiVersion, VkVersion(owner.requirements_.min_api_version), preferred});
        if (app.apiVersion > VkVersion(owner.requirements_.max_api_version)) {
            return VK_ERROR_INCOMPATIBLE_DRIVER;
        }
        vk_info.pApplicationInfo = &app;
        XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        create.systemId = owner.runtime_->SystemId();
        create.pfnGetInstanceProcAddr = get_proc;
        create.vulkanCreateInfo = &vk_info;
        create.vulkanAllocator = static_cast<const VkAllocationCallbacks*>(allocator);
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        const XrResult xr =
            owner.create_instance_(owner.runtime_->Instance(), &create, reinterpret_cast<VkInstance*>(out), &result);
        if (XR_SUCCEEDED(xr) && result == VK_SUCCESS) {
            owner.vk_instance_ = *reinterpret_cast<VkInstance*>(out);
            owner.instance_api_version_ = app.apiVersion;
        }
        return XR_SUCCEEDED(xr) ? result : VK_ERROR_INITIALIZATION_FAILED;
    }

    static int32_t GetPhysical(void* self, void* instance, void** out) {
        auto& owner = *static_cast<Impl*>(self);
        XrVulkanGraphicsDeviceGetInfoKHR get{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        get.systemId = owner.runtime_->SystemId();
        get.vulkanInstance = static_cast<VkInstance>(instance);
        return XR_SUCCEEDED(owner.get_device_(owner.runtime_->Instance(), &get, reinterpret_cast<VkPhysicalDevice*>(out)))
                   ? VK_SUCCESS
                   : VK_ERROR_INITIALIZATION_FAILED;
    }

    static int32_t CreateDevice(void* self, void* proc, void* physical, const void* info, const void* allocator,
                                void** out) {
        auto& owner = *static_cast<Impl*>(self);
        const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc);
        const auto vk_physical = static_cast<VkPhysicalDevice>(physical);
        auto vk_info = *static_cast<const VkDeviceCreateInfo*>(info);
        // A runtime that creates timeline semaphores on the application's device appends the
        // extension but not the feature; enable it (core from 1.2) when Dawn's chain does not.
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        if (get_proc != nullptr && owner.vk_instance_ != VK_NULL_HANDLE &&
            owner.instance_api_version_ >= VK_API_VERSION_1_2) {
            const auto properties_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                get_proc(owner.vk_instance_, "vkGetPhysicalDeviceProperties"));
            const auto features_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                get_proc(owner.vk_instance_, "vkGetPhysicalDeviceFeatures2"));
            VkPhysicalDeviceProperties properties{};
            if (properties_fn != nullptr) {
                properties_fn(vk_physical, &properties);
            }
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
            if (features_fn != nullptr && properties.apiVersion >= VK_API_VERSION_1_2) {
                features_fn(vk_physical, &features);
            }
            bool chained = false;
            for (auto* next = static_cast<const VkBaseInStructure*>(vk_info.pNext); next != nullptr;
                 next = next->pNext) {
                chained |= next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES ||
                           next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            }
            if (timeline.timelineSemaphore == VK_TRUE && !chained) {
                timeline.pNext = const_cast<void*>(vk_info.pNext);
                vk_info.pNext = &timeline;
            } else {
                timeline.timelineSemaphore = VK_FALSE;
            }
        }
        XrVulkanDeviceCreateInfoKHR create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        create.systemId = owner.runtime_->SystemId();
        create.pfnGetInstanceProcAddr = get_proc;
        create.vulkanPhysicalDevice = vk_physical;
        create.vulkanCreateInfo = &vk_info;
        create.vulkanAllocator = static_cast<const VkAllocationCallbacks*>(allocator);
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        const XrResult xr =
            owner.create_device_(owner.runtime_->Instance(), &create, reinterpret_cast<VkDevice*>(out), &result);
        if (XR_SUCCEEDED(xr) && result == VK_SUCCESS) {
            owner.timeline_semaphores_ = timeline.timelineSemaphore == VK_TRUE;
        }
        return XR_SUCCEEDED(xr) ? result : VK_ERROR_INITIALIZATION_FAILED;
    }

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime) {
        ClearError();
        if (!runtime.IsInitialized() || runtime.HasSession()) {
            return Fail("OpenXR must own an instance, but no session, before querying Vulkan requirements");
        }
        if (requirements_queried_ && runtime_ != &runtime) {
            return Fail("Vulkan graphics requirements were already queried from another OpenXR instance");
        }
        if (!aurora_vulkan_direct_available()) {
            return Fail("direct presentation needs the Dawn built with Aurora's patches (android/Build-QuestDawn.ps1)");
        }
        const auto& extensions = runtime.EnabledExtensions();
        if (std::find(extensions.begin(), extensions.end(), std::string(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) ==
            extensions.end()) {
            return Fail("direct presentation needs XR_KHR_vulkan_enable2");
        }
        runtime_ = &runtime;
        PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = nullptr;
        if (!runtime.LoadFunction("xrGetVulkanGraphicsRequirements2KHR", &get_requirements) ||
            get_requirements == nullptr || !runtime.LoadFunction("xrCreateVulkanInstanceKHR", &create_instance_) ||
            create_instance_ == nullptr || !runtime.LoadFunction("xrCreateVulkanDeviceKHR", &create_device_) ||
            create_device_ == nullptr || !runtime.LoadFunction("xrGetVulkanGraphicsDevice2KHR", &get_device_) ||
            get_device_ == nullptr) {
            runtime_ = nullptr;
            return Fail("the OpenXR runtime did not expose the XR_KHR_vulkan_enable2 functions");
        }
        XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        const XrResult result = get_requirements(runtime.Instance(), runtime.SystemId(), &requirements);
        runtime.ObserveResult(result);
        if (XR_FAILED(result)) {
            runtime_ = nullptr;
            std::ostringstream message;
            message << "xrGetVulkanGraphicsRequirements2KHR failed (" << result << ')';
            return Fail(message.str());
        }
        requirements_ = {requirements.minApiVersionSupported, requirements.maxApiVersionSupported, true};
        const AuroraDawnVulkanHooks hooks{this, CreateInstance, CreateDevice, GetPhysical};
        if (!aurora_vulkan_direct_configure(&hooks)) {
            runtime_ = nullptr;
            return Fail("Aurora's Dawn did not accept the OpenXR Vulkan hooks");
        }
        hooks_installed_ = true;
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = false;
            submission_unsafe_ = false;
        }
        requirements_queried_ = true;
        std::ostringstream message;
        message << "OpenXR Vulkan requirements: API " << XR_VERSION_MAJOR(requirements_.min_api_version) << '.'
                << XR_VERSION_MINOR(requirements_.min_api_version) << " to "
                << XR_VERSION_MAJOR(requirements_.max_api_version) << '.'
                << XR_VERSION_MINOR(requirements_.max_api_version)
                << " via XR_KHR_vulkan_enable2; Dawn creates its device through the runtime (direct presentation)";
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    bool BindAurora(OpenXRRuntime& runtime) {
        ClearError();
        if (!requirements_queried_ || runtime_ != &runtime || runtime.HasSession()) {
            return Fail("QueryGraphicsRequirements must succeed on this OpenXR instance before BindAurora");
        }
        if (bound_) {
            return Fail("OpenXR Vulkan backend is already bound");
        }
        AuroraDawnVulkanHandles handles{};
        int64_t format = 0;
        if (!aurora_vulkan_direct_get_handles(&handles, &format)) {
            return Fail("Aurora did not report a Dawn Vulkan device it can share with the session");
        }
        if (handles.instance != vk_instance_) {
            return Fail("Dawn's Vulkan instance was not created through the OpenXR runtime");
        }
        void* physical = nullptr;
        if (GetPhysical(this, handles.instance, &physical) != VK_SUCCESS || physical != handles.physicalDevice) {
            return Fail("Dawn's Vulkan GPU is not the OpenXR runtime's");
        }
        aurora_format_ = static_cast<VkFormat>(format);
        XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        binding.instance = static_cast<VkInstance>(handles.instance);
        binding.physicalDevice = static_cast<VkPhysicalDevice>(handles.physicalDevice);
        binding.device = static_cast<VkDevice>(handles.device);
        binding.queueFamilyIndex = handles.queueFamily;
        binding.queueIndex = handles.queueIndex;
        // From here the runtime's calls that use the queue take Dawn's device lock.
        runtime.SetGraphicsQueueGuard(aurora_vulkan_direct_lock_queue, aurora_vulkan_direct_unlock_queue);
        if (!runtime.CreateSession(&binding)) {
            runtime.SetGraphicsQueueGuard(nullptr, nullptr);
            return Fail("OpenXR rejected Dawn's Vulkan device binding");
        }
        owns_session_ = true;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            const auto& view = runtime.ViewConfiguration()[eye];
            eye_size_[eye] = {view.render_width, view.render_height};
        }
        requested_eye_size_ = eye_size_;
        const auto abandon_session = [&] {
            DestroySwapchains();
            passthrough_.Destroy();
            runtime.DestroySession();
            runtime.SetGraphicsQueueGuard(nullptr, nullptr);
            owns_session_ = false;
        };
        if (!SelectSwapchainFormat() || !CreateSwapchains()) {
            abandon_session();
            return false;
        }
        if (runtime.ShouldExit()) {
            abandon_session();
            return Fail("OpenXR session became loss-pending while creating Vulkan swapchains");
        }
        if (!aurora_vulkan_direct_enable(&Impl::OnAuroraSubmitted, this)) {
            abandon_session();
            return Fail("Aurora could not enable its direct Vulkan stereo bridge");
        }
        bridge_enabled_ = true;
        bound_ = true;
        std::ostringstream message;
        message << "OpenXR Vulkan swapchains ready: VkFormat " << static_cast<int64_t>(swapchain_format_)
                << " (Aurora " << static_cast<int64_t>(aurora_format_) << "), eyes " << eye_swapchains_[0].width << 'x'
                << eye_swapchains_[0].height << " / " << eye_swapchains_[1].width << 'x' << eye_swapchains_[1].height
                << ", Vulkan " << VK_API_VERSION_MAJOR(instance_api_version_) << '.'
                << VK_API_VERSION_MINOR(instance_api_version_) << " instance, timeline semaphores "
                << (timeline_semaphores_ ? "enabled" : "not enabled")
                << "; direct presentation (eyes drawn into the swapchain on Dawn's queue)";
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    void SetRenderScale(float scale) {
        if (runtime_ == nullptr) {
            return;
        }
        std::array<OpenXREyeSize, kOpenXREyeCount> requested{};
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            requested[eye] = OpenXRScaledEyeSize(runtime_->ViewConfiguration()[eye].properties, scale);
        }
        // Only a change: a refused size stays refused while it is still the one asked for.
        if (requested != requested_eye_size_) {
            requested_eye_size_ = requested;
            eye_size_ = requested;
        }
    }

    // ---- Frame-first pacing (interpolation) ---------------------------------------------------

    OpenXRBeginStatus BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame) {
        frame = {};
        frame.presentation = presentation;
        if (!bound_ || runtime_ == nullptr) {
            Fail("BeginFrame called before the Vulkan backend was bound");
            return OpenXRBeginStatus::Error;
        }
        if (frame_active_ || pending_packet_serial_ != 0) {
            Fail("BeginFrame called while another OpenXR frame is active");
            return OpenXRBeginStatus::Error;
        }
        if (!ResizeWritablePair()) {
            return OpenXRBeginStatus::Error;
        }
        const OpenXRFrameStatus status = runtime_->WaitFrame(frame.xr_frame);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(BeginStatusOperation(status));
            }
            return BeginStatusFor(status);
        }
        NoteDisplayTiming(frame.xr_frame);
        passthrough_.SetRunning(*runtime_, presentation.passthrough);
        if (!runtime_->BeginFrame(frame.xr_frame)) {
            Fail("xrBeginFrame failed");
            return OpenXRBeginStatus::Error;
        }
        frame_active_ = true;
        active_frame_serial_ = frame.xr_frame.serial;
        active_frame_ = frame.xr_frame;
        render_session_serial_ = runtime_->SessionRunSerial();
        render_space_serial_ = runtime_->LastReferenceSpaceChange().serial;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            frame.render_width[eye] = eye_swapchains_[eye].width;
            frame.render_height[eye] = eye_swapchains_[eye].height;
        }
        if (!frame.xr_frame.should_render) {
            return OpenXRBeginStatus::Ready;
        }
        if (!runtime_->LocateViews(frame.xr_frame)) {
            Fail("xrLocateViews failed");
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRBeginStatus::Error;
        }
        active_frame_ = frame.xr_frame;
        if (!frame.xr_frame.views_valid) {
            return OpenXRBeginStatus::Ready;
        }
        const OpenXRBeginStatus prepared = PrepareTargets(frame);
        if (prepared != OpenXRBeginStatus::Ready) {
            EndActiveFrameWithoutLayers(frame.xr_frame);
        }
        return prepared;
    }

    // Acquires the frame's images and hands them to Aurora; on failure nothing stays acquired.
    OpenXRBeginStatus PrepareTargets(OpenXRBackendFrame& frame) {
        const uint32_t target_count = PresentationTargetCount(frame.presentation);
        if (target_count == 1) {
            frame.render_width[1] = frame.render_width[0];
            frame.render_height[1] = frame.render_height[0];
        }
        std::array<AuroraVulkanDirectTarget, kOpenXREyeCount> targets{};
        const diagnostics::Stopwatch acquire_timer;
        for (uint32_t eye = 0; eye < target_count; ++eye) {
            auto& swapchain = eye_swapchains_[eye];
            if (!AcquireSwapchain(swapchain)) {
                ReleaseAcquiredSwapchains();
                return OpenXRBeginStatus::Error;
            }
            targets[eye] = {
                reinterpret_cast<uint64_t>(swapchain.images[swapchain.acquired_index].image),
                swapchain.width,
                swapchain.height,
                static_cast<int64_t>(swapchain_format_),
            };
        }
        // The settings panel's layer image, rendered with the eyes while it is open.
        AuroraVulkanDirectTarget panel_target{};
        const bool panel = frame.presentation.panel.requested && EnsurePanelSwapchains();
        frame.presentation.panel.requested = panel;
        if (panel) {
            if (!AcquireSwapchain(panel_swapchain_)) {
                ReleaseAcquiredSwapchains();
                return OpenXRBeginStatus::Error;
            }
            panel_target = {
                reinterpret_cast<uint64_t>(panel_swapchain_.images[panel_swapchain_.acquired_index].image),
                panel_swapchain_.width,
                panel_swapchain_.height,
                static_cast<int64_t>(swapchain_format_),
            };
        }
        diagnostics::OnSwapchainAcquire(acquire_timer);
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = frame.xr_frame.serial;
            submitted_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
        }
        if (!aurora_vulkan_direct_set_targets(frame.xr_frame.serial, targets.data(), target_count,
                                              panel ? &panel_target : nullptr)) {
            {
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = 0;
            }
            ReleaseAcquiredSwapchains();
            Fail("Aurora rejected the acquired OpenXR Vulkan swapchain images");
            return OpenXRBeginStatus::Error;
        }
        frame.expects_gpu_submission = true;
        return OpenXRBeginStatus::Ready;
    }

    OpenXRSubmissionStatus WaitForSubmission(const OpenXRBackendFrame& frame, uint32_t timeout_ms) {
        if (!frame.expects_gpu_submission) {
            return OpenXRSubmissionStatus::Success;
        }
        std::unique_lock lock(submission_mutex_);
        const auto ready = [&] {
            return shutting_down_ || (submission_arrived_ && submitted_token_ == frame.xr_frame.serial);
        };
        if (timeout_ms == std::numeric_limits<uint32_t>::max()) {
            submission_cv_.wait(lock, ready);
        } else if (!submission_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
            return OpenXRSubmissionStatus::Timeout;
        }
        if (shutting_down_) {
            return OpenXRSubmissionStatus::ShuttingDown;
        }
        return submission_success_ ? OpenXRSubmissionStatus::Success : OpenXRSubmissionStatus::Failed;
    }

    bool TryCancelPendingFrame(OpenXRBackendFrame& frame) {
        if (!frame_active_ || !frame.expects_gpu_submission || frame.xr_frame.serial != active_frame_serial_) {
            return false;
        }
        if (!aurora_vulkan_direct_cancel(frame.xr_frame.serial)) {
            return false;
        }
        // A cancellation is serialized against Aurora's encode and fires no callback, so no GPU
        // work references the acquired images.
        std::lock_guard lock(submission_mutex_);
        awaiting_token_ = 0;
        submitted_token_ = 0;
        submission_arrived_ = false;
        submission_success_ = false;
        submission_unsafe_ = false;
        frame.expects_gpu_submission = false;
        return true;
    }

    bool FinishFrame(OpenXRBackendFrame& frame, bool submit_layer) {
        if (!frame_active_ || runtime_ == nullptr || frame.xr_frame.serial != active_frame_serial_) {
            return Fail("FinishFrame received a stale or inactive OpenXR frame token");
        }
        bool submission_unsafe = false;
        {
            std::lock_guard lock(submission_mutex_);
            submission_unsafe = submission_arrived_ && submitted_token_ == frame.xr_frame.serial && submission_unsafe_;
        }
        if (submission_unsafe) {
            AbandonAcquiredSwapchains();
            Fail("Aurora's Vulkan stereo submission failed after GPU work may have been queued");
        }
        const diagnostics::Stopwatch release_timer;
        const bool release_ok = ReleaseAcquiredSwapchains();
        if (frame.xr_frame.should_render && frame.xr_frame.views_valid) {
            diagnostics::OnSwapchainRelease(release_timer);
        }
        const bool position_valid = (frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        const bool composition_pose_valid = frame.presentation.mode == OpenXRFrameMode::VirtualScreen || position_valid;
        const bool can_submit = submit_layer && release_ok && frame.xr_frame.should_render &&
                                frame.xr_frame.views_valid && frame.expects_gpu_submission && composition_pose_valid;
        if (submit_layer && !can_submit) {
            diagnostics::OnLayerRejected(
                diagnostics::ClassifyRejectedLayer(release_ok, frame.xr_frame.should_render, frame.xr_frame.views_valid));
        }
        if (can_submit) {
            // xrEndFrame references the most recently released image of a swapchain, so keep the
            // displayed pair separate from the pair Aurora may write next.
            std::swap(eye_swapchains_, retained_swapchains_);
            if (frame.presentation.panel.requested) {
                std::swap(panel_swapchain_, retained_panel_swapchain_);
            }
            retained_panel_valid_ = frame.presentation.panel.requested;
            retained_frame_ = frame;
            retained_session_serial_ = render_session_serial_;
            retained_space_serial_ = render_space_serial_;
            have_retained_frame_ = true;
        }
        const bool end_ok = EndRetainedFrame(can_submit);
        frame_active_ = false;
        active_frame_serial_ = 0;
        active_frame_ = {};
        frame.expects_gpu_submission = false;
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
        }
        return release_ok && end_ok;
    }

    bool RepeatFrame(const OpenXRBackendFrame& frame) {
        if (!frame_active_ || runtime_ == nullptr || frame.xr_frame.serial != active_frame_serial_) {
            return Fail("RepeatFrame received a stale or inactive render token");
        }
        const bool end_ok = EndRetainedFrame(false);
        // EndFrame consumes the compositor token even when it fails.
        frame_active_ = false;
        if (!end_ok) {
            return Fail("OpenXR could not resubmit the retained frame");
        }
        if (runtime_->PollEvents() != OpenXREventStatus::Continue || !runtime_->IsSessionRunning() ||
            runtime_->ShouldExit()) {
            return Fail("OpenXR session stopped while waiting for stereo rendering");
        }
        if (runtime_->WaitFrame(active_frame_) != OpenXRFrameStatus::Ready || !runtime_->BeginFrame(active_frame_)) {
            return Fail("OpenXR could not start a retained-frame compositor cycle");
        }
        NoteDisplayTiming(active_frame_);
        frame_active_ = true;
        return true;
    }

    // ---- Render-first pacing (see openxr_vulkan_direct.h) -------------------------------------

    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
        packet = {};
        packet.presentation = presentation;
        if (!bound_ || runtime_ == nullptr || frame_active_ || pending_packet_serial_ != 0) {
            Fail("PreparePacket called before binding or with work pending");
            return OpenXRBeginStatus::Error;
        }
        if (runtime_->ShouldExit()) {
            return OpenXRBeginStatus::ExitRequested;
        }
        if (!runtime_->IsSessionRunning()) {
            return OpenXRBeginStatus::SessionNotRunning;
        }
        // Before any frame ends on this presentation, the priming cycle below included.
        passthrough_.SetRunning(*runtime_, presentation.passthrough);
        if (timing_session_serial_ != runtime_->SessionRunSerial() || last_display_period_ <= 0) {
            // No display timing for this session yet: one compositor cycle learns it.
            const OpenXRBeginStatus primed = KeepAliveCycle();
            if (primed != OpenXRBeginStatus::Ready) {
                return primed;
            }
        }
        if (!ResizeWritablePair()) {
            return OpenXRBeginStatus::Error;
        }
        packet.xr_frame.serial = next_packet_serial_++;
        // The eyes are ready after at most one game frame plus the encode and show at the first
        // display slot after that: two periods past the last predicted display time.
        packet.xr_frame.predicted_display_time = last_display_time_ + 2 * last_display_period_;
        packet.xr_frame.predicted_display_period = last_display_period_;
        packet.xr_frame.should_render = last_should_render_;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            packet.render_width[eye] = eye_swapchains_[eye].width;
            packet.render_height[eye] = eye_swapchains_[eye].height;
        }
        if (!packet.xr_frame.should_render) {
            return OpenXRBeginStatus::Ready;
        }
        if (!runtime_->LocateViewsAt(packet.xr_frame.predicted_display_time, packet.xr_frame)) {
            Fail("xrLocateViews failed for a packet");
            return OpenXRBeginStatus::Error;
        }
        if (!packet.xr_frame.views_valid) {
            return OpenXRBeginStatus::Ready;
        }
        render_session_serial_ = runtime_->SessionRunSerial();
        render_space_serial_ = runtime_->LastReferenceSpaceChange().serial;
        const OpenXRBeginStatus status = PrepareTargets(packet);
        if (status == OpenXRBeginStatus::Ready) {
            pending_packet_serial_ = packet.xr_frame.serial;
        }
        return status;
    }

    bool TryCancelPendingPacket(OpenXRBackendFrame& packet) {
        if (frame_active_ || pending_packet_serial_ == 0 || packet.xr_frame.serial != pending_packet_serial_ ||
            !packet.expects_gpu_submission || !aurora_vulkan_direct_cancel(packet.xr_frame.serial)) {
            return false;
        }
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = 0;
            submitted_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
        }
        packet.expects_gpu_submission = false;
        pending_packet_serial_ = 0;
        const diagnostics::Stopwatch release_timer;
        const bool released = ReleaseAcquiredSwapchains();
        diagnostics::OnSwapchainRelease(release_timer);
        // A release error is fatal; keep it visible to the next prepare rather than letting it
        // hand Aurora new targets over still-acquired images.
        if (!released) {
            pending_packet_serial_ = packet.xr_frame.serial;
        }
        return true;
    }

    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
        frame = {};
        if (!bound_ || runtime_ == nullptr || frame_active_ || pending_packet_serial_ == 0 ||
            packet.xr_frame.serial != pending_packet_serial_ || !packet.expects_gpu_submission ||
            WaitForSubmission(packet, 0) != OpenXRSubmissionStatus::Success) {
            Fail("BeginFrameForPacket requires the completed current Vulkan packet");
            return OpenXRBeginStatus::Error;
        }
        const OpenXRBeginStatus status = BeginCompositorCycle();
        if (status != OpenXRBeginStatus::Ready) {
            // The packet completed; a stopped session must not strand it and block the next
            // prepare after the session is ready again.
            if (status == OpenXRBeginStatus::SessionNotRunning) {
                const bool released = ReleaseAcquiredSwapchains();
                pending_packet_serial_ = 0;
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = submitted_token_ = 0;
                submission_arrived_ = submission_success_ = submission_unsafe_ = false;
                if (!released) {
                    return OpenXRBeginStatus::Error;
                }
            }
            return status;
        }
        frame = packet;
        // The current compositor token and timing with the packet's render poses.
        frame.xr_frame.serial = active_frame_.serial;
        frame.xr_frame.predicted_display_time = active_frame_.predicted_display_time;
        frame.xr_frame.predicted_display_period = active_frame_.predicted_display_period;
        frame.xr_frame.wait_start_ns = active_frame_.wait_start_ns;
        frame.xr_frame.wait_ns = active_frame_.wait_ns;
        frame.xr_frame.should_render = active_frame_.should_render;
        active_frame_serial_ = frame.xr_frame.serial;
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = submitted_token_ = frame.xr_frame.serial;
        }
        pending_packet_serial_ = 0;
        return OpenXRBeginStatus::Ready;
    }

    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame) {
        if (!frame_active_ || frame.xr_frame.serial != active_frame_serial_) {
            Fail("CopyRenderedEyes received a stale Vulkan frame");
            return OpenXRSubmissionStatus::Failed;
        }
        // Aurora already drew the eyes into the images on the session's queue.
        return WaitForSubmission(frame, 0);
    }

    OpenXRBeginStatus KeepAliveCycle() {
        if (!bound_ || runtime_ == nullptr || frame_active_) {
            Fail("KeepAliveCycle called before binding or with an active frame");
            return OpenXRBeginStatus::Error;
        }
        const OpenXRBeginStatus status = BeginCompositorCycle();
        if (status != OpenXRBeginStatus::Ready) {
            return status;
        }
        const bool ended = EndRetainedFrame(false);
        frame_active_ = false;
        active_frame_ = {};
        if (!ended) {
            Fail("OpenXR could not resubmit the retained Vulkan frame");
            return OpenXRBeginStatus::Error;
        }
        return OpenXRBeginStatus::Ready;
    }

    bool Shutdown() {
        if (shutdown_unsafe_) {
            return false;
        }
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = true;
        }
        submission_cv_.notify_all();

        bool bridge_drained = true;
        if (bridge_enabled_) {
            bridge_drained = aurora_vulkan_direct_disable();
            bridge_enabled_ = false;
        }
        if (!bridge_drained) {
            AbandonAcquiredSwapchains();
            shutdown_unsafe_ = true;
            Fail("Vulkan queue completion is unknown; retaining the OpenXR session and graphics owners");
            return false;
        }
        // The drain retired every draw into an acquired image, including one after a failed
        // submission.
        AllowAcquiredSwapchainsAfterGpuDrain();
        ReleaseAcquiredSwapchains();
        if (frame_active_ && runtime_ != nullptr) {
            // On the XR owner thread with Aurora's worker idle: closing an abandoned frame is safe.
            if (runtime_->IsSessionRunning()) {
                runtime_->EndFrameWithoutLayers(active_frame_);
            }
            frame_active_ = false;
            active_frame_serial_ = 0;
            active_frame_ = {};
        }
        DestroySwapchains();
        pending_packet_serial_ = 0;
        last_display_period_ = 0;
        // Its handles belong to the session.
        passthrough_.Destroy();
        if (owns_session_ && runtime_ != nullptr) {
            runtime_->DestroySession();
            owns_session_ = false;
        }
        if (runtime_ != nullptr) {
            runtime_->SetGraphicsQueueGuard(nullptr, nullptr);
        }
        if (hooks_installed_) {
            aurora_vulkan_direct_configure(nullptr);
            hooks_installed_ = false;
        }
        vk_instance_ = VK_NULL_HANDLE;
        instance_api_version_ = 0;
        timeline_semaphores_ = false;
        bound_ = false;
        requirements_queried_ = false;
        runtime_ = nullptr;
        return true;
    }

    bool IsBound() const { return bound_; }
    bool PanelLayerAvailable() const { return !panel_layer_failed_; }
    const OpenXRVulkanGraphicsRequirements& GraphicsRequirements() const { return requirements_; }
    int64_t SwapchainFormat() const { return static_cast<int64_t>(swapchain_format_); }
    const std::string& LastError() const { return last_error_; }

private:
    void NoteDisplayTiming(const OpenXRFrame& frame) noexcept {
        last_display_time_ = frame.predicted_display_time;
        last_display_period_ = frame.predicted_display_period;
        last_should_render_ = frame.should_render;
        timing_session_serial_ = runtime_->SessionRunSerial();
    }

    OpenXRBeginStatus BeginCompositorCycle() {
        const OpenXRFrameStatus status = runtime_->WaitFrame(active_frame_);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(BeginStatusOperation(status));
            }
            return BeginStatusFor(status);
        }
        NoteDisplayTiming(active_frame_);
        if (!runtime_->BeginFrame(active_frame_)) {
            Fail("xrBeginFrame failed for a Vulkan compositor cycle");
            return OpenXRBeginStatus::Error;
        }
        frame_active_ = true;
        return OpenXRBeginStatus::Ready;
    }

    // fresh: the retained layer was completed for this call rather than repeated.
    bool EndRetainedFrame(bool fresh) {
        if (!runtime_->IsSessionRunning()) {
            return true;
        }
        // Old poses cannot be reused after the runtime changes their coordinate system, nor
        // content across session restarts.
        const bool session_changed = retained_session_serial_ != runtime_->SessionRunSerial();
        if (session_changed || retained_space_serial_ != runtime_->LastReferenceSpaceChange().serial) {
            if (have_retained_frame_) {
                diagnostics::OnRetainedLayerDiscarded(session_changed ? diagnostics::DiscardReason::SessionRestarted
                                                                      : diagnostics::DiscardReason::ReferenceSpaceChanged);
            }
            have_retained_frame_ = false;
        }
        if (!have_retained_frame_ || !active_frame_.should_render) {
            diagnostics::OnEmptyFrame(!active_frame_.should_render ? diagnostics::EmptyFrameReason::ShouldRenderOff
                                                                    : diagnostics::EmptyFrameReason::NoRetainedLayer);
            // Outside a race the room stays in view until there is an image to show (at start
            // and after a recenter), rather than flashing black.
            if (const XrCompositionLayerBaseHeader* passthrough = passthrough_.Layer()) {
                return runtime_->EndFrame(active_frame_, &passthrough, 1);
            }
            return runtime_->EndFrameWithoutLayers(active_frame_);
        }
        diagnostics::OnLayer(fresh);
        const auto& frame = retained_frame_;
        if (frame.presentation.mode == OpenXRFrameMode::VirtualScreen) {
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags = 0;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = retained_swapchains_[0].handle;
            // Only the picture, not the black bands letterboxing it into the eye-sized image: they
            // would frame it against the passthrough view.
            const uint32_t image_width = retained_swapchains_[0].width;
            quad.subImage.imageRect = OpenXRVirtualScreenContentRect(image_width, retained_swapchains_[0].height,
                                                                     frame.presentation.quad_content_aspect);
            quad.subImage.imageArrayIndex = 0;
            if (frame.presentation.quad_anchored) {
                quad.space = runtime_->AppSpace();
                quad.pose = frame.presentation.quad_pose;
            } else {
                quad.space = runtime_->ViewSpace();
                quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                quad.pose.position = {0.0f, 0.0f, -std::max(0.25f, frame.presentation.quad_distance_meters)};
            }
            // The whole image would be quad_width_meters across: the crop keeps that size per pixel,
            // so the picture stays exactly where the pointer and the settings panel expect it.
            const float meters_per_pixel =
                std::max(0.25f, frame.presentation.quad_width_meters) / static_cast<float>(image_width);
            quad.size.width = meters_per_pixel * static_cast<float>(quad.subImage.imageRect.extent.width);
            quad.size.height = meters_per_pixel * static_cast<float>(quad.subImage.imageRect.extent.height);
            return EndFrameWithPanel(frame, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
        }
        std::array<XrCompositionLayerProjectionView, kOpenXREyeCount> views{};
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[eye].pose.orientation = frame.xr_frame.views[eye].pose.orientation;
            views[eye].pose.position = frame.xr_frame.views[eye].pose.position;
            views[eye].fov = frame.xr_frame.views[eye].fov;
            views[eye].subImage.swapchain = retained_swapchains_[eye].handle;
            // The part of the image the eye was rendered into: all of it, except for the immersive
            // window's eyes, which are only the window (OpenXRPresentation::window_eyes).
            views[eye].subImage.imageRect = {
                {0, 0},
                {static_cast<int32_t>(std::min(frame.render_width[eye], retained_swapchains_[eye].width)),
                 static_cast<int32_t>(std::min(frame.render_height[eye], retained_swapchains_[eye].height))}};
            views[eye].subImage.imageArrayIndex = 0;
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        // The immersive window's eyes are transparent outside the window (premultiplied alpha), so
        // the room shows around it; otherwise the race covers the whole view and alpha is ignored.
        projection.layerFlags =
            frame.presentation.immersive_window ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        projection.space = runtime_->AppSpace();
        projection.viewCount = kOpenXREyeCount;
        projection.views = views.data();
        return EndFrameWithPanel(frame, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
    }

    // Ends the compositor frame with the scene's layer: over the room's camera view while that
    // runs and the scene is the virtual screen or the immersive window (never under a fully
    // immersive race's projection, which covers the whole view), and, while the retained frame
    // rendered it, under the settings panel's layer.
    bool EndFrameWithPanel(const OpenXRBackendFrame& frame, const XrCompositionLayerBaseHeader* scene) {
        const auto& panel = frame.presentation.panel;
        XrCompositionLayerQuad panel_quad{};
        const XrCompositionLayerBaseHeader* layers[3] = {};
        uint32_t count = 0;
        if (const XrCompositionLayerBaseHeader* passthrough = passthrough_.Layer();
            passthrough != nullptr &&
            (frame.presentation.mode == OpenXRFrameMode::VirtualScreen || frame.presentation.immersive_window)) {
            layers[count++] = passthrough;
        }
        layers[count++] = scene;
        if (retained_panel_valid_ && panel.requested && panel.placed) {
            panel_quad = OpenXRPanelQuadLayer(panel, runtime_->AppSpace(), retained_panel_swapchain_.handle);
            layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel_quad);
        }
        return runtime_->EndFrame(active_frame_, layers, count);
    }

    bool SelectSwapchainFormat() {
        const auto& formats = runtime_->SwapchainFormats();
        // Aurora's UNORM target holds gamma-encoded bytes; declaring the sRGB sibling makes the
        // compositor decode them instead of treating them as linear light. The blit decodes into
        // an sRGB attachment, and copies between UNORM and sRGB siblings are raw.
        const VkFormat srgb = SrgbSibling(aurora_format_);
        if (srgb != VK_FORMAT_UNDEFINED &&
            std::find(formats.begin(), formats.end(), static_cast<int64_t>(srgb)) != formats.end()) {
            swapchain_format_ = srgb;
            return true;
        }
        if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(aurora_format_)) != formats.end()) {
            swapchain_format_ = aurora_format_;
            return true;
        }
        const auto compatible = std::find_if(formats.begin(), formats.end(), [&](int64_t format) {
            return SameVkCopyFamily(aurora_format_, static_cast<VkFormat>(format));
        });
        if (compatible == formats.end()) {
            return Fail("OpenXR offered no swapchain format copy-compatible with Aurora's Vulkan colour format");
        }
        swapchain_format_ = static_cast<VkFormat>(*compatible);
        return true;
    }

    bool CreateSwapchains() { return CreateSwapchainPair(eye_swapchains_) && CreateSwapchainPair(retained_swapchains_); }

    bool CreateSwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            if (!CreateSwapchain(pair[eye], eye_size_[eye].width, eye_size_[eye].height,
                                 eye == 0 ? "left eye" : "right eye")) {
                return false;
            }
        }
        return true;
    }

    // As the D3D12 backend's. Aurora wraps each swapchain VkImage for Dawn, and the runtime may
    // hand the old handles out again, so the wraps go with the swapchains (ReapRetiredPairs).
    bool ResizeWritablePair() {
        ReapRetiredPairs(false);
        std::array<OpenXREyeSize, kOpenXREyeCount> current{};
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            if (eye_swapchains_[eye].acquired) {
                return true;
            }
            current[eye] = {eye_swapchains_[eye].width, eye_swapchains_[eye].height};
        }
        if (current == eye_size_) {
            return true;
        }
        std::array<EyeSwapchain, kOpenXREyeCount> replacement{};
        if (!CreateSwapchainPair(replacement)) {
            DestroySwapchainPair(replacement);
            std::ostringstream message;
            message << "OpenXR Vulkan eyes stay " << current[0].width << 'x' << current[0].height
                    << ": the runtime could not make " << eye_size_[0].width << 'x' << eye_size_[0].height
                    << " swapchains (" << last_error_ << ')';
            Log(OpenXRLogLevel::Warning, message.str());
            ClearError();
            // Back to this pair's size, which also returns the other pair to it if it was rebuilt.
            eye_size_ = current;
            return true;
        }
        // The compositor may still be reading the old pair (kOpenXRRetiredSwapchainCycles).
        retired_pairs_.push_back({std::move(eye_swapchains_), kOpenXRRetiredSwapchainCycles});
        eye_swapchains_ = std::move(replacement);
        std::ostringstream message;
        message << "OpenXR Vulkan eyes resized: " << eye_size_[0].width << 'x' << eye_size_[0].height << " / "
                << eye_size_[1].width << 'x' << eye_size_[1].height;
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    // Aurora drains Dawn's queue and drops its wraps of the images (under Dawn's device lock, so
    // before the lock is taken to destroy them).
    void ReapRetiredPairs(bool all) {
        for (auto it = retired_pairs_.begin(); it != retired_pairs_.end();) {
            if (!all && --it->cycles_left != 0) {
                ++it;
                continue;
            }
            std::vector<uint64_t> images;
            for (const EyeSwapchain& swapchain : it->swapchains) {
                for (const auto& image : swapchain.images) {
                    images.push_back(reinterpret_cast<uint64_t>(image.image));
                }
            }
            if (aurora_vulkan_direct_forget_targets(images.data(), static_cast<uint32_t>(images.size()))) {
                DestroySwapchainPair(it->swapchains);
            } else {
                Log(OpenXRLogLevel::Warning,
                    "Aurora could not retire its draws into a replaced Vulkan eye swapchain pair; "
                    "deferring its destruction to xrDestroySession");
            }
            it = retired_pairs_.erase(it);
        }
    }

    bool CreateSwapchain(EyeSwapchain& swapchain, uint32_t width, uint32_t height, const char* what) {
        swapchain.width = width;
        swapchain.height = height;
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create.format = static_cast<int64_t>(swapchain_format_);
        create.sampleCount = 1;
        create.width = swapchain.width;
        create.height = swapchain.height;
        create.faceCount = 1;
        create.arraySize = 1;
        create.mipCount = 1;
        XrResult result;
        {
            // Mid-session (the panel's swapchains, a new render resolution) Dawn's worker is
            // submitting on the queue a runtime may use to prepare the new images.
            const auto queue_guard = runtime_->LockGraphicsQueue();
            result = xrCreateSwapchain(runtime_->Session(), &create, &swapchain.handle);
        }
        ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrCreateSwapchain failed for the Vulkan " << what << " swapchain (" << result << ')';
            return Fail(message.str());
        }
        uint32_t count = 0;
        result = xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr);
        ObserveResult(result);
        if (XR_FAILED(result) || count == 0) {
            return Fail("OpenXR returned no Vulkan swapchain images");
        }
        swapchain.images.resize(count);
        for (auto& image : swapchain.images) {
            image = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
        }
        result = xrEnumerateSwapchainImages(swapchain.handle, count, &count,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrEnumerateSwapchainImages failed for a Vulkan eye swapchain");
        }
        return true;
    }

    bool AcquireSwapchain(EyeSwapchain& swapchain) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result;
        {
            const auto queue_guard = runtime_->LockGraphicsQueue();
            result = xrAcquireSwapchainImage(swapchain.handle, &acquire, &swapchain.acquired_index);
        }
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrAcquireSwapchainImage failed for a Vulkan eye swapchain");
        }
        swapchain.acquired = true;
        swapchain.waited = false;
        swapchain.release_forbidden = false;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        // The runtime does not use the queue here: Dawn keeps submitting while the compositor
        // frees the image.
        result = xrWaitSwapchainImage(swapchain.handle, &wait);
        ObserveResult(result);
        if (result == XR_TIMEOUT_EXPIRED) {
            return Fail("xrWaitSwapchainImage unexpectedly timed out for a Vulkan eye swapchain");
        }
        if (XR_FAILED(result)) {
            return Fail("xrWaitSwapchainImage failed for a Vulkan eye swapchain");
        }
        swapchain.waited = true;
        if (swapchain.acquired_index >= swapchain.images.size()) {
            return Fail("OpenXR returned an out-of-range Vulkan swapchain image index");
        }
        return true;
    }

    bool ReleaseAcquiredSwapchains() {
        const auto queue_guard =
            runtime_ != nullptr ? runtime_->LockGraphicsQueue() : OpenXRRuntime::GraphicsQueueGuard{nullptr, nullptr};
        bool success = true;
        for (auto& swapchain : eye_swapchains_) {
            success = ReleaseSwapchain(swapchain) && success;
        }
        return ReleaseSwapchain(panel_swapchain_) && success;
    }

    bool ReleaseSwapchain(EyeSwapchain& swapchain) {
        if (!swapchain.acquired || swapchain.handle == XR_NULL_HANDLE) {
            return true;
        }
        if (!swapchain.waited) {
            // OpenXR only permits release after a successful wait. Keep the image acquired and let
            // session teardown destroy the child.
            Log(OpenXRLogLevel::Warning, "cannot release an OpenXR Vulkan image whose wait did not complete");
            return false;
        }
        if (swapchain.release_forbidden) {
            // Aurora reported a failed submission after it may have queued GPU work: without a
            // trustworthy fence the release could race it, so leave the image for xrDestroySession.
            Log(OpenXRLogLevel::Warning, "deferring an OpenXR Vulkan image after an unsafe GPU submission");
            return false;
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult result = xrReleaseSwapchainImage(swapchain.handle, &release);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrReleaseSwapchainImage failed for a Vulkan swapchain");
        }
        swapchain.acquired = false;
        swapchain.waited = false;
        return true;
    }

    void AbandonAcquiredSwapchains() noexcept {
        for (auto* swapchain : {&eye_swapchains_[0], &eye_swapchains_[1], &panel_swapchain_}) {
            if (swapchain->acquired) {
                swapchain->release_forbidden = true;
            }
        }
    }

    void AllowAcquiredSwapchainsAfterGpuDrain() noexcept {
        for (auto* swapchain : {&eye_swapchains_[0], &eye_swapchains_[1], &panel_swapchain_}) {
            if (swapchain->acquired) {
                swapchain->release_forbidden = false;
            }
        }
    }

    // The settings panel's swapchain pair, made the first time the panel opens. A failure is
    // logged once and the panel is drawn into the eyes instead (PanelLayerAvailable).
    bool EnsurePanelSwapchains() {
        if (panel_swapchains_ready_) {
            return true;
        }
        if (panel_layer_failed_) {
            return false;
        }
        if (CreateSwapchain(panel_swapchain_, kOpenXRPanelLayerWidth, kOpenXRPanelLayerHeight, "settings panel") &&
            CreateSwapchain(retained_panel_swapchain_, kOpenXRPanelLayerWidth, kOpenXRPanelLayerHeight,
                            "settings panel")) {
            panel_swapchains_ready_ = true;
            Log(OpenXRLogLevel::Info, "OpenXR settings panel layer ready");
            return true;
        }
        DestroyPanelSwapchains();
        panel_layer_failed_ = true;
        Log(OpenXRLogLevel::Warning, "the settings panel could not get its own OpenXR layer; drawing it into the eyes");
        return false;
    }

    void DestroySwapchainHandle(EyeSwapchain& swapchain, const char* what) {
        if (swapchain.handle != XR_NULL_HANDLE && !swapchain.acquired) {
            const auto queue_guard = runtime_ != nullptr ? runtime_->LockGraphicsQueue()
                                                         : OpenXRRuntime::GraphicsQueueGuard{nullptr, nullptr};
            xrDestroySwapchain(swapchain.handle);
        } else if (swapchain.acquired) {
            std::ostringstream message;
            message << "Vulkan " << what
                    << " swapchain still owns an acquired image; deferring its destruction to xrDestroySession";
            Log(OpenXRLogLevel::Warning, message.str());
        }
        swapchain = {};
    }

    void DestroyPanelSwapchains() {
        DestroySwapchainHandle(panel_swapchain_, "panel");
        DestroySwapchainHandle(retained_panel_swapchain_, "panel");
        panel_swapchains_ready_ = false;
        retained_panel_valid_ = false;
    }

    void DestroySwapchains() {
        DestroyPanelSwapchains();
        ReapRetiredPairs(true);
        DestroySwapchainPair(eye_swapchains_);
        DestroySwapchainPair(retained_swapchains_);
        have_retained_frame_ = false;
        retained_frame_ = {};
        swapchain_format_ = VK_FORMAT_UNDEFINED;
    }

    void DestroySwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
        for (auto& swapchain : pair) {
            DestroySwapchainHandle(swapchain, "eye");
        }
    }

    void EndActiveFrameWithoutLayers(const OpenXRFrame& frame) {
        if (frame_active_ && runtime_ != nullptr && runtime_->IsSessionRunning()) {
            runtime_->EndFrameWithoutLayers(frame);
        }
        frame_active_ = false;
        active_frame_serial_ = 0;
        active_frame_ = {};
    }

    void ObserveResult(XrResult result) noexcept {
        if (runtime_ != nullptr) {
            runtime_->ObserveResult(result);
        }
    }

    static void OnAuroraSubmitted(uint64_t token, bool success, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr) {
            return;
        }
        {
            std::lock_guard lock(self->submission_mutex_);
            if (token != self->awaiting_token_) {
                return;
            }
            self->submitted_token_ = token;
            self->submission_success_ = success;
            self->submission_arrived_ = true;
            self->submission_unsafe_ = !success;
        }
        self->submission_cv_.notify_all();
    }

    bool Fail(std::string message) {
        last_error_ = std::move(message);
        Log(OpenXRLogLevel::Error, last_error_);
        return false;
    }

    void ClearError() { last_error_.clear(); }

    void Log(OpenXRLogLevel level, std::string_view message) const noexcept {
        if (!logger_) {
            return;
        }
        try {
            logger_(level, message);
        } catch (...) {
        }
    }

    PFN_xrCreateVulkanInstanceKHR create_instance_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_device_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device_ = nullptr;
    VkInstance vk_instance_ = VK_NULL_HANDLE;
    uint32_t instance_api_version_ = 0;
    bool timeline_semaphores_ = false;
    bool hooks_installed_ = false;
    OpenXRRuntime* runtime_ = nullptr;
    OpenXRLogCallback logger_;
    // The room around the virtual screen, started and paused as each presentation arrives.
    OpenXRPassthrough passthrough_{logger_};
    OpenXRVulkanGraphicsRequirements requirements_{};
    std::array<EyeSwapchain, kOpenXREyeCount> eye_swapchains_{};
    std::array<EyeSwapchain, kOpenXREyeCount> retained_swapchains_{};
    // As the D3D12 backend's.
    std::array<OpenXREyeSize, kOpenXREyeCount> eye_size_{};
    std::array<OpenXREyeSize, kOpenXREyeCount> requested_eye_size_{};
    struct RetiredPair {
        std::array<EyeSwapchain, kOpenXREyeCount> swapchains;
        uint32_t cycles_left;
    };
    std::vector<RetiredPair> retired_pairs_;
    // The settings panel's layer: written like the eyes into panel_swapchain_, shown from
    // retained_panel_swapchain_ (see FinishFrame).
    EyeSwapchain panel_swapchain_{};
    EyeSwapchain retained_panel_swapchain_{};
    bool panel_swapchains_ready_ = false;
    bool panel_layer_failed_ = false;
    // The retained frame rendered the panel's image into retained_panel_swapchain_.
    bool retained_panel_valid_ = false;
    OpenXRBackendFrame retained_frame_{};
    uint64_t retained_session_serial_ = 0;
    uint64_t retained_space_serial_ = 0;
    bool have_retained_frame_ = false;
    VkFormat aurora_format_ = VK_FORMAT_UNDEFINED;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    std::string last_error_;

    std::mutex submission_mutex_;
    std::condition_variable submission_cv_;
    uint64_t awaiting_token_ = 0;
    uint64_t submitted_token_ = 0;
    bool submission_arrived_ = false;
    bool submission_success_ = false;
    bool submission_unsafe_ = false;
    bool shutting_down_ = false;

    uint64_t pending_packet_serial_ = 0;
    // Packet tokens never collide with the runtime's frame serials.
    uint64_t next_packet_serial_ = 1ull << 40;
    uint64_t timing_session_serial_ = 0;
    XrTime last_display_time_ = 0;
    XrDuration last_display_period_ = 0;
    bool last_should_render_ = false;
    uint64_t active_frame_serial_ = 0;
    uint64_t render_session_serial_ = 0;
    uint64_t render_space_serial_ = 0;
    OpenXRFrame active_frame_{};
    bool requirements_queried_ = false;
    bool owns_session_ = false;
    bool bridge_enabled_ = false;
    bool bound_ = false;
    bool frame_active_ = false;
    bool shutdown_unsafe_ = false;
};

OpenXRVulkanDirectBackend::OpenXRVulkanDirectBackend(OpenXRLogCallback logger)
: m_impl(std::make_unique<Impl>(std::move(logger))) {}
OpenXRVulkanDirectBackend::~OpenXRVulkanDirectBackend() = default;
bool OpenXRVulkanDirectBackend::QueryGraphicsRequirements(OpenXRRuntime& runtime) {
    return m_impl->QueryGraphicsRequirements(runtime);
}
bool OpenXRVulkanDirectBackend::BindAurora(OpenXRRuntime& runtime) { return m_impl->BindAurora(runtime); }
void OpenXRVulkanDirectBackend::SetRenderScale(float scale) { m_impl->SetRenderScale(scale); }
OpenXRBeginStatus OpenXRVulkanDirectBackend::BeginFrame(const OpenXRPresentation& presentation,
                                                        OpenXRBackendFrame& frame) {
    return m_impl->BeginFrame(presentation, frame);
}
OpenXRSubmissionStatus OpenXRVulkanDirectBackend::WaitForSubmission(const OpenXRBackendFrame& frame,
                                                                    uint32_t timeout_ms) {
    return m_impl->WaitForSubmission(frame, timeout_ms);
}
bool OpenXRVulkanDirectBackend::TryCancelPendingFrame(OpenXRBackendFrame& frame) {
    return m_impl->TryCancelPendingFrame(frame);
}
bool OpenXRVulkanDirectBackend::RepeatFrame(const OpenXRBackendFrame& frame) { return m_impl->RepeatFrame(frame); }
bool OpenXRVulkanDirectBackend::FinishFrame(OpenXRBackendFrame& frame, bool submit_layer) {
    return m_impl->FinishFrame(frame, submit_layer);
}
OpenXRBeginStatus OpenXRVulkanDirectBackend::PreparePacket(const OpenXRPresentation& presentation,
                                                           OpenXRBackendFrame& packet) {
    return m_impl->PreparePacket(presentation, packet);
}
bool OpenXRVulkanDirectBackend::TryCancelPendingPacket(OpenXRBackendFrame& packet) {
    return m_impl->TryCancelPendingPacket(packet);
}
OpenXRBeginStatus OpenXRVulkanDirectBackend::BeginFrameForPacket(const OpenXRBackendFrame& packet,
                                                                 OpenXRBackendFrame& frame) {
    return m_impl->BeginFrameForPacket(packet, frame);
}
OpenXRSubmissionStatus OpenXRVulkanDirectBackend::CopyRenderedEyes(const OpenXRBackendFrame& frame) {
    return m_impl->CopyRenderedEyes(frame);
}
OpenXRBeginStatus OpenXRVulkanDirectBackend::KeepAliveCycle() { return m_impl->KeepAliveCycle(); }
bool OpenXRVulkanDirectBackend::Shutdown() { return m_impl->Shutdown(); }
bool OpenXRVulkanDirectBackend::IsBound() const { return m_impl->IsBound(); }
bool OpenXRVulkanDirectBackend::PanelLayerAvailable() const { return m_impl->PanelLayerAvailable(); }
const OpenXRVulkanGraphicsRequirements& OpenXRVulkanDirectBackend::GraphicsRequirements() const {
    return m_impl->GraphicsRequirements();
}
int64_t OpenXRVulkanDirectBackend::SwapchainFormat() const { return m_impl->SwapchainFormat(); }
const std::string& OpenXRVulkanDirectBackend::LastError() const { return m_impl->LastError(); }

// ---- OpenXRQuestVulkanBackend --------------------------------------------------------------------

OpenXRQuestVulkanBackend::OpenXRQuestVulkanBackend(OpenXRLogCallback logger) : logger_(std::move(logger)) {}
OpenXRQuestVulkanBackend::~OpenXRQuestVulkanBackend() = default;

bool OpenXRQuestVulkanBackend::QueryGraphicsRequirements(OpenXRRuntime& runtime) {
    if (direct_ != nullptr) {
        return direct_->QueryGraphicsRequirements(runtime);
    }
    if (shared_ != nullptr) {
        return shared_->QueryGraphicsRequirements(runtime);
    }
    if (RuntimeConfigFile::VrDirectPresentation()) {
        auto direct = std::make_unique<OpenXRVulkanDirectBackend>(logger_);
        if (direct->QueryGraphicsRequirements(runtime)) {
            direct_ = std::move(direct);
            return true;
        }
        if (logger_) {
            logger_(OpenXRLogLevel::Warning, "direct presentation unavailable (" + direct->LastError() +
                                                 "); sharing the eyes through AHardwareBuffers");
        }
    } else if (logger_) {
        logger_(OpenXRLogLevel::Info, "direct presentation off: sharing the eyes through AHardwareBuffers");
    }
    shared_ = std::make_unique<OpenXRVulkanBackend>(logger_);
    return shared_->QueryGraphicsRequirements(runtime);
}

#define MKW_QUEST_BACKEND_CALL(call, otherwise) \
    (direct_ != nullptr ? direct_->call : shared_ != nullptr ? shared_->call : (otherwise))

bool OpenXRQuestVulkanBackend::BindAurora(OpenXRRuntime& runtime) {
    return MKW_QUEST_BACKEND_CALL(BindAurora(runtime), false);
}
void OpenXRQuestVulkanBackend::SetRenderScale(float scale) {
    if (direct_ != nullptr) {
        direct_->SetRenderScale(scale);
    } else if (shared_ != nullptr) {
        shared_->SetRenderScale(scale);
    }
}
OpenXRBeginStatus OpenXRQuestVulkanBackend::BeginFrame(const OpenXRPresentation& presentation,
                                                       OpenXRBackendFrame& frame) {
    return MKW_QUEST_BACKEND_CALL(BeginFrame(presentation, frame), OpenXRBeginStatus::Error);
}
OpenXRSubmissionStatus OpenXRQuestVulkanBackend::WaitForSubmission(const OpenXRBackendFrame& frame,
                                                                   uint32_t timeout_ms) {
    return MKW_QUEST_BACKEND_CALL(WaitForSubmission(frame, timeout_ms), OpenXRSubmissionStatus::Failed);
}
bool OpenXRQuestVulkanBackend::TryCancelPendingFrame(OpenXRBackendFrame& frame) {
    return MKW_QUEST_BACKEND_CALL(TryCancelPendingFrame(frame), false);
}
bool OpenXRQuestVulkanBackend::RepeatFrame(const OpenXRBackendFrame& frame) {
    return MKW_QUEST_BACKEND_CALL(RepeatFrame(frame), false);
}
bool OpenXRQuestVulkanBackend::FinishFrame(OpenXRBackendFrame& frame, bool submit_layer) {
    return MKW_QUEST_BACKEND_CALL(FinishFrame(frame, submit_layer), false);
}
OpenXRBeginStatus OpenXRQuestVulkanBackend::PreparePacket(const OpenXRPresentation& presentation,
                                                          OpenXRBackendFrame& packet) {
    return MKW_QUEST_BACKEND_CALL(PreparePacket(presentation, packet), OpenXRBeginStatus::Error);
}
bool OpenXRQuestVulkanBackend::TryCancelPendingPacket(OpenXRBackendFrame& packet) {
    return MKW_QUEST_BACKEND_CALL(TryCancelPendingPacket(packet), false);
}
OpenXRBeginStatus OpenXRQuestVulkanBackend::BeginFrameForPacket(const OpenXRBackendFrame& packet,
                                                                OpenXRBackendFrame& frame) {
    return MKW_QUEST_BACKEND_CALL(BeginFrameForPacket(packet, frame), OpenXRBeginStatus::Error);
}
OpenXRSubmissionStatus OpenXRQuestVulkanBackend::CopyRenderedEyes(const OpenXRBackendFrame& frame) {
    return MKW_QUEST_BACKEND_CALL(CopyRenderedEyes(frame), OpenXRSubmissionStatus::Failed);
}
OpenXRBeginStatus OpenXRQuestVulkanBackend::KeepAliveCycle() {
    return MKW_QUEST_BACKEND_CALL(KeepAliveCycle(), OpenXRBeginStatus::Error);
}
bool OpenXRQuestVulkanBackend::Shutdown() { return MKW_QUEST_BACKEND_CALL(Shutdown(), true); }
bool OpenXRQuestVulkanBackend::IsBound() const { return MKW_QUEST_BACKEND_CALL(IsBound(), false); }
bool OpenXRQuestVulkanBackend::PanelLayerAvailable() const {
    return MKW_QUEST_BACKEND_CALL(PanelLayerAvailable(), false);
}
const OpenXRVulkanGraphicsRequirements& OpenXRQuestVulkanBackend::GraphicsRequirements() const {
    return MKW_QUEST_BACKEND_CALL(GraphicsRequirements(), kNoRequirements);
}
int64_t OpenXRQuestVulkanBackend::SwapchainFormat() const {
    return MKW_QUEST_BACKEND_CALL(SwapchainFormat(), int64_t{0});
}
const std::string& OpenXRQuestVulkanBackend::LastError() const {
    return MKW_QUEST_BACKEND_CALL(LastError(), kNoBackend);
}

#undef MKW_QUEST_BACKEND_CALL

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR) && defined(__ANDROID__)
