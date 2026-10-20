// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR)

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "vr/openxr_input.h"
#include "physical_wheel.h"
#include "runtime_config.h"
#include "settings_overlay.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/openxr_diagnostics.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <time.h>
#endif

namespace mkw::vr {
namespace {

// SDL holds its joystick lock for as long as an enumeration takes, and the
// Bluetooth Wii Remote rescan (F10 > Controller settings > Keep scanning)
// closes and reopens every HID device twice per scan: 15 ms on a plain desk,
// over 200 ms on a machine carrying several HID devices, such as a Lighthouse
// setup's base-station dongles. The pacing thread must never wait on that,
// because the OpenXR frame it holds open costs the compositor every display
// slot that passes. So it leaves the gamepad here, and the game thread writes
// it to SDL where it already polls controllers.
//
// The relay also owns the virtual joystick. Attaching and detaching it take the
// same lock, so a live switch to or from the None controller mode (which
// unplugs it, so the VR controllers hold no port) is followed on the game
// thread too; only the input's creation and destruction plug it in or out
// where they run.
//
// m_sdl is the lock the SDL work, and the joystick, are under; the pacing
// thread only ever takes m_state, and only for the copy. Both are taken in that
// order.
class VirtualGamepadRelay {
public:
    struct Pad {
        std::array<int16_t, SDL_GAMEPAD_AXIS_COUNT> axes{};
        std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    };

    // The controllers' input exists: plugs the joystick in unless the mode is
    // None. `logger` reports a joystick SDL refuses, from whichever thread.
    void Open(OpenXRLogCallback logger) {
        std::scoped_lock lock(m_sdl, m_state);
        m_logger = std::move(logger);
        m_open = true;
        m_attach_failed = false;
        m_pending = false;
        FollowControllerMode();
    }

    // The controllers' input is going away: unplugs the joystick.
    void Close() {
        std::scoped_lock lock(m_sdl, m_state);
        m_open = false;
        Detach();
        m_pending = false;
        m_logger = {};
    }

    // The plugged-in joystick's SDL_JoystickID, 0 while unplugged.
    uint32_t JoystickId() const noexcept { return m_id.load(std::memory_order_relaxed); }

    // Pacing thread.
    void Publish(const Pad& pad) {
        std::lock_guard lock(m_state);
        m_pad = pad;
        m_pending = true;
    }

    // Game thread. Holding m_sdl here is what keeps Close from closing the
    // joystick underneath the writes.
    void Apply() {
        std::lock_guard sdl(m_sdl);
        FollowControllerMode();
        Pad pad;
        {
            std::lock_guard lock(m_state);
            if (!m_pending || m_joystick == nullptr) {
                return;
            }
            pad = m_pad;
            m_pending = false;
        }
        for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
            SDL_SetJoystickVirtualAxis(m_joystick, static_cast<SDL_GamepadAxis>(axis),
                                       pad.axes[static_cast<size_t>(axis)]);
        }
        for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
            SDL_SetJoystickVirtualButton(m_joystick, static_cast<SDL_GamepadButton>(button),
                                         pad.buttons[static_cast<size_t>(button)]);
        }
    }

private:
    // m_sdl held. A joystick SDL refused is not asked for again until the mode
    // unplugs it or the input is created anew.
    void FollowControllerMode() {
        if (!m_open) {
            return;
        }
        if (OpenXRGetControllerMode() == OpenXRControllerMode::None) {
            m_attach_failed = false;
            Detach();
        } else if (m_joystick == nullptr && !m_attach_failed) {
            m_attach_failed = !Attach();
        }
    }

    bool Attach() {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                           (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                           (1u << SDL_GAMEPAD_BUTTON_START) | (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) |
                           (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK) |
                           (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) |
                           (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY) |
                         (1u << SDL_GAMEPAD_AXIS_RIGHTX) | (1u << SDL_GAMEPAD_AXIS_RIGHTY) |
                         (1u << SDL_GAMEPAD_AXIS_LEFT_TRIGGER) | (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        desc.name = "OpenXR Touch Controllers";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        if (id == 0) {
            Refused("SDL_AttachVirtualJoystick");
            return false;
        }
        SDL_Joystick* joystick = SDL_OpenJoystick(id);
        if (joystick == nullptr) {
            Refused("SDL_OpenJoystick");
            SDL_DetachVirtualJoystick(id);
            return false;
        }
        m_joystick = joystick;
        m_id.store(id, std::memory_order_relaxed);
        return true;
    }

    void Detach() {
        const SDL_JoystickID id = m_id.exchange(0, std::memory_order_relaxed);
        if (m_joystick != nullptr) {
            SDL_CloseJoystick(m_joystick);
            m_joystick = nullptr;
        }
        if (id != 0) {
            SDL_DetachVirtualJoystick(id);
        }
    }

    void Refused(const char* operation) const {
        if (!m_logger) {
            return;
        }
        try {
            m_logger(OpenXRLogLevel::Warning, std::string(operation) + " failed: " + SDL_GetError() +
                                                  "; OpenXR controllers will not reach the game");
        } catch (...) {
        }
    }

    std::mutex m_sdl;
    std::mutex m_state;
    OpenXRLogCallback m_logger;
    bool m_open = false;
    bool m_attach_failed = false;
    SDL_Joystick* m_joystick = nullptr;
    std::atomic<uint32_t> m_id{0};
    Pad m_pad;
    bool m_pending = false;
};

VirtualGamepadRelay& Relay() {
    static VirtualGamepadRelay relay;
    return relay;
}

#if defined(__ANDROID__)
// Debug-only remote button presses for headset experiments driven over adb, so
// a menu can be reached without someone wearing the headset:
//   adb shell setprop debug.wiicompiled.inject <sequence>:<button>
// A new sequence number holds the button for kInjectHoldFrames XR frames.
// Buttons: a, b, x, y, start, up, down, left, right, and for the Wii Remote
// presentation also home, c and z (x/y/start press 1/2/+ there, and the
// directions push the Nunchuk stick). `panel` presses the settings panel's
// button (left Y, or both thumbsticks for a gamepad), opening or closing it,
// where `a` then selects. `flick` plays the bare hands' flick, one shake of the
// remote; `throw_forward` and `throw_backward` play a throw of the held item.
// The property is unset in normal use, so this costs one property read every
// few frames.
constexpr uint32_t kInjectHoldFrames = 12;
constexpr uint32_t kInjectPollFrames = 4;

struct InjectedPress {
    long sequence = -1;
    std::string button;
    uint32_t frames_left = 0;
    uint32_t poll_countdown = 0;
};

InjectedPress& Injection() {
    static InjectedPress press;
    return press;
}

void PollInjection() {
    InjectedPress& press = Injection();
    if (press.frames_left > 0) {
        --press.frames_left;
    }
    if (press.poll_countdown > 0) {
        --press.poll_countdown;
        return;
    }
    press.poll_countdown = kInjectPollFrames;
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.wiicompiled.inject", value) <= 0) {
        return;
    }
    char* end = nullptr;
    const long sequence = std::strtol(value, &end, 10);
    if (end == value || *end != ':' || sequence == press.sequence) {
        return;
    }
    const bool first_read = press.sequence < 0;
    press.sequence = sequence;
    if (first_read) {
        return; // A value left over from an earlier run is not a new press.
    }
    press.button = end + 1;
    press.frames_left = kInjectHoldFrames;
}

bool Injected(const char* button) {
    const InjectedPress& press = Injection();
    return press.frames_left > 0 && press.button == button;
}

using ConvertNowToXrTime = XrResult(XRAPI_PTR*)(XrInstance, const struct timespec*, XrTime*);
#else
void PollInjection() {}
bool Injected(const char*) { return false; }

#if defined(_WIN32)
using ConvertNowToXrTime = XrResult(XRAPI_PTR*)(XrInstance, const LARGE_INTEGER*, XrTime*);
#endif
#endif

constexpr uint32_t kHandCount = 2;

// Re-sent every frame while the game holds the motor on, so a rumble whose stop
// never arrives (or a stalled pacing thread) dies out on its own.
constexpr XrDuration kRumblePulseNs = 50'000'000;

struct Binding {
    XrAction* action;
    const char* path;
};

Sint16 ToAxis(float value) noexcept {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<Sint16>(std::lround(clamped * 32767.0f));
}

// Trigger axes are reported by SDL gamepads on the positive half only.
Sint16 ToTrigger(float value) noexcept {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<Sint16>(std::lround(clamped * 32767.0f));
}

wii_remote::Pose ToWiiRemotePose(const XrPosef& pose) noexcept {
    return {{pose.position.x, pose.position.y, pose.position.z},
            {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}};
}

// The adb injection's buttons as the Wii Remote presentation's WPAD bits.
uint32_t InjectedWiiRemoteButtons() {
    uint32_t hold = 0;
    const auto press = [&hold](const char* name, uint32_t bit) {
        if (Injected(name)) {
            hold |= bit;
        }
    };
    press("a", wii_remote::kButtonA);
    press("b", wii_remote::kButtonB);
    press("x", wii_remote::kButtonOne);
    press("y", wii_remote::kButtonTwo);
    press("start", wii_remote::kButtonPlus);
    press("home", wii_remote::kButtonHome);
    press("c", wii_remote::kButtonC);
    press("z", wii_remote::kButtonZ);
    return hold;
}

} // namespace

