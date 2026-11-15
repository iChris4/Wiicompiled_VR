// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR) && defined(MKW_PLATFORM_VISIONOS)

#include "vr/openxr_metal.h"
#include "vr/openxr_diagnostics.h"
#include "vr/visionos/xr_visionos.h"

#include <aurora/metal_interop.h>

#include <CoreFoundation/CoreFoundation.h>

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

// MTLPixelFormat values, as aurora/metal_interop.h spells them.
bool SameCopyFamily(int64_t left, int64_t right) noexcept {
    const auto family = [](int64_t format) noexcept {
        switch (format) {
        case AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM:
        case AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM_SRGB:
            return 1;
        case AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM:
        case AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM_SRGB:
            return 2;
        case AURORA_MTL_PIXEL_FORMAT_RGBA16_FLOAT:
            return 3;
        default:
            return 0;
        }
    };
    const int left_family = family(left);
    return left_family != 0 && left_family == family(right);
}

int64_t SrgbSibling(int64_t format) noexcept {
    switch (format) {
    case AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM:
    case AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM_SRGB:
        return AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM_SRGB;
    case AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM:
    case AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM_SRGB:
        return AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM_SRGB;
    default:
        return AURORA_MTL_PIXEL_FORMAT_INVALID;
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

void ReleaseEvent(void*& event) noexcept {
    if (event != nullptr) {
        CFRelease(static_cast<CFTypeRef>(event));
    }
    event = nullptr;
}

} // namespace

class OpenXRMetalBackend::Impl final {
public:
    explicit Impl(OpenXRLogCallback logger) : logger_(std::move(logger)) {}

    ~Impl() { Shutdown(); }

    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<XrSwapchainImageMetalMKW> images;
        uint32_t acquired_index = 0;
        bool acquired = false;
        bool waited = false;
        bool release_forbidden = false;
    };

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime) {
        ClearError();
        if (!runtime.IsInitialized() || runtime.HasSession()) {
            return Fail("OpenXR must own an instance, but no session, before the Metal backend is queried");
        }
        if (requirements_queried_ && runtime_ != &runtime) {
            return Fail("Metal graphics requirements were already queried from another OpenXR instance");
        }
        if (xr_visionos_layer_renderer() == nullptr) {
            return Fail("no CompositorServices layer renderer: the immersive space has to be open before the game starts");
        }
        runtime_ = &runtime;
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = false;
            submission_unsafe_ = false;
        }
        requirements_queried_ = true;
        Log(OpenXRLogLevel::Info, "OpenXR Metal backend: CompositorServices layer renderer present");
        return true;
    }

    bool BindAurora(OpenXRRuntime& runtime) {
        ClearError();
        if (!requirements_queried_ || runtime_ != &runtime || runtime.HasSession()) {
            return Fail("QueryGraphicsRequirements must succeed on this OpenXR instance before BindAurora");
        }
        if (bound_) {
            return Fail("OpenXR Metal backend is already bound");
        }
        AuroraMetalNativeHandles handles{};
        if (!aurora_metal_get_native_handles(&handles)) {
            return Fail("Aurora did not expose a Dawn Metal device");
        }
        if (!handles.sharedTextureMemoryIOSurface || !handles.sharedFenceMTLSharedEvent) {
            return Fail("Aurora's Dawn device lacks the IOSurface shared-memory or MTLSharedEvent features");
        }
        aurora_format_ = handles.colorMetalPixelFormat;
        requirements_.aurora_pixel_format = aurora_format_;

        XrGraphicsBindingMetalMKW binding{XR_TYPE_GRAPHICS_BINDING_METAL_MKW, nullptr, nullptr};
        if (!runtime.CreateSession(&binding)) {
            return Fail("the visionOS OpenXR provider refused to create a session: " + std::string(xr_visionos_last_error()));
        }
        owns_session_ = true;
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
            return Fail("OpenXR session became loss-pending while creating Metal swapchains");
        }
        if (!aurora_metal_enable_stereo_bridge(&Impl::OnAuroraSubmitted, this)) {
            DestroySwapchains();
            runtime.DestroySession();
            owns_session_ = false;
            return Fail("Aurora could not enable its IOSurface stereo bridge");
        }
        bridge_enabled_ = true;
        bound_ = true;

        std::ostringstream message;
        message << "OpenXR Metal swapchains ready: pixel format " << swapchain_format_ << ", eyes "
                << eye_swapchains_[0].width << 'x' << eye_swapchains_[0].height << " / " << eye_swapchains_[1].width
                << 'x' << eye_swapchains_[1].height;
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    OpenXRBeginStatus BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame) {
        frame = {};
        frame.presentation = presentation;
        if (!bound_ || runtime_ == nullptr) {
            Fail("BeginFrame called before the Metal backend was bound");
            return OpenXRBeginStatus::Error;
        }
        if (frame_active_ || pending_packet_serial_ != 0) {
            Fail("BeginFrame called while another OpenXR frame is active");
            return OpenXRBeginStatus::Error;
        }
        ApplyEnvironment(presentation);
        if (!ResizeWritablePair()) {
            return OpenXRBeginStatus::Error;
        }
        const OpenXRFrameStatus status = runtime_->WaitFrame(frame.xr_frame);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(BeginStatusOperation(status));
            }
            return status == OpenXRFrameStatus::SessionNotRunning ? OpenXRBeginStatus::SessionNotRunning
                 : status == OpenXRFrameStatus::ExitRequested   ? OpenXRBeginStatus::ExitRequested
                                                                 : OpenXRBeginStatus::Error;
        }
        NoteDisplayTiming(frame.xr_frame);
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
        return PrepareTargets(frame);
    }

    OpenXRBeginStatus PrepareTargets(OpenXRBackendFrame& frame) {
        const uint32_t target_count =
            frame.presentation.mode == OpenXRFrameMode::VirtualScreen ? 1u : kOpenXREyeCount;
        if (target_count == 1) {
            frame.render_width[1] = frame.render_width[0];
            frame.render_height[1] = frame.render_height[0];
        }
        std::array<AuroraMetalStereoTarget, kOpenXREyeCount> targets{};
        const diagnostics::Stopwatch acquire_timer;
        for (uint32_t eye = 0; eye < target_count; ++eye) {
            auto& swapchain = eye_swapchains_[eye];
            if (!AcquireSwapchain(swapchain)) {
                ReleaseAcquiredSwapchains();
                EndActiveFrameWithoutLayers(frame.xr_frame);
                return OpenXRBeginStatus::Error;
            }
            targets[eye] = TargetFor(swapchain);
        }
        AuroraMetalStereoTarget panel_target{};
        const bool panel = frame.presentation.panel.requested && EnsurePanelSwapchains();
        frame.presentation.panel.requested = panel;
        if (panel) {
            if (!AcquireSwapchain(panel_swapchain_)) {
                ReleaseAcquiredSwapchains();
                EndActiveFrameWithoutLayers(frame.xr_frame);
                return OpenXRBeginStatus::Error;
            }
            panel_target = TargetFor(panel_swapchain_);
        }
        diagnostics::OnSwapchainAcquire(acquire_timer);

        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = frame.xr_frame.serial;
            submitted_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
            ClearReleasesLocked();
        }
        if (!diagnostics::Measure(diagnostics::Stage::SetTargets, [&] {
                return aurora_metal_set_stereo_targets_with_panel(frame.xr_frame.serial, targets.data(), target_count,
                                                                  panel ? &panel_target : nullptr);
            })) {
            {
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = 0;
            }
            ReleaseAcquiredSwapchains();
            Fail("Aurora rejected the acquired OpenXR Metal swapchain target");
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRBeginStatus::Error;
        }
        frame.expects_gpu_submission = true;
        return OpenXRBeginStatus::Ready;
    }

    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
        packet = {};
        packet.presentation = presentation;
        if (!bound_ || runtime_ == nullptr || frame_active_ || pending_packet_serial_ != 0) {
            Fail("PreparePacket called before binding or with work pending");
            return OpenXRBeginStatus::Error;
        }
        if (runtime_->ShouldExit()) return OpenXRBeginStatus::ExitRequested;
        if (!runtime_->IsSessionRunning()) return OpenXRBeginStatus::SessionNotRunning;
        ApplyEnvironment(presentation);
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
            Fail("xrLocateViews failed for a Metal packet");
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
        if (frame_active_ || pending_packet_serial_ == 0 || packet.xr_frame.serial != pending_packet_serial_ ||
            !packet.expects_gpu_submission || !aurora_metal_cancel_stereo_targets(packet.xr_frame.serial)) {
            return false;
        }
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = submitted_token_ = 0;
            submission_arrived_ = submission_success_ = submission_unsafe_ = false;
            ClearReleasesLocked();
        }
        packet.expects_gpu_submission = false;
        pending_packet_serial_ = 0;
        const diagnostics::Stopwatch release_timer;
        const bool released = ReleaseAcquiredSwapchains();
        diagnostics::OnSwapchainRelease(release_timer);
        if (!released) pending_packet_serial_ = packet.xr_frame.serial;
        return true;
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
                 : status == OpenXRFrameStatus::ExitRequested   ? OpenXRBeginStatus::ExitRequested
                                                                 : OpenXRBeginStatus::Error;
        }
        NoteDisplayTiming(active_frame_);
        if (!runtime_->BeginFrame(active_frame_)) {
            Fail("xrBeginFrame failed for a Metal compositor cycle");
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
            Fail("OpenXR could not resubmit the retained Metal frame");
            return OpenXRBeginStatus::Error;
        }
        return OpenXRBeginStatus::Ready;
    }

    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
        frame = {};
        if (!bound_ || runtime_ == nullptr || frame_active_ || pending_packet_serial_ == 0 ||
            packet.xr_frame.serial != pending_packet_serial_ || !packet.expects_gpu_submission ||
            WaitForSubmission(packet, 0) != OpenXRSubmissionStatus::Success) {
            Fail("BeginFrameForPacket requires the completed current Metal packet");
            return OpenXRBeginStatus::Error;
        }
        const auto status = BeginCompositorCycle();
        if (status != OpenXRBeginStatus::Ready) {
            if (status == OpenXRBeginStatus::SessionNotRunning) {
                const bool released = ReleaseAcquiredSwapchains();
                pending_packet_serial_ = 0;
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = submitted_token_ = 0;
                submission_arrived_ = submission_success_ = submission_unsafe_ = false;
                ClearReleasesLocked();
                if (!released) return OpenXRBeginStatus::Error;
            }
            return status;
        }
        frame = packet;
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
            Fail("CopyRenderedEyes received a stale Metal frame");
            return OpenXRSubmissionStatus::Failed;
        }
        // Aurora rendered straight into the swapchain images; the compositor reads
        // them after the release fences handed over in FinishFrame.
        return WaitForSubmission(frame, 0);
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
        if (submission_success_) {
            return OpenXRSubmissionStatus::Success;
        }
        return submission_unsafe_ ? OpenXRSubmissionStatus::Failed : OpenXRSubmissionStatus::Skipped;
    }

    bool TryCancelPendingFrame(OpenXRBackendFrame& frame) {
        if (!frame_active_ || !frame.expects_gpu_submission || frame.xr_frame.serial != active_frame_serial_) {
            return false;
        }
        if (!aurora_metal_cancel_stereo_targets(frame.xr_frame.serial)) {
            return false;
        }
        std::lock_guard lock(submission_mutex_);
        awaiting_token_ = 0;
        submitted_token_ = 0;
        submission_arrived_ = false;
        submission_success_ = false;
        submission_unsafe_ = false;
        ClearReleasesLocked();
        frame.expects_gpu_submission = false;
        return true;
    }

    bool FinishFrame(OpenXRBackendFrame& frame, bool submit_layer) {
        if (!frame_active_ || runtime_ == nullptr || frame.xr_frame.serial != active_frame_serial_) {
            return Fail("FinishFrame received a stale or inactive OpenXR frame token");
        }
        bool submission_unsafe = false;
        std::array<AuroraMetalStereoRelease, AURORA_METAL_STEREO_MAX_RELEASES> releases{};
        uint32_t release_count = 0;
        {
            std::lock_guard lock(submission_mutex_);
            submission_unsafe = submission_arrived_ && submitted_token_ == frame.xr_frame.serial && submission_unsafe_;
            if (submission_arrived_ && submitted_token_ == frame.xr_frame.serial && submission_success_) {
                releases = releases_;
                release_count = release_count_;
                releases_ = {};
                release_count_ = 0;
            }
        }
        if (submission_unsafe) {
            AbandonAcquiredSwapchains();
            Fail("Aurora's Metal stereo submission failed after GPU work may have been queued");
        }
        // Before the images are released: the compositor's read of each one waits
        // for Dawn's copy into it.
        HandOverReleaseFences(frame, releases, release_count);
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
            // xrEndFrame shows the most recently released image of a swapchain; keep
            // the displayed pair apart from the pair Aurora writes or cancels next.
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
            ClearReleasesLocked();
        }
        return release_ok && end_ok;
    }

    bool RepeatFrame(const OpenXRBackendFrame& frame) {
        if (!frame_active_ || runtime_ == nullptr || frame.xr_frame.serial != active_frame_serial_) {
            return Fail("RepeatFrame received a stale or inactive render token");
        }
        const bool end_ok = EndRetainedFrame(false);
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

    bool EndRetainedFrame(bool fresh) {
        if (!runtime_->IsSessionRunning()) {
            return true;
        }
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
            return runtime_->EndFrameWithoutLayers(active_frame_);
        }
        diagnostics::OnLayer(fresh);
        const auto& frame = retained_frame_;
        if (frame.presentation.mode == OpenXRFrameMode::VirtualScreen) {
            const auto& swapchain = retained_swapchains_[0];
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags = 0;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = swapchain.handle;
            // Only the part of the eye-sized image Aurora drew into, so the room (or
            // black) frames the picture rather than the image's own bands.
            quad.subImage.imageRect =
                OpenXRVirtualScreenContentRect(swapchain.width, swapchain.height, frame.presentation.quad_content_aspect);
            quad.subImage.imageArrayIndex = 0;
            if (frame.presentation.quad_anchored) {
                quad.space = runtime_->AppSpace();
                quad.pose = frame.presentation.quad_pose;
            } else {
                quad.space = runtime_->ViewSpace();
                quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                quad.pose.position = {0.0f, 0.0f, -std::max(0.25f, frame.presentation.quad_distance_meters)};
            }
            const float meters_per_pixel =
                std::max(0.25f, frame.presentation.quad_width_meters) / static_cast<float>(std::max(1u, swapchain.width));
            quad.size.width = meters_per_pixel * static_cast<float>(quad.subImage.imageRect.extent.width);
            quad.size.height = meters_per_pixel * static_cast<float>(quad.subImage.imageRect.extent.height);
            return EndFrameWithPanel(frame, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
        }
        std::array<XrCompositionLayerProjectionView, kOpenXREyeCount> views{};
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[eye].pose = frame.xr_frame.views[eye].pose;
            views[eye].fov = frame.xr_frame.views[eye].fov;
            views[eye].subImage.swapchain = retained_swapchains_[eye].handle;
            // The immersive window's eyes fill only the top-left part of the image.
            views[eye].subImage.imageRect = {
                {0, 0},
                {static_cast<int32_t>(std::min(frame.render_width[eye], retained_swapchains_[eye].width)),
                 static_cast<int32_t>(std::min(frame.render_height[eye], retained_swapchains_[eye].height))}};
            views[eye].subImage.imageArrayIndex = 0;
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.layerFlags =
            frame.presentation.immersive_window ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        projection.space = runtime_->AppSpace();
        projection.viewCount = kOpenXREyeCount;
        projection.views = views.data();
        return EndFrameWithPanel(frame, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
    }

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
            bridge_drained = aurora_metal_disable_stereo_bridge();
            bridge_enabled_ = false;
        }
        {
            std::lock_guard lock(submission_mutex_);
            ClearReleasesLocked();
        }
        if (!bridge_drained) {
            AbandonAcquiredSwapchains();
            shutdown_unsafe_ = true;
            Fail("Metal queue completion is unknown; retaining the OpenXR session and graphics owners");
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
    const OpenXRMetalGraphicsRequirements& GraphicsRequirements() const { return requirements_; }
    int64_t SwapchainFormat() const { return swapchain_format_; }
    const std::string& LastError() const { return last_error_; }

private:
    // The room around the virtual screen and the immersive window: the frame's
    // drawable is cleared transparent and the layers blended over it, which a
    // mixed-immersion space shows the surroundings through.
    void ApplyEnvironment(const OpenXRPresentation& presentation) {
        if (runtime_ != nullptr && runtime_->HasSession()) {
            xr_visionos_set_frame_environment(runtime_->Session(), presentation.passthrough);
        }
    }

    AuroraMetalStereoTarget TargetFor(const EyeSwapchain& swapchain) {
        AuroraMetalStereoTarget target{};
        const XrSwapchainImageMetalMKW& image = swapchain.images[swapchain.acquired_index];
        target.ioSurface = image.ioSurface;
        target.width = swapchain.width;
        target.height = swapchain.height;
        target.metalPixelFormat = swapchain_format_;
        void* event = nullptr;
        uint64_t value = 0;
        if (XR_SUCCEEDED(xr_visionos_swapchain_image_acquire_fence(swapchain.handle, swapchain.acquired_index, &event,
                                                                    &value))) {
            target.acquireEvent = event;
            target.acquireValue = value;
        }
        return target;
    }

    // Dawn's release fences, in the order the targets were given (eyes, then the
    // panel), to the images that are about to be released.
    void HandOverReleaseFences(const OpenXRBackendFrame& frame,
                               std::array<AuroraMetalStereoRelease, AURORA_METAL_STEREO_MAX_RELEASES>& releases,
                               uint32_t release_count) {
        uint32_t n = 0;
        const uint32_t target_count =
            frame.presentation.mode == OpenXRFrameMode::VirtualScreen ? 1u : kOpenXREyeCount;
        const auto hand = [&](EyeSwapchain& swapchain) {
            void* event = n < release_count ? releases[n].releaseEvent : nullptr;
            const uint64_t value = n < release_count ? releases[n].releaseValue : 0;
            if (swapchain.acquired && swapchain.handle != XR_NULL_HANDLE) {
                xr_visionos_swapchain_image_set_release_fence(swapchain.handle, swapchain.acquired_index, event, value);
            }
            ++n;
        };
        for (uint32_t eye = 0; eye < target_count; ++eye) {
            hand(eye_swapchains_[eye]);
        }
        if (frame.presentation.panel.requested) {
            hand(panel_swapchain_);
        }
        for (uint32_t i = 0; i < release_count; ++i) {
            ReleaseEvent(releases[i].releaseEvent);
        }
    }

    bool SelectSwapchainFormat() {
        const auto& formats = runtime_->SwapchainFormats();
        // Aurora's UNORM target holds gamma-encoded bytes; the sRGB sibling has
        // the compositor decode them as such (see the D3D12 backend).
        const int64_t srgb = SrgbSibling(aurora_format_);
        if (srgb != AURORA_MTL_PIXEL_FORMAT_INVALID && std::find(formats.begin(), formats.end(), srgb) != formats.end()) {
            swapchain_format_ = srgb;
            return true;
        }
        if (std::find(formats.begin(), formats.end(), aurora_format_) != formats.end()) {
            swapchain_format_ = aurora_format_;
            return true;
        }
        const auto compatible = std::find_if(formats.begin(), formats.end(),
                                             [&](int64_t format) { return SameCopyFamily(aurora_format_, format); });
        if (compatible == formats.end()) {
            return Fail("the visionOS provider offered no swapchain format copy-compatible with Aurora's Metal format");
        }
        swapchain_format_ = *compatible;
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

public:
    // [vr] render_scale changed live: the next writable pair is rebuilt at the new size
    // (ResizeWritablePair, from the pacing thread), as the D3D12 backend does.
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

private:
    // Rebuilds the pair Aurora writes next at eye_size_ when it is another size. Only that pair,
    // with none of its images acquired, never the one on display. The new swapchains are made
    // before the old ones go, so a size the provider cannot allocate leaves the pair as it was.
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
            message << "OpenXR Metal eyes stay " << current[0].width << 'x' << current[0].height
                    << ": the provider could not make " << eye_size_[0].width << 'x' << eye_size_[0].height
                    << " swapchains (" << last_error_ << ')';
            Log(OpenXRLogLevel::Warning, message.str());
            ClearError();
            eye_size_ = current;
            return true;
        }
        // The compositor may still be reading the old pair (kOpenXRRetiredSwapchainCycles).
        retired_pairs_.push_back({std::move(eye_swapchains_), kOpenXRRetiredSwapchainCycles});
        eye_swapchains_ = std::move(replacement);
        std::ostringstream message;
        message << "OpenXR Metal eyes resized: " << eye_size_[0].width << 'x' << eye_size_[0].height << " / "
                << eye_size_[1].width << 'x' << eye_size_[1].height;
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    // Destroys the retired pairs whose time is up (every one of them at shutdown), once Aurora
    // has dropped its imports of their IOSurfaces. A pair Aurora cannot let go of yet (a frame
    // is pending) waits for a later cycle, or for xrDestroySession at shutdown.
    void ReapRetiredPairs(bool all) {
        for (auto it = retired_pairs_.begin(); it != retired_pairs_.end();) {
            if (!all && it->cycles_left > 1) {
                --it->cycles_left;
                ++it;
                continue;
            }
            std::vector<void*> surfaces;
            for (const EyeSwapchain& swapchain : it->swapchains) {
                for (const auto& image : swapchain.images) {
                    surfaces.push_back(image.ioSurface);
                }
            }
            if (aurora_metal_forget_stereo_targets(surfaces.data(), static_cast<uint32_t>(surfaces.size()))) {
                DestroySwapchainPair(it->swapchains);
                it = retired_pairs_.erase(it);
            } else if (all) {
                Log(OpenXRLogLevel::Warning,
                    "Aurora could not drop a replaced Metal eye swapchain pair; leaving it to xrDestroySession");
                it = retired_pairs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void DestroySwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
        for (auto& swapchain : pair) {
            if (swapchain.handle != XR_NULL_HANDLE && !swapchain.acquired) {
                xrDestroySwapchain(swapchain.handle);
            }
            swapchain = {};
        }
    }

    bool CreateSwapchain(EyeSwapchain& swapchain, uint32_t width, uint32_t height, const char* what) {
        swapchain.width = width;
        swapchain.height = height;
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create.format = swapchain_format_;
        create.sampleCount = 1;
        create.width = width;
        create.height = height;
        create.faceCount = 1;
        create.arraySize = 1;
        create.mipCount = 1;
        XrResult result = xrCreateSwapchain(runtime_->Session(), &create, &swapchain.handle);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrCreateSwapchain failed for the Metal " << what << " swapchain (" << result << ')';
            return Fail(message.str());
        }
        uint32_t count = 0;
        result = xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr);
        ObserveResult(result);
        if (XR_FAILED(result) || count == 0) {
            return Fail("the visionOS provider returned no swapchain images");
        }
        swapchain.images.resize(count);
        for (auto& image : swapchain.images) {
            image = {XR_TYPE_SWAPCHAIN_IMAGE_METAL_MKW, nullptr, nullptr, nullptr, 0};
        }
        result = xrEnumerateSwapchainImages(swapchain.handle, count, &count,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrEnumerateSwapchainImages failed for a Metal swapchain");
        }
        return true;
    }

    bool AcquireSwapchain(EyeSwapchain& swapchain) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result = xrAcquireSwapchainImage(swapchain.handle, &acquire, &swapchain.acquired_index);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrAcquireSwapchainImage failed for a Metal swapchain");
        }
        swapchain.acquired = true;
        swapchain.waited = false;
        swapchain.release_forbidden = false;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrWaitSwapchainImage failed for a Metal swapchain");
        }
        swapchain.waited = true;
        if (swapchain.acquired_index >= swapchain.images.size()) {
            return Fail("the visionOS provider returned an out-of-range swapchain image index");
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
            Log(OpenXRLogLevel::Warning, "cannot release a Metal swapchain image whose wait did not complete");
            return false;
        }
        if (swapchain.release_forbidden) {
            Log(OpenXRLogLevel::Warning, "deferring a Metal swapchain image after an unsafe GPU submission");
            return false;
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult result = xrReleaseSwapchainImage(swapchain.handle, &release);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrReleaseSwapchainImage failed for a Metal swapchain");
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
        Log(OpenXRLogLevel::Warning, "the settings panel could not get its own layer; drawing it into the eyes");
        return false;
    }

    void DestroyPanelSwapchains() {
        for (auto* swapchain : {&panel_swapchain_, &retained_panel_swapchain_}) {
            if (swapchain->handle != XR_NULL_HANDLE && !swapchain->acquired) {
                xrDestroySwapchain(swapchain->handle);
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
        swapchain_format_ = AURORA_MTL_PIXEL_FORMAT_INVALID;
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

    // Under submission_mutex_.
    void ClearReleasesLocked() noexcept {
        for (uint32_t i = 0; i < release_count_; ++i) {
            ReleaseEvent(releases_[i].releaseEvent);
        }
        releases_ = {};
        release_count_ = 0;
    }

    // Aurora's frame worker, queue-submit mutex held: publish only.
    static void OnAuroraSubmitted(uint64_t token, bool success, bool gpu_work_queued,
                                  const AuroraMetalStereoRelease* releases, uint32_t release_count, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr) {
            return;
        }
        {
            std::lock_guard lock(self->submission_mutex_);
            if (token != self->awaiting_token_) {
                for (uint32_t i = 0; i < release_count; ++i) {
                    void* event = releases[i].releaseEvent;
                    ReleaseEvent(event);
                }
                return;
            }
            self->ClearReleasesLocked();
            const uint32_t kept = std::min<uint32_t>(release_count, AURORA_METAL_STEREO_MAX_RELEASES);
            for (uint32_t i = 0; i < kept; ++i) {
                self->releases_[i] = releases[i]; // retained (+1) by the bridge for us
            }
            for (uint32_t i = kept; i < release_count; ++i) {
                void* event = releases[i].releaseEvent;
                ReleaseEvent(event);
            }
            self->release_count_ = success ? kept : 0;
            if (!success) {
                self->ClearReleasesLocked();
            }
            self->submitted_token_ = token;
            self->submission_success_ = success;
            self->submission_arrived_ = true;
            self->submission_unsafe_ = !success && gpu_work_queued;
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
    OpenXRMetalGraphicsRequirements requirements_{};
    std::array<EyeSwapchain, kOpenXREyeCount> eye_swapchains_{};
    std::array<EyeSwapchain, kOpenXREyeCount> retained_swapchains_{};
    EyeSwapchain panel_swapchain_{};
    EyeSwapchain retained_panel_swapchain_{};
    bool panel_swapchains_ready_ = false;
    bool panel_layer_failed_ = false;
    bool retained_panel_valid_ = false;
    // The eye size the next writable pair is made at, and the one last asked for.
    std::array<OpenXREyeSize, kOpenXREyeCount> eye_size_{};
    std::array<OpenXREyeSize, kOpenXREyeCount> requested_eye_size_{};
    // Pairs replaced by a new size, destroyed once the compositor has moved on from them.
    struct RetiredPair {
        std::array<EyeSwapchain, kOpenXREyeCount> swapchains;
        uint32_t cycles_left;
    };
    std::vector<RetiredPair> retired_pairs_;
    OpenXRBackendFrame retained_frame_{};
    uint64_t retained_session_serial_ = 0;
    uint64_t retained_space_serial_ = 0;
    bool have_retained_frame_ = false;
    int64_t aurora_format_ = AURORA_MTL_PIXEL_FORMAT_INVALID;
    int64_t swapchain_format_ = AURORA_MTL_PIXEL_FORMAT_INVALID;
    std::string last_error_;

    std::mutex submission_mutex_;
    std::condition_variable submission_cv_;
    uint64_t awaiting_token_ = 0;
    uint64_t submitted_token_ = 0;
    bool submission_arrived_ = false;
    bool submission_success_ = false;
    bool submission_unsafe_ = false;
    bool shutting_down_ = false;
    std::array<AuroraMetalStereoRelease, AURORA_METAL_STEREO_MAX_RELEASES> releases_{};
    uint32_t release_count_ = 0;

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

OpenXRMetalBackend::OpenXRMetalBackend(OpenXRLogCallback logger) : m_impl(std::make_unique<Impl>(std::move(logger))) {}
OpenXRMetalBackend::~OpenXRMetalBackend() = default;
bool OpenXRMetalBackend::QueryGraphicsRequirements(OpenXRRuntime& runtime) { return m_impl->QueryGraphicsRequirements(runtime); }
bool OpenXRMetalBackend::BindAurora(OpenXRRuntime& runtime) { return m_impl->BindAurora(runtime); }
void OpenXRMetalBackend::SetRenderScale(float scale) { m_impl->SetRenderScale(scale); }
OpenXRBeginStatus OpenXRMetalBackend::BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame) {
    return m_impl->BeginFrame(presentation, frame);
}
OpenXRSubmissionStatus OpenXRMetalBackend::WaitForSubmission(const OpenXRBackendFrame& frame, uint32_t timeout_ms) {
    return m_impl->WaitForSubmission(frame, timeout_ms);
}
bool OpenXRMetalBackend::TryCancelPendingFrame(OpenXRBackendFrame& frame) { return m_impl->TryCancelPendingFrame(frame); }
bool OpenXRMetalBackend::RepeatFrame(const OpenXRBackendFrame& frame) { return m_impl->RepeatFrame(frame); }
bool OpenXRMetalBackend::FinishFrame(OpenXRBackendFrame& frame, bool submit_layer) { return m_impl->FinishFrame(frame, submit_layer); }
OpenXRBeginStatus OpenXRMetalBackend::PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
    return m_impl->PreparePacket(presentation, packet);
}
bool OpenXRMetalBackend::TryCancelPendingPacket(OpenXRBackendFrame& packet) { return m_impl->TryCancelPendingPacket(packet); }
OpenXRBeginStatus OpenXRMetalBackend::BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
    return m_impl->BeginFrameForPacket(packet, frame);
}
OpenXRSubmissionStatus OpenXRMetalBackend::CopyRenderedEyes(const OpenXRBackendFrame& frame) { return m_impl->CopyRenderedEyes(frame); }
OpenXRBeginStatus OpenXRMetalBackend::KeepAliveCycle() { return m_impl->KeepAliveCycle(); }
bool OpenXRMetalBackend::Shutdown() { return m_impl->Shutdown(); }
bool OpenXRMetalBackend::IsBound() const { return m_impl->IsBound(); }
bool OpenXRMetalBackend::PanelLayerAvailable() const { return m_impl->PanelLayerAvailable(); }
const OpenXRMetalGraphicsRequirements& OpenXRMetalBackend::GraphicsRequirements() const { return m_impl->GraphicsRequirements(); }
int64_t OpenXRMetalBackend::SwapchainFormat() const { return m_impl->SwapchainFormat(); }
const std::string& OpenXRMetalBackend::LastError() const { return m_impl->LastError(); }

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR) && defined(MKW_PLATFORM_VISIONOS)
