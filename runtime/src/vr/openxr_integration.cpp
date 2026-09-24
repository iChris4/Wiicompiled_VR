// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "vr/openxr_integration.h"

#include "runtime_config.h"
#include "gx_thread.h"
#include "runtime_log.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/mkw_vr_policy.h"
#include "vr/mkw_vr_instrumentation.h"
#include "vr/openxr_diagnostics.h"
#include <aurora/gfx.h>

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
#include <sstream>
#include <string>
#include <thread>

#if defined(MKW_ENABLE_OPENXR)
#include "vr/openxr_backend.h"
#include "vr/openxr_hand_mesh.h"
#include "vr/openxr_input.h"
#include "vr/openxr_runtime.h"
#if defined(_WIN32)
#include "vr/openxr_windows.h"
#define MKW_OPENXR_GRAPHICS_BACKEND 1
#elif defined(__ANDROID__)
#include "vr/openxr_android.h"
#include "vr/openxr_vulkan.h"
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#define XR_USE_TIMESPEC
#include <openxr/openxr_platform.h>
#define MKW_OPENXR_GRAPHICS_BACKEND 1
#else
#define MKW_OPENXR_GRAPHICS_BACKEND 0
#endif
#else
#define MKW_OPENXR_GRAPHICS_BACKEND 0
#endif

namespace mkw::vr {
namespace {

inline constexpr float kDegreesToRadians = 0.01745329252f;

// This is the VR build, so the headset path is what an unconfigured installation
// starts in, on the headset and on the desktop alike. Nothing is lost by it:
// with vr.required false, a missing runtime or headset falls back to desktop.
inline constexpr bool kVrEnabledDefault = true;

void ConfigurePolicy(bool enabled) noexcept {
    MkwVRPolicyReset();
    MkwVRPolicyConfig config{};
    config.enabled = enabled;
    config.immersive_races = !RuntimeConfigFile::VrFlatScreen();
    config.world_units_per_meter = RuntimeConfigFile::VrWorldUnitsPerMeter(500.0f);
    config.hud_distance_meters = RuntimeConfigFile::VrHudDistanceMeters(2.0f);
    config.hud_width_meters = RuntimeConfigFile::VrHudWidthMeters(2.4f);
    config.first_person_units_per_meter = RuntimeConfigFile::VrFirstPersonUnitsPerMeter();
    MkwVRPolicyConfigure(config);
    MkwVRInstrumentationInitialize();
    MkwVRFirstPersonApplyConfiguredSettings();
}

#if MKW_OPENXR_GRAPHICS_BACKEND

#if defined(_WIN32)
using GraphicsBackend = OpenXRWindowsBackend;

#else
using GraphicsBackend = OpenXRVulkanBackend;
inline constexpr const char* kGraphicsBackendName = "Vulkan";
#endif

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

// Hand steering draws the player's hands, in the runtime's own hand mesh where
// it offers one (XR_FB_hand_tracking_mesh). Only asked for when hand steering
// is on at launch; turning it on later uses the procedural gloves.
void AddHandMeshExtensions(OpenXRConfig& config) {
    if (RuntimeConfigFile::VrHandSteering()) {
        config.optional_extensions.push_back("XR_EXT_hand_tracking");
        config.optional_extensions.push_back("XR_FB_hand_tracking_mesh");
    }
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

// Distinct names from openxr_runtime.cpp's helpers: both files can share a
// unity-build translation unit and the same anonymous namespace.
const char* DiagnosticSpaceName(XrReferenceSpaceType type) noexcept {
    switch (type) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        return "VIEW";
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return "LOCAL";
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return "STAGE";
    default:
        return "OTHER";
    }
}

const char* DiagnosticBlendModeName(XrEnvironmentBlendMode mode) noexcept {
    switch (mode) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE:
        return "OPAQUE";
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:
        return "ADDITIVE";
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:
        return "ALPHA_BLEND";
    default:
        return "OTHER";
    }
}

// What the runtime reported about the eyes this frame. The cant is the angle
// between the two eyes' forward axes: zero for parallel displays, and the
// headset's display tilt on canted ones (Pimax) unless the runtime is asked for
// parallel projections.
diagnostics::ViewGeometry DiagnosticViewGeometry(const OpenXRBackendFrame& frame) noexcept {
    constexpr float kRadiansToDegrees = 57.29577951f;
    diagnostics::ViewGeometry geometry{};
    std::array<std::array<float, 3>, kOpenXREyeCount> forward{};
    for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
        const XrView& view = frame.xr_frame.views[eye];
        geometry.fov_degrees[eye] = {view.fov.angleLeft * kRadiansToDegrees, view.fov.angleRight * kRadiansToDegrees,
                                     view.fov.angleUp * kRadiansToDegrees, view.fov.angleDown * kRadiansToDegrees};
        const auto& q = view.pose.orientation;
        forward[eye] = Rotate(Normalize({q.x, q.y, q.z, q.w}), {0.0f, 0.0f, -1.0f});
        geometry.width[eye] = frame.render_width[eye];
        geometry.height[eye] = frame.render_height[eye];
    }
    const float dot = forward[0][0] * forward[1][0] + forward[0][1] * forward[1][1] + forward[0][2] * forward[1][2];
    geometry.cant_degrees = std::acos(std::clamp(dot, -1.0f, 1.0f)) * kRadiansToDegrees;
    if ((frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
        const auto& left = frame.xr_frame.views[0].pose.position;
        const auto& right = frame.xr_frame.views[1].pose.position;
        const float dx = right.x - left.x;
        const float dy = right.y - left.y;
        const float dz = right.z - left.z;
        geometry.ipd_millimeters = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f;
    }
    return geometry;
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
            SetError(std::string("OpenXR cannot be restarted after an unfenceable ") +
                     kGraphicsBackendName + " submission");
            return OpenXRStartupResult::Unavailable;
        }
        // The pacing thread is not running here (Shutdown above joined it), so
        // the sink may be replaced.
        diagnostics::SetLogSink(
            [](std::string_view line) { RT_LOG(RT_TAG_RUNTIME) << line << std::endl; });
        diagnostics::SetEnabled(RuntimeConfigFile::DiagnosticsOpenXRLogging(false));
        requested_ = RuntimeConfigFile::VrEnabled(kVrEnabledDefault);
        ConfigurePolicy(requested_);
        if (!requested_) {
            return OpenXRStartupResult::Disabled;
        }
        if (!BackendMatchesConfiguredGraphicsApi(aurora_config)) {
            return OpenXRStartupResult::Unavailable;
        }

        logger_ = [](OpenXRLogLevel level, std::string_view message) {
            const char* name = level == OpenXRLogLevel::Error ? "error" :
                               level == OpenXRLogLevel::Warning ? "warning" : "info";
            RT_LOG(RT_TAG_RUNTIME) << "[openxr::" << name << "] " << message << std::endl;
        };
#if defined(__ANDROID__)
        {
            std::string loader_error;
            if (!OpenXRAndroidInitializeLoader(logger_, &loader_error)) {
                SetError("OpenXR Android loader initialization failed: " + loader_error);
                return OpenXRStartupResult::Unavailable;
            }
        }
#endif
        runtime_ = std::make_unique<OpenXRRuntime>(logger_);
#if defined(_WIN32)
        backend_ = std::make_unique<GraphicsBackend>(logger_, kRequiredAuroraBackend == BACKEND_VULKAN);
#else
        backend_ = std::make_unique<GraphicsBackend>(logger_);
#endif

