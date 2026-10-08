// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR)

#include "vr/camera_toggle.h"
#include "vr/openxr_driving.h"
#include "vr/openxr_hand_tracking.h"
#include "vr/openxr_item_throw.h"
#include "vr/openxr_runtime.h"
#include "vr/openxr_settings_panel.h"
#include "vr/openxr_wii_remote.h"
#include "vr/steering_wheel.h"

#include <array>
#include <cstdint>
#include <string>

namespace mkw::vr {

// The rectangle the game's picture occupies on whichever virtual screen is
// showing it, in the application reference space: the target the Wii Remote
// pointer is aimed at. The pose faces +Z with +X right and +Y up across the
// picture. Invalid when no screen can be pointed at (for instance a race with
// its 2D layer left stretched across the eyes).
struct OpenXRPointerScreen {
    bool valid = false;
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float half_width_meters = 0.0f;
    float half_height_meters = 0.0f;
};

// OpenXR action-based controller input.
//
// Quest Touch controllers are not visible to SDL's joystick layer (the OS does
// not expose them as HID gamepads), so a standalone headset build would have no
// input at all. This module syncs an OpenXR action set on the pacing thread and
// feeds a virtual SDL joystick (SDL_AttachVirtualJoystick) that Aurora's
// existing controller code opens and assigns to a port exactly like a physical
// pad. What the game then sees on that port depends on OpenXRControllerMode:
//
// Wii Remote (default): the port is served through KPAD as a Wii Remote with a
// Nunchuk, like DolphinXR's OpenXR Wii Remote. Every XR frame the aim and grip
// poses are located at the measured current time, turned into both
// accelerometers and the IR pointer (the right aim ray against the virtual
// screen the renderer is showing), and published through openxr_wii_remote.h.
// Buttons are adapted from DolphinXR's "OpenXR Wii Remote" profile (see
// RemoteButtons).
// The game's rumble drives both controllers' haptics.
//
// Gamepad: the virtual joystick is read through PAD as a GameCube controller,
// and every binding the settings overlay offers applies:
//   right A / B            -> gamepad South / East (GameCube A / B)
//   left  X / Y            -> gamepad West / North (GameCube X / Y)
//   index triggers         -> left / right trigger axes
//   grip squeezes          -> left / right shoulder buttons
//   left / right thumbstick-> left / right stick axes, clicks -> stick buttons
//   left menu              -> Start
//
// None: the virtual joystick is unplugged, so the controllers hold no port and
// another controller (a desktop gamepad, a Bluetooth remote) plays in their
// place. The game sees them idle, the way it does while the settings panel has
// them; they still open the panel and switch the race's camera.
//
// Both are bound for the Oculus Touch profile; khr/simple_controller gets
// select/menu and the poses so an unknown runtime still offers something.
//
// In the first-person cockpit (openxr_driving.h) the grip poses are located in
// the seated frame every frame. With hand steering on, a squeezed grip near the
// steering wheel or handlebar takes hold of it; while held, the wheel replaces
// the left stick's X axis in both presentations and that grip no longer reaches
// the game (a shoulder on the gamepad; as a Wii Remote the grips are unbound,
// so a hand on the wheel cannot hold down a button).
//
// Throwing the held item ([vr] cockpit_item_throw, openxr_item_throw.h): in the
// cockpit, a quick swing forward or back of the hand showing the item pushes the
// left stick that way while the left trigger (Z, or the GameCube's L) is pressed
// and released, which the game reads as an aimed throw.
//
// Tracked hands ([vr] hand_tracking, with hand steering): two hand trackers
// (XR_EXT_hand_tracking) located every frame give the cockpit the hands' own
// joints, from the controllers' touch sensors while they are held and from the
// cameras once they are put down. On the Quest a hand that drives
// khr/simple_controller instead of the Touch profile has bare-hand buttons
// (openxr_hand_tracking.h): with the option off it presses nothing but the menu
// gesture, so the manifest's hand-tracking permission changes nothing for
// players who leave it off.
//
// A right-thumbstick click on its own switches the race in progress between
// first person and the game's camera, as F10 > Camera > This race does
// (first_person_toggle_click).
//
// Left Y (both thumbsticks clicked together as a gamepad) opens the in-headset
// settings panel (openxr_settings_panel.h). While it is open, and until every
// button has been released after it closes, the game sees idle controllers: the
// panel button, the pointer and the triggers belong to the panel.
//
// Lifetime: Create after the session exists (attaches the action set, which
// OpenXR permits once per session), Sync once per xrWaitFrame, Idle while the
// session is not running, Destroy before the session is destroyed. All of them
// run on the XR pacing thread. The Wii Remote bridge is internally locked, so
// the game thread may read it concurrently, and the virtual gamepad is only
// published here: OpenXRApplyVirtualGamepad() performs the SDL writes on the
// game thread, and plugs the joystick in or out as the controller mode
// changes, keeping SDL's joystick lock off this thread during a session.
class OpenXRInput final {
public:
    explicit OpenXRInput(OpenXRLogCallback logger = {});
    ~OpenXRInput();

