// SPDX-License-Identifier: GPL-3.0-or-later

// The runtime source tree is globbed even in explicitly non-VR builds. Keep
// this translation unit dependency-free in that configuration; callers that
// include the public OpenXR headers must use the same feature guard.
#if defined(MKW_ENABLE_OPENXR)

#include "vr/openxr_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

namespace mkw::vr {
namespace {

template <typename T>
bool Contains(const std::vector<T>& values, const T& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void CopyOpenXRName(char* destination, size_t destination_size, std::string_view source) {
    if (destination_size == 0) {
        return;
    }
    const size_t length = std::min(destination_size - 1, source.size());
    std::memcpy(destination, source.data(), length);
    destination[length] = '\0';
}

XrPosef IdentityPose() {
    return {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
}

uint32_t ScaledDimension(uint32_t recommended, uint32_t maximum, float scale) {
    const double scaled = std::round(static_cast<double>(recommended) *
                                     static_cast<double>(scale));
    const double clamped = std::clamp(
        scaled, 1.0, static_cast<double>(std::max(maximum, 1u)));
    return static_cast<uint32_t>(clamped);
}

const char* SessionStateName(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_UNKNOWN:
        return "UNKNOWN";
    case XR_SESSION_STATE_IDLE:
        return "IDLE";
    case XR_SESSION_STATE_READY:
        return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED:
        return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:
        return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:
        return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:
        return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING:
        return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:
        return "EXITING";
    default:
        return "INVALID";
    }
}

} // namespace

OpenXRRuntime::OpenXRRuntime(OpenXRLogCallback logger)
    : m_logger(std::move(logger)) {}

OpenXRRuntime::~OpenXRRuntime() {
    Shutdown();
}

bool OpenXRRuntime::Initialize(const OpenXRConfig& config) {
    ClearError();
    if (IsInitialized()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "Initialize",
                    "OpenXR is already initialized");
    }
    if (config.application_name.empty()) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "Initialize",
                    "application_name must not be empty");
    }
    if (!std::isfinite(config.resolution_scale) || config.resolution_scale <= 0.0f) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "Initialize",
                    "resolution_scale must be finite and greater than zero");
    }

    m_config = config;
    if (!EnumerateInstanceCapabilities() || !CreateInstance() ||
        !InitializeSystem() || !EnumerateViewConfiguration() ||
        !SelectEnvironmentBlendMode()) {
        if (m_instance != XR_NULL_HANDLE) {
            xrDestroyInstance(m_instance);
        }
        ResetInstanceState();
        return false;
    }

    std::ostringstream message;
    message << "OpenXR initialized: runtime '" << m_runtime_info.runtime_name
            << "', system '" << m_runtime_info.system_name << "'";
    Log(OpenXRLogLevel::Info, message.str());
    return true;
}