        OpenXRConfig config{};
        config.application_name = aurora_config.appName != nullptr ? aurora_config.appName
                                                                    : "WiiCompiled";
        config.engine_name = "Aurora";
        config.resolution_scale = RuntimeConfigFile::VrRenderScale();
#if defined(_WIN32)
        config.required_extensions = {kRequiredAuroraBackend == BACKEND_VULKAN ? "XR_KHR_vulkan_enable2" : "XR_KHR_D3D12_enable"};
        config.optional_extensions = {"XR_KHR_win32_convert_performance_counter_time",
                                      "XR_FB_display_refresh_rate", "XR_EXT_performance_settings"};
        AddHandMeshExtensions(config);
#else
        // Either Vulkan binding extension is acceptable; the backend picks
        // whichever the runtime enabled, preferring enable2.
        config.required_extensions = {"XR_KHR_android_create_instance"};
        // XR_FB_passthrough: the room around the virtual screen (OpenXRPassthrough), asked for
        // whatever [vr] passthrough says, since the setting is live.
        config.optional_extensions = {"XR_KHR_vulkan_enable2", "XR_KHR_vulkan_enable",
                                      "XR_KHR_convert_timespec_time",
                                      "XR_KHR_android_thread_settings",
                                      "XR_FB_display_refresh_rate", "XR_EXT_performance_settings",
                                      "XR_FB_passthrough"};
        AddHandMeshExtensions(config);
        config.instance_create_next = OpenXRAndroidInstanceCreateNext();
#endif
        if (!runtime_->Initialize(config)) {
            SetError("OpenXR instance initialization failed: " + runtime_->LastError().message);
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }
        const auto& extensions = runtime_->EnabledExtensions();
        const auto has_extension = [&](const char* name) {
            return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
        };
#if defined(_WIN32)
        if (has_extension("XR_KHR_win32_convert_performance_counter_time")) {
            runtime_->LoadFunction("xrConvertTimeToWin32PerformanceCounterKHR", &convert_display_time_);
        }
#else
        if (has_extension("XR_KHR_convert_timespec_time")) {
            runtime_->LoadFunction("xrConvertTimeToTimespecTimeKHR", &convert_display_time_);
        }
#endif
        if (has_extension("XR_FB_display_refresh_rate")) {
            runtime_->LoadFunction("xrGetDisplayRefreshRateFB", &get_display_refresh_rate_);
        }
        if (has_extension("XR_EXT_performance_settings")) {
            runtime_->LoadFunction("xrPerfSettingsSetPerformanceLevelEXT", &set_performance_level_);
        }
        interpolation_available_.store(convert_display_time_ != nullptr, std::memory_order_release);
        if (!backend_->QueryGraphicsRequirements(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }

        ApplyGraphicsRequirements(aurora_config);
        prepared_ = true;
        return OpenXRStartupResult::Prepared;
    }

    bool Start(AuroraBackend active_backend) {
        if (!prepared_ || runtime_ == nullptr || backend_ == nullptr) {
            return !requested_;
        }
        if (active_backend != kRequiredAuroraBackend) {
            SetError(std::string("Aurora could not create the OpenXR-required ") +
                     kGraphicsBackendName + " backend");
            ResetPreparedObjects();
            return false;
        }
        if (!backend_->BindAurora(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return false;
        }
        input_ = std::make_unique<OpenXRInput>(logger_);
        if (!input_->Create(*runtime_)) {
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR controller input unavailable: " << input_->LastError()
                                   << std::endl;
            input_.reset();
        }

#if defined(__ANDROID__)
        // The producer: SDL's main thread, which also carries every guest fiber.
        game_thread_id_ = static_cast<uint32_t>(gettid());
#endif
        stop_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(interpolation_mutex_);
            interpolation_stopping_ = false;
        }
        teardown_requested_.store(false, std::memory_order_release);
        WithdrawPublishedFrame();
        aurora_set_stereo_frame_provider(&OpenXRIntegration::ProvideStereoFrame, this);
        provider_registered_ = true;
        if (convert_display_time_ != nullptr) {
            diagnostics::SetDisplayTimeConverter(
                [this](int64_t xr_time) { return static_cast<int64_t>(DisplayTimeNanos(xr_time)); });
        }
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
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR asynchronous " << kGraphicsBackendName
                               << " presentation started" << std::endl;
        return true;
    }

    void Shutdown() noexcept {
        teardown_requested_.store(false, std::memory_order_release);
        // Called on the game thread: aurora's producer (the GX thread) must be
        // idle before the frame worker is quiesced.
        GxThread::Drain();
        // Stop idle replays before draining; no new worker job may race provider removal.
        {
            std::lock_guard lock(interpolation_mutex_);
            interpolation_stopping_ = true;
            aurora_set_stereo_frame_interpolation(false);
        }
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
        // The converter reads runtime_; the pacing thread has stopped using it.
        diagnostics::SetDisplayTimeConverter({});
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
        convert_display_time_ = nullptr;
        get_display_refresh_rate_ = nullptr;
        set_performance_level_ = nullptr;
        headset_hz_.store(0, std::memory_order_relaxed);
        rendered_fps_.store(0, std::memory_order_relaxed);
        interpolation_available_.store(false, std::memory_order_release);
        ResetTrackingOrigin();
        applied_session_run_serial_ = 0;
        session_was_active_ = false;
    }

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    void RequestRecenter() noexcept {
        recenter_requested_.store(true, std::memory_order_release);
    }

    void SetFrameInterpolationFps(uint32_t target) noexcept {
        frame_interpolation_fps_.store(NormalizeFrameInterpolationFps(target), std::memory_order_relaxed);
    }

    OpenXRFrameTiming FrameTiming() const noexcept {
        return {headset_hz_.load(std::memory_order_relaxed), rendered_fps_.load(std::memory_order_relaxed)};
    }

    bool FrameInterpolationAvailable() const noexcept {
        return interpolation_available_.load(std::memory_order_acquire);
    }