    OpenXRInput(const OpenXRInput&) = delete;
    OpenXRInput& operator=(const OpenXRInput&) = delete;

    // Creates the action set/actions/spaces and attaches them to the session.
    // Returns false (with LastError set) if the runtime rejects the action set;
    // the caller continues without controller input rather than failing VR.
    bool Create(OpenXRRuntime& runtime);
    void Destroy();

    // xrSyncActions + state reads, then publishes to the virtual gamepad and
    // the Wii Remote bridge. predicted_display_time is the frame's XrTime;
    // screen is where the Wii Remote pointer can land this frame, and
    // settings_panel where the settings panel is (its whole rectangle). seat is
    // the immersive seated frame, invalid outside an immersive race.
    void Sync(XrTime predicted_display_time, const OpenXRPointerScreen& screen,
              const OpenXRPointerScreen& settings_panel, const driving::SeatFrame& seat);

    // The cockpit state the last Sync published (also OpenXRReadDriving()).
    const DrivingSnapshot& Driving() const noexcept { return m_driving; }
    // The tracked hands' joints in the seated frame, as the last Sync located
    // them; read on the pacing thread only.
    const hand_tracking::HandJointFrame& HandJoints() const noexcept { return m_joint_frame; }
    // A hand's tracker while tracked hands keep one, else XR_NULL_HANDLE.
    XrHandTrackerEXT HandTracker(uint32_t hand) const noexcept {
        return hand < kHands ? m_hand_trackers[hand] : XR_NULL_HANDLE;
    }

    // Publishes a remote with nothing held, at rest and not pointing, and stops
    // the haptics, for frames without focused input.
    void Idle();

    // Rumble for the given hand (0 = left, 1 = right); amplitude 0..1.
    void ApplyHaptic(uint32_t hand, float amplitude, XrDuration duration);

    bool IsCreated() const noexcept { return m_created; }
    const std::string& LastError() const noexcept { return m_last_error; }

private:
    static constexpr uint32_t kHands = 2;

    bool CreateActions();
    bool SuggestBindings();
    void CreatePoseSpaces();
    void DestroyPoseSpaces();
    void LoadInputClock();
    XrTime InputSampleTime(XrTime predicted_display_time) const;
    // `withheld` publishes a remote at rest with nothing held and no pointer,
    // while still tracking motion so releasing it does not read as a jolt.
    void PublishWiiRemote(XrTime input_time, const OpenXRPointerScreen& screen,
                          const std::array<wii_remote::HandInputs, kHands>& hands, uint32_t injected_buttons,
                          bool withheld);
    void PublishSettingsPanel(XrTime input_time, const OpenXRPointerScreen& panel,
                              const settings_panel::Frame& frame);
    // Hand steering: locates the grips in the seated frame, runs the wheel and
    // hands its steering to `hands` before the game sees them.
    void UpdateDriving(XrTime display_time, const driving::SeatFrame& seat,
                       std::array<wii_remote::HandInputs, kHands>& hands, bool withheld);
    void ResetDriving();
    // After the wheel and the bare hands: the item hand's swing, and the throw it
    // plays on the left hand's stick and trigger.
    void UpdateItemThrow(float dt_seconds, std::array<wii_remote::HandInputs, kHands>& hands, bool withheld);
    // Tracked hands: the extension's functions at Create, the trackers as the
    // settings ask for them, and both hands located for `time`, their joints
    // in `seat` when it is valid.
    void LoadHandTracking();
    void UpdateSimultaneousHandsAndControllers(bool wanted);
    void UpdateHandTrackers();
    void DestroyHandTrackers();
    void LocateHands(XrTime time, const driving::SeatFrame& seat);
    void LogInteractionProfiles();
    void UpdateRumble();
    void StopRumble();
    bool Check(XrResult result, const char* operation);
    void Log(OpenXRLogLevel level, const std::string& message) const noexcept;