bool OpenXRRuntime::EnumerateInstanceCapabilities() {
    uint32_t extension_count = 0;
    if (!Check(xrEnumerateInstanceExtensionProperties(
                   nullptr, 0, &extension_count, nullptr),
               "xrEnumerateInstanceExtensionProperties(count)")) {
        return false;
    }

    std::vector<XrExtensionProperties> extension_properties(
        extension_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (extension_count != 0 &&
        !Check(xrEnumerateInstanceExtensionProperties(
                   nullptr, extension_count, &extension_count,
                   extension_properties.data()),
               "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }

    m_available_extensions.clear();
    m_available_extensions.reserve(extension_count);
    for (const XrExtensionProperties& extension : extension_properties) {
        m_available_extensions.emplace_back(extension.extensionName);
    }

    uint32_t layer_count = 0;
    if (!Check(xrEnumerateApiLayerProperties(0, &layer_count, nullptr),
               "xrEnumerateApiLayerProperties(count)")) {
        return false;
    }
    std::vector<XrApiLayerProperties> layer_properties(
        layer_count, XrApiLayerProperties{XR_TYPE_API_LAYER_PROPERTIES});
    if (layer_count != 0 &&
        !Check(xrEnumerateApiLayerProperties(
                   layer_count, &layer_count, layer_properties.data()),
               "xrEnumerateApiLayerProperties")) {
        return false;
    }

    m_available_api_layers.clear();
    m_available_api_layers.reserve(layer_count);
    for (const XrApiLayerProperties& layer : layer_properties) {
        m_available_api_layers.emplace_back(layer.layerName);
    }
    return true;
}

bool OpenXRRuntime::CreateInstance() {
    m_enabled_extensions.clear();
    for (const std::string& extension : m_config.required_extensions) {
        if (!Contains(m_available_extensions, extension)) {
            return Fail(XR_ERROR_EXTENSION_NOT_PRESENT, "xrCreateInstance",
                        "required extension is unavailable: " + extension);
        }
        if (!Contains(m_enabled_extensions, extension)) {
            m_enabled_extensions.push_back(extension);
        }
    }
    for (const std::string& extension : m_config.optional_extensions) {
        if (Contains(m_available_extensions, extension) &&
            !Contains(m_enabled_extensions, extension)) {
            m_enabled_extensions.push_back(extension);
        }
    }

    m_enabled_api_layers.clear();
    for (const std::string& layer : m_config.required_api_layers) {
        if (!Contains(m_available_api_layers, layer)) {
            return Fail(XR_ERROR_API_LAYER_NOT_PRESENT, "xrCreateInstance",
                        "required API layer is unavailable: " + layer);
        }
        if (!Contains(m_enabled_api_layers, layer)) {
            m_enabled_api_layers.push_back(layer);
        }
    }
    for (const std::string& layer : m_config.optional_api_layers) {
        if (Contains(m_available_api_layers, layer) &&
            !Contains(m_enabled_api_layers, layer)) {
            m_enabled_api_layers.push_back(layer);
        }
    }

    std::vector<const char*> extension_names;
    extension_names.reserve(m_enabled_extensions.size());
    for (const std::string& extension : m_enabled_extensions) {
        extension_names.push_back(extension.c_str());
    }
    std::vector<const char*> layer_names;
    layer_names.reserve(m_enabled_api_layers.size());
    for (const std::string& layer : m_enabled_api_layers) {
        layer_names.push_back(layer.c_str());
    }

    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    CopyOpenXRName(create_info.applicationInfo.applicationName,
                   XR_MAX_APPLICATION_NAME_SIZE, m_config.application_name);
    create_info.applicationInfo.applicationVersion = m_config.application_version;
    CopyOpenXRName(create_info.applicationInfo.engineName,
                   XR_MAX_ENGINE_NAME_SIZE, m_config.engine_name);
    create_info.applicationInfo.engineVersion = m_config.engine_version;
    create_info.applicationInfo.apiVersion = m_config.api_version;
    create_info.enabledExtensionCount =
        static_cast<uint32_t>(extension_names.size());
    create_info.enabledExtensionNames = extension_names.data();
    create_info.enabledApiLayerCount = static_cast<uint32_t>(layer_names.size());
    create_info.enabledApiLayerNames = layer_names.data();

    if (!Check(xrCreateInstance(&create_info, &m_instance), "xrCreateInstance")) {
        return false;
    }

    XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (!Check(xrGetInstanceProperties(m_instance, &instance_properties),
               "xrGetInstanceProperties")) {
        return false;
    }
    m_runtime_info.runtime_name = instance_properties.runtimeName;
    m_runtime_info.runtime_version = instance_properties.runtimeVersion;
    return true;
}

bool OpenXRRuntime::InitializeSystem() {
    XrSystemGetInfo get_info{XR_TYPE_SYSTEM_GET_INFO};
    get_info.formFactor = m_config.form_factor;
    if (!Check(xrGetSystem(m_instance, &get_info, &m_system_id), "xrGetSystem")) {
        return false;
    }

    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (!Check(xrGetSystemProperties(m_instance, m_system_id, &properties),
               "xrGetSystemProperties")) {
        return false;
    }
    m_runtime_info.system_name = properties.systemName;
    m_runtime_info.vendor_id = properties.vendorId;
    m_runtime_info.max_layer_count = properties.graphicsProperties.maxLayerCount;
    m_runtime_info.supports_orientation_tracking =
        properties.trackingProperties.orientationTracking == XR_TRUE;
    m_runtime_info.supports_position_tracking =
        properties.trackingProperties.positionTracking == XR_TRUE;
    return true;
}

bool OpenXRRuntime::EnumerateViewConfiguration() {
    uint32_t view_count = 0;
    if (!Check(xrEnumerateViewConfigurationViews(
                   m_instance, m_system_id, m_config.view_configuration,
                   0, &view_count, nullptr),
               "xrEnumerateViewConfigurationViews(count)")) {
        return false;
    }
    if (view_count != kOpenXREyeCount) {
        std::ostringstream detail;
        detail << "PRIMARY_STEREO must expose exactly " << kOpenXREyeCount
               << " views, runtime returned " << view_count;
        return Fail(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED,
                    "xrEnumerateViewConfigurationViews", detail.str());
    }

    std::array<XrViewConfigurationView, kOpenXREyeCount> properties{};
    for (XrViewConfigurationView& property : properties) {
        property.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    if (!Check(xrEnumerateViewConfigurationViews(
                   m_instance, m_system_id, m_config.view_configuration,
                   view_count, &view_count, properties.data()),
               "xrEnumerateViewConfigurationViews")) {
        return false;
    }

    for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
        OpenXRViewConfiguration& destination = m_view_configuration[eye];
        destination.properties = properties[eye];
        destination.render_width = ScaledDimension(
            properties[eye].recommendedImageRectWidth,
            properties[eye].maxImageRectWidth, m_config.resolution_scale);
        destination.render_height = ScaledDimension(
            properties[eye].recommendedImageRectHeight,
            properties[eye].maxImageRectHeight, m_config.resolution_scale);
    }
    return true;
}

bool OpenXRRuntime::SelectEnvironmentBlendMode() {
    uint32_t blend_mode_count = 0;
    if (!Check(xrEnumerateEnvironmentBlendModes(
                   m_instance, m_system_id, m_config.view_configuration,
                   0, &blend_mode_count, nullptr),
               "xrEnumerateEnvironmentBlendModes(count)")) {
        return false;
    }
    if (blend_mode_count == 0) {
        return Fail(XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED,
                    "xrEnumerateEnvironmentBlendModes",
                    "runtime returned no environment blend modes");
    }

    m_supported_blend_modes.resize(blend_mode_count);
    if (!Check(xrEnumerateEnvironmentBlendModes(
                   m_instance, m_system_id, m_config.view_configuration,
                   blend_mode_count, &blend_mode_count,
                   m_supported_blend_modes.data()),
               "xrEnumerateEnvironmentBlendModes")) {
        return false;
    }
    m_supported_blend_modes.resize(blend_mode_count);

    if (Contains(m_supported_blend_modes, m_config.preferred_blend_mode)) {
        m_blend_mode = m_config.preferred_blend_mode;
        return true;
    }
    if (Contains(m_supported_blend_modes, XR_ENVIRONMENT_BLEND_MODE_OPAQUE)) {
        m_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    } else {
        m_blend_mode = m_supported_blend_modes.front();
    }
    Log(OpenXRLogLevel::Warning,
        "preferred environment blend mode is unavailable; using runtime fallback");
    return true;
}

bool OpenXRRuntime::CreateSession(const void* graphics_binding) {
    ClearError();
    if (!IsInitialized()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "CreateSession",
                    "Initialize must succeed first");
    }
    if (m_instance_loss_pending) {
        return Fail(XR_ERROR_INSTANCE_LOST, "CreateSession",
                    "the OpenXR instance is loss-pending");
    }
    if (HasSession()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "CreateSession",
                    "a session already exists");
    }
    if (graphics_binding == nullptr) {
        return Fail(XR_ERROR_GRAPHICS_DEVICE_INVALID, "CreateSession",
                    "graphics binding must not be null");
    }

    XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
    create_info.next = graphics_binding;
    create_info.systemId = m_system_id;
    if (!Check(xrCreateSession(m_instance, &create_info, &m_session),
               "xrCreateSession")) {
        m_session = XR_NULL_HANDLE;
        return false;
    }

    if (!CreateReferenceSpaces() || !EnumerateSwapchainFormats()) {
        DestroyReferenceSpaces();
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
        ResetSessionState();
        return false;
    }

    m_session_state = XR_SESSION_STATE_UNKNOWN;
    m_exit_requested = false;
    Log(OpenXRLogLevel::Info,
        "OpenXR session created; waiting for the runtime READY event");
    return true;
}

bool OpenXRRuntime::GetInstanceProcAddress(
    const char* name, PFN_xrVoidFunction* function) {
    ClearError();
    if (!IsInitialized()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "xrGetInstanceProcAddr",
                    "OpenXR is not initialized");
    }
    if (name == nullptr || name[0] == '\0' || function == nullptr) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "xrGetInstanceProcAddr",
                    "function name and output pointer must be valid");
    }
    *function = nullptr;
    return Check(xrGetInstanceProcAddr(m_instance, name, function),
                 "xrGetInstanceProcAddr");
}