OpenXRInput::OpenXRInput(OpenXRLogCallback logger) : m_logger(std::move(logger)) {}

OpenXRInput::~OpenXRInput() {
    Destroy();
}

bool OpenXRInput::Create(OpenXRRuntime& runtime) {
    m_last_error.clear();
    if (m_created) {
        return true;
    }
    if (!runtime.IsInitialized() || !runtime.HasSession()) {
        m_last_error = "OpenXR input needs an initialized runtime with a session";
        return false;
    }
    m_runtime = &runtime;

    if (!Check(xrStringToPath(runtime.Instance(), "/user/hand/left", &m_hand_paths[0]),
               "xrStringToPath(/user/hand/left)") ||
        !Check(xrStringToPath(runtime.Instance(), "/user/hand/right", &m_hand_paths[1]),
               "xrStringToPath(/user/hand/right)")) {
        Destroy();
        return false;
    }
    if (!CreateActions() || !SuggestBindings()) {
        Destroy();
        return false;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &m_action_set;
    if (!Check(xrAttachSessionActionSets(runtime.Session(), &attach), "xrAttachSessionActionSets")) {
        Destroy();
        return false;
    }
    m_created = true;
    CreatePoseSpaces();
    LoadInputClock();
    LoadHandTracking();
    Relay().Open(m_logger);
    switch (OpenXRGetControllerMode()) {
    case OpenXRControllerMode::WiiRemote:
        Log(OpenXRLogLevel::Info, "OpenXR controller actions attached (Wii Remote + Nunchuk)");
        break;
    case OpenXRControllerMode::Gamepad:
        Log(OpenXRLogLevel::Info, "OpenXR controller actions attached (gamepad)");
        break;
    case OpenXRControllerMode::None:
        Log(OpenXRLogLevel::Info, "OpenXR controller actions attached (none: the game does not see them)");
        break;
    }
    return true;
}

bool OpenXRInput::CreateActions() {
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(set_info.actionSetName, "mkw_gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    set_info.priority = 0;
    if (!Check(xrCreateActionSet(m_runtime->Instance(), &set_info, &m_action_set), "xrCreateActionSet")) {
        return false;
    }

    struct Spec {
        XrAction* action;
        const char* name;
        const char* localized;
        XrActionType type;
    };
    const std::array<Spec, 10> specs{{
        {&m_thumbstick, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT},
        {&m_thumbstick_click, "thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_squeeze, "squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_button_primary, "button_primary", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_button_secondary, "button_secondary", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_aim_pose, "aim_pose", "Pointer", XR_ACTION_TYPE_POSE_INPUT},
        {&m_grip_pose, "grip_pose", "Motion", XR_ACTION_TYPE_POSE_INPUT},
        {&m_haptic, "haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},
    }};
    for (const Spec& spec : specs) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = spec.type;
        std::strncpy(info.actionName, spec.name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, spec.localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.countSubactionPaths = kHandCount;
        info.subactionPaths = m_hand_paths;
        if (!Check(xrCreateAction(m_action_set, &info, spec.action), spec.name)) {
            return false;
        }
    }
    return true;
}

bool OpenXRInput::SuggestBindings() {
    const auto suggest = [&](const char* profile, const std::vector<Binding>& bindings, bool required) {
        XrPath profile_path = XR_NULL_PATH;
        if (!Check(xrStringToPath(m_runtime->Instance(), profile, &profile_path), profile)) {
            return false;
        }
        std::vector<XrActionSuggestedBinding> suggested;
        suggested.reserve(bindings.size());
        for (const Binding& binding : bindings) {
            XrPath path = XR_NULL_PATH;
            if (XR_FAILED(xrStringToPath(m_runtime->Instance(), binding.path, &path))) {
                continue;
            }
            suggested.push_back({*binding.action, path});
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = profile_path;
        info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
        info.suggestedBindings = suggested.data();
        const XrResult result = xrSuggestInteractionProfileBindings(m_runtime->Instance(), &info);
        m_runtime->ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrSuggestInteractionProfileBindings(" << profile << ") failed (" << result << ')';
            if (required) {
                m_last_error = message.str();
                Log(OpenXRLogLevel::Error, m_last_error);
                return false;
            }
            Log(OpenXRLogLevel::Warning, message.str());
        }
        return true;
    };

    // Meta Quest Touch controllers (Quest 2 / 3 / Pro all expose this profile).
    const std::vector<Binding> touch{
        {&m_thumbstick, "/user/hand/left/input/thumbstick"},
        {&m_thumbstick, "/user/hand/right/input/thumbstick"},
        {&m_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
        {&m_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
        {&m_trigger, "/user/hand/left/input/trigger/value"},
        {&m_trigger, "/user/hand/right/input/trigger/value"},
        {&m_squeeze, "/user/hand/left/input/squeeze/value"},
        {&m_squeeze, "/user/hand/right/input/squeeze/value"},
        {&m_button_primary, "/user/hand/left/input/x/click"},
        {&m_button_primary, "/user/hand/right/input/a/click"},
        {&m_button_secondary, "/user/hand/left/input/y/click"},
        {&m_button_secondary, "/user/hand/right/input/b/click"},
        {&m_menu, "/user/hand/left/input/menu/click"},
        {&m_aim_pose, "/user/hand/left/input/aim/pose"},
        {&m_aim_pose, "/user/hand/right/input/aim/pose"},
        {&m_grip_pose, "/user/hand/left/input/grip/pose"},
        {&m_grip_pose, "/user/hand/right/input/grip/pose"},
        {&m_haptic, "/user/hand/left/output/haptic"},
        {&m_haptic, "/user/hand/right/output/haptic"},
    };
    if (!suggest("/interaction_profiles/oculus/touch_controller", touch, true)) {
        return false;
    }
    // Minimal fallback so an unfamiliar runtime still offers a select, a menu
    // and something to point with.
    const std::vector<Binding> simple{
        {&m_button_primary, "/user/hand/right/input/select/click"},
        {&m_button_secondary, "/user/hand/left/input/select/click"},
        {&m_menu, "/user/hand/left/input/menu/click"},
        {&m_aim_pose, "/user/hand/left/input/aim/pose"},
        {&m_aim_pose, "/user/hand/right/input/aim/pose"},
        {&m_grip_pose, "/user/hand/left/input/grip/pose"},
        {&m_grip_pose, "/user/hand/right/input/grip/pose"},
        {&m_haptic, "/user/hand/left/output/haptic"},
        {&m_haptic, "/user/hand/right/output/haptic"},
    };
    suggest("/interaction_profiles/khr/simple_controller", simple, false);
    return true;
}

void OpenXRInput::CreatePoseSpaces() {
    bool logged = false;
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        for (auto [action, spaces] : {std::pair{m_aim_pose, m_aim_spaces}, std::pair{m_grip_pose, m_grip_spaces}}) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = action;
            info.subactionPath = m_hand_paths[hand];
            info.poseInActionSpace.orientation.w = 1.0f;
            const XrResult result = xrCreateActionSpace(m_runtime->Session(), &info, &spaces[hand]);
            m_runtime->ObserveResult(result);
            if (XR_FAILED(result)) {
                spaces[hand] = XR_NULL_HANDLE;
                if (!logged) {
                    logged = true;
                    std::ostringstream message;
                    message << "xrCreateActionSpace failed (" << result
                            << "); the Wii Remote will have no motion or pointer";
                    Log(OpenXRLogLevel::Warning, message.str());
                }
            }
        }
    }
}

void OpenXRInput::DestroyPoseSpaces() {
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        for (XrSpace* space : {&m_aim_spaces[hand], &m_grip_spaces[hand]}) {
            if (*space != XR_NULL_HANDLE) {
                xrDestroySpace(*space);
                *space = XR_NULL_HANDLE;
            }
        }
    }
}

// Poses for input are located at the measured current time, not the frame's
// predicted display time: that lies tens of milliseconds ahead, and the runtime
// extrapolates a fast wrist turn that far past where the hand really is, which
// sprays the pointer and invents acceleration (DolphinXR's fast-motion fix).
void OpenXRInput::LoadInputClock() {
    const auto& extensions = m_runtime->EnabledExtensions();
    const auto enabled = [&](const char* name) {
        return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
    };
    PFN_xrVoidFunction function = nullptr;
#if defined(_WIN32)
    if (enabled("XR_KHR_win32_convert_performance_counter_time")) {
        m_runtime->GetInstanceProcAddress("xrConvertWin32PerformanceCounterToTimeKHR", &function);
    }
#elif defined(__ANDROID__)
    if (enabled("XR_KHR_convert_timespec_time")) {
        m_runtime->GetInstanceProcAddress("xrConvertTimespecTimeToTimeKHR", &function);
    }
#else
    (void)enabled;
#endif
    m_convert_now_to_xr_time = function;
    if (m_convert_now_to_xr_time == nullptr) {
        Log(OpenXRLogLevel::Info,
            "OpenXR offers no clock conversion; controller motion is sampled at display time");
    }
}

XrTime OpenXRInput::InputSampleTime(XrTime predicted_display_time) const {
    if (m_convert_now_to_xr_time == nullptr) {
        return predicted_display_time;
    }
    XrTime now = 0;
#if defined(_WIN32)
    LARGE_INTEGER counter{};
    if (QueryPerformanceCounter(&counter) == 0 ||
        XR_FAILED(reinterpret_cast<ConvertNowToXrTime>(m_convert_now_to_xr_time)(m_runtime->Instance(),
                                                                                  &counter, &now))) {
        return predicted_display_time;
    }
#elif defined(__ANDROID__)
    timespec spec{};
    if (clock_gettime(CLOCK_MONOTONIC, &spec) != 0 ||
        XR_FAILED(reinterpret_cast<ConvertNowToXrTime>(m_convert_now_to_xr_time)(m_runtime->Instance(),
                                                                                  &spec, &now))) {
        return predicted_display_time;
    }
#endif
    return now > 0 ? (std::min)(predicted_display_time, now) : predicted_display_time;
}

// The hand-tracking extensions are asked for at launch when hand steering or
// tracked hands are on (openxr_integration.cpp), so the option itself is live.
void OpenXRInput::LoadHandTracking() {
    const auto& extensions = m_runtime->EnabledExtensions();
    const auto enabled = [&](const char* name) {
        return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
    };
    if (!enabled(XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
        return;
    }
    PFN_xrVoidFunction create = nullptr, destroy = nullptr, locate = nullptr;
    if (!m_runtime->GetInstanceProcAddress("xrCreateHandTrackerEXT", &create) ||
        !m_runtime->GetInstanceProcAddress("xrDestroyHandTrackerEXT", &destroy) ||
        !m_runtime->GetInstanceProcAddress("xrLocateHandJointsEXT", &locate) || create == nullptr ||
        destroy == nullptr || locate == nullptr) {
        return;
    }
    m_create_hand_tracker = reinterpret_cast<PFN_xrCreateHandTrackerEXT>(create);
    m_destroy_hand_tracker = reinterpret_cast<PFN_xrDestroyHandTrackerEXT>(destroy);
    m_locate_hand_joints = reinterpret_cast<PFN_xrLocateHandJointsEXT>(locate);
    m_hand_data_source = enabled(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
    m_hand_aim = enabled(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME);
    if (enabled(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME)) {
        PFN_xrVoidFunction resume = nullptr, pause = nullptr;
        if (m_runtime->GetInstanceProcAddress("xrResumeSimultaneousHandsAndControllersTrackingMETA", &resume) &&
            m_runtime->GetInstanceProcAddress("xrPauseSimultaneousHandsAndControllersTrackingMETA", &pause) &&
            resume != nullptr && pause != nullptr) {
            m_resume_simultaneous = reinterpret_cast<PFN_xrResumeSimultaneousHandsAndControllersTrackingMETA>(resume);
            m_pause_simultaneous = reinterpret_cast<PFN_xrPauseSimultaneousHandsAndControllersTrackingMETA>(pause);
        }
    }
}

// Simultaneous hands and controllers (Meta's "multimodal"): a controller that
// is not in a hand no longer owns it, so the cameras track that hand straight
// away and the system stops switching back to the controllers whenever one
// lying on a table moves. A held controller keeps working, its fingers from its
// touch sensors. Only while tracked hands are on; off, the system's own
// switching between hands and controllers applies as before.
void OpenXRInput::UpdateSimultaneousHandsAndControllers(bool wanted) {
    if (m_resume_simultaneous == nullptr || m_pause_simultaneous == nullptr) {
        return;
    }
    if (!wanted) {
        m_simultaneous_failed = false;
        if (m_simultaneous) {
            XrSimultaneousHandsAndControllersTrackingPauseInfoMETA info{
                XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_PAUSE_INFO_META};
            m_runtime->ObserveResult(m_pause_simultaneous(m_runtime->Session(), &info));
            m_simultaneous = false;
            Log(OpenXRLogLevel::Info, "OpenXR simultaneous hands and controllers off");
        }
        return;
    }
    if (m_simultaneous || m_simultaneous_failed) {
        return;
    }
    XrSimultaneousHandsAndControllersTrackingResumeInfoMETA info{
        XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_RESUME_INFO_META};
    const XrResult result = m_resume_simultaneous(m_runtime->Session(), &info);
    m_runtime->ObserveResult(result);
    if (XR_FAILED(result)) {
        // Not asked again until tracked hands are turned off and on.
        m_simultaneous_failed = true;
        std::ostringstream message;
        message << "xrResumeSimultaneousHandsAndControllersTrackingMETA failed (" << result
                << "); putting a controller down hands over to the cameras only when the system switches";
        Log(OpenXRLogLevel::Warning, message.str());
        return;
    }
    m_simultaneous = true;
    Log(OpenXRLogLevel::Info, "OpenXR simultaneous hands and controllers on: a controller put down hands its "
                              "side to the cameras");
}

// Tracked hands can carry a cockpit item even while stick steering is used.
// They live as long as the session
// otherwise (Idle and the cockpit's reset keep them); a runtime that refuses
// them is not asked again until the option is turned off and on.
void OpenXRInput::UpdateHandTrackers() {
    const bool wanted = RuntimeConfigFile::VrHandTracking() &&
                        (RuntimeConfigFile::VrHandSteering() || RuntimeConfigFile::VrCockpitItemHand() != "off");
    UpdateSimultaneousHandsAndControllers(wanted);
    if (!wanted) {
        DestroyHandTrackers();
        m_hand_trackers_failed = false;
        m_logged_hand_restart = false;
        return;
    }
    if (m_hand_trackers[0] != XR_NULL_HANDLE || m_hand_trackers_failed) {
        return;
    }
    if (m_create_hand_tracker == nullptr) {
        if (!m_logged_hand_restart) {
            m_logged_hand_restart = true;
            Log(OpenXRLogLevel::Info, "OpenXR tracked hands apply after a restart: this session started "
                                      "without XR_EXT_hand_tracking");
        }
        return;
    }
    bool controller_hands = m_hand_data_source;
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        info.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        // Both sources: the cameras once the controllers are put down, the
        // controllers' touch sensors while they are held.
        XrHandTrackingDataSourceEXT sources[]{XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT,
                                              XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT};
        XrHandTrackingDataSourceInfoEXT source_info{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
        source_info.requestedDataSourceCount = 2;
        source_info.requestedDataSources = sources;
        info.next = controller_hands ? &source_info : nullptr;
        XrResult result = m_create_hand_tracker(m_runtime->Session(), &info, &m_hand_trackers[hand]);
        if (XR_FAILED(result) && info.next != nullptr) {
            // The cameras still work without the controller source.
            controller_hands = false;
            info.next = nullptr;
            result = m_create_hand_tracker(m_runtime->Session(), &info, &m_hand_trackers[hand]);
        }
        m_runtime->ObserveResult(result);
        if (XR_FAILED(result)) {
            m_hand_trackers[hand] = XR_NULL_HANDLE;
            DestroyHandTrackers();
            m_hand_trackers_failed = true;
            std::ostringstream message;
            message << "xrCreateHandTrackerEXT failed (" << result
                    << "); the cockpit hands keep following the controllers";
            Log(OpenXRLogLevel::Warning, message.str());
            return;
        }
    }
    Log(OpenXRLogLevel::Info, controller_hands ? "OpenXR tracked hands ready (controller-driven hands: yes)"
                                               : "OpenXR tracked hands ready (controller-driven hands: no)");
}

void OpenXRInput::DestroyHandTrackers() {
    for (XrHandTrackerEXT& tracker : m_hand_trackers) {
        if (tracker != XR_NULL_HANDLE) {
            if (m_destroy_hand_tracker != nullptr) {
                m_destroy_hand_tracker(tracker);
            }
            tracker = XR_NULL_HANDLE;
        }
    }
    m_tracked_hands = {};
    m_joint_frame = {};
}

// Both hands' joints for `time` in the application space, and, when the
// seated frame is valid, in it for the cockpit (m_joint_frame keeps a hand's
// last located joints until new ones arrive; UpdateDriving decides what is
// drawn). A hand counts only when every joint is located.
void OpenXRInput::LocateHands(XrTime time, const driving::SeatFrame& seat) {
    using hand_tracking::Source;
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        TrackedHand& tracked = m_tracked_hands[hand];
        tracked = {};
        if (m_hand_trackers[hand] == XR_NULL_HANDLE || m_locate_hand_joints == nullptr) {
            continue;
        }
        std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locations.jointLocations = joints.data();
        XrHandTrackingDataSourceStateEXT source{XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT};
        XrHandTrackingAimStateFB aim{XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
        void* next = nullptr;
        if (m_hand_data_source) {
            source.next = next;
            next = &source;
        }
        if (m_hand_aim) {
            aim.next = next;
            next = &aim;
        }
        locations.next = next;
        XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        info.baseSpace = m_runtime->AppSpace();
        info.time = time;
        if (XR_FAILED(m_locate_hand_joints(m_hand_trackers[hand], &info, &locations)) ||
            locations.isActive != XR_TRUE) {
            continue;
        }
        constexpr XrSpaceLocationFlags kValid =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (!std::all_of(joints.begin(), joints.end(),
                         [](const XrHandJointLocationEXT& joint) { return (joint.locationFlags & kValid) == kValid; })) {
            continue;
        }
        tracked.active = true;
        tracked.source = !m_hand_data_source || source.isActive != XR_TRUE ? Source::Unknown
                         : source.dataSource == XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT ? Source::Controller
                                                                                           : Source::Camera;
        if (m_hand_aim) {
            tracked.aim_valid = (aim.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB) != 0;
            tracked.aim_pinching = (aim.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB) != 0;
            tracked.aim_menu = (aim.status & XR_HAND_TRACKING_AIM_MENU_PRESSED_BIT_FB) != 0;
            tracked.aim_system_gesture = (aim.status & XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB) != 0;
        }
        for (size_t joint = 0; joint < hand_tracking::kJointCount; ++joint) {
            const XrPosef& pose = joints[joint].pose;
            tracked.positions[joint] = {pose.position.x, pose.position.y, pose.position.z};
            if (seat.valid) {
                m_joint_frame.seat_from_joint[hand][joint] =
                    driving::SeatFromApp(seat, {pose.position.x, pose.position.y, pose.position.z},
                                         {pose.orientation.x, pose.orientation.y, pose.orientation.z,
                                          pose.orientation.w});
                m_joint_frame.radius[hand][joint] = joints[joint].radius;
            }
        }
        tracked.seated = seat.valid;
    }
    // Camera-tracked hands drop in and out of view often; a change is logged at
    // most once a second, so the log still ends on the settled state.
    constexpr XrTime kSourceLogIntervalNs = 1'000'000'000;
    if ((m_tracked_hands[0].source != m_logged_sources[0] || m_tracked_hands[1].source != m_logged_sources[1]) &&
        (m_sources_logged_at == 0 || time - m_sources_logged_at >= kSourceLogIntervalNs)) {
        m_sources_logged_at = time;
        m_logged_sources = {m_tracked_hands[0].source, m_tracked_hands[1].source};
        std::ostringstream message;
        message << "OpenXR tracked hands: left " << hand_tracking::SourceLabel(m_logged_sources[0]) << ", right "
                << hand_tracking::SourceLabel(m_logged_sources[1]);
        Log(OpenXRLogLevel::Info, message.str());
    }
}

// Which interaction profile each hand moved to, whenever the runtime reports a
// change: on the Quest, the Touch profile with controllers and
// khr/simple_controller for bare hands.
void OpenXRInput::LogInteractionProfiles() {
    std::ostringstream message;
    message << "OpenXR interaction profiles:";
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        message << (hand == 0 ? " left " : ", right ");
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        char path[XR_MAX_PATH_LENGTH]{};
        uint32_t length = 0;
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_runtime->Session(), m_hand_paths[hand], &state)) &&
            state.interactionProfile != XR_NULL_PATH &&
            XR_SUCCEEDED(xrPathToString(m_runtime->Instance(), state.interactionProfile, sizeof(path), &length,
                                        path))) {
            message << path;
        } else {
            message << "none";
        }
    }
    Log(OpenXRLogLevel::Info, message.str());
}

void OpenXRApplyVirtualGamepad() noexcept {
    Relay().Apply();
}

void OpenXRInput::Destroy() {
    // The game must stop reading a remote whose controllers are going away.
    OpenXRWithdrawWiiRemote();
    ResetDriving();
    if (m_created) {
        StopRumble();
    }
    Relay().Close();
    DestroyHandTrackers();
    m_create_hand_tracker = nullptr;
    m_destroy_hand_tracker = nullptr;
    m_locate_hand_joints = nullptr;
    m_hand_data_source = m_hand_aim = false;
    // The session, and simultaneous tracking with it, goes away after this.
    m_resume_simultaneous = nullptr;
    m_pause_simultaneous = nullptr;
    m_simultaneous = m_simultaneous_failed = false;
    m_hand_trackers_failed = m_logged_hand_restart = false;
    m_logged_sources = {};
    m_sources_logged_at = 0;
    m_squeeze_active = m_hand_driven = m_pinch = {};
    m_injected_flick_held = false;
    m_injected_throw_held = false;
    m_profile_serial = 0;
    DestroyPoseSpaces();
    if (m_action_set != XR_NULL_HANDLE) {
        // Destroying the set destroys every action created from it.
        xrDestroyActionSet(m_action_set);
        m_action_set = XR_NULL_HANDLE;
    }
    m_thumbstick = m_thumbstick_click = m_trigger = m_squeeze = XR_NULL_HANDLE;
    m_button_primary = m_button_secondary = m_menu = m_haptic = XR_NULL_HANDLE;
    m_aim_pose = m_grip_pose = XR_NULL_HANDLE;
    m_hand_paths[0] = m_hand_paths[1] = XR_NULL_PATH;
    m_convert_now_to_xr_time = nullptr;
    for (auto& motion : m_motion) {
        motion.Rest();
    }
    m_pointer.Reset();
    m_horizon = {1.0f, 0.0f};
    m_panel_controls.Reset();
    m_last_input_time = 0;
    m_panel_select_held = false;
    m_created = false;
    m_runtime = nullptr;
}

void OpenXRInput::Idle() {
    if (!m_created) {
        return;
    }
    for (auto& motion : m_motion) {
        motion.Rest();
    }
    m_pointer.Reset();
    m_horizon = {1.0f, 0.0f};
    OpenXRPublishWiiRemote(Relay().JoystickId(), OpenXRWiiRemoteSample{});
    // The panel stays as it was; only what the controllers were holding is forgotten.
    m_panel_controls.Reset();
    m_last_input_time = 0;
    m_panel_select_held = false;
    OpenXRPublishSettingsPanelPointer(false, 0.0f, 0.0f, false, 0.0f);
    m_first_person_click.Reset();
    // The trackers stay with the session; only this frame's hands are forgotten.
    m_tracked_hands = {};
    m_squeeze_active = m_hand_driven = m_pinch = {};
    ResetDriving();
    StopRumble();
    // Nothing stays held on the gamepad either while input is away.
    Relay().Publish({});
}

void OpenXRInput::Sync(XrTime predicted_display_time, const OpenXRPointerScreen& screen,
                       const OpenXRPointerScreen& settings_panel, const driving::SeatFrame& seat) {
    if (!m_created || m_runtime == nullptr) {
        return;
    }
    if (!m_runtime->IsSessionFocused()) {
        Idle();
        return;
    }
    XrActiveActionSet active{m_action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    const XrResult result = diagnostics::Measure(diagnostics::Stage::SyncActions, [&] {
        return xrSyncActions(m_runtime->Session(), &sync);
    });
    m_runtime->ObserveResult(result);
    if (XR_FAILED(result)) {
        if (!m_logged_sync_failure) {
            m_logged_sync_failure = true;
            std::ostringstream message;
            message << "xrSyncActions failed (" << result << ')';
            Log(OpenXRLogLevel::Warning, message.str());
        }
        Idle();
        return;
    }

    // `active`, when given, says whether the action is bound to a source the
    // runtime has right now (a controller, or a tracked hand).
    const auto boolean = [&](XrAction action, uint32_t hand, bool* active = nullptr) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        const bool bound = XR_SUCCEEDED(xrGetActionStateBoolean(m_runtime->Session(), &info, &state)) &&
                           state.isActive == XR_TRUE;
        if (active != nullptr) {
            *active = bound;
        }
        return bound && state.currentState == XR_TRUE;
    };
    const auto scalar = [&](XrAction action, uint32_t hand, bool* active = nullptr) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        const bool bound = XR_SUCCEEDED(xrGetActionStateFloat(m_runtime->Session(), &info, &state)) &&
                           state.isActive == XR_TRUE;
        if (active != nullptr) {
            *active = bound;
        }
        return bound ? state.currentState : 0.0f;
    };
    const auto vector = [&](XrAction action, uint32_t hand) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xrGetActionStateVector2f(m_runtime->Session(), &info, &state)) ||
            state.isActive != XR_TRUE) {
            return XrVector2f{0.0f, 0.0f};
        }
        return state.currentState;
    };

    std::array<wii_remote::HandInputs, kHands> hands{};
    // simple_controller binds select to the right primary and the left secondary action.
    std::array<bool, kHands> select_active{};
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        wii_remote::HandInputs& inputs = hands[hand];
        bool primary_active = false, secondary_active = false, squeeze_active = false;
        inputs.primary = boolean(m_button_primary, hand, &primary_active);
        inputs.secondary = boolean(m_button_secondary, hand, &secondary_active);
        inputs.menu = boolean(m_menu, hand);
        inputs.thumbstick_click = boolean(m_thumbstick_click, hand);
        inputs.trigger = scalar(m_trigger, hand);
        inputs.squeeze = scalar(m_squeeze, hand, &squeeze_active);
        const XrVector2f stick = vector(m_thumbstick, hand);
        inputs.stick_x = stick.x;
        inputs.stick_y = stick.y;
        m_squeeze_active[hand] = squeeze_active;
        select_active[hand] = hand == 1 ? primary_active : secondary_active;
    }

    PollInjection();
    if (Injected("up")) {
        hands[0].stick_y = 1.0f;
    } else if (Injected("down")) {
        hands[0].stick_y = -1.0f;
    } else if (Injected("left")) {
        hands[0].stick_x = -1.0f;
    } else if (Injected("right")) {
        hands[0].stick_x = 1.0f;
    }

    if (m_runtime->InteractionProfileSerial() != m_profile_serial) {
        m_profile_serial = m_runtime->InteractionProfileSerial();
        LogInteractionProfiles();
    }
    // Tracked hands, before anything reads the hands (the settings panel, the
    // wheel, the game).
    const bool hand_tracking_on = RuntimeConfigFile::VrHandTracking();
    UpdateHandTrackers();
    LocateHands(predicted_display_time, seat);