    void SetPassthrough(bool enabled) noexcept {
        passthrough_.store(enabled, std::memory_order_relaxed);
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

#if defined(_WIN32)
    AuroraBackend kRequiredAuroraBackend = BACKEND_D3D12;
    const char* kGraphicsBackendName = "D3D12";
#else
    static constexpr AuroraBackend kRequiredAuroraBackend = BACKEND_VULKAN;
#endif
    // Skipped eye copies tolerated back to back before the session is given up: a few seconds
    // at the headset's refresh rate.
    static constexpr uint32_t kMaxConsecutiveSkips = 300;

    bool BackendMatchesConfiguredGraphicsApi(const AuroraConfig& aurora_config) {
#if defined(_WIN32)
        kRequiredAuroraBackend = aurora_config.desiredBackend == BACKEND_VULKAN ? BACKEND_VULKAN : BACKEND_D3D12;
        kGraphicsBackendName = kRequiredAuroraBackend == BACKEND_VULKAN ? "Vulkan" : "D3D12";
#endif
        if (aurora_config.desiredBackend == BACKEND_AUTO ||
            aurora_config.desiredBackend == kRequiredAuroraBackend) {
            return true;
        }
        SetError(std::string("OpenXR requires the ") + kGraphicsBackendName +
                 " graphics backend on this platform");
        return false;
    }

    void ApplyGraphicsRequirements(AuroraConfig& aurora_config) {
        aurora_config.desiredBackend = kRequiredAuroraBackend;
        aurora_config.xrInterop = true;
#if defined(__ANDROID__)
        // Foveated rendering: fragment density maps are decided with the device. They put a flag on
        // every render pipeline, so a session launched with foveation off does without them.
        aurora_config.xrFragmentDensityMap = RuntimeConfigFile::VrFoveation() != "off";
#endif
#if defined(_WIN32)
        if (kRequiredAuroraBackend != BACKEND_D3D12) return;
        const auto& requirements = backend_->GraphicsRequirements();
        aurora_config.hasD3D12AdapterLuid = true;
        aurora_config.d3d12AdapterLuidLow = requirements.adapter_luid_low;
        aurora_config.d3d12AdapterLuidHigh = requirements.adapter_luid_high;
#endif
    }

    void ResetPreparedObjects() {
        diagnostics::SetDisplayTimeConverter({});
        ShutdownOrRetainGraphicsObjects();
        input_.reset();
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
    }

    bool ShutdownOrRetainGraphicsObjects() noexcept {
        if (input_ != nullptr) {
            // Actions belong to the session and must go before it does.
            input_->Destroy();
            input_.reset();
        }
        if (backend_ != nullptr && !backend_->Shutdown()) {
            RT_LOG(RT_TAG_RUNTIME)
                << "OpenXR " << kGraphicsBackendName
                << " queue completion is unknown; retaining the backend, "
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

#if defined(__ANDROID__)
    // Aurora's frame worker publishes its native thread id once it runs; until then there is
    // nothing to hint. The hint itself may be refused by the runtime, which is only logged.
    bool RegisterAuroraFrameWorkerThread() {
        const uint32_t thread_id = aurora_get_frame_worker_native_thread_id();
        if (thread_id == 0 || runtime_ == nullptr) {
            return false;
        }
        const bool hinted = OpenXRAndroidRegisterThreadId(*runtime_, OpenXRAndroidThreadType::RendererMain, thread_id);
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: Android thread hint for Aurora's frame worker "
                               << (hinted ? "set" : "refused") << std::endl;
        return true;
    }
    bool RegisterGxThread() {
        const uint32_t thread_id = GxThread::NativeThreadId();
        if (thread_id == 0 || runtime_ == nullptr) {
            return false;
        }
        const bool hinted = OpenXRAndroidRegisterThreadId(*runtime_, OpenXRAndroidThreadType::RendererWorker, thread_id);
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: Android thread hint for the GX thread "
                               << (hinted ? "set" : "refused") << std::endl;
        return true;
    }
#endif

    // Asks the runtime for the configured performance level in both domains. Standalone
    // headsets clock their cores by this: a Quest 3 held the game thread at CPU level 4
    // (2.2 GHz of a possible 2.36) and the GPU at level 3 with the runtime's own choice. A
    // refusal is logged and changes nothing; desktop runtimes rarely offer the extension.
    void ApplyPerformanceLevel() {
        if (runtime_ == nullptr || set_performance_level_ == nullptr || !runtime_->HasSession()) {
            return;
        }
        const std::string requested = RuntimeConfigFile::VrPerformanceLevel();
        XrPerfSettingsLevelEXT level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
        if (requested == "default") {
            return;
        } else if (requested == "power_savings") {
            level = XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT;
        } else if (requested == "sustained_low") {
            level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
        } else if (requested == "boost") {
            level = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
        }
        const XrResult cpu = set_performance_level_(runtime_->Session(), XR_PERF_SETTINGS_DOMAIN_CPU_EXT, level);
        const XrResult gpu = set_performance_level_(runtime_->Session(), XR_PERF_SETTINGS_DOMAIN_GPU_EXT, level);
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: performance level \"" << requested << "\" CPU "
                               << (XR_SUCCEEDED(cpu) ? "set" : "refused") << " (" << cpu << "), GPU "
                               << (XR_SUCCEEDED(gpu) ? "set" : "refused") << " (" << gpu << ")" << std::endl;
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
        diagnostics::NotePacketConsumed();
        *output = frame->frame;
        return true;
    }

    void PacingThread() noexcept {
#if defined(__ANDROID__)
        // The runtime schedules hinted threads onto the fast cores. The game thread and Aurora's
        // frame worker, which submits the GPU work, are the ones that matter; this thread only
        // paces.
        bool worker_registered = false;
        bool gx_registered = false;
        if (runtime_ != nullptr) {
            const bool pacing_hinted =
                OpenXRAndroidRegisterThread(*runtime_, OpenXRAndroidThreadType::RendererWorker);
            bool game_hinted = false;
            if (game_thread_id_ != 0) {
                game_hinted = OpenXRAndroidRegisterThreadId(*runtime_, OpenXRAndroidThreadType::ApplicationMain,
                                                            game_thread_id_);
            }
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR: Android thread hints: game " << (game_hinted ? "set" : "refused")
                                   << ", pacing " << (pacing_hinted ? "set" : "refused") << std::endl;
            worker_registered = RegisterAuroraFrameWorkerThread();
            gx_registered = RegisterGxThread();
        }
#endif
        ApplyPerformanceLevel();
        bool fatal = false;
        uint32_t consecutive_skips = 0;
        bool store_gate_set = false;
        bool store_gate_racing = false;
        bool presentation_logged = false;
        VRPresentationMode logged_presentation = VRPresentationMode::Desktop;
        uint32_t presentation_log_count = 0;
        bool immersive_submission_logged = false;
        int last_pacing_mode = -1;
        while (!stop_.load(std::memory_order_acquire) && !fatal) {
#if defined(__ANDROID__)
            if (!worker_registered) {
                worker_registered = RegisterAuroraFrameWorkerThread();
            }
            if (!gx_registered) {
                gx_registered = RegisterGxThread();
            }
#endif
            const OpenXREventStatus events = diagnostics::Measure(diagnostics::Stage::PollEvents, [&] {
                return runtime_->PollEvents();
            });
            const bool session_active = runtime_->IsSessionRunning();
            MkwVRPolicySetSessionActive(session_active);
            const uint64_t session_run_serial = runtime_->SessionRunSerial();
            if (session_run_serial != applied_session_run_serial_) {
                applied_session_run_serial_ = session_run_serial;
                ResetTrackingOrigin();
                diagnostics::OnSessionStarted();
            }
            if (session_active != session_was_active_) {
                session_was_active_ = session_active;
                if (!session_active) {
                    ResetTrackingOrigin();
                    // Nothing is displayed while the session is not running (the system menu,
                    // the headset taken off), so the stall of a cache store is invisible here.
                    // Waiting for it is deliberate: the process may be ended next.
                    aurora_store_pipeline_caches();
                }
            }
            if (events == OpenXREventStatus::ExitRequested) {
                SetError("OpenXR runtime requested session exit; continuing on the mirror output");
                break;
            }
            if (events == OpenXREventStatus::Error) {
                SetError("OpenXR event processing failed: " + runtime_->LastError().message);
                break;
            }
            if (!session_active) {
                if (input_ != nullptr) {
                    input_->Idle();
                }
                SetInterpolationActive(false);
                interpolation_pacing_.Reset();
                rendered_fps_.store(0, std::memory_order_relaxed);
                WaitForStopOrDelay(std::chrono::milliseconds(5));
                continue;
            }

            const MkwVRPolicySnapshot policy = MkwVRPolicyGetSnapshot();
            // Diagnostics lift the cap: a presentation flickering between the
            // race and the virtual screen is exactly what a report needs to show.
            if ((!presentation_logged || policy.presentation != logged_presentation) &&
                (presentation_log_count < 16 || diagnostics::Enabled())) {
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
            OpenXRPresentation presentation{};
            const bool immersive = policy.presentation == VRPresentationMode::ImmersiveRace;
            presentation.mode = immersive ? OpenXRFrameMode::ImmersiveProjection
                                           : OpenXRFrameMode::VirtualScreen;
            presentation.quad_distance_meters = policy.config.hud_distance_meters;
            presentation.quad_width_meters = policy.config.hud_width_meters;
            if (float picture_aspect = 0.0f, snapshot_aspect = 0.0f;
                aurora_get_stereo_screen_aspects(&picture_aspect, &snapshot_aspect)) {
                presentation.quad_content_aspect = snapshot_aspect;
            }
            // The room around the menu screen and every other virtual screen, a
            // Flat Screen race included; an immersive race is fully virtual, and the
            // cameras are paused for it.
            presentation.passthrough = !immersive && passthrough_.load(std::memory_order_relaxed);
            // The settings panel gets a compositor layer of its own while it is
            // open, and Aurora leaves it out of the eyes. A backend that could
            // not make that layer has the panel drawn into the eyes instead.
            const bool panel_layer = backend_->PanelLayerAvailable() && !PanelLayerForcedOff();
            aurora_set_stereo_panel_layer(panel_layer);
            PollEyePassesOverride();
            PollFoveationOverride();
            presentation.panel.requested = panel_layer && OpenXRSettingsPanelOpen();

            // Pipeline caches are stored where their stall is least visible: once when a race
            // ends, and by the compiler itself while the headset shows the virtual screen. Never
            // mid-race, and a race on the virtual screen (Flat Screen mode) is still a race. The
            // race-exit store runs on Aurora's thread: this one keeps submitting frames while
            // Dawn holds its device to serialize, which is still a brief game stall.
            const bool racing = immersive || (!policy.config.immersive_races &&
                                              policy.scene.mode == VRSceneMode::Race);
            if (!store_gate_set || racing != store_gate_racing) {
                const bool left_race = store_gate_set && store_gate_racing && !racing;
                store_gate_set = true;
                store_gate_racing = racing;
                aurora_set_pipeline_cache_idle_store(!racing);
                if (left_race) {
                    aurora_request_pipeline_cache_store();
                }
            }

            // Updating this on the owner thread also confines retained replay to
            // validated race content. The provider checks policy tags again.
            const uint32_t interpolation_target = frame_interpolation_fps_.load(std::memory_order_relaxed);
            SetInterpolationActive(immersive && FrameInterpolationAvailable() && interpolation_target != 0);

            // With interpolation off, the eyes are rendered before
            // the compositor frame that shows them is begun, so that frame never waits for a
            // game frame. Interpolation keeps the frame-first order below: it renders for the
            // frame's own predicted display time.
            const bool render_first = !aurora_get_stereo_frame_interpolation();
            if (last_pacing_mode != static_cast<int>(render_first)) {
                last_pacing_mode = static_cast<int>(render_first);
                RT_LOG(RT_TAG_RUNTIME) << "OpenXR " << kGraphicsBackendName << " pacing: "
                    << (render_first ? "render-first" : "frame-first (VR interpolation)") << std::endl;
            }
            if (render_first) {
                if (!RenderFirstCycle(presentation, policy, immersive, consecutive_skips,
                                      immersive_submission_logged)) {
                    fatal = true;
                }
                continue;
            }
            OpenXRBackendFrame frame{};
            const OpenXRBeginStatus begin = backend_->BeginFrame(presentation, frame);
            if (begin == OpenXRBeginStatus::SessionNotRunning) {
                MkwVRPolicySetSessionActive(false);
                continue;
            }
            if (begin == OpenXRBeginStatus::ExitRequested) {
                SetError("OpenXR runtime requested session exit; continuing on the mirror output");
                break;
            }
            if (begin == OpenXRBeginStatus::Error) {
                SetError(backend_->LastError());
                fatal = true;
                break;
            }

            UpdateFrameTiming(frame.xr_frame);
            if (diagnostics::Enabled()) {
                NoteFrameDiagnostics(frame, immersive);
            }
            // Both of these read this frame's located head pose and must run
            // before FinishFrame submits a layer built from it.
            ServiceRecenterRequest();
            UpdateVirtualScreenPose(frame);
            const OpenXRPointerScreen panel_screen = SettingsPanelScreen(frame, policy, immersive);
            PlacePanelLayer(frame, panel_screen);
            if (input_ != nullptr) {
                const diagnostics::ScopedStage input_timer(diagnostics::Stage::InputSync);
                // After the screen is placed, so the pointer aims at this
                // frame's screen rather than the previous one's.
                input_->Sync(frame.xr_frame.predicted_display_time, PointerScreen(frame, policy, immersive),
                             panel_screen, InputSeatFrame(immersive));
            }

            if (!frame.expects_gpu_submission) {
                if (!backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }

            if (aurora_get_stereo_frame_interpolation() &&
                !interpolation_pacing_.ShouldRender(frame.xr_frame.predicted_display_time, interpolation_target)) {
                diagnostics::OnInterpolationSkip();
                if (!diagnostics::Measure(diagnostics::Stage::Cancel, [&] {
                    return backend_->TryCancelPendingFrame(frame);
                }) || !backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }

            {
                const diagnostics::ScopedStage publish_timer(diagnostics::Stage::Publish);
                std::lock_guard lock(published_mutex_);
                // First person renders at life-size scale, third person at the
                // configured diorama scale. Head translation and IPD are the
                // only things this multiplies, so a one-frame disagreement with
                // the camera's own switch is not observable.
                BuildPublishedFrame(frame, immersive, policy.EffectiveUnitsPerMeter(),
                                    policy.content_tag);
                diagnostics::OnPacketPublished();
                published_.store(&published_frame_, std::memory_order_release);
            }
            aurora_notify_stereo_frame();

            OpenXRSubmissionStatus submission = OpenXRSubmissionStatus::Timeout;
            bool canceled_before_encode = false;
            const auto cancel_after =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            while (!stop_.load(std::memory_order_acquire) &&
                   submission == OpenXRSubmissionStatus::Timeout) {
                // Fresh rendering wakes us immediately. A 50 ms keep-alive
                // protects stalls without issuing eager repeats during GPU work.
                submission = diagnostics::Measure(diagnostics::Stage::SubmissionWait, [&] {
                    return backend_->WaitForSubmission(frame, 50);
                });
                if (submission == OpenXRSubmissionStatus::Timeout) {
                    // A pause, minimized window, or guest stall may leave no GX
                    // frame to consume this packet. Withdraw it, then cancel the
                    // matching bridge target only if Encode has not taken ownership.
                    if (std::chrono::steady_clock::now() >= cancel_after) {
                        diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
                        canceled_before_encode = diagnostics::Measure(diagnostics::Stage::Cancel, [&] {
                            return backend_->TryCancelPendingFrame(frame);
                        });
                        if (canceled_before_encode) {
                            diagnostics::OnPacketCanceled();
                            break;
                        }
                    }
                    diagnostics::OnKeepaliveRepeat();
                    if (!backend_->RepeatFrame(frame)) {
                        SetError(backend_->LastError());
                        fatal = true;
                        break;
                    }
                }
            }
            diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
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
            if (fatal) {
                break;
            }
            const bool submit = submission == OpenXRSubmissionStatus::Success;
            diagnostics::OnSubmission(submit);
            if (!backend_->FinishFrame(frame, submit)) {
                SetError(backend_->LastError());
                fatal = true;
            } else if (submission == OpenXRSubmissionStatus::Skipped) {
                // No GPU work touched the compositor image or the shared buffers, so the frame
                // ended on the retained layer and the next one is tried normally. A long run of
                // skips means the copy path is broken for good.
                ++consecutive_skips;
                if (consecutive_skips == 1 || consecutive_skips % 60 == 0) {
                    RT_LOG(RT_TAG_RUNTIME) << "OpenXR: eye copy skipped (" << consecutive_skips
                                           << " in a row): " << backend_->LastError() << std::endl;
                }
                if (consecutive_skips >= kMaxConsecutiveSkips) {
                    SetError(std::string("Aurora's ") + kGraphicsBackendName +
                             " stereo copy keeps failing; continuing on the mirror output");
                    fatal = true;
                }
            } else if (!submit) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy failed; continuing on the mirror output");
                fatal = true;
            } else {
                consecutive_skips = 0;
                ++timing_submissions_;
            }
            if (submit && !fatal && immersive && !immersive_submission_logged) {
                immersive_submission_logged = true;
                RT_LOG(RT_TAG_RUNTIME)
                    << "[mkw-vr] first immersive packet consumed and submitted as "
                       "an OpenXR projection layer"
                    << std::endl;
            }
        }

        SetInterpolationActive(false);
        aurora_set_pipeline_cache_idle_store(false);
        aurora_set_stereo_panel_layer(false);
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

    // One compositor cycle on the retained layer, with no frame left active. False on a fatal
    // backend or runtime failure (the error is recorded).
    bool KeepAlive() {
        const OpenXRBeginStatus status = backend_->KeepAliveCycle();
        if (status == OpenXRBeginStatus::SessionNotRunning) {
            MkwVRPolicySetSessionActive(false);
            return true;
        }
        if (status == OpenXRBeginStatus::ExitRequested) {
            SetError("OpenXR runtime requested session exit; continuing on the mirror output");
            return false;
        }
        if (status == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return false;
        }
        return true;
    }

    // Render-first pacing (see each backend's PreparePacket). Returns false on a fatal
    // failure; a cycle that ends without a layer returns true and the loop tries again.
    bool RenderFirstCycle(OpenXRPresentation presentation, const MkwVRPolicySnapshot& policy, bool immersive,
                          uint32_t& consecutive_skips, bool& immersive_submission_logged) {
        OpenXRBackendFrame packet{};
        const OpenXRBeginStatus prepared = backend_->PreparePacket(presentation, packet);
        if (prepared == OpenXRBeginStatus::SessionNotRunning) {
            MkwVRPolicySetSessionActive(false);
            return true;
        }
        if (prepared == OpenXRBeginStatus::ExitRequested) {
            SetError("OpenXR runtime requested session exit; continuing on the mirror output");
            return false;
        }
        if (prepared == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return false;
        }
        // The head pose this packet was located with places the screens and aims the pointer.
        ServiceRecenterRequest();
        UpdateVirtualScreenPose(packet);
        const OpenXRPointerScreen panel_screen = SettingsPanelScreen(packet, policy, immersive);
        PlacePanelLayer(packet, panel_screen);
        if (input_ != nullptr) {
            const diagnostics::ScopedStage input_timer(diagnostics::Stage::InputSync);
            input_->Sync(packet.xr_frame.predicted_display_time, PointerScreen(packet, policy, immersive),
                         panel_screen, InputSeatFrame(immersive));
        }
        if (!packet.expects_gpu_submission) {
            // Nothing to render (no rendering requested or no tracking): keep the compositor fed.
            return KeepAlive();
        }
        {
            const diagnostics::ScopedStage publish_timer(diagnostics::Stage::Publish);
            std::lock_guard lock(published_mutex_);
            BuildPublishedFrame(packet, immersive, policy.EffectiveUnitsPerMeter(), policy.content_tag);
            diagnostics::OnPacketPublished();
            published_.store(&published_frame_, std::memory_order_release);
        }
        aurora_notify_stereo_frame();

        // Aurora renders the eyes at its next seal. Meanwhile the compositor keeps showing the
        // retained layer; a 50 ms stall repeats it explicitly and withdraws the packet.
        OpenXRSubmissionStatus submission = OpenXRSubmissionStatus::Timeout;
        bool canceled_before_encode = false;
        const auto cancel_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (!stop_.load(std::memory_order_acquire) && submission == OpenXRSubmissionStatus::Timeout) {
            submission = diagnostics::Measure(diagnostics::Stage::SubmissionWait, [&] {
                return backend_->WaitForSubmission(packet, 50);
            });
            if (submission == OpenXRSubmissionStatus::Timeout) {
                if (std::chrono::steady_clock::now() >= cancel_after) {
                    diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
                    canceled_before_encode = diagnostics::Measure(diagnostics::Stage::Cancel, [&] {
                        return backend_->TryCancelPendingPacket(packet);
                    });
                    if (canceled_before_encode) {
                        diagnostics::OnPacketCanceled();
                        break;
                    }
                }
                diagnostics::OnKeepaliveRepeat();
                if (!KeepAlive()) {
                    return false;
                }
            }
        }
        diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
        if (stop_.load(std::memory_order_acquire) ||
            submission == OpenXRSubmissionStatus::ShuttingDown) {
            return true;
        }
        if (canceled_before_encode) {
            // Refresh display timing after a canceled packet as well, so the
            // next estimate cannot remain stuck in the past during a game stall.
            return KeepAlive();
        }
        if (submission != OpenXRSubmissionStatus::Success) {
            // Nothing reached the shared buffers (Skipped) or Aurora failed after queuing GPU
            // work (Failed): same accounting as the frame-first path, on a keep-alive cycle.
            diagnostics::OnSubmission(false);
            if (submission == OpenXRSubmissionStatus::Failed) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy failed; continuing on the mirror output");
                return false;
            }
            ++consecutive_skips;
            if (consecutive_skips == 1 || consecutive_skips % 60 == 0) {
                RT_LOG(RT_TAG_RUNTIME) << "OpenXR: eye copy skipped (" << consecutive_skips
                                       << " in a row): " << backend_->LastError() << std::endl;
            }
            if (consecutive_skips >= kMaxConsecutiveSkips) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy keeps failing; continuing on the mirror output");
                return false;
            }
            return KeepAlive();
        }

        // The eyes are rendered: begin the compositor frame, complete the backend copy, end.
        OpenXRBackendFrame frame{};
        const OpenXRBeginStatus begin = backend_->BeginFrameForPacket(packet, frame);
        if (begin == OpenXRBeginStatus::SessionNotRunning) {
            MkwVRPolicySetSessionActive(false);
            return true;
        }
        if (begin == OpenXRBeginStatus::ExitRequested) {
            SetError("OpenXR runtime requested session exit; continuing on the mirror output");
            return false;
        }
        if (begin == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return false;
        }
        UpdateFrameTiming(frame.xr_frame);
        if (diagnostics::Enabled()) {
            NoteFrameDiagnostics(frame, immersive);
        }
        const OpenXRSubmissionStatus copy =
            frame.expects_gpu_submission ? backend_->CopyRenderedEyes(frame) : OpenXRSubmissionStatus::Skipped;
        const bool submit = copy == OpenXRSubmissionStatus::Success;
        diagnostics::OnSubmission(submit);
        if (!backend_->FinishFrame(frame, submit)) {
            SetError(backend_->LastError());
            return false;
        }
        if (copy == OpenXRSubmissionStatus::Failed) {
            SetError(std::string("Aurora's ") + kGraphicsBackendName +
                     " stereo copy failed; continuing on the mirror output");
            return false;
        }
        if (!submit) {
            ++consecutive_skips;
            if (consecutive_skips == 1 || consecutive_skips % 60 == 0) {
                RT_LOG(RT_TAG_RUNTIME) << "OpenXR: eye copy skipped (" << consecutive_skips
                                       << " in a row): " << backend_->LastError() << std::endl;
            }
            return consecutive_skips < kMaxConsecutiveSkips;
        }
        consecutive_skips = 0;
        ++timing_submissions_;
        if (immersive && !immersive_submission_logged) {
            immersive_submission_logged = true;
            RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first immersive packet consumed and submitted as "
                                      "an OpenXR projection layer"
                                   << std::endl;
        }
        return true;
    }

    void BuildPublishedFrame(const OpenXRBackendFrame& source, bool immersive,
                             float units_per_meter, uint64_t content_tag) noexcept {
        ApplyPendingReferenceSpaceChange(source.xr_frame);
        auto& destination = published_frame_.frame;
        destination = {};
        destination.frameToken = source.xr_frame.serial;
        destination.contentTag = content_tag;
        destination.displayTimeNanos = DisplayTimeNanos(source.xr_frame.predicted_display_time);
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
        BuildCockpit(source, position_valid, units_per_meter, lean_back_radians, destination.cockpit);
    }

    // The first-person cockpit's hands and separate wheel, in the seated frame
    // the eye transforms place at base + lean * seat (metres). Always carries
    // the packet's world scale, which Aurora rescales to the sealed frame's.
    void BuildCockpit(const OpenXRBackendFrame& source, bool position_valid, float units_per_meter,
                      float lean_back_radians, AuroraCockpit& cockpit) noexcept {
        cockpit.unitsPerMeter = units_per_meter;
        const DrivingSnapshot driving = input_ != nullptr ? input_->Driving() : DrivingSnapshot{};
        if (driving.hand_steering && !hand_meshes_loaded_ && runtime_ != nullptr) {
            hand_meshes_loaded_ = true;
            const bool loaded = LoadRuntimeHandMeshes(*runtime_);
            RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] cockpit hands: "
                                   << (loaded ? "the runtime's hand mesh" : "procedural gloves (no runtime hand mesh)")
                                   << std::endl;
        }
        cockpit.active = driving.cockpit_active && position_valid && base_position_valid_ &&
                         (driving.synthetic_control || driving.hand_steering);
        if (!cockpit.active) {
            return;
        }
        cockpit.wheelAngle = driving.visual_angle;
        cockpit.nativeWheel = !driving.synthetic_control;
        cockpit.bike = driving.bike;
        cockpit.handlebarRadius = driving.control.radius;
        for (int row = 0; row < 3; ++row) {
            cockpit.seatFromHandlebar[row * 4 + 0] = driving.control.right[row];
            cockpit.seatFromHandlebar[row * 4 + 1] = driving.control.up[row];
            cockpit.seatFromHandlebar[row * 4 + 2] = driving.control.normal[row];
            cockpit.seatFromHandlebar[row * 4 + 3] = driving.control.center[row];
        }
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            ViewFromBase(source.xr_frame.views[eye].pose, base_position_, true, 1.0f, lean_back_radians,
                         cockpit.eyeFromSeat[eye]);
        }
        for (size_t hand = 0; hand < 2; ++hand) {
            auto& target = cockpit.hands[hand];
            const auto& from = driving.hands[hand];
            target.tracked = from.tracked;
            target.held = from.held;
            target.squeeze = from.squeeze;
            std::copy(from.seat_from_grip.begin(), from.seat_from_grip.end(), target.seatFromGrip);
        }
    }

    // The seated frame the controllers are located in for hand steering: the
    // immersive base the eye transforms use, from the previous frame (this
    // frame's is latched after input).
    driving::SeatFrame InputSeatFrame(bool immersive) const noexcept {
        driving::SeatFrame seat;
        seat.valid = immersive && base_position_valid_ && last_immersive_;
        seat.base = base_position_;
        seat.lean_back_radians = lean_back_degrees_.load(std::memory_order_relaxed) * kDegreesToRadians;
        return seat;
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
    void UpdateVirtualScreenPose(OpenXRBackendFrame& frame) noexcept {
        if (frame.presentation.mode != OpenXRFrameMode::VirtualScreen) {
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

    // The rectangle the game picture covers on the screen this frame shows, in
    // the application space, for the Wii Remote pointer to aim at.
    //
    // Menus: the quad layer UpdateVirtualScreenPose placed (or its head-locked
    // fallback), sized like the backends size it: hud_width_meters across with
    // the eye texture's aspect, the desktop snapshot letterboxed into it and the
    // picture into the snapshot.
    //
    // Races: the 2D layer's screen, which Aurora hangs hud_distance_meters
    // ahead in the recorded centre-eye space. ViewFromBase maps a point p of
    // that space (in metres) to base + lean * p in the application space, so the
    // screen sits at base + lean * (0, 0, -distance), turned by the lean, its
    // height following the picture aspect as stereo_hud_screen's does. With the
    // 2D layer stretched across the eyes there is no screen to point at.
    OpenXRPointerScreen PointerScreen(const OpenXRBackendFrame& frame, const MkwVRPolicySnapshot& policy,
                                      bool immersive) const noexcept {
        OpenXRPointerScreen screen{};
        float picture_aspect = 0.0f;
        float snapshot_aspect = 0.0f;
        if (!aurora_get_stereo_screen_aspects(&picture_aspect, &snapshot_aspect)) {
            return screen;
        }

        if (immersive) {
            if (!aurora_get_stereo_hud_screen_enabled() || !(policy.config.hud_width_meters > 0.0f) ||
                !RaceScreenPose(frame, policy, screen.pose)) {
                return screen;
            }
            screen.half_width_meters = 0.5f * policy.config.hud_width_meters;
            screen.half_height_meters = screen.half_width_meters / picture_aspect;
            screen.valid = true;
            return screen;
        }

        if (frame.render_width[0] == 0 || frame.render_height[0] == 0 || !MenuScreenPose(frame, screen.pose)) {
            return screen;
        }
        const float eye_aspect =
            static_cast<float>(frame.render_width[0]) / static_cast<float>(frame.render_height[0]);
        const std::array<float, 2> extents = wii_remote::MenuPictureHalfExtents(
            std::max(0.25f, frame.presentation.quad_width_meters), eye_aspect, snapshot_aspect, picture_aspect);
        screen.half_width_meters = extents[0];
        screen.half_height_meters = extents[1];
        screen.valid = true;
        return screen;
    }

    // The settings panel's rectangle: centred on the same screen, a fixed
    // fraction of its width, the way Aurora lays it over the eyes
    // (aurora_imgui_set_stereo_overlay). Unlike the pointer's picture it is
    // there in a race even when the 2D layer is not on the screen.
    OpenXRPointerScreen SettingsPanelScreen(const OpenXRBackendFrame& frame, const MkwVRPolicySnapshot& policy,
                                            bool immersive) const noexcept {
        OpenXRPointerScreen screen{};
        const float screen_width =
            immersive ? policy.config.hud_width_meters : std::max(0.25f, frame.presentation.quad_width_meters);
        if (!(screen_width > 0.0f) ||
            !(immersive ? RaceScreenPose(frame, policy, screen.pose) : MenuScreenPose(frame, screen.pose))) {
            return screen;
        }
        const std::array<float, 2> extents = settings_panel::HalfExtents(screen_width);
        screen.half_width_meters = extents[0];
        screen.half_height_meters = extents[1];
        screen.valid = true;
        return screen;
    }

    // Android: `adb shell setprop debug.wiicompiled.panel_layer 0` draws the settings panel into
    // the eyes again, to compare the two ways or to rule out a runtime's quad layers. Read about
    // once a second.
    bool PanelLayerForcedOff() noexcept {
#if defined(__ANDROID__)
        if (panel_layer_poll_ == 0) {
            panel_layer_poll_ = 72;
            char value[PROP_VALUE_MAX] = {};
            const bool off =
                __system_property_get("debug.wiicompiled.panel_layer", value) > 0 && value[0] == '0';
            if (off != panel_layer_forced_off_) {
                panel_layer_forced_off_ = off;
                RT_LOG(RT_TAG_RUNTIME) << "OpenXR: settings panel "
                                       << (off ? "drawn into the eyes" : "on its own layer")
                                       << " (debug.wiicompiled.panel_layer)" << std::endl;
            }
        }
        --panel_layer_poll_;
        return panel_layer_forced_off_;
#else
        return false;
#endif
    }

    // Android: `adb shell setprop debug.wiicompiled.eye_passes 0` replays each eye in one render
    // pass per recorded pass again, and 1 forces the single pass, to compare the two within one
    // session. An empty value hands the switch back to the settings. Read about once a second.
    void PollEyePassesOverride() noexcept {
#if defined(__ANDROID__)
        if (eye_passes_poll_ != 0) {
            --eye_passes_poll_;
            return;
        }
        eye_passes_poll_ = 72;
        char value[PROP_VALUE_MAX] = {};
        int override_value = -1;
        if (__system_property_get("debug.wiicompiled.eye_passes", value) > 0 &&
            (value[0] == '0' || value[0] == '1')) {
            override_value = value[0] - '0';
        }
        if (override_value == eye_passes_override_) {
            return;
        }
        eye_passes_override_ = override_value;
        const bool single =
            override_value >= 0 ? override_value == 1 : RuntimeConfigFile::VrSinglePassEyes();
        aurora_set_stereo_single_pass_eyes(single);
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: eyes replayed in "
                               << (single ? "one render pass" : "one render pass per recorded pass")
                               << (override_value >= 0 ? " (debug.wiicompiled.eye_passes)"
                                                       : " (debug.wiicompiled.eye_passes cleared)")
                               << std::endl;
#endif
    }

    // Android: `adb shell setprop debug.wiicompiled.foveation <0-3>` overrides the foveation level
    // (off, low, medium, high) within one session, for A/B timing; an empty value hands it back to
    // the settings. Levels need a session launched with foveation on (see VrFoveation). Read about
    // once a second.
    void PollFoveationOverride() noexcept {
#if defined(__ANDROID__)
        if (foveation_poll_ != 0) {
            --foveation_poll_;
            return;
        }
        foveation_poll_ = 72;
        char value[PROP_VALUE_MAX] = {};
        int override_value = -1;
        if (__system_property_get("debug.wiicompiled.foveation", value) > 0 && value[0] >= '0' &&
            value[0] <= '3' && value[1] == '\0') {
            override_value = value[0] - '0';
        }
        if (override_value == foveation_override_) {
            return;
        }
        foveation_override_ = override_value;
        const uint32_t level = override_value >= 0
                                   ? static_cast<uint32_t>(override_value)
                                   : RuntimeConfigFile::VrFoveationLevelIndex(RuntimeConfigFile::VrFoveation());
        aurora_set_stereo_foveation(level);
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: foveation " << RuntimeConfigFile::kVrFoveationLevels[level]
                               << (override_value >= 0 ? " (debug.wiicompiled.foveation)"
                                                       : " (debug.wiicompiled.foveation cleared)")
                               << (aurora_stereo_foveation_available() ? "" : "; unavailable in this session")
                               << std::endl;
#endif
    }

    // The settings panel's layer hangs exactly where its pointer hits are
    // tested, the rectangle it used to cover in the eyes.
    static void PlacePanelLayer(OpenXRBackendFrame& frame, const OpenXRPointerScreen& screen) noexcept {
        OpenXRPanelLayer& panel = frame.presentation.panel;
        panel.placed = panel.requested && screen.valid;
        if (panel.placed) {
            panel.pose = screen.pose;
            panel.width_meters = 2.0f * screen.half_width_meters;
            panel.height_meters = 2.0f * screen.half_height_meters;
        }
    }

    // Centre of the race's 2D screen. ViewFromBase maps a point p of the
    // recorded centre-eye space (in metres) to base + lean * p in the
    // application space, so the screen Aurora hangs hud_distance_meters ahead
    // sits at base + lean * (0, 0, -distance), turned by the lean.
    bool RaceScreenPose(const OpenXRBackendFrame& frame, const MkwVRPolicySnapshot& policy,
                        XrPosef& pose) const noexcept {
        const OpenXRFrame& xr_frame = frame.xr_frame;
        const float distance = policy.config.hud_distance_meters;
        if (!(distance > 0.0f)) {
            return false;
        }
        std::array<float, 3> base{};
        if (base_position_valid_ && last_immersive_) {
            base = base_position_;
        } else if (xr_frame.views_valid &&
                   (xr_frame.view_state_flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
                   (xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
            // BuildPublishedFrame latches exactly this for the frame.
            base = CenterPosition(xr_frame);
        } else {
            return false;
        }
        const float half_angle = 0.5f * lean_back_degrees_.load(std::memory_order_relaxed) * kDegreesToRadians;
        const Quaternion lean{std::sin(half_angle), 0.0f, 0.0f, std::cos(half_angle)};
        const std::array<float, 3> ahead = Rotate(lean, {0.0f, 0.0f, -distance});
        pose.orientation = {lean.x, lean.y, lean.z, lean.w};
        pose.position = {base[0] + ahead[0], base[1] + ahead[1], base[2] + ahead[2]};
        return true;
    }

    // Centre of the menu quad: where UpdateVirtualScreenPose anchored it, or
    // its head-locked fallback straight ahead of the head.
    bool MenuScreenPose(const OpenXRBackendFrame& frame, XrPosef& pose) const noexcept {
        if (frame.presentation.mode != OpenXRFrameMode::VirtualScreen) {
            return false;
        }
        if (frame.presentation.quad_anchored) {
            pose = frame.presentation.quad_pose;
            return true;
        }
        const OpenXRFrame& xr_frame = frame.xr_frame;
        if (!xr_frame.views_valid || (xr_frame.view_state_flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0 ||
            (xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
            return false;
        }
        const auto& head = xr_frame.views[0].pose.orientation;
        const Quaternion orientation = Normalize({head.x, head.y, head.z, head.w});
        const std::array<float, 3> center = CenterPosition(xr_frame);
        const std::array<float, 3> ahead =
            Rotate(orientation, {0.0f, 0.0f, -std::max(0.25f, frame.presentation.quad_distance_meters)});
        pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
        pose.position = {center[0] + ahead[0], center[1] + ahead[1], center[2] + ahead[2]};
        return true;
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

    void SetInterpolationActive(bool active) noexcept {
        std::lock_guard lock(interpolation_mutex_);
        aurora_set_stereo_frame_interpolation(active && !interpolation_stopping_);
    }

    void UpdateFrameTiming(const OpenXRFrame& frame) noexcept {
        float hz = 0;
        if (get_display_refresh_rate_ == nullptr ||
            XR_FAILED(get_display_refresh_rate_(runtime_->Session(), &hz)) || !(hz > 0)) {
            if (frame.predicted_display_period > 0)
                hz = static_cast<float>(1.0e9 / static_cast<double>(frame.predicted_display_period));
        }
        headset_hz_.store(hz, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - timing_start_).count();
        if (elapsed >= 1.0f) {
            rendered_fps_.store(static_cast<float>(timing_submissions_) / elapsed, std::memory_order_relaxed);
            timing_start_ = now;
            timing_submissions_ = 0;
        }
    }

    // Pacing thread, only while diagnostics are on.
    void NoteFrameDiagnostics(const OpenXRBackendFrame& frame, bool immersive) {
        const XrViewStateFlags flags = frame.xr_frame.view_state_flags;
        diagnostics::OnFrameBegun(immersive, frame.xr_frame.should_render, frame.xr_frame.views_valid,
                                  (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0,
                                  (flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0);
        if (diagnostics::ConsumeSessionInfoRequest()) {
            LogDiagnosticSession(frame);
        }
        if (frame.xr_frame.should_render && frame.xr_frame.views_valid) {
            diagnostics::OnViewGeometry(DiagnosticViewGeometry(frame));
        }
    }

    // Everything about the headset and runtime that a pacing report is read
    // against. Written when logging starts and again for every new session.
    void LogDiagnosticSession(const OpenXRBackendFrame& frame) const {
        const auto& info = runtime_->RuntimeInfo();
        std::ostringstream line;
        line << "session: runtime '" << info.runtime_name << "' " << XR_VERSION_MAJOR(info.runtime_version) << '.'
             << XR_VERSION_MINOR(info.runtime_version) << '.' << XR_VERSION_PATCH(info.runtime_version)
             << ", system '" << info.system_name << "', vendor 0x" << std::hex << info.vendor_id << std::dec
             << ", orientation tracking " << info.supports_orientation_tracking << ", position tracking "
             << info.supports_position_tracking << ", max layers " << info.max_layer_count;
        diagnostics::Info(line.str());

        line.str({});
        line << "session: " << kGraphicsBackendName << " backend, reference space "
             << DiagnosticSpaceName(runtime_->AppSpaceType()) << ", blend mode "
             << DiagnosticBlendModeName(runtime_->EnvironmentBlendMode()) << ", extensions";
        for (const std::string& extension : runtime_->EnabledExtensions()) {
            line << ' ' << extension;
        }
        diagnostics::Info(line.str());

        line.str({});
        const auto& views = runtime_->ViewConfiguration();
        line << "session: recommended eye size " << views[0].properties.recommendedImageRectWidth << 'x'
             << views[0].properties.recommendedImageRectHeight << " / "
             << views[1].properties.recommendedImageRectWidth << 'x'
             << views[1].properties.recommendedImageRectHeight << ", max "
             << views[0].properties.maxImageRectWidth << 'x' << views[0].properties.maxImageRectHeight
             << ", render_scale " << runtime_->Config().resolution_scale << ", swapchains "
             << views[0].render_width << 'x' << views[0].render_height << " / " << views[1].render_width << 'x'
             << views[1].render_height;
        diagnostics::Info(line.str());

        line.str({});
        const uint32_t interpolation = frame_interpolation_fps_.load(std::memory_order_relaxed);
        line << "session: display period ";
        if (frame.xr_frame.predicted_display_period > 0) {
            const double period_ms = static_cast<double>(frame.xr_frame.predicted_display_period) / 1.0e6;
            line << period_ms << " ms (" << 1000.0 / period_ms << " Hz)";
        } else {
            line << "unknown";
        }
        line << ", refresh-rate extension " << (get_display_refresh_rate_ != nullptr ? "yes" : "no")
             << ", display-time conversion " << (convert_display_time_ != nullptr ? "yes" : "no")
             << ", VR frame interpolation "
             << (interpolation == 0 ? std::string("off")
                 : interpolation == 1 ? std::string("auto")
                                      : std::to_string(interpolation))
             << (FrameInterpolationAvailable() ? "" : " (unavailable)");
        diagnostics::Info(line.str());
    }

    // Converts the compositor's predicted display time onto the runtime's
    // steady clock, which is what Aurora's interpolation deadlines are paced by.
    uint64_t DisplayTimeNanos(XrTime display_time) noexcept {
        if (convert_display_time_ == nullptr) return 0;
        const auto now = std::chrono::steady_clock::now();
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
#if defined(_WIN32)
        LARGE_INTEGER display_counter{}, counter{}, frequency{};
        if (XR_FAILED(convert_display_time_(runtime_->Instance(), display_time, &display_counter)) ||
            !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
            !QueryPerformanceCounter(&counter)) return 0;
        const auto delta = static_cast<int64_t>(
            (static_cast<double>(display_counter.QuadPart) - static_cast<double>(counter.QuadPart)) *
            1.0e9 / static_cast<double>(frequency.QuadPart));
#else
        // XR_KHR_convert_timespec_time yields CLOCK_MONOTONIC, the clock behind
        // libc++'s steady_clock, so the delta is measured on that clock too.
        timespec display_spec{};
        timespec now_spec{};
        if (XR_FAILED(convert_display_time_(runtime_->Instance(), display_time, &display_spec)) ||
            clock_gettime(CLOCK_MONOTONIC, &now_spec) != 0) return 0;
        const int64_t display_ns = static_cast<int64_t>(display_spec.tv_sec) * 1'000'000'000ll + display_spec.tv_nsec;
        const int64_t monotonic_ns = static_cast<int64_t>(now_spec.tv_sec) * 1'000'000'000ll + now_spec.tv_nsec;
        const int64_t delta = display_ns - monotonic_ns;
#endif
        return now_ns + delta > 0 ? static_cast<uint64_t>(now_ns + delta) : 0;
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
    std::unique_ptr<GraphicsBackend> backend_;
#if defined(__ANDROID__)
    uint32_t panel_layer_poll_ = 0;
    bool panel_layer_forced_off_ = false;
    uint32_t eye_passes_poll_ = 0;
    int eye_passes_override_ = -1;
    uint32_t foveation_poll_ = 0;
    int foveation_override_ = -1;
#endif
    std::unique_ptr<OpenXRInput> input_;
    std::thread pacing_thread_;
    std::atomic_bool stop_{false};
    std::atomic_bool running_{false};
    std::atomic_bool teardown_requested_{false};
    std::atomic_bool recenter_requested_{false};
    std::atomic<float> lean_back_degrees_{RuntimeConfigFile::VrLeanBackDegrees()};
    std::atomic_bool passthrough_{RuntimeConfigFile::VrPassthrough()};
    std::atomic_uint32_t frame_interpolation_fps_{RuntimeConfigFile::VrFrameInterpolationFps()};
    std::atomic_bool interpolation_available_{false};
    std::mutex interpolation_mutex_;
    bool interpolation_stopping_ = true;
    FrameInterpolationPacing interpolation_pacing_;
    std::atomic<float> headset_hz_{0};
    std::atomic<float> rendered_fps_{0};
    std::chrono::steady_clock::time_point timing_start_ = std::chrono::steady_clock::now();
    uint32_t timing_submissions_ = 0;
    PFN_xrGetDisplayRefreshRateFB get_display_refresh_rate_ = nullptr;
    PFN_xrPerfSettingsSetPerformanceLevelEXT set_performance_level_ = nullptr;
#if defined(_WIN32)
    using ConvertDisplayTime = XrResult (XRAPI_PTR*)(XrInstance, XrTime, LARGE_INTEGER*);
#else
    using ConvertDisplayTime = XrResult (XRAPI_PTR*)(XrInstance, XrTime, struct timespec*);
#endif
    ConvertDisplayTime convert_display_time_ = nullptr;
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
    bool hand_meshes_loaded_ = false;
    uint64_t applied_session_run_serial_ = 0;
    bool session_was_active_ = false;
    bool requested_ = false;
    bool prepared_ = false;
    bool provider_registered_ = false;
    bool graphics_retained_ = false;
    uint32_t game_thread_id_ = 0;
};

#endif // MKW_OPENXR_GRAPHICS_BACKEND

} // namespace

OpenXRStartupResult OpenXRPrepareAurora(AuroraConfig& config) {
#if !defined(MKW_ENABLE_OPENXR)
    (void)config;
    ConfigurePolicy(false);
    return RuntimeConfigFile::VrEnabled(kVrEnabledDefault) ? OpenXRStartupResult::Unavailable
                                                           : OpenXRStartupResult::Disabled;
#elif MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().Prepare(config);
#else
    (void)config;
    ConfigurePolicy(RuntimeConfigFile::VrEnabled(kVrEnabledDefault));
    if (!RuntimeConfigFile::VrEnabled(kVrEnabledDefault)) {
        return OpenXRStartupResult::Disabled;
    }
    RT_LOG(RT_TAG_RUNTIME) << "OpenXR is not wired to a graphics backend on this platform" << std::endl;
    return OpenXRStartupResult::Unavailable;
#endif
}

bool OpenXRStartAfterAurora(AuroraBackend active_backend) {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().Start(active_backend);
#else
    (void)active_backend;
    return !RuntimeConfigFile::VrEnabled(kVrEnabledDefault);
#endif
}

void OpenXRShutdownBeforeAurora() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().Shutdown();
#else
    MkwVRPolicySetSessionActive(false);
#endif
}

void OpenXRServiceProducerFrameBoundary() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().ServiceProducerFrameBoundary();
#endif
}

bool OpenXRIsRunning() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().IsRunning();
#else
    return false;
#endif
}

void OpenXRApplyControllerState() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRApplyVirtualGamepad();
#endif
}

void OpenXRRequestRecenter() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().RequestRecenter();
#endif
}

void OpenXRSetLeanBackDegrees(float degrees) noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetLeanBackDegrees(degrees);
#else
    (void)degrees;
#endif
}

void OpenXRSetPassthrough(bool enabled) noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetPassthrough(enabled);
#else
    (void)enabled;
#endif
}

void OpenXRSetFrameInterpolationFps(uint32_t target) noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetFrameInterpolationFps(target);
#else
    (void)target;
#endif
}

OpenXRFrameTiming OpenXRGetFrameTiming() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().FrameTiming();
#else
    return {};
#endif
}

bool OpenXRFrameInterpolationAvailable() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().FrameInterpolationAvailable();
#else
    return false;
#endif
}

std::string OpenXRLastError() {
#if !defined(MKW_ENABLE_OPENXR)
    return "this build was compiled without OpenXR support";
#elif MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().LastError();
#else
    return "OpenXR is not wired to a graphics backend on this platform";
#endif
}

} // namespace mkw::vr
