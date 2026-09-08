// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "vr/openxr_integration.h"

#include "runtime_config.h"
#include "runtime_log.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/mkw_vr_policy.h"
#include "vr/mkw_vr_instrumentation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(MKW_ENABLE_OPENXR)
#include "vr/openxr_runtime.h"
#if defined(_WIN32)
#include "vr/openxr_d3d12.h"
#else
#include "vr/openxr_vulkan_backend.h"
#endif
#endif

namespace mkw::vr {
namespace {

inline constexpr float kDegreesToRadians = 0.01745329252f;

void ConfigurePolicy(bool enabled) noexcept {
    MkwVRPolicyReset();
    MkwVRPolicyConfig config{};
    config.enabled = enabled;
    config.immersive_races = true;
    config.world_units_per_meter = RuntimeConfigFile::VrWorldUnitsPerMeter(500.0f);
    config.hud_distance_meters = RuntimeConfigFile::VrHudDistanceMeters(2.0f);
    config.hud_width_meters = RuntimeConfigFile::VrHudWidthMeters(2.4f);
    config.first_person_units_per_meter = RuntimeConfigFile::VrFirstPersonUnitsPerMeter();
    MkwVRPolicyConfigure(config);
    MkwVRInstrumentationInitialize();
    MkwVRFirstPersonApplyConfiguredSettings();
}

#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quaternion Normalize(Quaternion value) noexcept {
    const float length_squared = value.x * value.x + value.y * value.y +
                                 value.z * value.z + value.w * value.w;
    if (!(length_squared > 1.0e-12f)) {
        return {};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    value.w *= inverse_length;
    return value;
}

Quaternion Conjugate(Quaternion value) noexcept {
    return {-value.x, -value.y, -value.z, value.w};
}

Quaternion Multiply(const Quaternion& left, const Quaternion& right) noexcept {
    return Normalize({
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
    });
}

std::array<float, 3> Rotate(const Quaternion& q, const std::array<float, 3>& value) noexcept {
    // Expanded q * [v,0] * conjugate(q), avoiding two temporary normalizations.
    const float tx = 2.0f * (q.y * value[2] - q.z * value[1]);
    const float ty = 2.0f * (q.z * value[0] - q.x * value[2]);
    const float tz = 2.0f * (q.x * value[1] - q.y * value[0]);
    return {
        value[0] + q.w * tx + (q.y * tz - q.z * ty),
        value[1] + q.w * ty + (q.z * tx - q.x * tz),
        value[2] + q.w * tz + (q.x * ty - q.y * tx),
    };
}

void RotationMatrix(const Quaternion& value, float matrix[9]) noexcept {
    const Quaternion q = Normalize(value);
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    matrix[0] = 1.0f - 2.0f * (yy + zz);
    matrix[1] = 2.0f * (xy - wz);
    matrix[2] = 2.0f * (xz + wy);
    matrix[3] = 2.0f * (xy + wz);
    matrix[4] = 1.0f - 2.0f * (xx + zz);
    matrix[5] = 2.0f * (yz - wx);
    matrix[6] = 2.0f * (xz - wy);
    matrix[7] = 2.0f * (yz + wx);
    matrix[8] = 1.0f - 2.0f * (xx + yy);
}

// Midpoint between the eyes: the head position the tracking origin is latched
// to. Callers check XR_VIEW_STATE_POSITION_VALID_BIT first.
std::array<float, 3> CenterPosition(const OpenXRFrame& frame) noexcept {
    const auto& left = frame.views[0].pose;
    const auto& right = frame.views[1].pose;
    return {
        (left.position.x + right.position.x) * 0.5f,
        (left.position.y + right.position.y) * 0.5f,
        (left.position.z + right.position.z) * 0.5f,
    };
}

// Places an upright screen `distance` metres ahead of the head. Only the
// head's yaw is used, so the screen is never pitched or rolled by whatever the
// player's head happened to be doing when it was anchored.
XrPosef ScreenPoseAhead(const OpenXRFrame& frame, float distance) noexcept {
    const auto& q = frame.views[0].pose.orientation;
    const float yaw =
        std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const std::array<float, 3> center = CenterPosition(frame);
    XrPosef pose{};
    pose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    pose.position = {center[0] - std::sin(yaw) * distance, center[1],
                     center[2] - std::cos(yaw) * distance};
    return pose;
}

void IdentityEye(AuroraStereoEye& eye) noexcept {
    std::fill(std::begin(eye.projection), std::end(eye.projection), 0.0f);
    eye.projection[0] = 1.0f;
    eye.projection[5] = 1.0f;
    eye.projection[10] = 1.0f;
    eye.projection[15] = 1.0f;
    std::fill(std::begin(eye.viewFromCenter), std::end(eye.viewFromCenter), 0.0f);
    eye.viewFromCenter[0] = 1.0f;
    eye.viewFromCenter[5] = 1.0f;
    eye.viewFromCenter[10] = 1.0f;
}

void ProjectionFromFov(const XrFovf& fov, float output[16]) noexcept {
    const float left = std::tan(fov.angleLeft);
    const float right = std::tan(fov.angleRight);
    const float down = std::tan(fov.angleDown);
    const float up = std::tan(fov.angleUp);
    const float inverse_width = 1.0f / (right - left);
    const float inverse_height = 1.0f / (up - down);
    std::fill(output, output + 16, 0.0f);
    output[0] = 2.0f * inverse_width;
    output[2] = (right + left) * inverse_width;
    output[5] = 2.0f * inverse_height;
    output[6] = (up + down) * inverse_height;
}

// The base is a position and nothing else. OpenXR keeps its reference spaces
// gravity-aligned, so handing the headset's rotation to the game camera as-is
// leaves the game's horizon level and its forward fixed to the reference space.
// Composing a latched head orientation in here instead would bake that instant's
// pitch and roll into the neutral and tilt the horizon for the rest of the session.
//
// lean_back_radians is the one deliberate exception: a fixed pitch of the game
// camera about the reference space's right axis, for a player sitting reclined.
// It multiplies in on the right, so it turns the world before the head rotation
// rather than after it, which is what makes it cancel a reclined head exactly
// and, when you then look sideways, roll the view the way a real recline would.
void ViewFromBase(const XrPosef& eye_pose, const std::array<float, 3>& base_position,
                  bool position_valid, float units_per_meter, float lean_back_radians,
                  float output[12]) noexcept {
    const Quaternion eye = Normalize({eye_pose.orientation.x, eye_pose.orientation.y,
                                      eye_pose.orientation.z, eye_pose.orientation.w});
    const Quaternion inverse_eye = Conjugate(eye);
    float rotation[9];
    if (lean_back_radians == 0.0f) {
        RotationMatrix(inverse_eye, rotation);
    } else {
        const float half_angle = 0.5f * lean_back_radians;
        const Quaternion lean{std::sin(half_angle), 0.0f, 0.0f, std::cos(half_angle)};
        RotationMatrix(Multiply(inverse_eye, lean), rotation);
    }

    std::array<float, 3> translation{};
    if (position_valid) {
        const std::array<float, 3> base_to_eye{
            base_position[0] - eye_pose.position.x,
            base_position[1] - eye_pose.position.y,
            base_position[2] - eye_pose.position.z,
        };
        translation = Rotate(inverse_eye, base_to_eye);
    }
    output[0] = rotation[0];
    output[1] = rotation[1];
    output[2] = rotation[2];
    output[3] = translation[0] * units_per_meter;
    output[4] = rotation[3];
    output[5] = rotation[4];
    output[6] = rotation[5];
    output[7] = translation[1] * units_per_meter;
    output[8] = rotation[6];
    output[9] = rotation[7];
    output[10] = rotation[8];
    output[11] = translation[2] * units_per_meter;
}

class OpenXRIntegration final {
public:
    static OpenXRIntegration& Get() {
        static OpenXRIntegration integration;
        return integration;
    }

