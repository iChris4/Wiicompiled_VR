// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Steering wheel and hand steering in the first-person cockpit.
//
// The OpenXR pacing thread locates the controllers in the seated frame, runs
// the SteeringWheel (steering_wheel.h, ported from heurazy's
// mario-kart-wii-VR-port) and publishes one DrivingSnapshot per XR frame. The
// guest thread reads the latest one to turn the vehicle's own wheel mesh, and
// the pacing thread hands the same state to Aurora's cockpit overlay. Nothing
// in this header depends on OpenXR, so the guest side builds without it and
// the rules below are tested headlessly (tests/vr_hand_steering_tests.cpp).
//
// The seated frame is the application space re-based on the immersive head
// position and turned by the lean-back angle, in metres: +X right, +Y up, -Z
// forward. It is the frame the first-person anchor places the vehicle in, so
// hands, wheel geometry and eye transforms all meet there.

#include "vr/openxr_hand_tracking.h"
#include "vr/openxr_wii_remote.h"
#include "vr/steering_wheel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace mkw::vr {

// One tracked hand in the seated frame.
struct DrivingHand {
    bool tracked = false;
    bool held = false;
    float squeeze = 0.0f;
    // Row-major 3x4 from the controller's grip space into the seated frame.
    std::array<float, 12> seat_from_grip{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    // Tracked hands ([vr] hand_tracking, openxr_hand_tracking.h). The joints
    // themselves stay with OpenXRInput::HandJoints(), on the pacing thread;
    // this says whether the cockpit draws the hand from them, and carries what
    // the settings panel reads out: where they came from, whether the hand is
    // bare (camera-tracked, no controller), how closed it is and whether it
    // pinches.
    bool joints_valid = false;
    bool bare = false;
    hand_tracking::Source source = hand_tracking::Source::None;
    float grasp = 0.0f;
    bool pinch = false;
    // seat_from_grip is a located controller grip this frame, whether or not
    // the cockpit draws the hand: what the character body's arm reaches for.
    // A bare hand with joints has its palm there in the grip's axes
    // (hand_tracking::GripFromPalm); a hand-driven hand without them has none.
    bool grip_valid = false;
};

struct DrivingSnapshot {
    // When the pacing thread published it (steady clock, ns), so the guest can
    // tell a fresh pose from one left over after the cockpit stopped updating.
    uint64_t published_ns = 0;
    // FirstPersonAnchor::vehicle_identity the poses were mapped for.
    uint64_t vehicle_identity = 0;
    // The headset in the seated frame, for the character body's lean.
    bool head_tracked = false;
    std::array<float, 12> seat_from_head{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    // The first-person cockpit is engaged and the controllers are mapped into it.
    bool cockpit_active = false;
    // Hand steering is on: a squeezed grip near the wheel takes hold of it.
    bool hand_steering = false;
    std::array<bool, 2> held{};
    // The steering the vehicle turns with, -1..1: a USB wheel's, otherwise the
    // game's own from whatever controller it reads (SteeringShown).
    float steering_input = 0.0f;
    // What the wheel or handlebar shows, in radians. Positive turns it
    // clockwise as the driver sees it, i.e. to the right.
    float visual_angle = 0.0f;
    std::array<DrivingHand, 2> hands{};
    // What the cockpit overlay draws: a separate VR wheel or handlebar when the
    // vehicle's own is not the one turning. `control` places the handlebar
    // (and, when its geometry is valid, is what the hands reach for).
    bool synthetic_control = false;
    bool bike = false;
    WheelGeometry control{};
};

// Pacing thread publishes; any thread reads the latest. A default snapshot
// (nothing held, centred) is returned before the first publication.
void OpenXRPublishDriving(const DrivingSnapshot& snapshot) noexcept;
DrivingSnapshot OpenXRReadDriving() noexcept;

namespace driving {

inline bool IsFinite(float value) noexcept {
    // Bit test: the runtime may be built with -ffast-math.
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

// The wheel angle at full steering lock, in radians.
inline float MaxWheelAngle(bool bike, const WheelTuning& tuning) noexcept {
    const float degrees = bike ? tuning.bikeDegrees : tuning.kartDegrees;
    const float clamped = IsFinite(degrees) ? std::clamp(degrees, 20.0f, 180.0f) : (bike ? 45.0f : 90.0f);
    return clamped * 0.01745329252f;
}

// Whether the overlay draws a separate VR wheel or handlebar: only when
// placeholder_steering_wheel asks for one and the vehicle's own is not the one
// turning. Without it the hands reach for the vehicle's own wherever its
// geometry is known, turning or not.
inline bool DrawsPlaceholderControl(bool steering_wheel, bool native_steering_wheel, bool native_mesh_prepared,
                                    bool placeholder) noexcept {
    return placeholder && steering_wheel && !(native_steering_wheel && native_mesh_prepared);
}

// Grips only grab. As a Wii Remote they press nothing at all (C, the game's
// look-behind, is right B); as a gamepad they are the shoulders, so a holding
// hand's squeeze is released for the game. The wheel replaces the left stick's
// X axis, which both controller modes steer with, and the stick's Y axis keeps
// aiming items forwards and backwards.
inline void ApplyHandSteering(std::array<wii_remote::HandInputs, 2>& hands, const WheelState& wheel) noexcept {
    for (size_t hand = 0; hand < hands.size(); ++hand) {
        if (wheel.held[hand]) {
            hands[hand].squeeze = 0.0f;
        }
    }
    if ((wheel.held[0] || wheel.held[1]) && IsFinite(wheel.steering)) {
        hands[0].stick_x = std::clamp(wheel.steering, -1.0f, 1.0f);
    }
}

// The steering an unheld wheel follows: the game's own, so a gamepad, a
// keyboard or a wheel the game reads as a controller turns it as well as the
// VR controllers do. Only while that cannot be read does it fall back to the
// left stick, centred when the controllers are nothing to the game.
inline float SteeringShown(bool game_valid, float game, float stick_x, bool withheld) noexcept {
    if (game_valid && IsFinite(game)) {
        return std::clamp(game, -1.0f, 1.0f);
    }
    return withheld || !IsFinite(stick_x) ? 0.0f : std::clamp(stick_x, -1.0f, 1.0f);
}

// The angle the wheel shows. A held wheel shows the hands' own angle; otherwise
// it follows the steering at the configured full-lock angle, eased so a
// flicked stick does not snap it round.
class WheelVisual {
public:
    float Update(bool held, float held_angle, float stick_x, float max_angle, float dt) noexcept {
        if (!IsFinite(dt)) {
            dt = 0.0f;
        }
        if (held && IsFinite(held_angle)) {
            angle_ = held_angle;
            return angle_;
        }
        const float stick = IsFinite(stick_x) ? std::clamp(stick_x, -1.0f, 1.0f) : 0.0f;
        const float target = stick * (IsFinite(max_angle) ? max_angle : 0.0f);
        angle_ += (target - angle_) * (1.0f - std::exp(-15.0f * std::clamp(dt, 0.0f, 0.1f)));
        return angle_;
    }
    void Reset() noexcept { angle_ = 0.0f; }

private:
    float angle_ = 0.0f;
};

// Where the seated frame is: the immersive head position in the application
// space, turned about +X by the lean-back angle.
struct SeatFrame {
    bool valid = false;
    std::array<float, 3> base{};
    float lean_back_radians = 0.0f;
};

// A pose in the application space (position, then a unit quaternion x, y, z, w)
// as a row-major 3x4 in the seated frame: R_lean^T * (p - base) for the
// position and R_lean^T * R for the orientation. The inverse of how the eye
// transforms place the seated frame (world = base + R_lean * seat).
inline std::array<float, 12> SeatFromApp(const SeatFrame& seat, const std::array<float, 3>& position,
                                         const std::array<float, 4>& orientation) noexcept {
    float x = orientation[0], y = orientation[1], z = orientation[2], w = orientation[3];
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    if (IsFinite(length) && length > 1e-6f) {
        x /= length;
        y /= length;
        z /= length;
        w /= length;
    } else {
        x = y = z = 0.0f;
        w = 1.0f;
    }
    const float r[9]{1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                     2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                     2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    const float c = std::cos(seat.lean_back_radians), s = std::sin(seat.lean_back_radians);
    // R_lean about +X is rows (1,0,0), (0,c,-s), (0,s,c); its transpose applied to v:
    const auto unlean = [c, s](float vx, float vy, float vz) {
        return std::array<float, 3>{vx, c * vy + s * vz, -s * vy + c * vz};
    };
    std::array<float, 12> out{};
    for (int col = 0; col < 3; ++col) {
        const auto column = unlean(r[col], r[3 + col], r[6 + col]);
        out[col] = column[0];
        out[4 + col] = column[1];
        out[8 + col] = column[2];
    }
    const auto p = unlean(position[0] - seat.base[0], position[1] - seat.base[1], position[2] - seat.base[2]);
    out[3] = p[0];
    out[7] = p[1];
    out[11] = p[2];
    return out;
}

} // namespace driving

} // namespace mkw::vr