#if defined(__ANDROID__)
    // A bare hand (no controller in it: it drives simple_controller, or the
    // cameras track it): its pinch and menu gesture come from the runtime's
    // recognition, and the menu gesture pauses from either hand.
    bool menu_gesture = false;
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        const TrackedHand& located = m_tracked_hands[hand];
        m_hand_driven[hand] = hand_tracking::HandDriven(
            m_squeeze_active[hand], select_active[hand],
            hand_tracking_on && located.active && located.source == hand_tracking::Source::Camera);
        if (!m_hand_driven[hand]) {
            m_pinch[hand] = false;
            continue;
        }
        const hand_tracking::Gestures gestures = hand_tracking::GesturesOf(
            located.aim_valid, located.aim_pinching, located.aim_menu, located.aim_system_gesture,
            hand_tracking::SelectOf(hands[hand], hand), hands[hand].menu);
        m_pinch[hand] = gestures.pinch;
        menu_gesture = menu_gesture || gestures.menu;
        hands[hand].menu = false;
    }
    hand_tracking::ApplyHandDrivenButtons(hands, m_hand_driven, m_pinch, hand_tracking_on);
    if (menu_gesture) {
        hands[0].menu = true;
    }
#else
    (void)select_active;
    (void)hand_tracking_on;
#endif

    const XrTime input_time = InputSampleTime(predicted_display_time);
    const float dt_seconds =
        m_last_input_time != 0 && input_time > m_last_input_time
            ? static_cast<float>(input_time - m_last_input_time) * 1.0e-9f
            : 0.0f;
    m_last_input_time = input_time;
    std::array<wii_remote::HandInputs, kHands> panel_hands = hands;
    if (Injected("panel")) {
        // The panel button in either controller mode.
        panel_hands[0].secondary = true;
        panel_hands[0].thumbstick_click = true;
        panel_hands[1].thumbstick_click = true;
    }
    if (Injected("a")) {
        panel_hands[1].primary = true;
    }
    // The game thread may open or close the panel too; only a change made here
    // is written back.
    const bool was_open = OpenXRSettingsPanelOpen();
    bool open = was_open;
    const OpenXRControllerMode mode = OpenXRGetControllerMode();
    const settings_panel::Frame panel = m_panel_controls.Update(panel_hands, open, dt_seconds, mode);
    // While the panel has the controllers, and always when they are nothing to
    // the game, the game sees them idle.
    const bool withheld = panel.withheld || mode == OpenXRControllerMode::None;
    // Pointer first: the game thread reads it as soon as it sees the panel open.
    PublishSettingsPanel(input_time, settings_panel, panel);
    if (open != was_open) {
        OpenXRSetSettingsPanelOpen(open);
    }

    // A clean right-thumbstick click switches the race in progress between
    // first person and the game's camera. It fires on release, so the
    // two-thumbstick panel chord never switches it, and never while the panel
    // has the controllers. The tick answers only a click that can switch: not
    // in menus, during the race's intro, in Flat Screen or without a single
    // local racer.
    if (m_first_person_click.Update(hands[1].thumbstick_click, hands[0].thumbstick_click,
                                    panel.open || panel.withheld) &&
        RuntimeConfigFile::VrFirstPersonToggleClick()) {
        settings_overlay::RequestFirstPersonToggle();
        if (MkwVRCameraSwitchAvailable()) {
            constexpr XrDuration kToggleTickNs = 20'000'000;
            ApplyHaptic(1, 0.35f, kToggleTickNs);
        }
    }

    // The cockpit's wheel before the game reads the controllers: a held wheel
    // steers through the left stick and keeps its grips from the game.
    UpdateDriving(predicted_display_time, seat, hands, withheld);

