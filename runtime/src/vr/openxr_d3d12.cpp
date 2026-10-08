// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)

// OpenXR's D3D12 structures are selected when openxr_platform.h is parsed.
#define XR_USE_GRAPHICS_API_D3D12
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "vr/openxr_d3d12.h"
#include "vr/openxr_diagnostics.h"

#include <aurora/d3d12_interop.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace mkw::vr {
namespace {

bool SameDxgiCopyFamily(DXGI_FORMAT left, DXGI_FORMAT right) noexcept {
    const auto family = [](DXGI_FORMAT format) noexcept {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return 1;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 2;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return 3;
        default:
            return 0;
        }
    };
    const int left_family = family(left);
    return left_family != 0 && left_family == family(right);
}

DXGI_FORMAT SrgbSibling(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool IsSrgbFormat(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
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

} // namespace

class OpenXRD3D12Backend::Impl final {
public:
    explicit Impl(OpenXRLogCallback logger) : logger_(std::move(logger)) {}

    ~Impl() { Shutdown(); }

    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<XrSwapchainImageD3D12KHR> images;
        uint32_t acquired_index = 0;
        bool acquired = false;
        bool waited = false;
        bool release_forbidden = false;
    };

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime) {
        ClearError();
        if (!runtime.IsInitialized() || runtime.HasSession()) {
            return Fail("OpenXR must own an instance, but no session, before querying D3D12 requirements");
        }
        if (requirements_queried_ && runtime_ != &runtime) {
            return Fail("D3D12 graphics requirements were already queried from another OpenXR instance");
        }

        PFN_xrGetD3D12GraphicsRequirementsKHR get_requirements = nullptr;
        if (!runtime.LoadFunction("xrGetD3D12GraphicsRequirementsKHR", &get_requirements) ||
            get_requirements == nullptr) {
            return Fail("OpenXR runtime did not expose xrGetD3D12GraphicsRequirementsKHR");
        }

        XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        const XrResult result = get_requirements(runtime.Instance(), runtime.SystemId(), &requirements);
        runtime.ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrGetD3D12GraphicsRequirementsKHR failed (" << result << ')';
            return Fail(message.str());
        }

        runtime_ = &runtime;
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = false;
            submission_unsafe_ = false;
        }
        requirements_ = {
            requirements.adapterLuid.LowPart,
            requirements.adapterLuid.HighPart,
            static_cast<uint32_t>(requirements.minFeatureLevel),
        };
        requirements_queried_ = true;

        std::ostringstream message;
        message << "OpenXR D3D12 adapter LUID " << std::hex
                << static_cast<uint32_t>(requirements_.adapter_luid_high) << ':'
                << requirements_.adapter_luid_low << ", minimum feature level 0x"
                << requirements_.minimum_feature_level;
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    bool BindAurora(OpenXRRuntime& runtime) {
        ClearError();
        if (!requirements_queried_ || runtime_ != &runtime || runtime.HasSession()) {
            return Fail("QueryGraphicsRequirements must succeed on this OpenXR instance before BindAurora");
        }
        if (bound_) {
            return Fail("OpenXR D3D12 backend is already bound");
        }

        AuroraD3D12NativeHandles handles{};
        if (!aurora_d3d12_get_native_handles(&handles) || handles.device == nullptr ||
            handles.queue == nullptr) {
            return Fail("Aurora did not expose a Dawn D3D12 device and command queue");
        }
        if (handles.adapterLuidLow != requirements_.adapter_luid_low ||
            handles.adapterLuidHigh != requirements_.adapter_luid_high) {
            std::ostringstream message;
            message << "Aurora selected D3D12 adapter " << std::hex
                    << static_cast<uint32_t>(handles.adapterLuidHigh) << ':'
                    << handles.adapterLuidLow << ", but OpenXR requires "
                    << static_cast<uint32_t>(requirements_.adapter_luid_high) << ':'
                    << requirements_.adapter_luid_low;
            return Fail(message.str());
        }

        auto* device = static_cast<ID3D12Device*>(handles.device);
        const D3D_FEATURE_LEVEL minimum =
            static_cast<D3D_FEATURE_LEVEL>(requirements_.minimum_feature_level);
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels{1, &minimum, D3D_FEATURE_LEVEL_1_0_CORE};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels,
                                               sizeof(levels))) ||
            levels.MaxSupportedFeatureLevel < minimum) {
            return Fail("Aurora's D3D12 device does not satisfy the OpenXR minimum feature level");
        }

        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding.device = device;
        binding.queue = static_cast<ID3D12CommandQueue*>(handles.queue);
        if (!runtime.CreateSession(&binding)) {
            return Fail("OpenXR rejected Aurora's D3D12 device/queue binding");
        }
        owns_session_ = true;
        aurora_format_ = static_cast<DXGI_FORMAT>(handles.colorDxgiFormat);
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            const auto& view = runtime.ViewConfiguration()[eye];
            eye_size_[eye] = {view.render_width, view.render_height};
        }
        requested_eye_size_ = eye_size_;

        if (!SelectSwapchainFormat() || !CreateSwapchains()) {
            DestroySwapchains();
            runtime.DestroySession();
            owns_session_ = false;
            return false;
        }
        if (runtime.ShouldExit()) {
            DestroySwapchains();
            runtime.DestroySession();
            owns_session_ = false;
            return Fail("OpenXR session became loss-pending while creating D3D12 swapchains");
        }
        if (!aurora_d3d12_enable_stereo_bridge(&Impl::OnAuroraSubmitted, this)) {
            DestroySwapchains();
            runtime.DestroySession();
            owns_session_ = false;
            return Fail("Aurora could not enable its zero-readback D3D12 stereo bridge");
        }
        bridge_enabled_ = true;
        bound_ = true;

        std::ostringstream message;
        message << "OpenXR D3D12 swapchains ready: DXGI format "
                << static_cast<int64_t>(swapchain_format_) << ", eyes "
                << eye_swapchains_[0].width << 'x' << eye_swapchains_[0].height << " / "
                << eye_swapchains_[1].width << 'x' << eye_swapchains_[1].height;
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

    OpenXRD3D12BeginStatus BeginFrame(const OpenXRD3D12Presentation& presentation,
                                      OpenXRD3D12Frame& frame) {
        frame = {};
        frame.presentation = presentation;
        if (!bound_ || runtime_ == nullptr) {
            Fail("BeginFrame called before the D3D12 backend was bound");
            return OpenXRD3D12BeginStatus::Error;
        }
        if (frame_active_ || pending_packet_serial_ != 0) {
            Fail("BeginFrame called while another OpenXR frame is active");
            return OpenXRD3D12BeginStatus::Error;
        }
        if (!ResizeWritablePair()) {
            return OpenXRD3D12BeginStatus::Error;
        }

        const OpenXRFrameStatus status = runtime_->WaitFrame(frame.xr_frame);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(BeginStatusOperation(status));
            }
            switch (status) {
            case OpenXRFrameStatus::SessionNotRunning:
                return OpenXRD3D12BeginStatus::SessionNotRunning;
            case OpenXRFrameStatus::ExitRequested:
                return OpenXRD3D12BeginStatus::ExitRequested;
            case OpenXRFrameStatus::Error:
                return OpenXRD3D12BeginStatus::Error;
            case OpenXRFrameStatus::Ready:
                break;
            }
        }
        NoteDisplayTiming(frame.xr_frame);
        if (!runtime_->BeginFrame(frame.xr_frame)) {
            Fail("xrBeginFrame failed");
            return OpenXRD3D12BeginStatus::Error;
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
            return OpenXRD3D12BeginStatus::Ready;
        }
        if (!runtime_->LocateViews(frame.xr_frame)) {
            Fail("xrLocateViews failed");
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRD3D12BeginStatus::Error;
        }
        active_frame_ = frame.xr_frame;
        if (!frame.xr_frame.views_valid) {
            return OpenXRD3D12BeginStatus::Ready;
        }

        return PrepareTargets(frame);
    }

    // Both pacing paths retain ownership until completion or cancellation.
    OpenXRD3D12BeginStatus PrepareTargets(OpenXRD3D12Frame& frame) {
        const uint32_t target_count =
            frame.presentation.mode == OpenXRD3D12FrameMode::VirtualScreen ? 1u : kOpenXREyeCount;
        if (target_count == 1) {
            frame.render_width[1] = frame.render_width[0];
            frame.render_height[1] = frame.render_height[0];
        }

        std::array<AuroraD3D12StereoTarget, kOpenXREyeCount> targets{};
        const diagnostics::Stopwatch acquire_timer;
        for (uint32_t eye = 0; eye < target_count; ++eye) {
            auto& swapchain = eye_swapchains_[eye];
            if (!AcquireSwapchain(swapchain)) {
                ReleaseAcquiredSwapchains();
                EndActiveFrameWithoutLayers(frame.xr_frame);
                return OpenXRD3D12BeginStatus::Error;
            }
            targets[eye] = {
                swapchain.images[swapchain.acquired_index].texture,
                swapchain.width,
                swapchain.height,
                static_cast<int64_t>(swapchain_format_),
            };
        }
        // The settings panel's layer image, rendered with the eyes while it is open.
        AuroraD3D12StereoTarget panel_target{};
        const bool panel = frame.presentation.panel.requested && EnsurePanelSwapchains();
        frame.presentation.panel.requested = panel;
        if (panel) {
            if (!AcquireSwapchain(panel_swapchain_)) {
                ReleaseAcquiredSwapchains();
                EndActiveFrameWithoutLayers(frame.xr_frame);
                return OpenXRD3D12BeginStatus::Error;
            }
            panel_target = {
                panel_swapchain_.images[panel_swapchain_.acquired_index].texture,
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
        if (!diagnostics::Measure(diagnostics::Stage::SetTargets, [&] {
            return aurora_d3d12_set_stereo_targets_with_panel(frame.xr_frame.serial, targets.data(), target_count,
                                                              panel ? &panel_target : nullptr);
        })) {
            {
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = 0;
            }
            ReleaseAcquiredSwapchains();
            Fail("Aurora rejected the acquired OpenXR D3D12 swapchain target");
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRD3D12BeginStatus::Error;
        }
        frame.expects_gpu_submission = true;
        return OpenXRD3D12BeginStatus::Ready;
    }

    // D3D12 renders straight into the non-retained XR swapchain pair. Acquiring
    // images is independent of the compositor cycle; keep them acquired while
    // Aurora owns them, and never expose them through the retained pair early.
    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
        packet = {};
        packet.presentation = presentation;
        if (!bound_ || runtime_ == nullptr || frame_active_ || pending_packet_serial_ != 0) {
            Fail("PreparePacket called before binding or with work pending");
            return OpenXRBeginStatus::Error;
        }
        if (runtime_->ShouldExit()) return OpenXRBeginStatus::ExitRequested;
        if (!runtime_->IsSessionRunning()) return OpenXRBeginStatus::SessionNotRunning;
        if (timing_session_serial_ != runtime_->SessionRunSerial() || last_display_period_ <= 0) {
            const auto status = KeepAliveCycle();
            if (status != OpenXRBeginStatus::Ready) return status;
        }
        if (!ResizeWritablePair()) return OpenXRBeginStatus::Error;
        packet.xr_frame.serial = next_packet_serial_++;
        packet.xr_frame.predicted_display_time = last_display_time_ + 2 * last_display_period_;
        packet.xr_frame.predicted_display_period = last_display_period_;
        packet.xr_frame.should_render = last_should_render_;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            packet.render_width[eye] = eye_swapchains_[eye].width;
            packet.render_height[eye] = eye_swapchains_[eye].height;
        }
        if (!packet.xr_frame.should_render) return OpenXRBeginStatus::Ready;
        if (!runtime_->LocateViewsAt(packet.xr_frame.predicted_display_time, packet.xr_frame)) {
            Fail("xrLocateViews failed for a D3D12 packet");
            return OpenXRBeginStatus::Error;
        }
        if (!packet.xr_frame.views_valid) return OpenXRBeginStatus::Ready;
        render_session_serial_ = runtime_->SessionRunSerial();
        render_space_serial_ = runtime_->LastReferenceSpaceChange().serial;
        const auto status = PrepareTargets(packet);
        if (status == OpenXRBeginStatus::Ready) pending_packet_serial_ = packet.xr_frame.serial;
        return status;
    }

    bool TryCancelPendingPacket(OpenXRBackendFrame& packet) {
        if (frame_active_ || pending_packet_serial_ == 0 ||
            packet.xr_frame.serial != pending_packet_serial_ || !packet.expects_gpu_submission ||
            !aurora_d3d12_cancel_stereo_targets(packet.xr_frame.serial)) return false;
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = submitted_token_ = 0;
            submission_arrived_ = submission_success_ = submission_unsafe_ = false;
        }
        packet.expects_gpu_submission = false;
        pending_packet_serial_ = 0;
        const diagnostics::Stopwatch release_timer;
        const bool released = ReleaseAcquiredSwapchains();
        diagnostics::OnSwapchainRelease(release_timer);
        // A release error is fatal; keep it visible to the next prepare rather
        // than letting it register new targets over still-acquired images.
        if (!released) pending_packet_serial_ = packet.xr_frame.serial;
        return true; // Encoding was canceled; a release error blocks the next prepare.
    }

    void NoteDisplayTiming(const OpenXRFrame& frame) {
        last_display_time_ = frame.predicted_display_time;
        last_display_period_ = frame.predicted_display_period;
        last_should_render_ = frame.should_render;
        timing_session_serial_ = runtime_->SessionRunSerial();
    }

    OpenXRBeginStatus BeginCompositorCycle() {
        const auto status = runtime_->WaitFrame(active_frame_);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) Fail(BeginStatusOperation(status));
            return status == OpenXRFrameStatus::SessionNotRunning ? OpenXRBeginStatus::SessionNotRunning
                 : status == OpenXRFrameStatus::ExitRequested ? OpenXRBeginStatus::ExitRequested
                                                             : OpenXRBeginStatus::Error;
        }
        NoteDisplayTiming(active_frame_);
        if (!runtime_->BeginFrame(active_frame_)) {
            Fail("xrBeginFrame failed for a D3D12 compositor cycle");
            return OpenXRBeginStatus::Error;
        }
        frame_active_ = true;
        return OpenXRBeginStatus::Ready;
    }

    OpenXRBeginStatus KeepAliveCycle() {
        if (!bound_ || runtime_ == nullptr || frame_active_) {
            Fail("KeepAliveCycle called before binding or with an active frame");
            return OpenXRBeginStatus::Error;
        }
        const auto status = BeginCompositorCycle();
        if (status != OpenXRBeginStatus::Ready) return status;
        const bool ended = EndRetainedFrame(false);
        frame_active_ = false;
        active_frame_ = {};
        if (!ended) {
            Fail("OpenXR could not resubmit the retained D3D12 frame");
            return OpenXRBeginStatus::Error;
        }
        return OpenXRBeginStatus::Ready;
    }

    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
        frame = {};
        if (!bound_ || runtime_ == nullptr || frame_active_ || pending_packet_serial_ == 0 ||
            packet.xr_frame.serial != pending_packet_serial_ || !packet.expects_gpu_submission ||
            WaitForSubmission(packet, 0) != OpenXRSubmissionStatus::Success) {
            Fail("BeginFrameForPacket requires the completed current D3D12 packet");
            return OpenXRBeginStatus::Error;
        }
        const auto status = BeginCompositorCycle();
        if (status != OpenXRBeginStatus::Ready) {
            // Completion was already confirmed. A stopped session must not
            // strand a packet and block preparation after the next READY event.
            if (status == OpenXRBeginStatus::SessionNotRunning) {
                const bool released = ReleaseAcquiredSwapchains();
                pending_packet_serial_ = 0;
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = submitted_token_ = 0;
                submission_arrived_ = submission_success_ = submission_unsafe_ = false;
                if (!released) return OpenXRBeginStatus::Error;
            }
            return status;
        }
        frame = packet;
        // Use the current compositor token/time but the original render poses.
        frame.xr_frame.serial = active_frame_.serial;
        frame.xr_frame.predicted_display_time = active_frame_.predicted_display_time;
        frame.xr_frame.predicted_display_period = active_frame_.predicted_display_period;
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
            Fail("CopyRenderedEyes received a stale D3D12 frame");
            return OpenXRSubmissionStatus::Failed;
        }
        // The native bridge already queued the copy on the session's D3D12
        // queue before publishing completion. There is no second copy on PC.
        return WaitForSubmission(frame, 0);
    }

    OpenXRD3D12SubmissionStatus WaitForSubmission(const OpenXRD3D12Frame& frame,
                                                  uint32_t timeout_ms) {
        if (!frame.expects_gpu_submission) {
            return OpenXRD3D12SubmissionStatus::Success;
        }
        std::unique_lock lock(submission_mutex_);
        const auto ready = [&] {
            return shutting_down_ ||
                   (submission_arrived_ && submitted_token_ == frame.xr_frame.serial);
        };
        if (timeout_ms == std::numeric_limits<uint32_t>::max()) {
            submission_cv_.wait(lock, ready);
        } else if (!submission_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
            return OpenXRD3D12SubmissionStatus::Timeout;
        }
        if (shutting_down_) {
            return OpenXRD3D12SubmissionStatus::ShuttingDown;
        }
        return submission_success_ ? OpenXRD3D12SubmissionStatus::Success
                                   : OpenXRD3D12SubmissionStatus::Failed;
    }

    bool TryCancelPendingFrame(OpenXRD3D12Frame& frame) {
        if (!frame_active_ || !frame.expects_gpu_submission ||
            frame.xr_frame.serial != active_frame_serial_) {
            return false;
        }
        if (!aurora_d3d12_cancel_stereo_targets(frame.xr_frame.serial)) {
            return false;
        }

        // A successful bridge cancellation is serialized against Encode and
        // never generates a callback, so this token has no GPU ownership.
        std::lock_guard lock(submission_mutex_);
        awaiting_token_ = 0;
        submitted_token_ = 0;
        submission_arrived_ = false;
        submission_success_ = false;
        submission_unsafe_ = false;
        frame.expects_gpu_submission = false;
        return true;
    }

    bool FinishFrame(OpenXRD3D12Frame& frame, bool submit_layer) {
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
            Fail("Aurora's D3D12 stereo submission failed after GPU work may have been queued");
        }
        const diagnostics::Stopwatch release_timer;
        bool release_ok = ReleaseAcquiredSwapchains();
        if (frame.xr_frame.should_render && frame.xr_frame.views_valid) {
            diagnostics::OnSwapchainRelease(release_timer);
        }
        const bool position_valid =
            (frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        const bool composition_pose_valid =
            frame.presentation.mode == OpenXRD3D12FrameMode::VirtualScreen || position_valid;
        const bool can_submit = submit_layer && release_ok && frame.xr_frame.should_render &&
                                frame.xr_frame.views_valid && frame.expects_gpu_submission &&
                                composition_pose_valid;
        if (submit_layer && !can_submit) {
            diagnostics::OnLayerRejected(diagnostics::ClassifyRejectedLayer(
                release_ok, frame.xr_frame.should_render, frame.xr_frame.views_valid));
        }
        if (can_submit) {
            // xrEndFrame references the MOST RECENTLY RELEASED image of a
            // swapchain, not an explicit image index. Keep the displayed pair
            // separate from the pair Aurora can write or cancel next.
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

    bool RepeatFrame(const OpenXRD3D12Frame& frame) {
        if (!frame_active_ || runtime_ == nullptr ||
            frame.xr_frame.serial != active_frame_serial_) {
            return Fail("RepeatFrame received a stale or inactive render token");
        }
        const bool end_ok = EndRetainedFrame(false);
        // EndFrame consumes the compositor token even when submission fails.
        // Teardown must not try to end that same token again.
        frame_active_ = false;
        if (!end_ok) {
            return Fail("OpenXR could not resubmit the retained frame");
        }
        // active_frame_serial_ continues to identify Aurora's pending render;
        // active_frame_ identifies the independently advancing compositor cycle.
        if (runtime_->PollEvents() != OpenXREventStatus::Continue ||
            !runtime_->IsSessionRunning() || runtime_->ShouldExit()) {
            return Fail("OpenXR session stopped while waiting for stereo rendering");
        }
        if (runtime_->WaitFrame(active_frame_) != OpenXRFrameStatus::Ready ||
            !runtime_->BeginFrame(active_frame_)) {
            return Fail("OpenXR could not start a retained-frame compositor cycle");
        }
        NoteDisplayTiming(active_frame_);
        frame_active_ = true;
        return true;
    }

    // fresh: the retained layer was completed for this call rather than repeated.
    bool EndRetainedFrame(bool fresh) {
        if (!runtime_->IsSessionRunning()) {
            // A session that is no longer running needs no compositor frame
            // completion call. Preserve the original backend failure instead
            // of replacing it with a stale-token error.
            return true;
        }
        // Old poses cannot be reused after the runtime changes their coordinate
        // system. Also discard content across session restarts.
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
            return runtime_->EndFrameWithoutLayers(active_frame_);
        }
        diagnostics::OnLayer(fresh);
        const auto& frame = retained_frame_;
        if (frame.presentation.mode == OpenXRD3D12FrameMode::VirtualScreen) {
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags = 0;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = retained_swapchains_[0].handle;
            quad.subImage.imageRect = {{0, 0},
                                       {static_cast<int32_t>(retained_swapchains_[0].width),
                                        static_cast<int32_t>(retained_swapchains_[0].height)}};
            quad.subImage.imageArrayIndex = 0;
            if (frame.presentation.quad_anchored) {
                // Placed in the application space, so the screen keeps its place
                // in the room while the player looks around it.
                quad.space = runtime_->AppSpace();
                quad.pose = frame.presentation.quad_pose;
            } else {
                // No head pose to anchor against yet: keep it in front of the
                // player so the menus are never left stranded behind them.
                quad.space = runtime_->ViewSpace();
                quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                quad.pose.position = {
                    0.0f, 0.0f, -std::max(0.25f, frame.presentation.quad_distance_meters)};
            }
            quad.size.width = std::max(0.25f, frame.presentation.quad_width_meters);
            quad.size.height = quad.size.width * static_cast<float>(retained_swapchains_[0].height) /
                               static_cast<float>(retained_swapchains_[0].width);
            return EndFrameWithPanel(frame, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
        } else {
            std::array<XrCompositionLayerProjectionView, kOpenXREyeCount> views{};
            for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
                views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                views[eye].pose.orientation = frame.xr_frame.views[eye].pose.orientation;
                views[eye].pose.position = frame.xr_frame.views[eye].pose.position;
                views[eye].fov = frame.xr_frame.views[eye].fov;
                views[eye].subImage.swapchain = retained_swapchains_[eye].handle;
                views[eye].subImage.imageRect = {
                    {0, 0},
                    {static_cast<int32_t>(retained_swapchains_[eye].width),
                     static_cast<int32_t>(retained_swapchains_[eye].height)}};
                views[eye].subImage.imageArrayIndex = 0;
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.layerFlags = 0;
            projection.space = runtime_->AppSpace();
            projection.viewCount = kOpenXREyeCount;
            projection.views = views.data();
            return EndFrameWithPanel(frame, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
        }
    }

    // Ends the compositor frame with the scene's layer and, while the retained
    // frame rendered it, the settings panel's layer over it.
    bool EndFrameWithPanel(const OpenXRBackendFrame& frame, const XrCompositionLayerBaseHeader* scene) {
        const auto& panel = frame.presentation.panel;
        XrCompositionLayerQuad panel_quad{};
        const XrCompositionLayerBaseHeader* layers[2] = {scene, nullptr};
        uint32_t count = 1;
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
            bridge_drained = aurora_d3d12_disable_stereo_bridge();
            bridge_enabled_ = false;
        }
        if (!bridge_drained) {
            AbandonAcquiredSwapchains();
            shutdown_unsafe_ = true;
            Fail("D3D12 queue completion is unknown; retaining the OpenXR session and graphics owners");
            return false;
        }

        // A queue-tail fence proved that no bridge command can still reference
        // an acquired image. This also makes a conservatively abandoned image
        // releasable after an earlier submission failure.
        AllowAcquiredSwapchainsAfterGpuDrain();
        ReleaseAcquiredSwapchains();
        if (frame_active_ && runtime_ != nullptr) {
            // Shutdown is required to run on the XR owner thread after Aurora's
            // worker is idle, so it is safe to close an abandoned frame here.
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
        if (owns_session_ && runtime_ != nullptr) {
            runtime_->DestroySession();
            owns_session_ = false;
        }
        bound_ = false;
        requirements_queried_ = false;
        runtime_ = nullptr;
        return true;
    }

    bool IsBound() const { return bound_; }
    bool PanelLayerAvailable() const { return !panel_layer_failed_; }
    const OpenXRD3D12GraphicsRequirements& GraphicsRequirements() const { return requirements_; }
    int64_t SwapchainFormat() const { return static_cast<int64_t>(swapchain_format_); }
    const std::string& LastError() const { return last_error_; }

private:
    bool SelectSwapchainFormat() {
        const auto& formats = runtime_->SwapchainFormats();
        // Aurora's UNORM target contains the gamma-encoded bytes expected by
        // the desktop compositor. OpenXR must declare the compatible sRGB
        // sibling so the headset compositor decodes those raw bytes instead
        // of treating them as linear light (which appears severely washed out).
        const DXGI_FORMAT srgb = SrgbSibling(aurora_format_);
        if (srgb != DXGI_FORMAT_UNKNOWN &&
            std::find(formats.begin(), formats.end(), static_cast<int64_t>(srgb)) !=
                formats.end()) {
            swapchain_format_ = srgb;
            return true;
        }
        const auto exact = std::find(formats.begin(), formats.end(), static_cast<int64_t>(aurora_format_));
        if (exact != formats.end()) {
            swapchain_format_ = aurora_format_;
            return true;
        }
        const auto compatible = std::find_if(formats.begin(), formats.end(), [&](int64_t format) {
            return SameDxgiCopyFamily(aurora_format_, static_cast<DXGI_FORMAT>(format));
        });
        if (compatible == formats.end()) {
            return Fail("OpenXR offered no swapchain format copy-compatible with Aurora's D3D12 color format");
        }
        swapchain_format_ = static_cast<DXGI_FORMAT>(*compatible);
        return true;
    }

    bool CreateSwapchains() {
        return CreateSwapchainPair(eye_swapchains_) && CreateSwapchainPair(retained_swapchains_);
    }

    bool CreateSwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            if (!CreateSwapchain(pair[eye], eye_size_[eye].width, eye_size_[eye].height,
                                 eye == 0 ? "left eye" : "right eye")) {
                return false;
            }
        }
        return true;
    }

    // Rebuilds the pair Aurora writes next at eye_size_ when it is another size. Only that pair,
    // with none of its images acquired, never the one on display. The new swapchains are made
    // before the old ones go, so a size the runtime cannot allocate leaves the pair as it was.
    // False only on a failure that ends the session.
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
            message << "OpenXR D3D12 eyes stay " << current[0].width << 'x' << current[0].height
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
        message << "OpenXR D3D12 eyes resized: " << eye_size_[0].width << 'x' << eye_size_[0].height << " / "
                << eye_size_[1].width << 'x' << eye_size_[1].height;
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    // Destroys the retired pairs whose time is up (every one of them at shutdown), once Aurora's
    // copies into their images are known complete. A pair Aurora cannot vouch for is left to
    // xrDestroySession rather than destroyed under a possibly live copy.
    void ReapRetiredPairs(bool all) {
        for (auto it = retired_pairs_.begin(); it != retired_pairs_.end();) {
            if (!all && --it->cycles_left != 0) {
                ++it;
                continue;
            }
            std::vector<void*> images;
            for (const EyeSwapchain& swapchain : it->swapchains) {
                for (const auto& image : swapchain.images) {
                    images.push_back(image.texture);
                }
            }
            if (aurora_d3d12_forget_stereo_targets(images.data(), static_cast<uint32_t>(images.size()))) {
                DestroySwapchainPair(it->swapchains);
            } else {
                Log(OpenXRLogLevel::Warning,
                    "Aurora could not retire its copies into a replaced D3D12 eye swapchain pair; "
                    "deferring its destruction to xrDestroySession");
            }
            it = retired_pairs_.erase(it);
        }
    }

    bool CreateSwapchain(EyeSwapchain& swapchain, uint32_t width, uint32_t height, const char* what) {
        swapchain.width = width;
        swapchain.height = height;

        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        if (IsSrgbFormat(swapchain_format_)) {
            // Matches DolphinXR's raw-UNORM-write/sRGB-compositor path and
            // asks D3D runtimes to expose a typeless-compatible resource.
            create.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        }
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
            message << "xrCreateSwapchain failed for the D3D12 " << what << " swapchain (" << result << ')';
            return Fail(message.str());
        }

        uint32_t count = 0;
        result = xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr);
        ObserveResult(result);
        if (XR_FAILED(result) || count == 0) {
            return Fail("OpenXR returned no D3D12 swapchain images");
        }
        swapchain.images.resize(count);
        for (auto& image : swapchain.images) {
            image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        }
        result = xrEnumerateSwapchainImages(
            swapchain.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrEnumerateSwapchainImages failed for a D3D12 eye swapchain");
        }
        return true;
    }

    bool AcquireSwapchain(EyeSwapchain& swapchain) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result = xrAcquireSwapchainImage(swapchain.handle, &acquire,
                                                   &swapchain.acquired_index);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrAcquireSwapchainImage failed for a D3D12 eye swapchain");
        }
        swapchain.acquired = true;
        swapchain.waited = false;
        swapchain.release_forbidden = false;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait);
        ObserveResult(result);
        if (result == XR_TIMEOUT_EXPIRED) {
            return Fail("xrWaitSwapchainImage unexpectedly timed out for a D3D12 eye swapchain");
        }
        if (XR_FAILED(result)) {
            return Fail("xrWaitSwapchainImage failed for a D3D12 eye swapchain");
        }
        swapchain.waited = true;
        if (swapchain.acquired_index >= swapchain.images.size()) {
            return Fail("OpenXR returned an out-of-range D3D12 swapchain image index");
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
            Log(OpenXRLogLevel::Warning, "cannot release an OpenXR D3D12 image whose wait did not complete");
            return false;
        }
        if (swapchain.release_forbidden) {
            // Aurora reported a failed submission after it may already have
            // queued GPU work. Without a trustworthy fence the release could race
            // that work, so leave the image acquired for xrDestroySession.
            Log(OpenXRLogLevel::Warning, "deferring an OpenXR D3D12 image after an unsafe GPU submission");
            return false;
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult result = xrReleaseSwapchainImage(swapchain.handle, &release);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrReleaseSwapchainImage failed for an D3D12 swapchain");
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
                    "D3D12 panel swapchain still owns an acquired image; deferring its destruction to xrDestroySession");
            }
            *swapchain = {};
        }
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
        swapchain_format_ = DXGI_FORMAT_UNKNOWN;
    }

    void DestroySwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
        for (auto& swapchain : pair) {
            if (swapchain.handle != XR_NULL_HANDLE && !swapchain.acquired) {
                xrDestroySwapchain(swapchain.handle);
            } else if (swapchain.acquired) {
                Log(OpenXRLogLevel::Warning,
                    "D3D12 swapchain still owns an acquired image; deferring its destruction to xrDestroySession");
            }
            swapchain = {};
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

    OpenXRRuntime* runtime_ = nullptr;
    OpenXRLogCallback logger_;
    OpenXRD3D12GraphicsRequirements requirements_{};
    std::array<EyeSwapchain, kOpenXREyeCount> eye_swapchains_{};
    std::array<EyeSwapchain, kOpenXREyeCount> retained_swapchains_{};
    // The size a rebuilt eye pair gets, and the one the render scale last asked
    // for, which differ while a size the runtime refused is being kept.
    std::array<OpenXREyeSize, kOpenXREyeCount> eye_size_{};
    std::array<OpenXREyeSize, kOpenXREyeCount> requested_eye_size_{};
    // Pairs replaced by a new size, destroyed once the compositor has moved on from them.
    struct RetiredPair {
        std::array<EyeSwapchain, kOpenXREyeCount> swapchains;
        uint32_t cycles_left;
    };
    std::vector<RetiredPair> retired_pairs_;
    // The settings panel's layer: written like the eyes into panel_swapchain_,
    // shown from retained_panel_swapchain_ (see FinishFrame).
    EyeSwapchain panel_swapchain_{};
    EyeSwapchain retained_panel_swapchain_{};
    bool panel_swapchains_ready_ = false;
    bool panel_layer_failed_ = false;
    // The retained frame rendered the panel's image into retained_panel_swapchain_.
    bool retained_panel_valid_ = false;
    OpenXRD3D12Frame retained_frame_{};
    uint64_t retained_session_serial_ = 0;
    uint64_t retained_space_serial_ = 0;
    bool have_retained_frame_ = false;
    DXGI_FORMAT aurora_format_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT swapchain_format_ = DXGI_FORMAT_UNKNOWN;
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

OpenXRD3D12Backend::OpenXRD3D12Backend(OpenXRLogCallback logger)
    : m_impl(std::make_unique<Impl>(std::move(logger))) {}

OpenXRD3D12Backend::~OpenXRD3D12Backend() = default;

bool OpenXRD3D12Backend::QueryGraphicsRequirements(OpenXRRuntime& runtime) {
    return m_impl->QueryGraphicsRequirements(runtime);
}

bool OpenXRD3D12Backend::BindAurora(OpenXRRuntime& runtime) {
    return m_impl->BindAurora(runtime);
}

void OpenXRD3D12Backend::SetRenderScale(float scale) { m_impl->SetRenderScale(scale); }

OpenXRD3D12BeginStatus OpenXRD3D12Backend::BeginFrame(
    const OpenXRD3D12Presentation& presentation, OpenXRD3D12Frame& frame) {
    return m_impl->BeginFrame(presentation, frame);
}

OpenXRD3D12SubmissionStatus OpenXRD3D12Backend::WaitForSubmission(
    const OpenXRD3D12Frame& frame, uint32_t timeout_ms) {
    return m_impl->WaitForSubmission(frame, timeout_ms);
}

bool OpenXRD3D12Backend::TryCancelPendingFrame(OpenXRD3D12Frame& frame) {
    return m_impl->TryCancelPendingFrame(frame);
}

bool OpenXRD3D12Backend::FinishFrame(OpenXRD3D12Frame& frame, bool submit_layer) {
    return m_impl->FinishFrame(frame, submit_layer);
}

bool OpenXRD3D12Backend::RepeatFrame(const OpenXRD3D12Frame& frame) {
    return m_impl->RepeatFrame(frame);
}

OpenXRBeginStatus OpenXRD3D12Backend::PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
    return m_impl->PreparePacket(presentation, packet);
}
bool OpenXRD3D12Backend::TryCancelPendingPacket(OpenXRBackendFrame& packet) {
    return m_impl->TryCancelPendingPacket(packet);
}
OpenXRBeginStatus OpenXRD3D12Backend::BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
    return m_impl->BeginFrameForPacket(packet, frame);
}
OpenXRSubmissionStatus OpenXRD3D12Backend::CopyRenderedEyes(const OpenXRBackendFrame& frame) {
    return m_impl->CopyRenderedEyes(frame);
}
OpenXRBeginStatus OpenXRD3D12Backend::KeepAliveCycle() { return m_impl->KeepAliveCycle(); }

bool OpenXRD3D12Backend::Shutdown() { return m_impl->Shutdown(); }

bool OpenXRD3D12Backend::IsBound() const { return m_impl->IsBound(); }

bool OpenXRD3D12Backend::PanelLayerAvailable() const { return m_impl->PanelLayerAvailable(); }

const OpenXRD3D12GraphicsRequirements& OpenXRD3D12Backend::GraphicsRequirements() const {
    return m_impl->GraphicsRequirements();
}

int64_t OpenXRD3D12Backend::SwapchainFormat() const { return m_impl->SwapchainFormat(); }

const std::string& OpenXRD3D12Backend::LastError() const { return m_impl->LastError(); }

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