bool OpenXRRuntime::CreateReferenceSpaces() {
    uint32_t space_count = 0;
    if (!Check(xrEnumerateReferenceSpaces(m_session, 0, &space_count, nullptr),
               "xrEnumerateReferenceSpaces(count)")) {
        return false;
    }
    m_supported_reference_spaces.resize(space_count);
    if (space_count != 0 &&
        !Check(xrEnumerateReferenceSpaces(
                   m_session, space_count, &space_count,
                   m_supported_reference_spaces.data()),
               "xrEnumerateReferenceSpaces")) {
        return false;
    }
    m_supported_reference_spaces.resize(space_count);

    XrReferenceSpaceCreateInfo view_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    view_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    view_info.poseInReferenceSpace = IdentityPose();
    if (!Check(xrCreateReferenceSpace(m_session, &view_info, &m_view_space),
               "xrCreateReferenceSpace(VIEW)")) {
        return false;
    }

    m_app_space_type = m_config.reference_space;
    if (!Contains(m_supported_reference_spaces, m_app_space_type)) {
        if (Contains(m_supported_reference_spaces, XR_REFERENCE_SPACE_TYPE_LOCAL)) {
            m_app_space_type = XR_REFERENCE_SPACE_TYPE_LOCAL;
            Log(OpenXRLogLevel::Warning,
                "requested reference space is unavailable; using LOCAL");
        } else if (Contains(m_supported_reference_spaces,
                            XR_REFERENCE_SPACE_TYPE_STAGE)) {
            m_app_space_type = XR_REFERENCE_SPACE_TYPE_STAGE;
            Log(OpenXRLogLevel::Warning,
                "requested reference space is unavailable; using STAGE");
        } else {
            return Fail(XR_ERROR_REFERENCE_SPACE_UNSUPPORTED,
                        "xrCreateReferenceSpace",
                        "runtime exposes neither requested, LOCAL, nor STAGE space");
        }
    }

    XrReferenceSpaceCreateInfo app_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    app_info.referenceSpaceType = m_app_space_type;
    app_info.poseInReferenceSpace = IdentityPose();
    return Check(xrCreateReferenceSpace(m_session, &app_info, &m_app_space),
                 "xrCreateReferenceSpace(application)");
}