#if defined(__ANDROID__)
    // Bare hands in the cockpit. While one of them holds the wheel it holds the
    // gas and a free hand's pinch uses an item; with none on the wheel (the
    // pause menu, the results, coasting) a right pinch stays A. The game's own
    // pointer cannot tell the two apart: MKW keeps it on while driving.
    const bool cockpit_hands =
        hand_tracking_on && m_driving.cockpit_active && m_driving.hand_steering && !withheld;
    std::array<bool, kHands> bare_held{};
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        bare_held[hand] = cockpit_hands && m_bare_latch[hand].Bare() && m_wheel_held[hand];
    }
    if (bare_held[0] || bare_held[1]) {
        std::array<bool, kHands> item_pinch{};
        for (uint32_t hand = 0; hand < kHands; ++hand) {
            const bool pinch = hand_tracking::ItemPinch(m_pinch[hand], m_driving.hands[hand].grasp);
            item_pinch[hand] = m_hand_driven[hand] && m_pinch_gate[hand].Update(pinch, m_wheel_held[hand], dt_seconds);
        }
        hand_tracking::ApplyBareHandRace(hands, bare_held, item_pinch);
    } else {
        // A pinch already held when a hand takes the wheel is not an item.
        for (auto& gate : m_pinch_gate) {
            gate.Reset();
        }
    }
    // A trick or a wheelie: the remote's shake, which the gamepad cannot give.
    if (cockpit_hands) {
        std::array<hand_tracking::FlickHand, kHands> flick{};
        for (uint32_t hand = 0; hand < kHands; ++hand) {
            flick[hand] = {m_bare_latch[hand].Tracked(), m_wheel_held[hand],
                           m_joint_frame.seat_from_joint[hand][hand_tracking::kPalm][7]};
        }
        if (m_flick.Update(flick, dt_seconds) && mode == OpenXRControllerMode::WiiRemote) {
            m_flick_start = input_time;
        }
    } else {
        m_flick.Reset();
    }
    // `debug.wiicompiled.inject <n>:flick` plays the same shake, with the
    // controllers or unattended, to tune it apart from the gesture.
    const bool injected_flick = Injected("flick");
    if (injected_flick && !m_injected_flick_held) {
        m_flick_start = input_time;
    }
    m_injected_flick_held = injected_flick;