    OpenXRStartupResult Prepare(AuroraConfig& aurora_config) {
        Shutdown();
        {
            std::lock_guard lock(error_mutex_);
            last_error_.clear();
        }
        if (graphics_retained_) {
            SetError("OpenXR cannot be restarted after an unfenceable D3D12 submission");
            return OpenXRStartupResult::Unavailable;
        }
        requested_ = RuntimeConfigFile::VrEnabled(false);
        ConfigurePolicy(requested_);
        if (!requested_) {
            return OpenXRStartupResult::Disabled;
        }
        if (aurora_config.desiredBackend != BACKEND_AUTO &&
            aurora_config.desiredBackend != BACKEND_D3D12) {
            SetError("OpenXR currently requires the D3D12 graphics backend on Windows");
            return OpenXRStartupResult::Unavailable;
        }

        logger_ = [](OpenXRLogLevel level, std::string_view message) {
            const char* name = level == OpenXRLogLevel::Error ? "error" :
                               level == OpenXRLogLevel::Warning ? "warning" : "info";
            RT_LOG(RT_TAG_RUNTIME) << "[openxr::" << name << "] " << message << std::endl;
        };
        runtime_ = std::make_unique<OpenXRRuntime>(logger_);
        backend_ = std::make_unique<OpenXRD3D12Backend>(logger_);

        OpenXRConfig config{};
        config.application_name = aurora_config.appName != nullptr ? aurora_config.appName
                                                                    : "WiiCompiled";
        config.engine_name = "Aurora";
        config.resolution_scale = RuntimeConfigFile::VrRenderScale(1.0f);
        config.required_extensions = {"XR_KHR_D3D12_enable"};
        if (!runtime_->Initialize(config)) {
            SetError("OpenXR instance initialization failed: " + runtime_->LastError().message);
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }
        if (!backend_->QueryGraphicsRequirements(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }

        const auto& requirements = backend_->GraphicsRequirements();
        aurora_config.desiredBackend = BACKEND_D3D12;
        aurora_config.xrInterop = true;
        aurora_config.hasD3D12AdapterLuid = true;
        aurora_config.d3d12AdapterLuidLow = requirements.adapter_luid_low;
        aurora_config.d3d12AdapterLuidHigh = requirements.adapter_luid_high;
        prepared_ = true;
        return OpenXRStartupResult::Prepared;
    }

    bool Start(AuroraBackend active_backend) {
        if (!prepared_ || runtime_ == nullptr || backend_ == nullptr) {
            return !requested_;
        }
        if (active_backend != BACKEND_D3D12) {
            SetError("Aurora could not create the OpenXR-required D3D12 backend");
            ResetPreparedObjects();
            return false;
        }
        if (!backend_->BindAurora(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return false;
        }

        stop_.store(false, std::memory_order_release);
        teardown_requested_.store(false, std::memory_order_release);
        WithdrawPublishedFrame();
        aurora_set_stereo_frame_provider(&OpenXRIntegration::ProvideStereoFrame, this);
        provider_registered_ = true;
        running_.store(true, std::memory_order_release);
        try {
            pacing_thread_ = std::thread([this] { PacingThread(); });
        } catch (const std::exception& exception) {
            running_.store(false, std::memory_order_release);
            aurora_set_stereo_frame_provider(nullptr, nullptr);
            provider_registered_ = false;
            SetError(std::string("could not start the OpenXR pacing thread: ") + exception.what());
            ResetPreparedObjects();
            return false;
        }
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR asynchronous D3D12 presentation started" << std::endl;
        return true;
    }

    void Shutdown() noexcept {
        teardown_requested_.store(false, std::memory_order_release);
        if (pacing_thread_.joinable()) {
            // Registration changes are only safe while no sealed frame is in
            // flight. The caller invokes us before Aurora teardown.
            aurora_quiesce_frame_worker();
            aurora_set_stereo_frame_provider(nullptr, nullptr);
            provider_registered_ = false;
            WithdrawPublishedFrame();
            {
                // Pair the predicate update with the wait mutex. Otherwise a
                // terminal pacing thread can observe false, miss the notify,
                // and make join wait forever.
                std::lock_guard lock(stop_mutex_);
                stop_.store(true, std::memory_order_release);
            }
            stop_cv_.notify_all();
            pacing_thread_.join();
        } else {
            if (provider_registered_) {
                aurora_quiesce_frame_worker();
                aurora_set_stereo_frame_provider(nullptr, nullptr);
                provider_registered_ = false;
            }
            ShutdownOrRetainGraphicsObjects();
        }
        running_.store(false, std::memory_order_release);
        MkwVRPolicySetSessionActive(false);
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
        ResetTrackingOrigin();
        applied_session_run_serial_ = 0;
        session_was_active_ = false;
    }

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    void RequestRecenter() noexcept {
        recenter_requested_.store(true, std::memory_order_release);
    }

    void SetLeanBackDegrees(float degrees) noexcept {
        lean_back_degrees_.store(
            std::clamp(degrees, -RuntimeConfigFile::kVrLeanBackDegreesLimit,
                       RuntimeConfigFile::kVrLeanBackDegreesLimit),
            std::memory_order_relaxed);
    }

    void ServiceProducerFrameBoundary() noexcept {
        if (teardown_requested_.load(std::memory_order_acquire)) {
            Shutdown();
        }
    }

    std::string LastError() const {
        std::lock_guard lock(error_mutex_);
        return last_error_;
    }

private:
    struct PublishedFrame {
        AuroraStereoFrame frame{};
    };

    void ResetPreparedObjects() {
        ShutdownOrRetainGraphicsObjects();
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
    }

    bool ShutdownOrRetainGraphicsObjects() noexcept {
        if (backend_ != nullptr && !backend_->Shutdown()) {
            RT_LOG(RT_TAG_RUNTIME)
                << "OpenXR D3D12 queue completion is unknown; retaining the backend, "
                   "runtime, session, and graphics resources until process exit"
                << std::endl;
            (void)backend_.release();
            (void)runtime_.release();
            graphics_retained_ = true;
            return false;
        }
        if (runtime_ != nullptr) {
            runtime_->Shutdown();
        }
        return true;
    }

    static bool ProvideStereoFrame(uint32_t, AuroraStereoFrame* output, void* userdata) {
        auto* self = static_cast<OpenXRIntegration*>(userdata);
        if (self == nullptr || output == nullptr) {
            return false;
        }
        // The packet storage is reused by the XR thread. Claim and copy it
        // under one short lock so cancellation cannot begin the next packet
        // while this callback is preempted between exchange and copy.
        std::lock_guard lock(self->published_mutex_);
        PublishedFrame* frame = self->published_.exchange(nullptr, std::memory_order_acq_rel);
        if (frame == nullptr) {
            return false;
        }
        *output = frame->frame;
        return true;
    }

    void PacingThread() noexcept {
        bool fatal = false;
        bool presentation_logged = false;
        VRPresentationMode logged_presentation = VRPresentationMode::Desktop;
        uint32_t presentation_log_count = 0;
        bool immersive_submission_logged = false;
        while (!stop_.load(std::memory_order_acquire) && !fatal) {
            const OpenXREventStatus events = runtime_->PollEvents();
            const bool session_active = runtime_->IsSessionRunning();
            MkwVRPolicySetSessionActive(session_active);
            const uint64_t session_run_serial = runtime_->SessionRunSerial();
            if (session_run_serial != applied_session_run_serial_) {
                applied_session_run_serial_ = session_run_serial;
                ResetTrackingOrigin();
            }
            if (session_active != session_was_active_) {
                session_was_active_ = session_active;
                if (!session_active) {
                    ResetTrackingOrigin();
                }
            }
            if (events == OpenXREventStatus::ExitRequested) {
                SetError("OpenXR runtime requested session exit; continuing on the desktop mirror");
                break;
            }
            if (events == OpenXREventStatus::Error) {
                SetError("OpenXR event processing failed: " + runtime_->LastError().message);
                break;
            }
            if (!session_active) {
                WaitForStopOrDelay(std::chrono::milliseconds(5));
                continue;
            }

            const MkwVRPolicySnapshot policy = MkwVRPolicyGetSnapshot();
            if ((!presentation_logged || policy.presentation != logged_presentation) &&
                presentation_log_count < 16) {
                presentation_logged = true;
                logged_presentation = policy.presentation;
                ++presentation_log_count;
                RT_LOG(RT_TAG_RUNTIME)
                    << "[mkw-vr] presentation="
                    << (policy.presentation == VRPresentationMode::ImmersiveRace
                            ? "immersive-race"
                            : policy.presentation == VRPresentationMode::VirtualScreen
                                  ? "virtual-screen"
                                  : "desktop")
                    << ", scene=" << static_cast<unsigned>(policy.scene.mode)
                    << ", screens=" << policy.scene.local_player_count
                    << ", camera-valid=" << policy.camera.valid
                    << ", scene-frame=" << policy.scene.guest_frame_index
                    << ", camera-frame=" << policy.camera.guest_frame_index
                    << ", bindings=0x" << std::hex << policy.available_bindings
                    << std::dec << std::endl;
            }
            OpenXRD3D12Presentation presentation{};
            const bool immersive = policy.presentation == VRPresentationMode::ImmersiveRace;
            presentation.mode = immersive ? OpenXRD3D12FrameMode::ImmersiveProjection
                                           : OpenXRD3D12FrameMode::VirtualScreen;
            presentation.quad_distance_meters = policy.config.hud_distance_meters;
            presentation.quad_width_meters = policy.config.hud_width_meters;

            OpenXRD3D12Frame frame{};
            const OpenXRD3D12BeginStatus begin = backend_->BeginFrame(presentation, frame);
            if (begin == OpenXRD3D12BeginStatus::SessionNotRunning) {
                MkwVRPolicySetSessionActive(false);
                continue;
            }
            if (begin == OpenXRD3D12BeginStatus::ExitRequested) {
                SetError("OpenXR runtime requested session exit; continuing on the desktop mirror");
                break;
            }
            if (begin == OpenXRD3D12BeginStatus::Error) {
                SetError(backend_->LastError());
                fatal = true;
                break;
            }

            // Both of these read this frame's located head pose and must run
            // before FinishFrame submits a layer built from it.
            ServiceRecenterRequest();
            UpdateVirtualScreenPose(frame);

            if (!frame.expects_gpu_submission) {
                if (!backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }

            {
                std::lock_guard lock(published_mutex_);
                // First person renders at life-size scale, third person at the
                // configured diorama scale. Head translation and IPD are the
                // only things this multiplies, so a one-frame disagreement with
                // the camera's own switch is not observable.
                BuildPublishedFrame(frame, immersive, policy.EffectiveUnitsPerMeter(),
                                    policy.content_tag);
                published_.store(&published_frame_, std::memory_order_release);
            }

            OpenXRD3D12SubmissionStatus submission = OpenXRD3D12SubmissionStatus::Timeout;
            bool canceled_before_encode = false;
            bool submission_stalled = false;
            const auto submission_deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (!stop_.load(std::memory_order_acquire) &&
                   submission == OpenXRD3D12SubmissionStatus::Timeout) {
                submission = backend_->WaitForSubmission(frame, 50);
                if (submission == OpenXRD3D12SubmissionStatus::Timeout) {
                    // A pause, minimized window, or guest stall may leave no GX
                    // frame to consume this packet. Withdraw it, then cancel the
                    // matching bridge target only if Encode has not taken ownership.
                    WithdrawPublishedFrame();
                    canceled_before_encode = backend_->TryCancelPendingFrame(frame);
                    if (canceled_before_encode) {
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= submission_deadline) {
                        // Cancellation may lose a race to completion. Recheck
                        // the callback-published predicate under the backend
                        // mutex before declaring a stall at the deadline.
                        submission = backend_->WaitForSubmission(frame, 0);
                        submission_stalled =
                            submission == OpenXRD3D12SubmissionStatus::Timeout;
                        break;
                    }
                }
            }
            WithdrawPublishedFrame();
            if (stop_.load(std::memory_order_acquire)) {
                // Aurora has been drained by Shutdown(); backend shutdown below
                // cancels its pending target, then either safely releases the
                // XR image or retains the entire graph if GPU completion is unknown.
                break;
            }
            if (canceled_before_encode) {
                if (!backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }
            if (submission_stalled) {
                SetError("Aurora did not complete the OpenXR stereo submission; "
                         "requesting a safe desktop fallback");
                fatal = true;
                break;
            }
            const bool submit = submission == OpenXRD3D12SubmissionStatus::Success;
            if (!backend_->FinishFrame(frame, submit)) {
                SetError(backend_->LastError());
                fatal = true;
            } else if (!submit) {
                SetError("Aurora's D3D12 stereo copy failed; continuing on the desktop mirror");
                fatal = true;
            } else if (immersive && !immersive_submission_logged) {
                immersive_submission_logged = true;
                RT_LOG(RT_TAG_RUNTIME)
                    << "[mkw-vr] first immersive packet consumed and submitted as "
                       "an OpenXR projection layer"
                    << std::endl;
            }
        }

        running_.store(false, std::memory_order_release);
        MkwVRPolicySetSessionActive(false);
        if (!stop_.load(std::memory_order_acquire)) {
            // A runtime/backend failure can happen while Aurora is submitting.
            // Ask the producer to reach a safe frame boundary, drain Aurora,
            // and unregister the provider before this XR owner destroys state.
            teardown_requested_.store(true, std::memory_order_release);
            std::unique_lock lock(stop_mutex_);
            stop_cv_.wait(lock, [this] { return stop_.load(std::memory_order_acquire); });
        }
        ShutdownOrRetainGraphicsObjects();
    }

    void BuildPublishedFrame(const OpenXRD3D12Frame& source, bool immersive,
                             float units_per_meter, uint64_t content_tag) noexcept {
        ApplyPendingReferenceSpaceChange(source.xr_frame);
        auto& destination = published_frame_.frame;
        destination = {};
        destination.frameToken = source.xr_frame.serial;
        destination.contentTag = content_tag;
        destination.mode = immersive ? AURORA_STEREO_FRAME_IMMERSIVE_REPLAY
                                      : AURORA_STEREO_FRAME_VIRTUAL_SCREEN;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            destination.eyes[eye].width = source.render_width[eye];
            destination.eyes[eye].height = source.render_height[eye];
            IdentityEye(destination.eyes[eye]);
        }
        if (!immersive) {
            last_immersive_ = false;
            return;
        }

        const bool position_valid =
            (source.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        // Latch on the first immersive frame, after a recenter or an origin
        // change, and on re-entry from the virtual screen so a race start
        // recenters a player who shifted during the menus. Position only: the
        // heading and the horizon belong to the reference space, so no
        // transition here can tilt the view or redefine forward.
        if (position_valid && (!base_position_valid_ || !last_immersive_)) {
            base_position_ = CenterPosition(source.xr_frame);
            base_position_valid_ = true;
        }
        last_immersive_ = true;
        // Read once so both eyes are built from the same angle even if the
        // settings slider moves between them.
        const float lean_back_radians =
            lean_back_degrees_.load(std::memory_order_relaxed) * kDegreesToRadians;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            ProjectionFromFov(source.xr_frame.views[eye].fov,
                              destination.eyes[eye].projection);
            ViewFromBase(source.xr_frame.views[eye].pose, base_position_,
                         position_valid && base_position_valid_, units_per_meter,
                         lean_back_radians, destination.eyes[eye].viewFromCenter);
        }
    }

    // Runs once per located frame, before the virtual screen is placed and
    // before the immersive origin is latched, so a recenter reaches both from
    // this frame's head pose rather than the next one's.
    void ServiceRecenterRequest() noexcept {
        if (recenter_requested_.exchange(false, std::memory_order_acq_rel)) {
            ResetTrackingOrigin();
        }
    }

    // Anchors the menu screen in the application space and holds it there. The
    // pose is captured once, from the first frame whose head pose is good enough
    // to place it, and released again by a recenter or an origin change.
    void UpdateVirtualScreenPose(OpenXRD3D12Frame& frame) noexcept {
        if (frame.presentation.mode != OpenXRD3D12FrameMode::VirtualScreen) {
            return;
        }
        constexpr XrViewStateFlags kPoseUsable =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if (!virtual_screen_pose_valid_ && frame.xr_frame.views_valid &&
            (frame.xr_frame.view_state_flags & kPoseUsable) == kPoseUsable) {
            virtual_screen_pose_ = ScreenPoseAhead(
                frame.xr_frame, std::max(0.25f, frame.presentation.quad_distance_meters));
            virtual_screen_pose_valid_ = true;
        }
        frame.presentation.quad_anchored = virtual_screen_pose_valid_;
        frame.presentation.quad_pose = virtual_screen_pose_;
    }

    void ApplyPendingReferenceSpaceChange(const OpenXRFrame& frame) noexcept {
        if (runtime_->ConsumeAppSpaceChangesThrough(frame.predicted_display_time)) {
            ResetTrackingOrigin();
        }
    }

    void ResetTrackingOrigin() noexcept {
        base_position_ = {};
        base_position_valid_ = false;
        last_immersive_ = false;
        // The anchored menu screen is placed in the same space, so it is stale
        // for exactly the same reasons and is re-placed on the next frame.
        virtual_screen_pose_valid_ = false;
    }

    void WaitForStopOrDelay(std::chrono::milliseconds delay) {
        std::unique_lock lock(stop_mutex_);
        stop_cv_.wait_for(lock, delay,
                          [this] { return stop_.load(std::memory_order_acquire); });
    }

    void WithdrawPublishedFrame() noexcept {
        std::lock_guard lock(published_mutex_);
        published_.store(nullptr, std::memory_order_release);
    }

    void SetError(std::string message) {
        {
            std::lock_guard lock(error_mutex_);
            last_error_ = std::move(message);
        }
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: " << LastError() << std::endl;
    }

    OpenXRLogCallback logger_;
    std::unique_ptr<OpenXRRuntime> runtime_;
    std::unique_ptr<OpenXRD3D12Backend> backend_;
    std::thread pacing_thread_;
    std::atomic_bool stop_{false};
    std::atomic_bool running_{false};
    std::atomic_bool teardown_requested_{false};
    std::atomic_bool recenter_requested_{false};
    std::atomic<float> lean_back_degrees_{RuntimeConfigFile::VrLeanBackDegrees()};
    std::atomic<PublishedFrame*> published_{nullptr};
    PublishedFrame published_frame_{};
    std::mutex published_mutex_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::array<float, 3> base_position_{};
    bool base_position_valid_ = false;
    XrPosef virtual_screen_pose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    bool virtual_screen_pose_valid_ = false;
    bool last_immersive_ = false;
    uint64_t applied_session_run_serial_ = 0;
    bool session_was_active_ = false;
    bool requested_ = false;
    bool prepared_ = false;
    bool provider_registered_ = false;
    bool graphics_retained_ = false;
};

#endif // defined(MKW_ENABLE_OPENXR) && defined(_WIN32)

} // namespace

OpenXRStartupResult OpenXRPrepareAurora(AuroraConfig& config) {
#if !defined(MKW_ENABLE_OPENXR)
    (void)config;
    ConfigurePolicy(false);
    return RuntimeConfigFile::VrEnabled(false) ? OpenXRStartupResult::Unavailable
                                               : OpenXRStartupResult::Disabled;
#elif defined(_WIN32)
    return OpenXRIntegration::Get().Prepare(config);
#else
    ConfigurePolicy(RuntimeConfigFile::VrEnabled(false));
    if (!RuntimeConfigFile::VrEnabled(false)) {
        return OpenXRStartupResult::Disabled;
    }
    const OpenXRVulkanCapabilityInfo capability = OpenXRVulkanBackend::DawnInteropCapability();
    RT_LOG(RT_TAG_RUNTIME) << "OpenXR Vulkan unavailable: " << capability.reason << std::endl;
    return OpenXRStartupResult::Unavailable;
#endif
}

bool OpenXRStartAfterAurora(AuroraBackend active_backend) {
#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
    return OpenXRIntegration::Get().Start(active_backend);
#else
    (void)active_backend;
    return !RuntimeConfigFile::VrEnabled(false);
#endif
}

void OpenXRShutdownBeforeAurora() noexcept {
#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
    OpenXRIntegration::Get().Shutdown();
#else
    MkwVRPolicySetSessionActive(false);
#endif
}

void OpenXRServiceProducerFrameBoundary() noexcept {
#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
    OpenXRIntegration::Get().ServiceProducerFrameBoundary();
#endif
}

bool OpenXRIsRunning() noexcept {
#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
    return OpenXRIntegration::Get().IsRunning();
#else
    return false;
#endif
}

void OpenXRRequestRecenter() noexcept {
#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
    OpenXRIntegration::Get().RequestRecenter();
#endif
}

void OpenXRSetLeanBackDegrees(float degrees) noexcept {
#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
    OpenXRIntegration::Get().SetLeanBackDegrees(degrees);
#else
    (void)degrees;
#endif
}

std::string OpenXRLastError() {
#if !defined(MKW_ENABLE_OPENXR)
    return "this build was compiled without OpenXR support";
#elif defined(_WIN32)
    return OpenXRIntegration::Get().LastError();
#else
    return OpenXRVulkanBackend::DawnInteropCapability().reason;
#endif
}

} // namespace mkw::vr