bool OpenXRRuntime::EnumerateSwapchainFormats() {
    uint32_t format_count = 0;
    if (!Check(xrEnumerateSwapchainFormats(
                   m_session, 0, &format_count, nullptr),
               "xrEnumerateSwapchainFormats(count)")) {
        return false;
    }
    if (format_count == 0) {
        return Fail(XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED,
                    "xrEnumerateSwapchainFormats",
                    "runtime returned no swapchain formats");
    }
    m_swapchain_formats.resize(format_count);
    if (!Check(xrEnumerateSwapchainFormats(
                   m_session, format_count, &format_count,
                   m_swapchain_formats.data()),
               "xrEnumerateSwapchainFormats")) {
        return false;
    }
    m_swapchain_formats.resize(format_count);
    return true;
}

OpenXREventStatus OpenXRRuntime::PollEvents() {
    if (!IsInitialized()) {
        Fail(XR_ERROR_CALL_ORDER_INVALID, "PollEvents",
             "OpenXR is not initialized");
        return OpenXREventStatus::Error;
    }

    for (;;) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult result = xrPollEvent(m_instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) {
            return ShouldExit() ? OpenXREventStatus::ExitRequested
                                : OpenXREventStatus::Continue;
        }
        if (XR_FAILED(result)) {
            Check(result, "xrPollEvent");
            if (result == XR_ERROR_INSTANCE_LOST) {
                m_instance_loss_pending = true;
            }
            return OpenXREventStatus::Error;
        }

        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& state_event =
                *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (state_event.session == m_session &&
                !HandleSessionStateChanged(state_event)) {
                return OpenXREventStatus::Error;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            m_instance_loss_pending = true;
            Log(OpenXRLogLevel::Warning,
                "OpenXR runtime reported instance loss pending");
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto& space_event =
                *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
            if (space_event.session == m_session) {
                ++m_reference_space_change.serial;
                m_reference_space_change.type = space_event.referenceSpaceType;
                m_reference_space_change.change_time = space_event.changeTime;
                m_reference_space_change.pose_in_previous_space_valid =
                    space_event.poseValid == XR_TRUE;
                m_reference_space_change.pose_in_previous_space =
                    space_event.poseInPreviousSpace;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
            const auto& lost_event =
                *reinterpret_cast<const XrEventDataEventsLost*>(&event);
            std::ostringstream message;
            message << "OpenXR runtime lost " << lost_event.lostEventCount
                    << " event(s)";
            Log(OpenXRLogLevel::Warning, message.str());
            break;
        }
        default:
            break;
        }
    }
}

