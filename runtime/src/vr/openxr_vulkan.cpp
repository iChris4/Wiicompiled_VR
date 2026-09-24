// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR) && defined(__ANDROID__)

// OpenXR's Vulkan structures are selected when openxr_platform.h is parsed.
#define VK_USE_PLATFORM_ANDROID_KHR
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_ANDROID

#include "vr/openxr_vulkan.h"
#include "vr/openxr_diagnostics.h"
#include "vr/openxr_passthrough.h"

#include <aurora/vulkan_interop.h>

#include <android/hardware_buffer.h>
#include <jni.h>
#include <unistd.h>
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

// Two buffers per eye: Dawn writes one while this backend's queue copies the
// other into the compositor image. The sync-fd handshake orders the two
// devices on each buffer, so a deeper ring only adds latency.
constexpr uint32_t kSlotCount = 2;
// Images one frame copies into the compositor: the eyes, then the settings
// panel's layer image, in the order of Aurora's release entries.
constexpr uint32_t kMaxCopies = AURORA_VULKAN_STEREO_MAX_RELEASES;
static_assert(kMaxCopies == kOpenXREyeCount + 1);
// Copy command buffers in flight before the oldest fence is waited on.
constexpr uint32_t kSubmissionRingSize = 3;
constexpr uint64_t kFenceTimeoutNanos = 2'000'000'000ull;

int CopyFamily(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return 1;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 3;
    default:
        return 0;
    }
}

bool SameCopyFamily(VkFormat left, VkFormat right) noexcept {
    const int family = CopyFamily(left);
    return family != 0 && family == CopyFamily(right);
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

std::vector<std::string> SplitExtensionList(const std::string& text) {
    std::vector<std::string> names;
    std::istringstream stream(text);
    std::string name;
    while (stream >> name) {
        names.push_back(name);
    }
    return names;
}

void CloseFd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
    }
    fd = -1;
}

int DupFd(int fd) noexcept {
    return fd >= 0 ? ::dup(fd) : -1;
}

std::string VkFailure(const char* what, VkResult result) {
    std::ostringstream message;
    message << what << " (VkResult " << static_cast<int32_t>(result) << ')';
    return message.str();
}

} // namespace

class OpenXRVulkanBackend::Impl final {
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

    // One AHardwareBuffer shared with Dawn, imported into this backend's own
    // device. Dawn writes it, this device reads it into the compositor image.
    struct EyeSlot {
        AHardwareBuffer* buffer = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        // Signalled by this device's copy and exported as the fence Dawn waits on.
        VkSemaphore signal_semaphore = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        // The sync fd of this slot's last copy-out, kept until the next copy replaces it. Dawn
        // gets a duplicate per frame, so a frame cancelled before encoding cannot lose it.
        int pending_acquire_fd = -1;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct Submission {
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        // Dawn's release fences are imported here, one semaphore per image. They belong to the
        // submission rather than the slot: importing into a semaphore whose previous wait is
        // still pending is invalid, and only the submission's fence proves that wait completed.
        std::array<VkSemaphore, kMaxCopies> wait_semaphores{};
        bool busy = false;
    };

    enum class CopyOutcome {
        Submitted,
        // Nothing reached the queue: the shared buffers and the compositor image are untouched.
        Skipped,
        // Work may have been queued with no completion marker to wait on.
        Unsafe,
    };

    struct PendingCopy {
        uint64_t token = 0;
        uint32_t slot = 0;
        uint32_t target_count = 0;
        // The settings panel's layer image follows the eyes.
        bool panel = false;
        std::array<VkImage, kMaxCopies> swapchain_images{};