    OpenXRLogCallback m_logger;
    OpenXRRuntime* m_runtime = nullptr;
    XrActionSet m_action_set = XR_NULL_HANDLE;
    XrAction m_thumbstick = XR_NULL_HANDLE;
    XrAction m_thumbstick_click = XR_NULL_HANDLE;
    XrAction m_trigger = XR_NULL_HANDLE;
    XrAction m_squeeze = XR_NULL_HANDLE;
    XrAction m_button_primary = XR_NULL_HANDLE;   // A / X
    XrAction m_button_secondary = XR_NULL_HANDLE; // B / Y
    XrAction m_menu = XR_NULL_HANDLE;
    XrAction m_aim_pose = XR_NULL_HANDLE;
    XrAction m_grip_pose = XR_NULL_HANDLE;
    XrAction m_haptic = XR_NULL_HANDLE;
    XrPath m_hand_paths[kHands]{};
    XrSpace m_aim_spaces[kHands]{};
    XrSpace m_grip_spaces[kHands]{};
    // xrConvertWin32PerformanceCounterToTimeKHR / xrConvertTimespecTimeToTimeKHR,
    // when the runtime offers them; the input time falls back to display time.
    PFN_xrVoidFunction m_convert_now_to_xr_time = nullptr;
    wii_remote::MotionTracker m_motion[kHands];
    wii_remote::PointerFilter m_pointer;
    settings_panel::Controls m_panel_controls;
    XrTime m_last_input_time = 0;
    bool m_panel_select_held = false;
    std::array<float, 2> m_horizon{1.0f, 0.0f};
    bool m_haptics_active[kHands]{};
    ClickToggle m_first_person_click;
    SteeringWheel m_wheel;
    WheelReferenceLatch m_wheel_reference;
    driving::WheelVisual m_wheel_visual;
    std::array<bool, kHands> m_wheel_held{};
    XrTime m_wheel_time = 0;
    bool m_wheel_uses_geometry = false;
    bool m_wheel_bike = false;
    DrivingSnapshot m_driving{};

    // Tracked hands.
    struct TrackedHand {
        bool active = false; // joints located this frame
        bool seated = false; // and written into m_joint_frame, in the seated frame
        hand_tracking::Source source = hand_tracking::Source::None;
        hand_tracking::JointPositions positions{}; // application space
        // XR_FB_hand_tracking_aim: the runtime's own pinch and menu gesture.
        bool aim_valid = false;
        bool aim_pinching = false;
        bool aim_menu = false;
        bool aim_system_gesture = false;
    };
    PFN_xrCreateHandTrackerEXT m_create_hand_tracker = nullptr;
    PFN_xrDestroyHandTrackerEXT m_destroy_hand_tracker = nullptr;
    PFN_xrLocateHandJointsEXT m_locate_hand_joints = nullptr;
    bool m_hand_data_source = false; // XR_EXT_hand_tracking_data_source
    bool m_hand_aim = false;         // XR_FB_hand_tracking_aim
    // XR_META_simultaneous_hands_and_controllers: resumed while tracked hands
    // are on, so a controller put down gives its hand to the cameras at once.
    PFN_xrResumeSimultaneousHandsAndControllersTrackingMETA m_resume_simultaneous = nullptr;
    PFN_xrPauseSimultaneousHandsAndControllersTrackingMETA m_pause_simultaneous = nullptr;
    bool m_simultaneous = false;
    bool m_simultaneous_failed = false;
    XrHandTrackerEXT m_hand_trackers[kHands]{};
    bool m_hand_trackers_failed = false;
    bool m_logged_hand_restart = false;
    std::array<TrackedHand, kHands> m_tracked_hands{};
    hand_tracking::HandJointFrame m_joint_frame{};
    std::array<hand_tracking::Source, kHands> m_logged_sources{hand_tracking::Source::None,
                                                               hand_tracking::Source::None};
    XrTime m_sources_logged_at = 0;
    // Per frame, from the actions: a controller is in the hand (its squeeze is
    // bound), and the hand drives simple_controller (Android only).
    std::array<bool, kHands> m_squeeze_active{};
    std::array<bool, kHands> m_hand_driven{};
    std::array<bool, kHands> m_pinch{};
    // Bare-hand driving: each hand's bare latch (camera joints, last grasp) and
    // item pinch gate.
    std::array<hand_tracking::BareLatch, kHands> m_bare_latch{};
    std::array<hand_tracking::PinchGate, kHands> m_pinch_gate{};
    // A flick of the bare hands plays one shake on the remote's accelerometer
    // from this input time (0 when none is playing).
    hand_tracking::FlickDetector m_flick;
    XrTime m_flick_start = 0;
    bool m_injected_flick_held = false;
    // Throwing the held item.
    item_throw::ThrowDetector m_throw;
    item_throw::ThrowSequence m_throw_sequence;
    bool m_injected_throw_held = false;
    uint64_t m_profile_serial = 0;

    bool m_created = false;
    bool m_logged_sync_failure = false;
    bool m_logged_pointer = false;
    std::string m_last_error;
};

// Game thread: plugs the virtual gamepad in or out as the controller mode asks,
// then writes the gamepad the pacing thread last published, if any. Does
// nothing when no OpenXR controllers are attached.
void OpenXRApplyVirtualGamepad() noexcept;

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