bool OpenXRRuntime::HandleSessionStateChanged(
    const XrEventDataSessionStateChanged& event) {
    m_session_state = event.state;
    std::ostringstream message;
    message << "OpenXR session state -> " << SessionStateName(event.state);
    Log(OpenXRLogLevel::Info, message.str());

    switch (event.state) {
    case XR_SESSION_STATE_READY: {
        if (m_shutting_down_session || m_session_running) {
            return true;
        }
        XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
        begin_info.primaryViewConfigurationType = m_config.view_configuration;
        if (!Check(xrBeginSession(m_session, &begin_info), "xrBeginSession")) {
            return false;
        }
        m_session_running = true;
        return true;
    }
    case XR_SESSION_STATE_STOPPING:
        if (m_session_running) {
            if (!Check(xrEndSession(m_session), "xrEndSession")) {
                return false;
            }
            m_session_running = false;
        }
        return true;
    case XR_SESSION_STATE_EXITING:
        m_exit_requested = true;
        m_session_running = false;
        return true;
    case XR_SESSION_STATE_LOSS_PENDING:
        m_instance_loss_pending = true;
        m_session_running = false;
        return true;
    default:
        return true;
    }
}

bool OpenXRRuntime::RequestExitSession() {
    ClearError();
    if (!HasSession() || !m_session_running) {
        return true;
    }
    return Check(xrRequestExitSession(m_session), "xrRequestExitSession");
}

OpenXRFrameStatus OpenXRRuntime::WaitFrame(OpenXRFrame& frame) {
    ClearError();
    if (ShouldExit()) {
        return OpenXRFrameStatus::ExitRequested;
    }
    if (!HasSession() || !m_session_running) {
        return OpenXRFrameStatus::SessionNotRunning;
    }
    if (m_frame_phase != FramePhase::Idle) {
        Fail(XR_ERROR_CALL_ORDER_INVALID, "xrWaitFrame",
             "the previous frame has not been ended");
        return OpenXRFrameStatus::Error;
    }

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(m_session, &wait_info, &state), "xrWaitFrame")) {
        return OpenXRFrameStatus::Error;
    }

    frame = {};
    frame.serial = m_next_frame_serial++;
    frame.predicted_display_time = state.predictedDisplayTime;
    frame.predicted_display_period = state.predictedDisplayPeriod;
    frame.should_render = state.shouldRender == XR_TRUE;
    m_active_frame_serial = frame.serial;
    m_active_frame_display_time = frame.predicted_display_time;
    m_frame_phase = FramePhase::Waited;
    return OpenXRFrameStatus::Ready;
}

bool OpenXRRuntime::BeginFrame(const OpenXRFrame& frame) {
    ClearError();
    if (!IsFrameTokenCurrent(frame, FramePhase::Waited)) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "xrBeginFrame",
                    "frame token is stale or xrWaitFrame was not called");
    }

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult result = xrBeginFrame(m_session, &begin_info);
    if (XR_FAILED(result)) {
        m_frame_phase = FramePhase::Idle;
        m_active_frame_serial = 0;
        m_active_frame_display_time = 0;
        return Check(result, "xrBeginFrame");
    }
    m_frame_phase = FramePhase::Begun;
    return true;
}