#endif

    // Throwing the held item: a quick swing of the item hand forward or back
    // plays an aimed throw on the stick and the item button.
    UpdateItemThrow(dt_seconds, hands, withheld);

    // What the game sees of the controllers: nothing held while they are withheld.
    static const std::array<wii_remote::HandInputs, kHands> kIdleHands{};
    const auto& game_hands = withheld ? kIdleHands : hands;
    const wii_remote::HandInputs& left = game_hands[0];
    const wii_remote::HandInputs& right = game_hands[1];
    const auto injected = [withheld](const char* button) { return !withheld && Injected(button); };

    if (Relay().JoystickId() != 0) {
        VirtualGamepadRelay::Pad pad;
        // OpenXR thumbsticks report +Y up; SDL gamepads report +Y down.
        pad.axes[SDL_GAMEPAD_AXIS_LEFTX] = ToAxis(left.stick_x);
        pad.axes[SDL_GAMEPAD_AXIS_LEFTY] = ToAxis(-left.stick_y);
        pad.axes[SDL_GAMEPAD_AXIS_RIGHTX] = ToAxis(right.stick_x);
        pad.axes[SDL_GAMEPAD_AXIS_RIGHTY] = ToAxis(-right.stick_y);
        pad.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = ToTrigger(left.trigger);
        pad.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = ToTrigger(right.trigger);

        pad.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = right.primary || injected("a");
        pad.buttons[SDL_GAMEPAD_BUTTON_EAST] = right.secondary || injected("b");
        pad.buttons[SDL_GAMEPAD_BUTTON_WEST] = left.primary || injected("x");
        pad.buttons[SDL_GAMEPAD_BUTTON_NORTH] = left.secondary || injected("y");
        pad.buttons[SDL_GAMEPAD_BUTTON_START] = left.menu || injected("start");
        pad.buttons[SDL_GAMEPAD_BUTTON_LEFT_STICK] = left.thumbstick_click;
        pad.buttons[SDL_GAMEPAD_BUTTON_RIGHT_STICK] = right.thumbstick_click;
        pad.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = left.squeeze > 0.5f;
        pad.buttons[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = right.squeeze > 0.5f;
        Relay().Publish(pad);
    }

    PublishWiiRemote(input_time, screen, game_hands, withheld ? 0u : InjectedWiiRemoteButtons(),
                     withheld);
    UpdateRumble();
}