        uint32_t Count() const noexcept { return target_count + (panel ? 1u : 0u); }
    };

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime) {
        ClearError();
        if (!runtime.IsInitialized() || runtime.HasSession()) {
            return Fail("OpenXR must own an instance, but no session, before querying Vulkan requirements");
        }
        if (requirements_queried_ && runtime_ != &runtime) {
            return Fail("Vulkan graphics requirements were already queried from another OpenXR instance");
        }

        const auto& extensions = runtime.EnabledExtensions();
        const bool enable2 = std::find(extensions.begin(), extensions.end(),
                                       std::string(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) != extensions.end();
        const bool enable1 = std::find(extensions.begin(), extensions.end(),
                                       std::string(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) != extensions.end();
        if (!enable2 && !enable1) {
            return Fail("the OpenXR runtime offers neither XR_KHR_vulkan_enable2 nor XR_KHR_vulkan_enable");
        }

        XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        XrResult result = XR_ERROR_RUNTIME_FAILURE;
        if (enable2) {
            PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = nullptr;
            if (!runtime.LoadFunction("xrGetVulkanGraphicsRequirements2KHR", &get_requirements) ||
                get_requirements == nullptr) {
                return Fail("OpenXR runtime did not expose xrGetVulkanGraphicsRequirements2KHR");
            }
            result = get_requirements(runtime.Instance(), runtime.SystemId(), &requirements);
        } else {
            PFN_xrGetVulkanGraphicsRequirementsKHR get_requirements = nullptr;
            if (!runtime.LoadFunction("xrGetVulkanGraphicsRequirementsKHR", &get_requirements) ||
                get_requirements == nullptr) {
                return Fail("OpenXR runtime did not expose xrGetVulkanGraphicsRequirementsKHR");
            }
            result = get_requirements(runtime.Instance(), runtime.SystemId(), &requirements);
        }
        runtime.ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrGetVulkanGraphicsRequirements failed (" << result << ')';
            return Fail(message.str());
        }

        runtime_ = &runtime;
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = false;
            submission_unsafe_ = false;
        }
        requirements_ = {
            requirements.minApiVersionSupported,
            requirements.maxApiVersionSupported,
            enable2,
        };
        requirements_queried_ = true;

        std::ostringstream message;
        message << "OpenXR Vulkan requirements: API "
                << XR_VERSION_MAJOR(requirements_.min_api_version) << '.'
                << XR_VERSION_MINOR(requirements_.min_api_version) << " to "
                << XR_VERSION_MAJOR(requirements_.max_api_version) << '.'
                << XR_VERSION_MINOR(requirements_.max_api_version) << " via "
                << (enable2 ? XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME : XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
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

        AuroraVulkanNativeHandles handles{};
        if (!aurora_vulkan_get_native_handles(&handles)) {
            return Fail("Aurora did not report a Dawn Vulkan device");
        }
        if (!handles.sharedTextureMemoryAHardwareBuffer || !handles.sharedFenceSyncFd) {
            return Fail("Aurora's Dawn device lacks AHardwareBuffer shared memory or sync-fd fences");
        }
        aurora_format_ = static_cast<VkFormat>(handles.colorVkFormat);
        if (CopyFamily(aurora_format_) == 2) {
            return Fail("Aurora selected a BGRA colour format, which Android cannot share through AHardwareBuffer");
        }
        if (CopyFamily(aurora_format_) == 0) {
            return Fail("Aurora's colour format cannot be copied into an OpenXR swapchain");
        }

        if (!CreateVulkanObjects()) {
            DestroyVulkanObjects();
            return false;
        }

        XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        binding.instance = vk_instance_;
        binding.physicalDevice = vk_physical_;
        binding.device = vk_device_;
        binding.queueFamilyIndex = queue_family_;
        binding.queueIndex = 0;
        if (!runtime.CreateSession(&binding)) {
            DestroyVulkanObjects();
            return Fail("OpenXR rejected the Vulkan device binding");
        }
        owns_session_ = true;

        if (!SelectSwapchainFormat() || !CreateSwapchains() || !AllocateSlots()) {
            DestroySwapchains();
            DestroySlots();
            runtime.DestroySession();
            owns_session_ = false;
            DestroyVulkanObjects();
            return false;
        }
        if (runtime.ShouldExit()) {
            DestroySwapchains();
            DestroySlots();
            runtime.DestroySession();
            owns_session_ = false;
            DestroyVulkanObjects();
            return Fail("OpenXR session became loss-pending while creating Vulkan swapchains");
        }
        if (!aurora_vulkan_enable_stereo_bridge(&Impl::OnAuroraSubmitted, this)) {
            DestroySwapchains();
            DestroySlots();
            runtime.DestroySession();
            owns_session_ = false;
            DestroyVulkanObjects();
            return Fail("Aurora could not enable its AHardwareBuffer stereo bridge");
        }
        bridge_enabled_ = true;
        bound_ = true;

        std::ostringstream message;
        message << "OpenXR Vulkan swapchains ready: VkFormat "
                << static_cast<int64_t>(swapchain_format_) << ", eyes "
                << eye_swapchains_[0].width << 'x' << eye_swapchains_[0].height << " / "
                << eye_swapchains_[1].width << 'x' << eye_swapchains_[1].height
                << ", shared buffers " << kSlotCount << " per eye";
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    OpenXRBeginStatus BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame) {
        frame = {};
        frame.presentation = presentation;
        if (!bound_ || runtime_ == nullptr) {
            Fail("BeginFrame called before the Vulkan backend was bound");
            return OpenXRBeginStatus::Error;
        }
        if (frame_active_) {
            Fail("BeginFrame called while another OpenXR frame is active");
            return OpenXRBeginStatus::Error;
        }

        const OpenXRFrameStatus status = runtime_->WaitFrame(frame.xr_frame);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(BeginStatusOperation(status));
            }
            switch (status) {
            case OpenXRFrameStatus::SessionNotRunning:
                return OpenXRBeginStatus::SessionNotRunning;
            case OpenXRFrameStatus::ExitRequested:
                return OpenXRBeginStatus::ExitRequested;
            case OpenXRFrameStatus::Error:
                return OpenXRBeginStatus::Error;
            case OpenXRFrameStatus::Ready:
                break;
            }
        }
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

        const uint32_t target_count =
            presentation.mode == OpenXRFrameMode::VirtualScreen ? 1u : kOpenXREyeCount;
        if (target_count == 1) {
            frame.render_width[1] = frame.render_width[0];
            frame.render_height[1] = frame.render_height[0];
        }

        // The settings panel's layer image, rendered with the eyes while it is open.
        const bool panel = frame.presentation.panel.requested && EnsurePanelResources();
        frame.presentation.panel.requested = panel;
        const diagnostics::Stopwatch acquire_timer;
        for (uint32_t eye = 0; eye < target_count; ++eye) {
            if (!AcquireSwapchain(eye_swapchains_[eye])) {
                ReleaseAcquiredSwapchains();
                EndActiveFrameWithoutLayers(frame.xr_frame);
                return OpenXRBeginStatus::Error;
            }
        }
        if (panel && !AcquireSwapchain(panel_swapchain_)) {
            ReleaseAcquiredSwapchains();
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRBeginStatus::Error;
        }
        diagnostics::OnSwapchainAcquire(acquire_timer);

        std::array<AuroraVulkanStereoTarget, kOpenXREyeCount> targets{};
        AuroraVulkanStereoTarget panel_target{};
        const uint32_t slot = next_slot_;
        {
            std::lock_guard lock(vk_mutex_);
            for (uint32_t eye = 0; eye < target_count; ++eye) {
                targets[eye] = SlotTargetLocked(slots_[eye][slot]);
            }
            if (panel) {
                panel_target = SlotTargetLocked(panel_slots_[slot]);
            }
            pending_copy_ = {frame.xr_frame.serial, slot, target_count, panel, {}};
            for (uint32_t eye = 0; eye < target_count; ++eye) {
                pending_copy_.swapchain_images[eye] =
                    eye_swapchains_[eye].images[eye_swapchains_[eye].acquired_index].image;
            }
            if (panel) {
                pending_copy_.swapchain_images[target_count] =
                    panel_swapchain_.images[panel_swapchain_.acquired_index].image;
            }
        }
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = frame.xr_frame.serial;
            submitted_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
        }
        if (!aurora_vulkan_set_stereo_targets_with_panel(frame.xr_frame.serial, targets.data(), target_count,
                                                         panel ? &panel_target : nullptr)) {
            // The duplicates were not taken; the slots keep their own descriptors.
            for (uint32_t eye = 0; eye < target_count; ++eye) {
                CloseFd(targets[eye].acquireFenceFd);
            }
            if (panel) {
                CloseFd(panel_target.acquireFenceFd);
            }
            {
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = 0;
            }
            ReleaseAcquiredSwapchains();
            Fail("Aurora rejected the AHardwareBuffer stereo targets");
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRBeginStatus::Error;
        }
        next_slot_ = (slot + 1) % kSlotCount;
        frame.expects_gpu_submission = true;
        return OpenXRBeginStatus::Ready;
    }

    OpenXRSubmissionStatus WaitForSubmission(const OpenXRBackendFrame& frame, uint32_t timeout_ms) {
        if (!frame.expects_gpu_submission) {
            return OpenXRSubmissionStatus::Success;
        }
        std::unique_lock lock(submission_mutex_);
        const auto ready = [&] {
            return shutting_down_ ||
                   (submission_arrived_ && submitted_token_ == frame.xr_frame.serial);
        };
        if (timeout_ms == std::numeric_limits<uint32_t>::max()) {
            submission_cv_.wait(lock, ready);
        } else if (!submission_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
            return OpenXRSubmissionStatus::Timeout;
        }
        if (shutting_down_) {
            return OpenXRSubmissionStatus::ShuttingDown;
        }
        if (submission_success_) {
            return OpenXRSubmissionStatus::Success;
        }
        return submission_unsafe_ ? OpenXRSubmissionStatus::Failed : OpenXRSubmissionStatus::Skipped;
    }

    bool TryCancelPendingFrame(OpenXRBackendFrame& frame) {
        if (!frame_active_ || !frame.expects_gpu_submission ||
            frame.xr_frame.serial != active_frame_serial_) {
            return false;
        }
        if (!aurora_vulkan_cancel_stereo_targets(frame.xr_frame.serial)) {
            return false;
        }
        std::lock_guard lock(submission_mutex_);
        awaiting_token_ = 0;
        submitted_token_ = 0;
        submission_arrived_ = false;
        submission_success_ = false;
        submission_unsafe_ = false;
        frame.expects_gpu_submission = false;
        return true;
    }

    // ---- Render-first pacing (see openxr_vulkan.h) ---------------------------------------------

    void DiscardPendingReleasesLocked() noexcept {
        if (!have_pending_releases_) {
            return;
        }
        for (auto& release : pending_releases_) {
            CloseFd(release.releaseFenceFd);
            release = {-1, VK_IMAGE_LAYOUT_UNDEFINED};
        }
        have_pending_releases_ = false;
    }

    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
        packet = {};
        packet.presentation = presentation;
        if (!bound_ || runtime_ == nullptr) {
            Fail("PreparePacket called before the Vulkan backend was bound");
            return OpenXRBeginStatus::Error;
        }
        if (frame_active_) {
            Fail("PreparePacket called while an OpenXR frame is active");
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
        if (last_display_period_ <= 0) {
            // No display timing yet: one compositor cycle learns it.
            const OpenXRBeginStatus primed = KeepAliveCycle();
            if (primed != OpenXRBeginStatus::Ready) {
                return primed;
            }
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

        const uint32_t target_count =
            presentation.mode == OpenXRFrameMode::VirtualScreen ? 1u : kOpenXREyeCount;
        if (target_count == 1) {
            packet.render_width[1] = packet.render_width[0];
            packet.render_height[1] = packet.render_height[0];
        }
        // The settings panel's layer image, rendered with the eyes while it is open.
        const bool panel = packet.presentation.panel.requested && EnsurePanelResources();
        packet.presentation.panel.requested = panel;
        std::array<AuroraVulkanStereoTarget, kOpenXREyeCount> targets{};
        AuroraVulkanStereoTarget panel_target{};
        const uint32_t slot = next_slot_;
        {
            std::lock_guard lock(vk_mutex_);
            DiscardPendingReleasesLocked();
            for (uint32_t eye = 0; eye < target_count; ++eye) {
                targets[eye] = SlotTargetLocked(slots_[eye][slot]);
            }
            if (panel) {
                panel_target = SlotTargetLocked(panel_slots_[slot]);
            }
            // The compositor images are acquired by BeginFrameForPacket, once the eyes exist.
            pending_copy_ = {packet.xr_frame.serial, slot, target_count, panel, {}};
            deferred_copy_ = true;
        }
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = packet.xr_frame.serial;
            submitted_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
        }
        if (!aurora_vulkan_set_stereo_targets_with_panel(packet.xr_frame.serial, targets.data(), target_count,
                                                         panel ? &panel_target : nullptr)) {
            for (uint32_t eye = 0; eye < target_count; ++eye) {
                CloseFd(targets[eye].acquireFenceFd);
            }
            if (panel) {
                CloseFd(panel_target.acquireFenceFd);
            }
            {
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = 0;
            }
            {
                std::lock_guard lock(vk_mutex_);
                deferred_copy_ = false;
            }
            Fail("Aurora rejected the AHardwareBuffer stereo targets");
            return OpenXRBeginStatus::Error;
        }
        next_slot_ = (slot + 1) % kSlotCount;
        packet.expects_gpu_submission = true;
        return OpenXRBeginStatus::Ready;
    }

    bool TryCancelPendingPacket(OpenXRBackendFrame& packet) {
        if (!packet.expects_gpu_submission || !aurora_vulkan_cancel_stereo_targets(packet.xr_frame.serial)) {
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
        {
            std::lock_guard lock(vk_mutex_);
            DiscardPendingReleasesLocked();
            deferred_copy_ = false;
        }
        packet.expects_gpu_submission = false;
        return true;
    }

    bool FinishFrame(OpenXRBackendFrame& frame, bool submit_layer) {
        if (!frame_active_ || runtime_ == nullptr ||
            frame.xr_frame.serial != active_frame_serial_) {
            return Fail("FinishFrame received a stale or inactive OpenXR frame token");
        }

        bool submission_unsafe = false;
        {
            std::lock_guard lock(submission_mutex_);
            submission_unsafe = submission_arrived_ &&
                                submitted_token_ == frame.xr_frame.serial &&
                                submission_unsafe_;
        }
        if (submission_unsafe) {
            AbandonAcquiredSwapchains();
            Fail("Aurora's stereo submission failed after GPU work may have been queued");
        }
        const diagnostics::Stopwatch release_timer;
        bool release_ok = ReleaseAcquiredSwapchains();
        if (frame.xr_frame.should_render && frame.xr_frame.views_valid) {
            diagnostics::OnSwapchainRelease(release_timer);
        }
        const bool position_valid =
            (frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        const bool composition_pose_valid =
            frame.presentation.mode == OpenXRFrameMode::VirtualScreen || position_valid;
        const bool can_submit = submit_layer && release_ok && frame.xr_frame.should_render &&
                                frame.xr_frame.views_valid && frame.expects_gpu_submission &&
                                composition_pose_valid;
        if (submit_layer && !can_submit) {
            diagnostics::OnLayerRejected(diagnostics::ClassifyRejectedLayer(
                release_ok, frame.xr_frame.should_render, frame.xr_frame.views_valid));
        }
        if (can_submit) {
            // xrEndFrame references the most recently released image of a
            // swapchain, so keep the displayed pair separate from the pair
            // Aurora may write next.
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
            // Eyes rendered for a packet that this frame did not copy are dropped with it.
            std::lock_guard lock(vk_mutex_);
            DiscardPendingReleasesLocked();
            deferred_copy_ = false;
        }
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
        }
        return release_ok && end_ok;
    }

    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
        frame = {};
        frame.presentation = packet.presentation;
        frame.render_width = packet.render_width;
        frame.render_height = packet.render_height;
        if (!bound_ || runtime_ == nullptr) {
            Fail("BeginFrameForPacket called before the Vulkan backend was bound");
            return OpenXRBeginStatus::Error;
        }
        if (frame_active_) {
            Fail("BeginFrameForPacket called while another OpenXR frame is active");
            return OpenXRBeginStatus::Error;
        }
        const OpenXRFrameStatus status = runtime_->WaitFrame(frame.xr_frame);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(BeginStatusOperation(status));
            }
            return status == OpenXRFrameStatus::SessionNotRunning ? OpenXRBeginStatus::SessionNotRunning
                   : status == OpenXRFrameStatus::ExitRequested  ? OpenXRBeginStatus::ExitRequested
                                                                  : OpenXRBeginStatus::Error;
        }
        NoteDisplayTiming(frame.xr_frame);
        if (!runtime_->BeginFrame(frame.xr_frame)) {
            Fail("xrBeginFrame failed");
            return OpenXRBeginStatus::Error;
        }
        frame_active_ = true;
        active_frame_serial_ = frame.xr_frame.serial;
        render_session_serial_ = runtime_->SessionRunSerial();
        render_space_serial_ = runtime_->LastReferenceSpaceChange().serial;
        // The layer shows the eyes as they were rendered: it carries the packet's located views.
        frame.xr_frame.views = packet.xr_frame.views;
        frame.xr_frame.view_state_flags = packet.xr_frame.view_state_flags;
        frame.xr_frame.views_valid = packet.xr_frame.views_valid;
        active_frame_ = frame.xr_frame;
        frame.expects_gpu_submission = packet.expects_gpu_submission;
        if (!frame.xr_frame.should_render || !frame.expects_gpu_submission || !frame.xr_frame.views_valid) {
            // No swapchain image is acquired for this frame, so the rendered eyes cannot be
            // copied; FinishFrame drops them with the frame.
            frame.expects_gpu_submission = false;
            return OpenXRBeginStatus::Ready;
        }

        const uint32_t target_count =
            presentation_target_count(frame.presentation);
        const bool panel = frame.presentation.panel.requested;
        const diagnostics::Stopwatch acquire_timer;
        for (uint32_t eye = 0; eye < target_count; ++eye) {
            if (!AcquireSwapchain(eye_swapchains_[eye])) {
                ReleaseAcquiredSwapchains();
                EndActiveFrameWithoutLayers(frame.xr_frame);
                return OpenXRBeginStatus::Error;
            }
        }
        if (panel && !AcquireSwapchain(panel_swapchain_)) {
            ReleaseAcquiredSwapchains();
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRBeginStatus::Error;
        }
        diagnostics::OnSwapchainAcquire(acquire_timer);
        {
            std::lock_guard lock(vk_mutex_);
            for (uint32_t eye = 0; eye < target_count; ++eye) {
                pending_copy_.swapchain_images[eye] =
                    eye_swapchains_[eye].images[eye_swapchains_[eye].acquired_index].image;
            }
            if (panel) {
                pending_copy_.swapchain_images[target_count] =
                    panel_swapchain_.images[panel_swapchain_.acquired_index].image;
            }
        }
        return OpenXRBeginStatus::Ready;
    }

    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame) {
        CopyOutcome outcome = CopyOutcome::Skipped;
        {
            std::lock_guard lock(vk_mutex_);
            if (!frame_active_ || !have_pending_releases_ || !deferred_copy_) {
                return OpenXRSubmissionStatus::Skipped;
            }
            outcome = RecordAndSubmitCopyLocked(pending_releases_);
            have_pending_releases_ = false;
            deferred_copy_ = false;
        }
        {
            // FinishFrame judges the queue's safety by this frame's token.
            std::lock_guard lock(submission_mutex_);
            submitted_token_ = frame.xr_frame.serial;
            submission_arrived_ = true;
            submission_success_ = outcome == CopyOutcome::Submitted;
            submission_unsafe_ = outcome == CopyOutcome::Unsafe;
        }
        return outcome == CopyOutcome::Submitted ? OpenXRSubmissionStatus::Success
               : outcome == CopyOutcome::Unsafe  ? OpenXRSubmissionStatus::Failed
                                                 : OpenXRSubmissionStatus::Skipped;
    }

    OpenXRBeginStatus KeepAliveCycle() {
        if (!bound_ || runtime_ == nullptr || frame_active_) {
            Fail("KeepAliveCycle called with a frame active or before binding");
            return OpenXRBeginStatus::Error;
        }
        OpenXRFrame cycle{};
        const OpenXRFrameStatus status = runtime_->WaitFrame(cycle);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(BeginStatusOperation(status));
            }
            return status == OpenXRFrameStatus::SessionNotRunning ? OpenXRBeginStatus::SessionNotRunning
                   : status == OpenXRFrameStatus::ExitRequested  ? OpenXRBeginStatus::ExitRequested
                                                                  : OpenXRBeginStatus::Error;
        }
        NoteDisplayTiming(cycle);
        if (!runtime_->BeginFrame(cycle)) {
            Fail("xrBeginFrame failed for a keep-alive cycle");
            return OpenXRBeginStatus::Error;
        }
        active_frame_ = cycle;
        frame_active_ = true;
        const bool end_ok = EndRetainedFrame(false);
        frame_active_ = false;
        active_frame_ = {};
        if (!end_ok) {
            Fail("OpenXR could not resubmit the retained frame");
            return OpenXRBeginStatus::Error;
        }
        return OpenXRBeginStatus::Ready;
    }

    void NoteDisplayTiming(const OpenXRFrame& frame) noexcept {
        last_display_time_ = frame.predicted_display_time;
        last_display_period_ = frame.predicted_display_period;
        last_should_render_ = frame.should_render;
    }

    static uint32_t presentation_target_count(const OpenXRPresentation& presentation) noexcept {
        return presentation.mode == OpenXRFrameMode::VirtualScreen ? 1u : kOpenXREyeCount;
    }

    bool RepeatFrame(const OpenXRBackendFrame& frame) {
        if (!frame_active_ || runtime_ == nullptr ||
            frame.xr_frame.serial != active_frame_serial_) {
            return Fail("RepeatFrame received a stale or inactive render token");
        }
        const bool end_ok = EndRetainedFrame(false);
        frame_active_ = false;
        if (!end_ok) {
            return Fail("OpenXR could not resubmit the retained frame");
        }
        if (runtime_->PollEvents() != OpenXREventStatus::Continue ||
            !runtime_->IsSessionRunning() || runtime_->ShouldExit()) {
            return Fail("OpenXR session stopped while waiting for stereo rendering");
        }
        if (runtime_->WaitFrame(active_frame_) != OpenXRFrameStatus::Ready ||
            !runtime_->BeginFrame(active_frame_)) {
            return Fail("OpenXR could not start a retained-frame compositor cycle");
        }
        frame_active_ = true;
        return true;
    }

    // fresh: the retained layer was completed for this call rather than repeated.
    bool EndRetainedFrame(bool fresh) {
        if (!runtime_->IsSessionRunning()) {
            return true;
        }
        const bool session_changed = retained_session_serial_ != runtime_->SessionRunSerial();
        if (session_changed || retained_space_serial_ != runtime_->LastReferenceSpaceChange().serial) {
            if (have_retained_frame_) {
                diagnostics::OnRetainedLayerDiscarded(session_changed
                                                          ? diagnostics::DiscardReason::SessionRestarted
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
            quad.subImage.imageRect = OpenXRVirtualScreenContentRect(
                image_width, retained_swapchains_[0].height, frame.presentation.quad_content_aspect);
            quad.subImage.imageArrayIndex = 0;
            if (frame.presentation.quad_anchored) {
                quad.space = runtime_->AppSpace();
                quad.pose = frame.presentation.quad_pose;
            } else {
                quad.space = runtime_->ViewSpace();
                quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                quad.pose.position = {
                    0.0f, 0.0f, -std::max(0.25f, frame.presentation.quad_distance_meters)};
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

    // Ends the compositor frame with the scene's layer: over the room's camera
    // view while that runs and the scene is the virtual screen or the immersive
    // window (never under a fully immersive race's projection, which covers the
    // whole view), and, while the retained frame rendered it, under the settings
    // panel's layer.
    bool EndFrameWithPanel(const OpenXRBackendFrame& frame, const XrCompositionLayerBaseHeader* scene) {
        const auto& panel = frame.presentation.panel;
        XrCompositionLayerQuad panel_quad{};
        const XrCompositionLayerBaseHeader* layers[3] = {};
        uint32_t count = 0;
        if (const XrCompositionLayerBaseHeader* passthrough = passthrough_.Layer();
            passthrough != nullptr && (frame.presentation.mode == OpenXRFrameMode::VirtualScreen ||
                                       frame.presentation.immersive_window)) {
            layers[count++] = passthrough;
        }
        layers[count++] = scene;
        if (retained_panel_valid_ && panel.requested && panel.placed) {
            panel_quad = OpenXRPanelQuadLayer(panel, runtime_->AppSpace(), retained_panel_swapchain_.handle);
            layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel_quad);
        }
        return runtime_->EndFrame(active_frame_, layers, count);
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
            bridge_drained = aurora_vulkan_disable_stereo_bridge();
            bridge_enabled_ = false;
        }
        bool device_idle = true;
        if (vk_device_ != VK_NULL_HANDLE) {
            std::lock_guard lock(vk_mutex_);
            device_idle = vkDeviceWaitIdle(vk_device_) == VK_SUCCESS;
        }
        if (!bridge_drained || !device_idle) {
            AbandonAcquiredSwapchains();
            shutdown_unsafe_ = true;
            Fail("Vulkan queue completion is unknown; retaining the OpenXR session and graphics owners");
            return false;
        }

        AllowAcquiredSwapchainsAfterGpuDrain();
        ReleaseAcquiredSwapchains();
        if (frame_active_ && runtime_ != nullptr) {
            if (runtime_->IsSessionRunning()) {
                runtime_->EndFrameWithoutLayers(active_frame_);
            }
            frame_active_ = false;
            active_frame_serial_ = 0;
            active_frame_ = {};
        }
        DestroySwapchains();
        DestroySlots();
        // Its handles belong to the session.
        passthrough_.Destroy();
        if (owns_session_ && runtime_ != nullptr) {
            runtime_->DestroySession();
            owns_session_ = false;
        }
        DestroyVulkanObjects();
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
    // ---- Vulkan device owned by the OpenXR side ------------------------------

    bool CreateVulkanObjects() {
        // Vulkan 1.1 brings external memory/semaphores and dedicated allocation
        // into core; every Quest ships at least that. XR_KHR_vulkan_enable
        // runtimes have been seen reporting max 1.0 while accepting 1.1.
        uint32_t api_version = VK_API_VERSION_1_1;
        if (requirements_.max_api_version != 0 &&
            XR_VERSION_MAJOR(requirements_.max_api_version) == 1 &&
            XR_VERSION_MINOR(requirements_.max_api_version) == 0 && requirements_.uses_enable2) {
            api_version = VK_API_VERSION_1_1;
        }

        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "WiiCompiled";
        application.applicationVersion = 1;
        application.pEngineName = "Aurora OpenXR bridge";
        application.engineVersion = 1;
        application.apiVersion = api_version;

        std::vector<std::string> instance_extension_storage;
        std::vector<const char*> instance_extensions;
        if (!requirements_.uses_enable2) {
            PFN_xrGetVulkanInstanceExtensionsKHR get_instance_extensions = nullptr;
            if (!runtime_->LoadFunction("xrGetVulkanInstanceExtensionsKHR", &get_instance_extensions) ||
                get_instance_extensions == nullptr) {
                return Fail("xrGetVulkanInstanceExtensionsKHR is unavailable");
            }
            uint32_t length = 0;
            get_instance_extensions(runtime_->Instance(), runtime_->SystemId(), 0, &length, nullptr);
            std::string text(length, '\0');
            if (length != 0) {
                get_instance_extensions(runtime_->Instance(), runtime_->SystemId(), length, &length, text.data());
            }
            instance_extension_storage = SplitExtensionList(text);
        }
        for (const std::string& name : instance_extension_storage) {
            instance_extensions.push_back(name.c_str());
        }

        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.pApplicationInfo = &application;
        instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size());
        instance_info.ppEnabledExtensionNames = instance_extensions.data();

        if (requirements_.uses_enable2) {
            PFN_xrCreateVulkanInstanceKHR create_instance = nullptr;
            if (!runtime_->LoadFunction("xrCreateVulkanInstanceKHR", &create_instance) ||
                create_instance == nullptr) {
                return Fail("xrCreateVulkanInstanceKHR is unavailable");
            }
            XrVulkanInstanceCreateInfoKHR info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
            info.systemId = runtime_->SystemId();
            info.createFlags = 0;
            info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
            info.vulkanCreateInfo = &instance_info;
            info.vulkanAllocator = nullptr;
            VkResult vk_result = VK_SUCCESS;
            const XrResult result = create_instance(runtime_->Instance(), &info, &vk_instance_, &vk_result);
            ObserveResult(result);
            if (XR_FAILED(result) || vk_result != VK_SUCCESS || vk_instance_ == VK_NULL_HANDLE) {
                std::ostringstream message;
                message << "xrCreateVulkanInstanceKHR failed (" << result << ", VkResult " << vk_result << ')';
                return Fail(message.str());
            }
        } else {
            const VkResult vk_result = vkCreateInstance(&instance_info, nullptr, &vk_instance_);
            if (vk_result != VK_SUCCESS || vk_instance_ == VK_NULL_HANDLE) {
                std::ostringstream message;
                message << "vkCreateInstance failed (VkResult " << vk_result << ')';
                return Fail(message.str());
            }
        }

        if (requirements_.uses_enable2) {
            PFN_xrGetVulkanGraphicsDevice2KHR get_device = nullptr;
            if (!runtime_->LoadFunction("xrGetVulkanGraphicsDevice2KHR", &get_device) || get_device == nullptr) {
                return Fail("xrGetVulkanGraphicsDevice2KHR is unavailable");
            }
            XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
            info.systemId = runtime_->SystemId();
            info.vulkanInstance = vk_instance_;
            const XrResult result = get_device(runtime_->Instance(), &info, &vk_physical_);
            ObserveResult(result);
            if (XR_FAILED(result) || vk_physical_ == VK_NULL_HANDLE) {
                return Fail("xrGetVulkanGraphicsDevice2KHR failed");
            }
        } else {
            PFN_xrGetVulkanGraphicsDeviceKHR get_device = nullptr;
            if (!runtime_->LoadFunction("xrGetVulkanGraphicsDeviceKHR", &get_device) || get_device == nullptr) {
                return Fail("xrGetVulkanGraphicsDeviceKHR is unavailable");
            }
            const XrResult result =
                get_device(runtime_->Instance(), runtime_->SystemId(), vk_instance_, &vk_physical_);
            ObserveResult(result);
            if (XR_FAILED(result) || vk_physical_ == VK_NULL_HANDLE) {
                return Fail("xrGetVulkanGraphicsDeviceKHR failed");
            }
        }

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_, &family_count, families.data());
        queue_family_ = UINT32_MAX;
        for (uint32_t index = 0; index < family_count; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[index].queueCount > 0) {
                queue_family_ = index;
                break;
            }
        }
        if (queue_family_ == UINT32_MAX) {
            return Fail("the OpenXR physical device exposes no graphics queue family");
        }

        // Extensions this backend needs on top of whatever the runtime adds.
        // Everything promoted to core in 1.1 is still requested by name when
        // the driver lists it, which keeps a 1.0-only report working too.
        uint32_t available_count = 0;
        vkEnumerateDeviceExtensionProperties(vk_physical_, nullptr, &available_count, nullptr);
        std::vector<VkExtensionProperties> available(available_count);
        vkEnumerateDeviceExtensionProperties(vk_physical_, nullptr, &available_count, available.data());
        const auto has_extension = [&](const char* name) {
            return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) {
                return std::strcmp(e.extensionName, name) == 0;
            });
        };
        const std::array<const char*, 2> mandatory{
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        };
        const std::array<const char*, 8> desirable{
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
            VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
            VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
            VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
            VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        };
        std::vector<std::string> device_extension_storage;
        for (const char* name : mandatory) {
            if (!has_extension(name)) {
                return Fail(std::string("the OpenXR physical device lacks ") + name);
            }
            device_extension_storage.emplace_back(name);
        }
        for (const char* name : desirable) {
            if (has_extension(name)) {
                device_extension_storage.emplace_back(name);
            }
        }
        if (!requirements_.uses_enable2) {
            PFN_xrGetVulkanDeviceExtensionsKHR get_device_extensions = nullptr;
            if (!runtime_->LoadFunction("xrGetVulkanDeviceExtensionsKHR", &get_device_extensions) ||
                get_device_extensions == nullptr) {
                return Fail("xrGetVulkanDeviceExtensionsKHR is unavailable");
            }
            uint32_t length = 0;
            get_device_extensions(runtime_->Instance(), runtime_->SystemId(), 0, &length, nullptr);
            std::string text(length, '\0');
            if (length != 0) {
                get_device_extensions(runtime_->Instance(), runtime_->SystemId(), length, &length, text.data());
            }
            for (const std::string& name : SplitExtensionList(text)) {
                if (std::find(device_extension_storage.begin(), device_extension_storage.end(), name) ==
                    device_extension_storage.end()) {
                    device_extension_storage.push_back(name);
                }
            }
        }
        std::vector<const char*> device_extensions;
        for (const std::string& name : device_extension_storage) {
            device_extensions.push_back(name.c_str());
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.data();
        device_info.pEnabledFeatures = &features;

        if (requirements_.uses_enable2) {
            PFN_xrCreateVulkanDeviceKHR create_device = nullptr;
            if (!runtime_->LoadFunction("xrCreateVulkanDeviceKHR", &create_device) || create_device == nullptr) {
                return Fail("xrCreateVulkanDeviceKHR is unavailable");
            }
            XrVulkanDeviceCreateInfoKHR info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
            info.systemId = runtime_->SystemId();
            info.createFlags = 0;
            info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
            info.vulkanPhysicalDevice = vk_physical_;
            info.vulkanCreateInfo = &device_info;
            info.vulkanAllocator = nullptr;
            VkResult vk_result = VK_SUCCESS;
            const XrResult result = create_device(runtime_->Instance(), &info, &vk_device_, &vk_result);
            ObserveResult(result);
            if (XR_FAILED(result) || vk_result != VK_SUCCESS || vk_device_ == VK_NULL_HANDLE) {
                std::ostringstream message;
                message << "xrCreateVulkanDeviceKHR failed (" << result << ", VkResult " << vk_result << ')';
                return Fail(message.str());
            }
        } else {
            const VkResult vk_result = vkCreateDevice(vk_physical_, &device_info, nullptr, &vk_device_);
            if (vk_result != VK_SUCCESS || vk_device_ == VK_NULL_HANDLE) {
                std::ostringstream message;
                message << "vkCreateDevice failed (VkResult " << vk_result << ')';
                return Fail(message.str());
            }
        }
        vkGetDeviceQueue(vk_device_, queue_family_, 0, &vk_queue_);

        pfn_import_semaphore_fd_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            vkGetDeviceProcAddr(vk_device_, "vkImportSemaphoreFdKHR"));
        pfn_get_semaphore_fd_ = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            vkGetDeviceProcAddr(vk_device_, "vkGetSemaphoreFdKHR"));
        pfn_get_ahb_properties_ = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            vkGetDeviceProcAddr(vk_device_, "vkGetAndroidHardwareBufferPropertiesANDROID"));
        if (pfn_import_semaphore_fd_ == nullptr || pfn_get_semaphore_fd_ == nullptr ||
            pfn_get_ahb_properties_ == nullptr) {
            return Fail("the OpenXR Vulkan device did not resolve the sync-fd or AHardwareBuffer entry points");
        }

        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family_;
        if (vkCreateCommandPool(vk_device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
            return Fail("vkCreateCommandPool failed");
        }
        for (Submission& submission : submissions_) {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = command_pool_;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vkAllocateCommandBuffers(vk_device_, &allocate, &submission.command_buffer) != VK_SUCCESS ||
                vkCreateFence(vk_device_, &fence_info, nullptr, &submission.fence) != VK_SUCCESS) {
                return Fail("could not allocate the OpenXR copy command buffers");
            }
            VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            for (VkSemaphore& semaphore : submission.wait_semaphores) {
                if (vkCreateSemaphore(vk_device_, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
                    return Fail("vkCreateSemaphore failed for a copy wait semaphore");
                }
            }
        }
        return true;
    }

    void DestroyVulkanObjects() {
        std::lock_guard lock(vk_mutex_);
        if (vk_device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vk_device_);
            for (Submission& submission : submissions_) {
                if (submission.fence != VK_NULL_HANDLE) {
                    vkDestroyFence(vk_device_, submission.fence, nullptr);
                }
                for (VkSemaphore semaphore : submission.wait_semaphores) {
                    if (semaphore != VK_NULL_HANDLE) {
                        vkDestroySemaphore(vk_device_, semaphore, nullptr);
                    }
                }
                submission = {};
            }
            if (command_pool_ != VK_NULL_HANDLE) {
                vkDestroyCommandPool(vk_device_, command_pool_, nullptr);
                command_pool_ = VK_NULL_HANDLE;
            }
            vkDestroyDevice(vk_device_, nullptr);
            vk_device_ = VK_NULL_HANDLE;
        }
        if (vk_instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(vk_instance_, nullptr);
            vk_instance_ = VK_NULL_HANDLE;
        }
        vk_physical_ = VK_NULL_HANDLE;
        vk_queue_ = VK_NULL_HANDLE;
        queue_family_ = UINT32_MAX;
        pfn_import_semaphore_fd_ = nullptr;
        pfn_get_semaphore_fd_ = nullptr;
        pfn_get_ahb_properties_ = nullptr;
    }

    // ---- Shared eye buffers ---------------------------------------------------

    bool AllocateSlots() {
        std::lock_guard lock(vk_mutex_);
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
                if (!AllocateSlot(slots_[eye][slot], eye_swapchains_[eye].width, eye_swapchains_[eye].height)) {
                    return false;
                }
            }
        }
        return true;
    }

    // Aurora's view of a shared buffer: it waits on the slot's last copy-out.
    AuroraVulkanStereoTarget SlotTargetLocked(const EyeSlot& slot) const noexcept {
        return {
            slot.buffer,
            slot.width,
            slot.height,
            static_cast<int64_t>(aurora_format_),
            DupFd(slot.pending_acquire_fd),
            static_cast<int32_t>(slot.layout),
        };
    }

    // The settings panel's swapchain pair and shared buffers, made the first
    // time it opens and kept for the session.
    bool EnsurePanelResources() {
        if (!EnsurePanelSwapchains()) {
            return false;
        }
        std::lock_guard lock(vk_mutex_);
        if (panel_slots_ready_) {
            return true;
        }
        for (EyeSlot& slot : panel_slots_) {
            if (!AllocateSlot(slot, kOpenXRPanelLayerWidth, kOpenXRPanelLayerHeight)) {
                for (EyeSlot& allocated : panel_slots_) {
                    DestroySlotLocked(allocated);
                }
                DestroyPanelSwapchains();
                panel_layer_failed_ = true;
                Log(OpenXRLogLevel::Warning,
                    "the settings panel's shared buffers could not be allocated; drawing it into the eyes");
                return false;
            }
        }
        panel_slots_ready_ = true;
        return true;
    }

    bool AllocateSlot(EyeSlot& slot, uint32_t width, uint32_t height) {
        AHardwareBuffer_Desc desc{};
        desc.width = width;
        desc.height = height;
        desc.layers = 1;
        desc.format = CopyFamily(aurora_format_) == 3 ? AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT
                                                       : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        // SAMPLED lets Dawn read (copy source) and FRAMEBUFFER lets it write
        // (copy destination / render attachment); both sides transfer.
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;
        if (AHardwareBuffer_allocate(&desc, &slot.buffer) != 0 || slot.buffer == nullptr) {
            return Fail("AHardwareBuffer_allocate failed for an eye buffer");
        }
        slot.width = width;
        slot.height = height;

        VkAndroidHardwareBufferFormatPropertiesANDROID format_properties{
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
        VkAndroidHardwareBufferPropertiesANDROID properties{
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        properties.pNext = &format_properties;
        if (pfn_get_ahb_properties_(vk_device_, slot.buffer, &properties) != VK_SUCCESS) {
            return Fail("vkGetAndroidHardwareBufferPropertiesANDROID failed");
        }
        if (format_properties.format == VK_FORMAT_UNDEFINED ||
            !SameCopyFamily(format_properties.format, aurora_format_)) {
            return Fail("the AHardwareBuffer's Vulkan format does not match Aurora's colour format");
        }
        slot.format = format_properties.format;

        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.pNext = &external;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = slot.format;
        image_info.extent = {width, height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(vk_device_, &image_info, nullptr, &slot.image) != VK_SUCCESS) {
            return Fail("vkCreateImage failed for an imported eye buffer");
        }

        uint32_t memory_type = UINT32_MAX;
        for (uint32_t bit = 0; bit < 32; ++bit) {
            if ((properties.memoryTypeBits & (1u << bit)) != 0) {
                memory_type = bit;
                break;
            }
        }
        if (memory_type == UINT32_MAX) {
            return Fail("the AHardwareBuffer reports no usable memory type");
        }
        VkImportAndroidHardwareBufferInfoANDROID import_info{
            VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
        import_info.buffer = slot.buffer;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &import_info;
        dedicated.image = slot.image;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &dedicated;
        allocate.allocationSize = properties.allocationSize;
        allocate.memoryTypeIndex = memory_type;
        if (vkAllocateMemory(vk_device_, &allocate, nullptr, &slot.memory) != VK_SUCCESS) {
            return Fail("vkAllocateMemory failed while importing an eye buffer");
        }
        if (vkBindImageMemory(vk_device_, slot.image, slot.memory, 0) != VK_SUCCESS) {
            return Fail("vkBindImageMemory failed for an imported eye buffer");
        }

        VkExportSemaphoreCreateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkSemaphoreCreateInfo exportable{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        exportable.pNext = &export_info;
        if (vkCreateSemaphore(vk_device_, &exportable, nullptr, &slot.signal_semaphore) != VK_SUCCESS) {
            return Fail("vkCreateSemaphore failed for the exportable eye semaphore");
        }
        slot.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        slot.pending_acquire_fd = -1;
        return true;
    }

    void DestroySlots() {
        std::lock_guard lock(vk_mutex_);
        DiscardPendingReleasesLocked();
        if (vk_device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vk_device_);
        }
        for (auto& eye : slots_) {
            for (EyeSlot& slot : eye) {
                DestroySlotLocked(slot);
            }
        }
        for (EyeSlot& slot : panel_slots_) {
            DestroySlotLocked(slot);
        }
        panel_slots_ready_ = false;
    }

    void DestroySlotLocked(EyeSlot& slot) noexcept {
        CloseFd(slot.pending_acquire_fd);
        if (vk_device_ != VK_NULL_HANDLE) {
            if (slot.signal_semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(vk_device_, slot.signal_semaphore, nullptr);
            }
            if (slot.image != VK_NULL_HANDLE) {
                vkDestroyImage(vk_device_, slot.image, nullptr);
            }
            if (slot.memory != VK_NULL_HANDLE) {
                vkFreeMemory(vk_device_, slot.memory, nullptr);
            }
        }
        if (slot.buffer != nullptr) {
            AHardwareBuffer_release(slot.buffer);
        }
        slot = {};
    }

    // ---- The copy into the compositor image -----------------------------------

    static void OnAuroraSubmitted(uint64_t token, bool success, bool gpu_work_queued,
                                  const AuroraVulkanStereoRelease* releases, uint32_t release_count,
                                  void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        std::array<AuroraVulkanStereoRelease, kMaxCopies> owned{};
        for (auto& release : owned) {
            release = {-1, VK_IMAGE_LAYOUT_UNDEFINED};
        }
        for (uint32_t i = 0; i < release_count && i < owned.size(); ++i) {
            owned[i] = releases[i];
        }
        if (self == nullptr) {
            for (auto& release : owned) {
                CloseFd(release.releaseFenceFd);
            }
            return;
        }

        CopyOutcome outcome = CopyOutcome::Skipped;
        {
            std::lock_guard lock(self->vk_mutex_);
            bool expected = false;
            {
                std::lock_guard submission_lock(self->submission_mutex_);
                expected = token == self->awaiting_token_ && token == self->pending_copy_.token;
            }
            if (expected && success && release_count >= self->pending_copy_.Count()) {
                if (self->deferred_copy_) {
                    // No compositor frame is open yet: keep Dawn's release fences for
                    // CopyRenderedEyes, which records the copy once the frame is begun.
                    self->DiscardPendingReleasesLocked();
                    self->pending_releases_ = owned;
                    self->have_pending_releases_ = true;
                    outcome = CopyOutcome::Submitted;
                } else {
                    outcome = self->RecordAndSubmitCopyLocked(owned);
                }
            } else {
                for (auto& release : owned) {
                    CloseFd(release.releaseFenceFd);
                }
                // Aurora failing after it queued GPU work may have written the shared buffer
                // with no completion marker to wait on; failing before that touched nothing.
                if (expected && !success && gpu_work_queued) {
                    outcome = CopyOutcome::Unsafe;
                }
            }
            if (!expected) {
                return;
            }
        }
        {
            std::lock_guard lock(self->submission_mutex_);
            if (token != self->awaiting_token_) {
                return;
            }
            self->submitted_token_ = token;
            self->submission_success_ = outcome == CopyOutcome::Submitted;
            self->submission_arrived_ = true;
            self->submission_unsafe_ = outcome == CopyOutcome::Unsafe;
        }
        self->submission_cv_.notify_all();
    }

    // The slot image `n` of a copy reads: an eye's, or after the eyes the panel's.
    EyeSlot& CopySlotLocked(const PendingCopy& copy, uint32_t n) noexcept {
        return n < copy.target_count ? slots_[n][copy.slot] : panel_slots_[copy.slot];
    }

    CopyOutcome RecordAndSubmitCopyLocked(std::array<AuroraVulkanStereoRelease, kMaxCopies>& releases) {
        const auto close_releases = [&releases] {
            for (auto& release : releases) {
                CloseFd(release.releaseFenceFd);
            }
        };
        CopyOutcome acquire_failure = CopyOutcome::Skipped;
        Submission* submission = AcquireSubmissionLocked(acquire_failure);
        if (submission == nullptr) {
            close_releases();
            return acquire_failure;
        }
        const PendingCopy copy = pending_copy_;
        VkCommandBuffer cmd = submission->command_buffer;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        const VkResult begun = vkBeginCommandBuffer(cmd, &begin);
        if (begun != VK_SUCCESS) {
            close_releases();
            Fail(VkFailure("vkBeginCommandBuffer failed for the eye copy", begun));
            return CopyOutcome::Skipped;
        }

        std::array<VkSemaphore, kMaxCopies> waits{};
        std::array<VkPipelineStageFlags, kMaxCopies> wait_stages{};
        std::array<VkSemaphore, kMaxCopies> signals{};
        uint32_t wait_count = 0;
        uint32_t signal_count = 0;
        constexpr VkImageSubresourceRange kColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        for (uint32_t n = 0; n < copy.Count(); ++n) {
            EyeSlot& slot = CopySlotLocked(copy, n);
            AuroraVulkanStereoRelease& release = releases[n];

            if (release.releaseFenceFd >= 0) {
                const VkSemaphore wait_semaphore = submission->wait_semaphores[n];
                VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
                import.semaphore = wait_semaphore;
                import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
                import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
                import.fd = release.releaseFenceFd;
                const VkResult imported = pfn_import_semaphore_fd_(vk_device_, &import);
                if (imported == VK_SUCCESS) {
                    // Ownership of the descriptor moved to Vulkan.
                    release.releaseFenceFd = -1;
                    waits[wait_count] = wait_semaphore;
                    wait_stages[wait_count] = VK_PIPELINE_STAGE_TRANSFER_BIT;
                    ++wait_count;
                } else {
                    close_releases();
                    vkEndCommandBuffer(cmd);
                    Fail(VkFailure("vkImportSemaphoreFdKHR rejected Dawn's release fence", imported));
                    return CopyOutcome::Skipped;
                }
            }

            // Dawn released the buffer with an ownership transfer whose old/new
            // layouts it reported; the acquire here must repeat that pair before
            // the image can be transitioned for reading.
            VkImageLayout released = static_cast<VkImageLayout>(release.releasedImageLayout);
            if (released == VK_IMAGE_LAYOUT_UNDEFINED) {
                released = VK_IMAGE_LAYOUT_GENERAL;
            }
            VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            acquire.srcAccessMask = 0;
            acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            acquire.oldLayout = released;
            acquire.newLayout = released;
            acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            acquire.dstQueueFamilyIndex = queue_family_;
            acquire.image = slot.image;
            acquire.subresourceRange = kColorRange;
            VkImageMemoryBarrier to_source = acquire;
            to_source.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_source.oldLayout = released;
            to_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            VkImageMemoryBarrier to_destination{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_destination.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_destination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_destination.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            to_destination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_destination.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_destination.image = copy.swapchain_images[n];
            to_destination.subresourceRange = kColorRange;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &acquire);
            const std::array pre{to_source, to_destination};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                 static_cast<uint32_t>(pre.size()), pre.data());

            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {slot.width, slot.height, 1};
            vkCmdCopyImage(cmd, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.swapchain_images[n],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            // Back to the layout the compositor expects, and hand the shared
            // buffer back to Dawn in GENERAL without a transition inside the
            // ownership release so Dawn's acquire can mirror it exactly.
            VkImageMemoryBarrier to_attachment = to_destination;
            to_attachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_attachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkImageMemoryBarrier to_general = to_source;
            to_general.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_general.dstAccessMask = 0;
            to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            const std::array post{to_attachment, to_general};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, static_cast<uint32_t>(post.size()), post.data());
            VkImageMemoryBarrier release_barrier = acquire;
            release_barrier.srcAccessMask = 0;
            release_barrier.dstAccessMask = 0;
            release_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            release_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            release_barrier.srcQueueFamilyIndex = queue_family_;
            release_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &release_barrier);
            slot.layout = VK_IMAGE_LAYOUT_GENERAL;
            signals[signal_count++] = slot.signal_semaphore;
        }
        const VkResult ended = vkEndCommandBuffer(cmd);
        if (ended != VK_SUCCESS) {
            Fail(VkFailure("vkEndCommandBuffer failed for the eye copy", ended));
            return CopyOutcome::Skipped;
        }

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = wait_count;
        submit.pWaitSemaphores = waits.data();
        submit.pWaitDstStageMask = wait_stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = signal_count;
        submit.pSignalSemaphores = signals.data();
        const VkResult submitted = vkQueueSubmit(vk_queue_, 1, &submit, submission->fence);
        if (submitted != VK_SUCCESS) {
            Fail(VkFailure("vkQueueSubmit failed for the eye copy", submitted));
            // A memory failure leaves every referenced resource untouched, so the frame merely
            // has no copy; only a lost device leaves the queue's state unknown.
            return submitted == VK_ERROR_DEVICE_LOST ? CopyOutcome::Unsafe : CopyOutcome::Skipped;
        }
        submission->busy = true;

        // Exporting a sync fd from a binary semaphore resets it, so the same
        // semaphore serves the next copy. Dawn waits on this before it writes
        // the buffer again.
        for (uint32_t n = 0; n < copy.Count(); ++n) {
            EyeSlot& slot = CopySlotLocked(copy, n);
            VkSemaphoreGetFdInfoKHR get_fd{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
            get_fd.semaphore = slot.signal_semaphore;
            get_fd.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            int fd = -1;
            CloseFd(slot.pending_acquire_fd);
            if (pfn_get_semaphore_fd_(vk_device_, &get_fd, &fd) == VK_SUCCESS) {
                slot.pending_acquire_fd = fd;
            } else {
                // Without the fence Dawn would write while the copy may still
                // read; wait for this submission on the CPU instead.
                vkWaitForFences(vk_device_, 1, &submission->fence, VK_TRUE, kFenceTimeoutNanos);
                Log(OpenXRLogLevel::Warning, "vkGetSemaphoreFdKHR failed; the eye copy was waited on the CPU");
            }
        }
        return CopyOutcome::Submitted;
    }

    Submission* AcquireSubmissionLocked(CopyOutcome& failure) {
        Submission& submission = submissions_[next_submission_];
        next_submission_ = (next_submission_ + 1) % kSubmissionRingSize;
        if (submission.busy) {
            const VkResult waited = vkWaitForFences(vk_device_, 1, &submission.fence, VK_TRUE, kFenceTimeoutNanos);
            if (waited != VK_SUCCESS) {
                // An older copy is still outstanding, so the queue's state is unknown.
                failure = CopyOutcome::Unsafe;
                Fail(VkFailure("a previous eye copy did not complete in time", waited));
                return nullptr;
            }
            submission.busy = false;
        }
        vkResetFences(vk_device_, 1, &submission.fence);
        const VkResult reset = vkResetCommandBuffer(submission.command_buffer, 0);
        if (reset != VK_SUCCESS) {
            failure = CopyOutcome::Skipped;
            Fail(VkFailure("vkResetCommandBuffer failed for the eye copy", reset));
            return nullptr;
        }
        return &submission;
    }

    // ---- OpenXR swapchains ----------------------------------------------------

    bool SelectSwapchainFormat() {
        const auto& formats = runtime_->SwapchainFormats();
        // Aurora's UNORM target holds gamma-encoded bytes; declaring the sRGB
        // sibling makes the compositor decode them instead of treating them as
        // linear light. vkCmdCopyImage between UNORM and SRGB siblings is legal.
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
            return SameCopyFamily(aurora_format_, static_cast<VkFormat>(format));
        });
        if (compatible == formats.end()) {
            return Fail("OpenXR offered no swapchain format copy-compatible with Aurora's Vulkan colour format");
        }
        swapchain_format_ = static_cast<VkFormat>(*compatible);
        return true;
    }

    bool CreateSwapchains() {
        return CreateSwapchainPair(eye_swapchains_) && CreateSwapchainPair(retained_swapchains_);
    }

    bool CreateSwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            const auto& view = runtime_->ViewConfiguration()[eye];
            if (!CreateSwapchain(pair[eye], view.render_width, view.render_height,
                                 eye == 0 ? "left eye" : "right eye")) {
                return false;
            }
        }
        return true;
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
        XrResult result = xrCreateSwapchain(runtime_->Session(), &create, &swapchain.handle);
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
        result = xrEnumerateSwapchainImages(
            swapchain.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrEnumerateSwapchainImages failed for a Vulkan eye swapchain");
        }
        return true;
    }

    bool AcquireSwapchain(EyeSwapchain& swapchain) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result = xrAcquireSwapchainImage(swapchain.handle, &acquire, &swapchain.acquired_index);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrAcquireSwapchainImage failed for a Vulkan eye swapchain");
        }
        swapchain.acquired = true;
        swapchain.waited = false;
        swapchain.release_forbidden = false;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
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
            // OpenXR only permits release after a successful wait. Keep the
            // image acquired and let session teardown destroy the child.
            Log(OpenXRLogLevel::Warning, "cannot release an OpenXR Vulkan image whose wait did not complete");
            return false;
        }
        if (swapchain.release_forbidden) {
            // Aurora reported a failed submission after it may already have
            // queued GPU work. Without a trustworthy fence the release could race
            // that work, so leave the image acquired for xrDestroySession.
            Log(OpenXRLogLevel::Warning, "deferring an OpenXR Vulkan image after an unsafe GPU submission");
            return false;
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult result = xrReleaseSwapchainImage(swapchain.handle, &release);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrReleaseSwapchainImage failed for an Vulkan swapchain");
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

    // The settings panel's swapchain pair, made the first time the panel opens.
    // A failure is logged once and the panel is drawn into the eyes again.
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

    void DestroyPanelSwapchains() {
        for (auto* swapchain : {&panel_swapchain_, &retained_panel_swapchain_}) {
            if (swapchain->handle != XR_NULL_HANDLE && !swapchain->acquired) {
                xrDestroySwapchain(swapchain->handle);
            } else if (swapchain->acquired) {
                Log(OpenXRLogLevel::Warning,
                    "Vulkan panel swapchain still owns an acquired image; deferring its destruction to xrDestroySession");
            }
            *swapchain = {};
        }
        panel_swapchains_ready_ = false;
        retained_panel_valid_ = false;
    }

    void DestroySwapchains() {
        DestroyPanelSwapchains();
        DestroySwapchainPair(eye_swapchains_);
        DestroySwapchainPair(retained_swapchains_);
        have_retained_frame_ = false;
        retained_frame_ = {};
        swapchain_format_ = VK_FORMAT_UNDEFINED;
    }

    void DestroySwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
        for (auto& swapchain : pair) {
            if (swapchain.handle != XR_NULL_HANDLE && !swapchain.acquired) {
                xrDestroySwapchain(swapchain.handle);
            } else if (swapchain.acquired) {
                Log(OpenXRLogLevel::Warning,
                    "Vulkan swapchain still owns an acquired image; deferring its destruction to xrDestroySession");
            }
            swapchain = {};
        }
    }

    void EndActiveFrameWithoutLayers(const OpenXRFrame& frame) {
        if (runtime_ != nullptr && runtime_->IsSessionRunning()) {
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

    OpenXRRuntime* runtime_ = nullptr;
    OpenXRLogCallback logger_;
    // The room around the virtual screen, started and paused as each presentation arrives.
    OpenXRPassthrough passthrough_{logger_};
    OpenXRVulkanGraphicsRequirements requirements_{};
    std::array<EyeSwapchain, kOpenXREyeCount> eye_swapchains_{};
    std::array<EyeSwapchain, kOpenXREyeCount> retained_swapchains_{};
    // The settings panel's layer: written like the eyes into panel_swapchain_,
    // shown from retained_panel_swapchain_ (see FinishFrame).
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

    // Vulkan objects owned here, guarded by vk_mutex_ because Aurora's worker
    // records the copy while the pacing thread prepares the next frame.
    std::mutex vk_mutex_;
    VkInstance vk_instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice vk_physical_ = VK_NULL_HANDLE;
    VkDevice vk_device_ = VK_NULL_HANDLE;
    VkQueue vk_queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = UINT32_MAX;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    PFN_vkImportSemaphoreFdKHR pfn_import_semaphore_fd_ = nullptr;
    PFN_vkGetSemaphoreFdKHR pfn_get_semaphore_fd_ = nullptr;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID pfn_get_ahb_properties_ = nullptr;
    std::array<std::array<EyeSlot, kSlotCount>, kOpenXREyeCount> slots_{};
    // The settings panel's shared buffers, allocated when it first opens.
    std::array<EyeSlot, kSlotCount> panel_slots_{};
    bool panel_slots_ready_ = false;
    std::array<Submission, kSubmissionRingSize> submissions_{};
    uint32_t next_submission_ = 0;
    uint32_t next_slot_ = 0;
    PendingCopy pending_copy_{};
    // Render-first pacing (PreparePacket): Aurora's release fences arrive while no compositor
    // frame is active, so the copy is recorded later by CopyRenderedEyes.
    bool deferred_copy_ = false;
    std::array<AuroraVulkanStereoRelease, kMaxCopies> pending_releases_{};
    bool have_pending_releases_ = false;
    uint64_t next_packet_serial_ = 1ull << 40; // never collides with the runtime's frame serials
    XrTime last_display_time_ = 0;
    XrDuration last_display_period_ = 0;
    bool last_should_render_ = false;

    std::mutex submission_mutex_;
    std::condition_variable submission_cv_;
    uint64_t awaiting_token_ = 0;
    uint64_t submitted_token_ = 0;
    bool submission_arrived_ = false;
    bool submission_success_ = false;
    bool submission_unsafe_ = false;
    bool shutting_down_ = false;

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

OpenXRVulkanBackend::OpenXRVulkanBackend(OpenXRLogCallback logger)
    : m_impl(std::make_unique<Impl>(std::move(logger))) {}

OpenXRVulkanBackend::~OpenXRVulkanBackend() = default;

bool OpenXRVulkanBackend::QueryGraphicsRequirements(OpenXRRuntime& runtime) {
    return m_impl->QueryGraphicsRequirements(runtime);
}

bool OpenXRVulkanBackend::BindAurora(OpenXRRuntime& runtime) {
    return m_impl->BindAurora(runtime);
}

OpenXRBeginStatus OpenXRVulkanBackend::BeginFrame(const OpenXRPresentation& presentation,
                                                  OpenXRBackendFrame& frame) {
    return m_impl->BeginFrame(presentation, frame);
}

OpenXRSubmissionStatus OpenXRVulkanBackend::WaitForSubmission(const OpenXRBackendFrame& frame,
                                                              uint32_t timeout_ms) {
    return m_impl->WaitForSubmission(frame, timeout_ms);
}
OpenXRBeginStatus OpenXRVulkanBackend::PreparePacket(const OpenXRPresentation& presentation,
                                                     OpenXRBackendFrame& packet) {
    return m_impl->PreparePacket(presentation, packet);
}
bool OpenXRVulkanBackend::TryCancelPendingPacket(OpenXRBackendFrame& packet) {
    return m_impl->TryCancelPendingPacket(packet);
}
OpenXRBeginStatus OpenXRVulkanBackend::BeginFrameForPacket(const OpenXRBackendFrame& packet,
                                                           OpenXRBackendFrame& frame) {
    return m_impl->BeginFrameForPacket(packet, frame);
}
OpenXRSubmissionStatus OpenXRVulkanBackend::CopyRenderedEyes(const OpenXRBackendFrame& frame) {
    return m_impl->CopyRenderedEyes(frame);
}
OpenXRBeginStatus OpenXRVulkanBackend::KeepAliveCycle() { return m_impl->KeepAliveCycle(); }

bool OpenXRVulkanBackend::TryCancelPendingFrame(OpenXRBackendFrame& frame) {
    return m_impl->TryCancelPendingFrame(frame);
}

bool OpenXRVulkanBackend::FinishFrame(OpenXRBackendFrame& frame, bool submit_layer) {
    return m_impl->FinishFrame(frame, submit_layer);
}

bool OpenXRVulkanBackend::RepeatFrame(const OpenXRBackendFrame& frame) {
    return m_impl->RepeatFrame(frame);
}

bool OpenXRVulkanBackend::Shutdown() { return m_impl->Shutdown(); }

bool OpenXRVulkanBackend::IsBound() const { return m_impl->IsBound(); }

bool OpenXRVulkanBackend::PanelLayerAvailable() const { return m_impl->PanelLayerAvailable(); }

const OpenXRVulkanGraphicsRequirements& OpenXRVulkanBackend::GraphicsRequirements() const {
    return m_impl->GraphicsRequirements();
}

int64_t OpenXRVulkanBackend::SwapchainFormat() const { return m_impl->SwapchainFormat(); }

const std::string& OpenXRVulkanBackend::LastError() const { return m_impl->LastError(); }

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR) && defined(__ANDROID__)