bool OpenXRRuntime::LocateViews(OpenXRFrame& frame) {
    ClearError();
    if (!IsFrameTokenCurrent(frame, FramePhase::Begun)) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "xrLocateViews",
                    "frame token is stale or xrBeginFrame was not called");
    }
    frame.views_valid = false;
    frame.view_state_flags = 0;
    if (!frame.should_render) {
        return true;
    }

    for (XrView& view : frame.views) {
        view = {XR_TYPE_VIEW};
    }
    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = m_config.view_configuration;
    locate_info.displayTime = frame.predicted_display_time;
    locate_info.space = m_app_space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    uint32_t view_count = 0;
    if (!Check(xrLocateViews(m_session, &locate_info, &view_state,
                             kOpenXREyeCount, &view_count, frame.views.data()),
               "xrLocateViews")) {
        return false;
    }
    if (view_count != kOpenXREyeCount) {
        return Fail(XR_ERROR_RUNTIME_FAILURE, "xrLocateViews",
                    "runtime returned an unexpected stereo view count");
    }

    frame.view_state_flags = view_state.viewStateFlags;
    frame.views_valid =
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    return true;
}

bool OpenXRRuntime::EndFrame(
    const OpenXRFrame& frame,
    const XrCompositionLayerBaseHeader* const* layers,
    uint32_t layer_count) {
    ClearError();
    if (!IsFrameTokenCurrent(frame, FramePhase::Begun)) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "xrEndFrame",
                    "frame token is stale or xrBeginFrame was not called");
    }
    if (layer_count != 0 && layers == nullptr) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "xrEndFrame",
                    "non-zero layer_count requires a layer array");
    }

    // The runtime explicitly requested no application rendering. Ending with an
    // empty layer list preserves the frame protocol without presenting stale work.
    if (!frame.should_render) {
        layers = nullptr;
        layer_count = 0;
    }

    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame.predicted_display_time;
    end_info.environmentBlendMode = m_blend_mode;
    end_info.layerCount = layer_count;
    end_info.layers = layers;
    const XrResult result = xrEndFrame(m_session, &end_info);
    m_frame_phase = FramePhase::Idle;
    m_active_frame_serial = 0;
    m_active_frame_display_time = 0;
    return Check(result, "xrEndFrame");
}

bool OpenXRRuntime::EndFrame(
    const OpenXRFrame& frame,
    const std::vector<const XrCompositionLayerBaseHeader*>& layers) {
    return EndFrame(frame, layers.data(), static_cast<uint32_t>(layers.size()));
}

bool OpenXRRuntime::EndFrameWithoutLayers(const OpenXRFrame& frame) {
    return EndFrame(frame, nullptr, 0);
}

bool OpenXRRuntime::ResetAppSpace(const XrPosef& pose_in_reference_space) {
    ClearError();
    if (!HasSession()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "ResetAppSpace",
                    "no OpenXR session exists");
    }
    if (m_frame_phase != FramePhase::Idle) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "ResetAppSpace",
                    "reference space cannot change during a frame");
    }

    XrReferenceSpaceCreateInfo create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    create_info.referenceSpaceType = m_app_space_type;
    create_info.poseInReferenceSpace = pose_in_reference_space;
    XrSpace replacement = XR_NULL_HANDLE;
    if (!Check(xrCreateReferenceSpace(m_session, &create_info, &replacement),
               "xrCreateReferenceSpace(recenter)")) {
        return false;
    }
    if (m_app_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_app_space);
    }
    m_app_space = replacement;
    return true;
}