// The pointing hand's aim ray against the whole panel, in canvas pixels, with a
// short tick in that hand when a selection starts.
void OpenXRInput::PublishSettingsPanel(XrTime input_time, const OpenXRPointerScreen& panel,
                                       const settings_panel::Frame& frame) {
    if (!frame.open) {
        // Nothing may still read as held when the panel next opens.
        m_panel_select_held = false;
        OpenXRPublishSettingsPanelPointer(false, 0.0f, 0.0f, false, 0.0f);
        return;
    }
    constexpr XrSpaceLocationFlags kPoseValid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    bool valid = false;
    std::array<float, 2> point{};
    const XrSpace aim_space = m_aim_spaces[frame.pointing_hand];
    if (panel.valid && aim_space != XR_NULL_HANDLE) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(aim_space, m_runtime->AppSpace(), input_time, &location)) &&
            (location.locationFlags & kPoseValid) == kPoseValid) {
            wii_remote::Screen target{};
            target.pose = ToWiiRemotePose(panel.pose);
            target.half_width = panel.half_width_meters;
            target.half_height = panel.half_height_meters;
            const wii_remote::ScreenHit hit = wii_remote::RaycastScreen(ToWiiRemotePose(location.pose), target);
            if (hit.valid) {
                valid = true;
                point = settings_panel::CanvasPoint(hit);
            }
        }
    }
    if (frame.select && !m_panel_select_held) {
        constexpr XrDuration kTickNs = 15'000'000;
        ApplyHaptic(frame.pointing_hand, 0.35f, kTickNs);
    }
    m_panel_select_held = frame.select;
    OpenXRPublishSettingsPanelPointer(valid, point[0], point[1], frame.select, frame.wheel);
}

void OpenXRInput::PublishWiiRemote(XrTime input_time, const OpenXRPointerScreen& screen,
                                   const std::array<wii_remote::HandInputs, kHands>& hands,
                                   uint32_t injected_buttons, bool withheld) {
    constexpr XrSpaceLocationFlags kPoseValid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

    OpenXRWiiRemoteSample sample{};
    sample.hold = wii_remote::RemoteButtons(hands[0], hands[1]) | injected_buttons;
    sample.stick = wii_remote::NunchukStick(hands[0]);

    // Left is the Nunchuk, right is the remote.
    std::array<wii_remote::Pose, kHands> aims{};
    std::array<bool, kHands> aim_valid{};
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        if (m_aim_spaces[hand] != XR_NULL_HANDLE) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(m_aim_spaces[hand], m_runtime->AppSpace(), input_time, &location)) &&
                (location.locationFlags & kPoseValid) == kPoseValid) {
                aims[hand] = ToWiiRemotePose(location.pose);
                aim_valid[hand] = true;
            }
        }
        if (m_hand_driven[hand]) {
            // A bare hand keeps its pointer but is a still remote: camera-tracked
            // poses are too noisy to differentiate twice (turning the wheel would
            // trick and wheelie), and resting every frame clears the history, so
            // picking a controller back up cannot read as a jolt.
            m_motion[hand].Rest();
            (hand == 0 ? sample.nunchuk_acc : sample.acc) = OpenXRWiiRemoteSample{}.acc;
            continue;
        }
        wii_remote::Vec3 grip_position{};
        wii_remote::Vec3 grip_velocity{};
        bool position_valid = false;
        bool velocity_valid = false;
        if (m_grip_spaces[hand] != XR_NULL_HANDLE) {
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            location.next = &velocity;
            if (XR_SUCCEEDED(xrLocateSpace(m_grip_spaces[hand], m_runtime->AppSpace(), input_time, &location))) {
                position_valid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
                velocity_valid = (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
                grip_position = {location.pose.position.x, location.pose.position.y, location.pose.position.z};
                grip_velocity = {velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z};
            }
        }
        const wii_remote::Vec3 acc =
            m_motion[hand].Update(aim_valid[hand] ? &aims[hand].orientation : nullptr,
                                  position_valid ? &grip_position : nullptr,
                                  velocity_valid ? &grip_velocity : nullptr, input_time);
        (hand == 0 ? sample.nunchuk_acc : sample.acc) = acc;
    }

    if (withheld) {
        // Waving a controller around the panel must not trick or wheelie.
        sample.acc = OpenXRWiiRemoteSample{}.acc;
        sample.nunchuk_acc = OpenXRWiiRemoteSample{}.nunchuk_acc;
        m_pointer.Reset();
        m_flick_start = 0;
        OpenXRPublishWiiRemote(Relay().JoystickId(), sample);
        return;
    }
    if (m_flick_start != 0) {
        // A bare-hand flick: one clean shake, whatever the hands' own motion.
        bool playing = false;
        const wii_remote::Vec3 acc = hand_tracking::FlickPulse(input_time - m_flick_start, &playing);
        if (playing) {
            sample.acc = acc;
        } else {
            m_flick_start = 0;
        }
    }

    wii_remote::Screen target{};
    wii_remote::ScreenHit hit{};
    if (screen.valid && aim_valid[1]) {
        target.pose = ToWiiRemotePose(screen.pose);
        target.half_width = screen.half_width_meters;
        target.half_height = screen.half_height_meters;
        hit = wii_remote::RaycastScreen(aims[1], target);
    }
    if (screen.valid && aim_valid[1]) {
        m_horizon = wii_remote::Horizon(aims[1], target);
    }
    const wii_remote::ScreenHit pointer = m_pointer.Update(hit, input_time);
    if (pointer.valid) {
        sample.pointer_valid = true;
        sample.pointer = wii_remote::KpadPosition(pointer);
        // Held with the position through a tracking blip.
        sample.horizon = m_horizon;
        sample.distance_meters = pointer.distance_meters;
        if (!m_logged_pointer) {
            m_logged_pointer = true;
            Log(OpenXRLogLevel::Info, "OpenXR Wii Remote pointer reached the virtual screen");
        }
    }
    OpenXRPublishWiiRemote(Relay().JoystickId(), sample);
}

