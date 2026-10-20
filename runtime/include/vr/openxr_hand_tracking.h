// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The cockpit hands from the headset's hand tracking ([vr] hand_tracking), and
// what bare hands do once the controllers are put down.
//
// With the option on, the OpenXR pacing thread locates both hands' 26 joints
// (XR_EXT_hand_tracking) every XR frame. Where a hand holds a controller, the
// Quest builds them from the controller's touch sensors
// (XR_EXT_hand_tracking_data_source's controller source); once the controllers
// are put down, from its cameras, and the runtime then drives
// /interaction_profiles/khr/simple_controller from the hand: select is an index
// pinch, the left menu the palm-up menu gesture. The rules that turn all that
// into the game's input live here, free of OpenXR, so they are tested headlessly
// (tests/vr_hand_tracking_tests.cpp):
//
// - A hand is hand-driven when its squeeze action is inactive and its select
//   action active: the Touch profile binds squeeze, simple_controller does not.
//   That decides what its buttons mean, and needs no tracker, so the manifest's
//   hand-tracking permission can never let resting hands press anything.
// - It is bare while it is hand-driven with camera-tracked joints, latched
//   through the wheel's tracking grace. That decides the wheel grab (the palm's
//   position, a grasp from the fingers' flexion) and the flick.
//
// Only the Android build applies these: a PC runtime can drive real controllers
// through simple_controller and synthesize joints for them.

#include "vr/openxr_wii_remote.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mkw::vr::hand_tracking {

// XR_HAND_JOINT_*_EXT order.
inline constexpr size_t kJointCount = 26;
inline constexpr size_t kPalm = 0;
inline constexpr size_t kWrist = 1;
// Each finger's metacarpal joint, thumb first. A finger's chain runs from there
// to its tip; the thumb's has no intermediate joint, so it is one shorter.
inline constexpr std::array<size_t, 5> kMetacarpal{2, 6, 11, 16, 21};
inline constexpr std::array<size_t, 5> kTip{5, 10, 15, 20, 25};

using Vec3 = std::array<float, 3>;
using JointPositions = std::array<Vec3, kJointCount>;

// Where a hand's joints came from this frame.
enum class Source : uint8_t {
    None,       // not located
    Controller, // built from a held controller's touch sensors
    Camera,     // the headset's cameras
    Unknown,    // located, but the runtime does not say how
};

inline const char* SourceLabel(Source source) noexcept {
    switch (source) {
    case Source::Controller: return "controller";
    case Source::Camera: return "camera";
    case Source::Unknown: return "tracked";
    default: return "none";
    }
}

// Both hands' joints in the seated frame (openxr_driving.h), for the cockpit
// overlay: row-major 3x4 per joint, and each joint's radius in metres.
struct HandJointFrame {
    std::array<bool, 2> valid{};
    std::array<std::array<std::array<float, 12>, kJointCount>, 2> seat_from_joint{};
    std::array<std::array<float, kJointCount>, 2> radius{};
};

