// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hand steering's hand-off to the game and the wheel's displayed angle, tested
// without a headset (vr/openxr_driving.h).

#include "vr/openxr_driving.h"

#include <cmath>
#include <iostream>

namespace {

using namespace mkw::vr;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

void CheckNear(float actual, float expected, const char* what, float tolerance = 1.0e-3f) {
    if (!(std::fabs(actual - expected) <= tolerance)) {
        ++g_failures;
        std::cerr << "FAILED: " << what << " (expected " << expected << ", got " << actual << ")\n";
    }
}

std::array<wii_remote::HandInputs, 2> Hands() {
    std::array<wii_remote::HandInputs, 2> hands{};
    hands[0].stick_x = -0.3f;
    hands[0].stick_y = 0.8f;
    hands[0].squeeze = 1.0f;
    hands[1].squeeze = 1.0f;
    return hands;
}

void TestHandOff() {
    WheelState wheel{};
    wheel.steering = 0.6f;
    auto hands = Hands();
    driving::ApplyHandSteering(hands, wheel);
    CheckNear(hands[0].stick_x, -0.3f, "an unheld wheel leaves the stick alone");
    CheckNear(hands[0].squeeze, 1.0f, "an unheld wheel leaves the grips alone");

    wheel.held = {false, true};
    hands = Hands();
    driving::ApplyHandSteering(hands, wheel);
    CheckNear(hands[0].stick_x, 0.6f, "a held wheel steers through the left stick");
    CheckNear(hands[0].stick_y, 0.8f, "the stick keeps aiming items");
    CheckNear(hands[0].squeeze, 1.0f, "a free hand's grip still reaches the game");
    CheckNear(hands[1].squeeze, 0.0f, "a holding grip does not press a shoulder");

    wheel.held = {true, true};
    wheel.steering = 3.0f;
    hands = Hands();
    driving::ApplyHandSteering(hands, wheel);
    CheckNear(hands[0].stick_x, 1.0f, "steering is clamped to full lock");
    CheckNear(hands[0].squeeze, 0.0f, "both holding grips are released for the game");

    wheel.steering = std::nanf("");
    hands = Hands();
    driving::ApplyHandSteering(hands, wheel);
    CheckNear(hands[0].stick_x, -0.3f, "a non-finite wheel never reaches the game");
}

void TestMaxAngle() {
    WheelTuning tuning{};
    CheckNear(driving::MaxWheelAngle(false, tuning), 90.0f * 0.01745329252f, "kart full lock");
    CheckNear(driving::MaxWheelAngle(true, tuning), 45.0f * 0.01745329252f, "bike full lock");
    tuning.kartDegrees = 1000.0f;
    CheckNear(driving::MaxWheelAngle(false, tuning), 180.0f * 0.01745329252f, "full lock is capped");
}

void TestVisual() {
    driving::WheelVisual visual;
    const float max = driving::MaxWheelAngle(false, WheelTuning{});
    float angle = 0.0f;
    for (int i = 0; i < 90; ++i) {
        angle = visual.Update(false, 0.0f, 1.0f, max, 1.0f / 90.0f);
    }
    CheckNear(angle, max, "the wheel follows full right stick to full lock", 1e-3f);
    angle = visual.Update(false, 0.0f, -1.0f, max, 1.0f / 90.0f);
    Check(angle > 0.0f && angle < max, "a flicked stick eases the wheel rather than snapping it");
    angle = visual.Update(true, -2.5f, 1.0f, max, 1.0f / 90.0f);
    CheckNear(angle, -2.5f, "a held wheel shows the hands' angle exactly");
}

void TestSteeringShown() {
    // Arguments: game_valid, game, stick_x, withheld.
    CheckNear(driving::SteeringShown(true, -0.6f, 0.0f, false), -0.6f,
              "another controller's steering turns the wheel while the VR stick rests");
    CheckNear(driving::SteeringShown(true, 0.4f, 0.0f, true), 0.4f,
              "the game's steering turns the wheel with the VR controllers withheld");
    CheckNear(driving::SteeringShown(true, -1.0f, 1.0f, false), -1.0f,
              "the game's steering wins over the VR stick (mirror cups flip it)");
    CheckNear(driving::SteeringShown(true, 3.0f, 0.0f, false), 1.0f, "the game's steering is clamped");
    CheckNear(driving::SteeringShown(false, 0.0f, 0.7f, false), 0.7f,
              "the VR stick stands in while the game's steering cannot be read");
    CheckNear(driving::SteeringShown(false, 0.0f, 0.7f, true), 0.0f,
              "withheld controllers do not turn the wheel");
    CheckNear(driving::SteeringShown(true, std::nanf(""), -0.5f, false), -0.5f,
              "a non-finite game value falls back to the VR stick");
}

void TestSeatFrame() {
    driving::SeatFrame seat;
    seat.valid = true;
    seat.base = {1.0f, 1.5f, -0.5f};
    auto m = driving::SeatFromApp(seat, {1.2f, 1.2f, -0.9f}, {0, 0, 0, 1});
    CheckNear(m[3], 0.2f, "seat x is relative to the head");
    CheckNear(m[7], -0.3f, "seat y is relative to the head");
    CheckNear(m[11], -0.4f, "seat z is relative to the head");
    CheckNear(m[0], 1.0f, "unrotated grip keeps its axes");

    // Leaning back by 90 degrees: the eye transforms place seat point P at
    // base + R_lean * P, so an app-space point straight up from the head is
    // straight ahead (-Z) in the seat.
    seat.lean_back_radians = 1.5707963f;
    m = driving::SeatFromApp(seat, {1.0f, 2.5f, -0.5f}, {0, 0, 0, 1});
    CheckNear(m[3], 0.0f, "lean keeps x");
    CheckNear(m[7], 0.0f, "lean moves up out of y", 1e-5f);
    CheckNear(m[11], -1.0f, "lean turns up into forward", 1e-5f);
}

void TestPlaceholderControl() {
    // Arguments: steering_wheel, native_steering_wheel, native_mesh_prepared, placeholder.
    Check(!driving::DrawsPlaceholderControl(true, true, false, false),
          "no placeholder while the vehicle's own is not animated, unless asked for");
    Check(!driving::DrawsPlaceholderControl(true, false, false, false),
          "no placeholder with the vehicle's own wheel off, unless asked for");
    Check(driving::DrawsPlaceholderControl(true, true, false, true),
          "the placeholder stands in while the vehicle's own is not animated");
    Check(driving::DrawsPlaceholderControl(true, false, true, true),
          "the placeholder replaces the vehicle's own wheel when that is off");
    Check(!driving::DrawsPlaceholderControl(true, true, true, true),
          "no placeholder while the vehicle's own is the one turning");
    Check(!driving::DrawsPlaceholderControl(false, false, false, true), "no wheel turns at all");
}

} // namespace

int main() {
    TestHandOff();
    TestMaxAngle();
    TestVisual();
    TestSteeringShown();
    TestSeatFrame();
    TestPlaceholderControl();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "vr hand steering tests passed\n";
    return 0;
}