void OpenXRRuntime::DestroySession() {
    if (!HasSession()) {
        ResetSessionState();
        return;
    }

    if (m_frame_phase == FramePhase::Begun) {
        Log(OpenXRLogLevel::Warning,
            "ending an active OpenXR frame without layers during teardown");
        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime = m_active_frame_display_time;
        end_info.environmentBlendMode = m_blend_mode;
        const XrResult end_result = xrEndFrame(m_session, &end_info);
        if (XR_FAILED(end_result)) {
            Log(OpenXRLogLevel::Warning,
                "xrEndFrame failed during session teardown");
        }
        m_frame_phase = FramePhase::Idle;
        m_active_frame_serial = 0;
        m_active_frame_display_time = 0;
    }

    m_shutting_down_session = true;
    if (m_session_running) {
        const XrResult request_result = xrRequestExitSession(m_session);
        if (XR_FAILED(request_result)) {
            Log(OpenXRLogLevel::Warning,
                "xrRequestExitSession failed during bounded teardown");
        } else {
            const auto timeout =
                std::chrono::milliseconds(m_config.shutdown_timeout_ms);
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (m_session_running &&
                   std::chrono::steady_clock::now() < deadline) {
                if (PollEvents() == OpenXREventStatus::Error) {
                    break;
                }
                if (m_session_running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (m_session_running) {
                Log(OpenXRLogLevel::Warning,
                    "OpenXR runtime did not finish session exit before timeout");
            }
        }
    }

    DestroyReferenceSpaces();
    const XrResult result = xrDestroySession(m_session);
    if (XR_FAILED(result)) {
        Log(OpenXRLogLevel::Warning, "xrDestroySession failed");
    }
    m_session = XR_NULL_HANDLE;
    ResetSessionState();
}

void OpenXRRuntime::Shutdown() {
    DestroySession();
    if (m_instance != XR_NULL_HANDLE) {
        const XrResult result = xrDestroyInstance(m_instance);
        if (XR_FAILED(result)) {
            Log(OpenXRLogLevel::Warning, "xrDestroyInstance failed");
        }
    }
    ResetInstanceState();
}

void OpenXRRuntime::DestroyReferenceSpaces() {
    if (m_view_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_view_space);
        m_view_space = XR_NULL_HANDLE;
    }
    if (m_app_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_app_space);
        m_app_space = XR_NULL_HANDLE;
    }
}

void OpenXRRuntime::ResetSessionState() {
    m_session_state = XR_SESSION_STATE_UNKNOWN;
    m_app_space_type = m_config.reference_space;
    m_session_running = false;
    m_exit_requested = false;
    m_shutting_down_session = false;
    m_frame_phase = FramePhase::Idle;
    m_active_frame_serial = 0;
    m_active_frame_display_time = 0;
    m_supported_reference_spaces.clear();
    m_swapchain_formats.clear();
    m_reference_space_change = {};
}

void OpenXRRuntime::ResetInstanceState() {
    m_instance = XR_NULL_HANDLE;
    m_system_id = XR_NULL_SYSTEM_ID;
    m_instance_loss_pending = false;
    m_runtime_info = {};
    m_view_configuration = {};
    m_available_extensions.clear();
    m_available_api_layers.clear();
    m_enabled_extensions.clear();
    m_enabled_api_layers.clear();
    m_supported_blend_modes.clear();
    ResetSessionState();
}

bool OpenXRRuntime::IsFrameTokenCurrent(
    const OpenXRFrame& frame, FramePhase expected) const {
    return m_frame_phase == expected && frame.serial != 0 &&
           frame.serial == m_active_frame_serial;
}

bool OpenXRRuntime::Check(XrResult result, std::string_view operation) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    return Fail(result, operation, {});
}

bool OpenXRRuntime::Fail(
    XrResult result, std::string_view operation, std::string_view detail) {
    m_last_error.result = result;
    m_last_error.operation.assign(operation);
    std::ostringstream message;
    message << operation << " failed: " << ResultString(result);
    if (!detail.empty()) {
        message << " (" << detail << ')';
    }
    m_last_error.message = message.str();
    Log(OpenXRLogLevel::Error, m_last_error.message);
    return false;
}

void OpenXRRuntime::ClearError() {
    m_last_error = {};
}

void OpenXRRuntime::Log(
    OpenXRLogLevel level, std::string_view message) const noexcept {
    if (!m_logger) {
        return;
    }
    try {
        m_logger(level, message);
    } catch (...) {
        // Diagnostic callbacks must never make XR teardown throw.
    }
}

std::string OpenXRRuntime::ResultString(XrResult result) const {
    if (m_instance != XR_NULL_HANDLE) {
        char text[XR_MAX_RESULT_STRING_SIZE]{};
        if (XR_SUCCEEDED(xrResultToString(m_instance, result, text))) {
            return text;
        }
    }
    return std::to_string(static_cast<int32_t>(result));
}

} // namespace mkw::vr

#endif // MKW_ENABLE_OPENXR