void OpenXRInput::ResetDriving() {
    m_wheel = {};
    WheelGeometry unused{};
    m_wheel_reference.Resolve(unused, false, false, false, false, 0, 0.0f);
    m_wheel_visual.Reset();
    m_wheel_held = {};
    m_wheel_time = 0;
    m_driving = {};
    m_joint_frame.valid = {};
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        m_bare_latch[hand].Reset();
        m_pinch_gate[hand].Reset();
    }
    m_flick.Reset();
    m_flick_start = 0;
    m_throw.Reset();
    m_throw_sequence.Reset();
    OpenXRPublishDriving(m_driving);
}

void OpenXRInput::UpdateItemThrow(float dt_seconds, std::array<wii_remote::HandInputs, kHands>& hands,
                                  bool withheld) {
    const std::string item_hand = RuntimeConfigFile::VrCockpitItemHand();
    const uint32_t hand = item_hand == "right" ? 1u : 0u;
    item_throw::Direction direction = item_throw::Direction::None;
    if (!withheld && m_driving.cockpit_active && item_hand != "off" && RuntimeConfigFile::VrCockpitItemThrow()) {
        // The hand as the cockpit draws the item on it: the palm joint when the
        // hand is drawn from joints, else the grip. The detector follows it with
        // or without an item, so it always knows when the hand left the wheel.
        const DrivingHand& located = m_driving.hands[hand];
        const std::array<float, 12>& pose =
            located.joints_valid ? m_joint_frame.seat_from_joint[hand][hand_tracking::kPalm] : located.seat_from_grip;
        item_throw::HandSample sample{};
        sample.tracked = located.tracked;
        sample.held = located.held;
        sample.bare = located.bare;
        sample.position = {pose[3], pose[7], pose[11]};
        direction = m_throw.Update(sample, dt_seconds);
        // A swing with nothing to throw throws nothing: an item in the hand, or
        // one already out behind or around the kart, which empties the hand.
        if (direction != item_throw::Direction::None) {
            const HeldItem item = MkwVRFirstPersonGetHeldItem();
            if (!item.tethered && (!item.valid || item.count == 0)) {
                direction = item_throw::Direction::None;
            }
        }
    } else {
        m_throw.Reset();
    }
    // `debug.wiicompiled.inject <n>:throw_forward` or `throw_backward` plays the
    // same throw without the swing, to try it unattended.
    const bool injected_forward = Injected("throw_forward");
    const bool injected_backward = Injected("throw_backward");
    bool injected = false;
    if ((injected_forward || injected_backward) && !m_injected_throw_held && !withheld) {
        direction = injected_forward ? item_throw::Direction::Forward : item_throw::Direction::Backward;
        injected = true;
    }
    m_injected_throw_held = injected_forward || injected_backward;
    if (direction != item_throw::Direction::None) {
        m_throw_sequence.Start(direction);
        if (RuntimeConfigFile::VrWheelTuning().haptics) {
            constexpr XrDuration kThrowPulseNs = 40'000'000;
            ApplyHaptic(hand, 0.5f, kThrowPulseNs);
        }
        // What the swing measured, to tune the thresholds from a run log.
        std::ostringstream message;
        message << "Held item thrown " << item_throw::DirectionLabel(direction);
        if (injected) {
            message << " (injected)";
        } else {
            const auto& measure = m_throw.Last();
            message << " (" << (measure.bare ? "bare hand" : "controller") << ", "
                    << std::lround(std::fabs(measure.travel) * 100.0f) << " cm in "
                    << std::lround(measure.seconds * 1000.0f) << " ms"
                    << (measure.bridged ? ", across a tracking gap" : "") << ')';
        }
        Log(OpenXRLogLevel::Info, message.str());
    }
    if (withheld) {
        m_throw_sequence.Reset();
        return;
    }
    m_throw_sequence.Apply(hands[0], dt_seconds);
}