inline bool IsFinite(float value) noexcept {
    // Bit test: the runtime may be built with -ffast-math.
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

// A palm joint (+Y out of the back of the hand, -Z towards the fingertips) in
// a controller grip's axes, which the character body's palm profiles were
// recorded against. The OpenXR grip's +X is the palm normal, away from the palm
// on the left hand and into it on the right, and its -Z runs from the little
// finger to the thumb, so the fingers lie along grip -Y as along palm -Z: grip
// X, Y, Z are the palm's s*Y, Z, s*X about the same origin, s -1 on the left
// hand (0) and +1 on the right.
inline std::array<float, 12> GripFromPalm(const std::array<float, 12>& seat_from_palm, size_t hand) noexcept {
    const float s = hand == 0 ? -1.0f : 1.0f;
    std::array<float, 12> grip{};
    for (size_t row = 0; row < 3; ++row) {
        grip[row * 4 + 0] = s * seat_from_palm[row * 4 + 1];
        grip[row * 4 + 1] = seat_from_palm[row * 4 + 2];
        grip[row * 4 + 2] = s * seat_from_palm[row * 4 + 0];
        grip[row * 4 + 3] = seat_from_palm[row * 4 + 3];
    }
    return grip;
}

// The fingers' summed flexion that reads as an open hand, and as a hand closed
// on the wheel's rim. A relaxed hand bends its three finger joints by roughly
// 1.0 to 1.6 radians in all, which must stay under the wheel's 0.15 release;
// a grip round a rim bends them well past the 0.55 press. Starting values for
// tuning with the headset panel's readout.
inline constexpr float kGraspOpenRadians = 1.2f;
inline constexpr float kGraspClosedRadians = 3.0f;

// The angle between two successive bones a->b and b->c, 0 for a straight
// finger. Negative when a bone is too short to have a direction.
inline float BendRadians(const Vec3& a, const Vec3& b, const Vec3& c) noexcept {
    const Vec3 u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const Vec3 v{c[0] - b[0], c[1] - b[1], c[2] - b[2]};
    const float uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
    const float vv = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (!(uu > 1.0e-8f) || !(vv > 1.0e-8f)) {
        return -1.0f;
    }
    const Vec3 cross{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float sine = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
    const float cosine = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    return std::atan2(sine, cosine);
}

// How far a hand is closed, 0 (open) to 1 (closed on a rim or in a fist): the
// middle, ring and little fingers' flexion, each summed over their three
// joints. The index finger and thumb are left out so pinching does not grab.
// Angles only, so it is the same for either hand, at any size and in any
// orientation. 0 for joints that are not finite or collapse onto each other.
inline float GraspFromJoints(const JointPositions& joints) noexcept {
    for (const Vec3& joint : joints) {
        if (!IsFinite(joint[0]) || !IsFinite(joint[1]) || !IsFinite(joint[2])) {
            return 0.0f;
        }
    }
    float sum = 0.0f;
    for (size_t finger = 2; finger < 5; ++finger) {
        const size_t base = kMetacarpal[finger];
        float flex = 0.0f;
        for (size_t joint = base; joint + 2 <= base + 4; ++joint) {
            const float bend = BendRadians(joints[joint], joints[joint + 1], joints[joint + 2]);
            if (!(bend >= 0.0f)) {
                return 0.0f;
            }
            flex += bend;
        }
        sum += std::clamp((flex - kGraspOpenRadians) / (kGraspClosedRadians - kGraspOpenRadians), 0.0f, 1.0f);
    }
    return sum / 3.0f;
}

// Keeps a camera-tracked hand bare, with its last grasp, through a short loss
// of tracking (fingers hidden behind the other hand, a hand turned edge-on):
// long enough for the wheel's own grace to keep hold. A controller's squeeze
// clears it at once, since a hand that picked one up is no longer bare.
class BareLatch {
public:
    // `hand_driven`: the hand drives simple_controller this frame.
    // `camera_joints`: its joints were located this frame, not from a controller.
    // `grace`: the wheel's tracking grace in seconds.
    bool Update(bool hand_driven, bool squeeze_active, bool camera_joints, float grasp, float dt,
                float grace) noexcept {
        if (squeeze_active) {
            Reset();
            return false;
        }
        if (hand_driven && camera_joints) {
            m_bare = true;
            m_tracked = true;
            m_lost = 0.0f;
            m_grasp = IsFinite(grasp) ? std::clamp(grasp, 0.0f, 1.0f) : 0.0f;
            return true;
        }
        if (m_bare) {
            m_tracked = false;
            m_lost += IsFinite(dt) ? std::clamp(dt, 0.0f, 0.1f) : 0.0f;
            if (m_lost <= grace) {
                return true;
            }
        }
        Reset();
        return false;
    }

    bool Bare() const noexcept { return m_bare; }
    // False while the latch is only bridging a loss of tracking.
    bool Tracked() const noexcept { return m_bare && m_tracked; }
    float Grasp() const noexcept { return m_bare ? m_grasp : 0.0f; }
    void Reset() noexcept { *this = BareLatch{}; }

private:
    bool m_bare = false;
    bool m_tracked = false;
    float m_lost = 0.0f;
    float m_grasp = 0.0f;
};

// A pinch uses an item only when it starts on a hand that has been off the
// wheel for a moment: a hand opening off the rim often reads as a pinch for a
// frame or two. Holding the pinch holds the button, which trails an item.
class PinchGate {
public:
    static constexpr float kFreeSeconds = 0.15f;

    bool Update(bool pinch, bool held, float dt) noexcept {
        dt = IsFinite(dt) ? std::clamp(dt, 0.0f, 0.1f) : 0.0f;
        if (held) {
            m_free = 0.0f;
            m_firing = false;
            m_blocked = pinch;
            return false;
        }
        if (!pinch) {
            m_firing = false;
            m_blocked = false;
            m_free = std::min(m_free + dt, 1.0f);
            return false;
        }
        if (!m_firing && !m_blocked) {
            (m_free >= kFreeSeconds ? m_firing : m_blocked) = true;
        }
        m_free = std::min(m_free + dt, 1.0f);
        return m_firing;
    }

    void Reset() noexcept { *this = PinchGate{}; }

private:
    float m_free = 0.0f;
    bool m_firing = false;
    bool m_blocked = false;
};

// A pinch uses an item only from a hand still mostly open: a hand closing on
// the rim can bring thumb and index together on the way, and a fist is not a
// pinch.
inline constexpr float kItemPinchMaxGrasp = 0.5f;

inline bool ItemPinch(bool pinch, float grasp) noexcept {
    return pinch && IsFinite(grasp) && grasp < kItemPinchMaxGrasp;
}

// A hand's pinch and menu gesture this frame. The runtime's own recognition
// (XR_FB_hand_tracking_aim) is preferred when it is valid, and a pinch made
// while the hand is in the system gesture (palm towards the face) is the menu
// gesture, not a pinch. Otherwise the select and menu actions, as
// simple_controller delivers them.
struct Gestures {
    bool pinch = false;
    bool menu = false;
};

inline Gestures GesturesOf(bool aim_valid, bool aim_pinching, bool aim_menu, bool aim_system_gesture,
                           bool action_select, bool action_menu) noexcept {
    if (aim_valid) {
        return {aim_pinching && !aim_system_gesture, aim_menu || action_menu};
    }
    return {action_select, action_menu};
}

// Whether a hand is a bare hand this frame, for its buttons: no controller in
// it (its squeeze action, which only the Touch profile binds, is inactive), and
// either the runtime drives khr/simple_controller from it (select active) or
// the cameras track it. The second covers a hand the runtime gives no profile
// our actions are bound in, as simultaneous hands and controllers may do.
inline bool HandDriven(bool squeeze_active, bool select_active, bool camera_joints) noexcept {
    return !squeeze_active && (select_active || camera_joints);
}

// The select action as simple_controller delivers it: right select is bound to
// the primary action, left select to the secondary one (openxr_input.cpp).
inline bool SelectOf(const wii_remote::HandInputs& inputs, size_t hand) noexcept {
    return hand == 1 ? inputs.primary : inputs.secondary;
}

// Before anything reads the hands (the settings panel, then the game). A
// hand-driven hand's select is replaced: with tracked hands on, a right pinch
// stays A, for the menus, and a left one no longer toggles the settings panel;
// with them off, the hand presses nothing but the menu gesture, so resting hands
// with the controllers put down never press anything. Controller hands are left
// alone.
inline void ApplyHandDrivenButtons(std::array<wii_remote::HandInputs, 2>& hands,
                                   const std::array<bool, 2>& hand_driven, const std::array<bool, 2>& pinch,
                                   bool option_on) noexcept {
    for (size_t hand = 0; hand < hands.size(); ++hand) {
        if (!hand_driven[hand]) {
            continue;
        }
        hands[hand].primary = option_on && hand == 1 && pinch[hand];
        hands[hand].secondary = false;
    }
}

// In the cockpit, after the wheel. While a bare hand holds the wheel it holds
// the gas (A: the right primary button, whatever a right pinch says), and an
// item pinch uses an item (the left trigger: the Wii Remote's Z, the
// GameCube's L). With no bare hand on the wheel nothing changes, so a right
// pinch stays A for the menus inside a race (the pause menu, the results),
// which the game's pointer cannot tell from driving: MKW keeps it on in a
// race. The grasp itself never becomes a squeeze, which would press the
// gamepad's shoulders (GameCube R drifts). True while a bare hand holds.
inline bool ApplyBareHandRace(std::array<wii_remote::HandInputs, 2>& hands, const std::array<bool, 2>& bare_held,
                              const std::array<bool, 2>& item_pinch) noexcept {
    if (!bare_held[0] && !bare_held[1]) {
        return false;
    }
    hands[1].primary = true;
    if (item_pinch[0] || item_pinch[1]) {
        hands[0].trigger = 1.0f;
    }
    return true;
}

// One bare hand for the flick detector: its palm's height in the seated frame.
struct FlickHand {
    bool tracked = false;
    bool held = false;
    float height = 0.0f;
};

// A quick upward flick of the hands, the bare-hand shake that does a trick off
// a ramp or pulls a wheelie. Both hands on the wheel must rise together at a
// similar speed, so a turn, where one hand rises as the other drops, never
// counts; a free hand counts alone, but a lone hand on the wheel does not
// (that is a turn too). A rise must last a few samples and cover some height
// soon enough, so tracking noise and a pose jumping when tracking comes back
// never count; the second is also caught as an impossible speed.
class FlickDetector {
public:
    static constexpr float kRisingSpeed = 0.3f;        // m/s: the hand is going up
    static constexpr float kJumpSpeed = 5.0f;          // m/s: faster than a hand, a tracking jump
    static constexpr float kMinRise = 0.05f;           // m
    static constexpr float kWindowSeconds = 0.15f;     // to cover kMinRise
    static constexpr int kMinSamples = 3;
    static constexpr float kPairSpeed = 1.2f;          // mean of both hands, m/s
    static constexpr float kPairEachSpeed = 0.6f;
    static constexpr float kPairSpeedDifference = 0.6f;
    static constexpr float kFreeSpeed = 1.5f;
    static constexpr float kCooldownSeconds = 0.5f;

    bool Update(const std::array<FlickHand, 2>& hands, float dt) noexcept {
        if (!IsFinite(dt) || dt <= 0.0f) {
            return false;
        }
        dt = std::min(dt, 0.1f);
        m_cooldown = std::max(m_cooldown - dt, 0.0f);
        std::array<float, 2> speed{};
        std::array<bool, 2> rising{};
        for (size_t hand = 0; hand < 2; ++hand) {
            rising[hand] = Step(m_tracks[hand], hands[hand], dt, speed[hand]);
        }
        if (m_cooldown > 0.0f) {
            return false;
        }
        bool fire = false;
        if (hands[0].held && hands[1].held) {
            fire = rising[0] && rising[1] && speed[0] >= kPairEachSpeed && speed[1] >= kPairEachSpeed &&
                   0.5f * (speed[0] + speed[1]) >= kPairSpeed &&
                   std::fabs(speed[0] - speed[1]) <= kPairSpeedDifference;
        }
        for (size_t hand = 0; hand < 2 && !fire; ++hand) {
            fire = hands[hand].tracked && !hands[hand].held && rising[hand] && speed[hand] >= kFreeSpeed;
        }
        if (fire) {
            m_cooldown = kCooldownSeconds;
            for (Track& track : m_tracks) {
                track.spent = true;
            }
        }
        return fire;
    }

    void Reset() noexcept { *this = FlickDetector{}; }

private:
    struct Track {
        bool has_last = false;
        float last = 0.0f;
        bool rising = false;
        bool spent = false; // this rise already flicked, or took too long
        float start = 0.0f;
        float time = 0.0f;
        int samples = 0;
    };

    // Whether the hand is in a rise that qualifies, and that rise's mean speed.
    static bool Step(Track& track, const FlickHand& hand, float dt, float& speed) noexcept {
        if (!hand.tracked || !IsFinite(hand.height)) {
            track = {};
            return false;
        }
        if (!track.has_last) {
            track.has_last = true;
            track.last = hand.height;
            return false;
        }
        const float velocity = (hand.height - track.last) / dt;
        if (std::fabs(velocity) > kJumpSpeed) {
            track = {};
            track.has_last = true;
            track.last = hand.height;
            return false;
        }
        if (velocity >= kRisingSpeed) {
            if (!track.rising) {
                track.rising = true;
                track.spent = false;
                track.start = track.last;
                track.time = 0.0f;
                track.samples = 0;
            }
            track.time += dt;
            ++track.samples;
        } else {
            track.rising = false;
        }
        track.last = hand.height;
        if (!track.rising || track.spent) {
            return false;
        }
        const float rise = hand.height - track.start;
        if (rise < kMinRise) {
            if (track.time > kWindowSeconds) {
                track.spent = true; // a slow lift, not a flick
            }
            return false;
        }
        if (track.samples < kMinSamples) {
            return false;
        }
        speed = rise / track.time;
        return true;
    }

    std::array<Track, 2> m_tracks{};
    float m_cooldown = 0.0f;
};

// The remote's accelerometer through one flick: up, then down, one cycle of a
// shake, as Dolphin's emulated shake produces them. It lasts long enough that
// the guest, which reads only the latest sample, sees it on at least three of
// its frames; KPAD derives acc_speed from the change between them.
inline constexpr int64_t kFlickPulseNs = 150'000'000;
inline constexpr float kFlickPulseG = 3.0f;

inline wii_remote::Vec3 FlickPulse(int64_t elapsed_ns, bool* active) noexcept {
    const wii_remote::Vec3 rest{0.0f, -1.0f, 0.0f};
    if (elapsed_ns < 0 || elapsed_ns >= kFlickPulseNs) {
        if (active != nullptr) {
            *active = false;
        }
        return rest;
    }
    if (active != nullptr) {
        *active = true;
    }
    const float phase = static_cast<float>(elapsed_ns) / static_cast<float>(kFlickPulseNs);
    const float swing = kFlickPulseG * std::sin(phase * 6.2831853f);
    return {0.0f, std::clamp(rest[1] + swing, -wii_remote::kAccelRangeG, wii_remote::kAccelRangeG), 0.0f};
}

} // namespace mkw::vr::hand_tracking