void OpenXRInput::UpdateDriving(XrTime display_time, const driving::SeatFrame& seat,
                                std::array<wii_remote::HandInputs, kHands>& hands, bool withheld) {
    const FirstPersonAnchor anchor = MkwVRFirstPersonGetAnchor();
    if (!seat.valid || !anchor.valid || !anchor.cockpit) {
        if (m_driving.cockpit_active || m_wheel_time != 0) {
            ResetDriving();
        }
        return;
    }
    const WheelTuning tuning = RuntimeConfigFile::VrWheelTuning();
    const bool hand_steering = RuntimeConfigFile::VrHandSteering();
    const bool steering_wheel = RuntimeConfigFile::VrSteeringWheel();
    const bool native_steering_wheel = RuntimeConfigFile::VrNativeSteeringWheel();
    const bool placeholder_steering_wheel = RuntimeConfigFile::VrPlaceholderSteeringWheel();
    const float dt = m_wheel_time != 0 && display_time > m_wheel_time
                         ? static_cast<float>(display_time - m_wheel_time) * 1.0e-9f
                         : 1.0f / 90.0f;
    m_wheel_time = display_time;

    DrivingSnapshot snapshot{};
    snapshot.cockpit_active = true;
    snapshot.vehicle_identity = anchor.vehicle_identity;
    snapshot.hand_steering = hand_steering;
    snapshot.bike = anchor.bike;
    // A separate wheel only when asked for and the vehicle's own is not the one
    // turning.
    snapshot.synthetic_control = driving::DrawsPlaceholderControl(
        steering_wheel, native_steering_wheel, anchor.native_mesh_prepared, placeholder_steering_wheel);

    // Which control the hands reach for: the vehicle's own wherever its
    // geometry is known and no separate wheel is drawn, a handlebar always
    // (held over a brief gap while gripped), otherwise the VR wheel in front
    // of the seat.
    WheelGeometry geometry = anchor.native_wheel;
    const bool geometry_valid = m_wheel_reference.Resolve(geometry, true, geometry.valid,
                                                          m_wheel_held[0] || m_wheel_held[1], anchor.bike,
                                                          anchor.vehicle_identity, dt);
    if (anchor.bike && !geometry_valid) {
        geometry = {};
        geometry.center = {0.0f, SteeringWheel::Height, SteeringWheel::Depth};
        geometry.right = {1.0f, 0.0f, 0.0f};
        geometry.up = {0.0f, 0.0f, -1.0f};
        geometry.normal = {0.0f, 1.0f, 0.0f};
        geometry.radius = 0.25f;
        geometry.valid = true;
    }
    const bool uses_geometry = anchor.bike || (geometry_valid && !snapshot.synthetic_control);
    if (uses_geometry != m_wheel_uses_geometry || anchor.bike != m_wheel_bike) {
        m_wheel = {};
        m_wheel_uses_geometry = uses_geometry;
        m_wheel_bike = anchor.bike;
    }
    snapshot.control = geometry;

    constexpr XrSpaceLocationFlags kPoseValid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    // The headset in the seated frame, which the character body leans with;
    // nothing else reads it, so it is only located for the body.
    XrSpaceLocation head_location{XR_TYPE_SPACE_LOCATION};
    if (RuntimeConfigFile::VrBodyIk() &&
        XR_SUCCEEDED(xrLocateSpace(m_runtime->ViewSpace(), m_runtime->AppSpace(), display_time, &head_location)) &&
        (head_location.locationFlags & kPoseValid) == kPoseValid) {
        const auto& pose = head_location.pose;
        snapshot.head_tracked = true;
        snapshot.seat_from_head = driving::SeatFromApp(seat, {pose.position.x, pose.position.y, pose.position.z},
                                                       {pose.orientation.x, pose.orientation.y,
                                                        pose.orientation.z, pose.orientation.w});
    }
    const bool hand_tracking_on = RuntimeConfigFile::VrHandTracking();
    // The wheel's own tracking grace (SteeringWheel::Update clamps it the same way).
    const float grace = driving::IsFinite(tuning.trackingGrace) ? std::clamp(tuning.trackingGrace, 0.05f, 0.5f)
                                                                : 0.2f;
    std::array<WheelHand, kHands> wheel_hands{};
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        bool tracked = false;
        std::array<float, 12> seat_from_grip = snapshot.hands[hand].seat_from_grip;
        if (m_grip_spaces[hand] != XR_NULL_HANDLE) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(m_grip_spaces[hand], m_runtime->AppSpace(), display_time, &location)) &&
                (location.locationFlags & kPoseValid) == kPoseValid) {
                tracked = true;
                const auto& pose = location.pose;
                seat_from_grip = driving::SeatFromApp(seat, {pose.position.x, pose.position.y, pose.position.z},
                                                      {pose.orientation.x, pose.orientation.y,
                                                       pose.orientation.z, pose.orientation.w});
            }
        }
        const float squeeze = hands[hand].squeeze;
        const TrackedHand& located = m_tracked_hands[hand];
        const bool fresh_joints = hand_tracking_on && located.active && located.seated;
        // A bare hand: camera-tracked joints on a hand driving simple_controller
        // (Android), latched through the wheel's grace with its last grasp.
        const bool camera_joints = fresh_joints && located.source != hand_tracking::Source::Controller;
        const float grasp = camera_joints ? hand_tracking::GraspFromJoints(located.positions) : 0.0f;
        const bool bare = hand_tracking_on &&
                          m_bare_latch[hand].Update(m_hand_driven[hand], m_squeeze_active[hand], camera_joints,
                                                    grasp, dt, grace);
        if (!hand_tracking_on) {
            m_bare_latch[hand].Reset();
        }
        // With tracked hands on, a hand whose joints were located is drawn from
        // them (the controller's touch sensors, or the cameras), and a bare hand
        // holding the wheel keeps its last joints through a short loss. A bare
        // hand is never drawn or steered from its grip pose, which would show
        // an open hand wherever it rests.
        const bool joints = fresh_joints || (bare && m_wheel_held[hand]);
        if (m_hand_driven[hand] || bare) {
            tracked = false;
        }
        DrivingHand& out = snapshot.hands[hand];
        // The selected item hand is visible with stick steering too.
        const auto item_hand = RuntimeConfigFile::VrCockpitItemHand();
        const bool displays_item = item_hand == (hand == 0 ? "left" : "right");
        out.tracked = (hand_steering || displays_item) && (tracked || joints);
        // Shown or not, a located controller grip is what the character
        // body's arm reaches for, and a bare hand's palm in the grip's axes:
        // the character's own hand then follows the tracked hand, and the
        // cockpit draws no mesh over a hand the character draws.
        out.grip_valid = tracked;
        out.held = false;
        out.squeeze = squeeze;
        out.seat_from_grip = seat_from_grip;
        if (bare && joints) {
            out.grip_valid = true;
            out.seat_from_grip =
                hand_tracking::GripFromPalm(m_joint_frame.seat_from_joint[hand][hand_tracking::kPalm], hand);
        }
        out.joints_valid = joints;
        out.bare = bare;
        out.source = located.source;
        out.grasp = bare ? m_bare_latch[hand].Grasp() : grasp;
        out.pinch = m_pinch[hand];
        m_joint_frame.valid[hand] = joints;
        if (bare) {
            // The palm stands in for the grip, and the fingers' grasp for the
            // squeeze; the grasp never reaches the game's buttons.
            const auto& palm = m_joint_frame.seat_from_joint[hand][hand_tracking::kPalm];
            wheel_hands[hand] = {palm[3], palm[7], palm[11], m_bare_latch[hand].Grasp(),
                                 m_bare_latch[hand].Tracked()};
        } else {
            wheel_hands[hand] = {seat_from_grip[3], seat_from_grip[7], seat_from_grip[11], squeeze, tracked};
        }
        if (uses_geometry) {
            wheel_hands[hand] = geometry.ToWheel(wheel_hands[hand]);
        }
    }
    // A USB wheel drives the race through the GameCube pad: it steers, the
    // cockpit's wheel shows its angle, and the hands cannot take hold.
    float hardware_steering = 0.0f;
    const bool hardware_wheel = physical_wheel::SteeringSnapshot(hardware_steering);
    const bool active = hand_steering && !withheld && !hardware_wheel;
    const WheelState wheel = m_wheel.Update(wheel_hands, active, dt,
                                            uses_geometry ? geometry.radius : SteeringWheel::Radius,
                                            anchor.bike, tuning);
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        if (wheel.held[hand] != m_wheel_held[hand] && active && tuning.haptics && !m_hand_driven[hand]) {
            constexpr XrDuration kGrabPulseNs = 25'000'000;
            constexpr XrDuration kReleasePulseNs = 15'000'000;
            ApplyHaptic(hand, wheel.held[hand] ? 0.25f : 0.12f, wheel.held[hand] ? kGrabPulseNs : kReleasePulseNs);
        }
        snapshot.hands[hand].held = wheel.held[hand];
    }
    m_wheel_held = wheel.held;
    snapshot.held = wheel.held;
    driving::ApplyHandSteering(hands, wheel);
    const float max_angle = driving::MaxWheelAngle(anchor.bike, tuning);
    if (hardware_wheel) {
        snapshot.steering_input = std::clamp(hardware_steering, -1.0f, 1.0f);
        // The hardware wheel is already smooth; follow it directly.
        snapshot.visual_angle = m_wheel_visual.Update(true, snapshot.steering_input * max_angle, 0.0f, max_angle, dt);
    } else {
        snapshot.steering_input =
            driving::SteeringShown(anchor.steering_valid, anchor.steering, hands[0].stick_x, withheld);
        snapshot.visual_angle = m_wheel_visual.Update(wheel.held[0] || wheel.held[1], wheel.visualAngle,
                                                      snapshot.steering_input, max_angle, dt);
    }
    m_driving = snapshot;
    OpenXRPublishDriving(snapshot);
}

void OpenXRInput::UpdateRumble() {
    if (!OpenXRWiiRemoteRumbleRequested() || !OpenXRWiiRemoteOwnsGamepad(Relay().JoystickId())) {
        StopRumble();
        return;
    }
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        ApplyHaptic(hand, 1.0f, kRumblePulseNs);
        m_haptics_active[hand] = true;
    }
}

void OpenXRInput::StopRumble() {
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        if (m_haptics_active[hand]) {
            ApplyHaptic(hand, 0.0f, 0);
            m_haptics_active[hand] = false;
        }
    }
}

void OpenXRInput::ApplyHaptic(uint32_t hand, float amplitude, XrDuration duration) {
    if (!m_created || m_runtime == nullptr || hand >= kHandCount || m_haptic == XR_NULL_HANDLE) {
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vibration.duration = duration;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = m_haptic;
    info.subactionPath = m_hand_paths[hand];
    if (vibration.amplitude <= 0.0f) {
        xrStopHapticFeedback(m_runtime->Session(), &info);
        return;
    }
    xrApplyHapticFeedback(m_runtime->Session(), &info,
                          reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

bool OpenXRInput::Check(XrResult result, const char* operation) {
    if (m_runtime != nullptr) {
        m_runtime->ObserveResult(result);
    }
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    std::ostringstream message;
    message << operation << " failed (" << result << ')';
    m_last_error = message.str();
    Log(OpenXRLogLevel::Error, m_last_error);
    return false;
}

void OpenXRInput::Log(OpenXRLogLevel level, const std::string& message) const noexcept {
    if (!m_logger) {
        return;
    }
    try {
        m_logger(level, message);
    } catch (...) {
    }
}

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
