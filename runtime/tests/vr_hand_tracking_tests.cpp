// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tracked hands: the grasp read from the fingers, the bare-hand latch, pinch
// gating, the bare-hand buttons and the flick, tested without a headset
// (vr/openxr_hand_tracking.h).

#include "vr/openxr_hand_tracking.h"
#include "vr/steering_wheel.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

using namespace mkw::vr;
using namespace mkw::vr::hand_tracking;

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

// A hand with its fingers pointing along -Z and bending towards -Y (the palm
// side), each finger bent by the same three angles at its knuckle, middle and
// end joints. `side` mirrors it across X for the other hand.
JointPositions Hand(float knuckle, float middle, float end, float side = 1.0f) {
    JointPositions joints{};
    joints[kPalm] = {0.0f, 0.0f, 0.0f};
    joints[kWrist] = {0.0f, 0.0f, 0.08f};
    for (size_t finger = 0; finger < 5; ++finger) {
        const size_t base = kMetacarpal[finger];
        const size_t count = finger == 0 ? 4 : 5;
        const float x = side * (finger == 0 ? -0.04f : -0.03f + 0.02f * static_cast<float>(finger - 1));
        joints[base] = {x, 0.0f, 0.06f};
        joints[base + 1] = {x, 0.0f, -0.01f};
        const float lengths[3]{0.045f, 0.025f, 0.02f};
        const float bends[3]{knuckle, middle, end};
        float angle = 0.0f;
        // The phalanges after the knuckle: three for a finger, two for the thumb.
        for (size_t bone = 0; bone + 2 < count; ++bone) {
            angle += bends[bone];
            const Vec3& from = joints[base + 1 + bone];
            joints[base + 2 + bone] = {from[0], from[1] - std::sin(angle) * lengths[bone],
                                       from[2] - std::cos(angle) * lengths[bone]};
        }
    }
    return joints;
}

// A row-major 3x4 from its three axes (columns) and origin.
std::array<float, 12> Frame(const Vec3& x, const Vec3& y, const Vec3& z, const Vec3& origin) {
    return {x[0], y[0], z[0], origin[0], x[1], y[1], z[1], origin[1], x[2], y[2], z[2], origin[2]};
}

void CheckAxis(const std::array<float, 12>& m, int column, const Vec3& expected, const char* what) {
    for (int row = 0; row < 3; ++row) {
        CheckNear(m[row * 4 + column], expected[row], what);
    }
}

void TestGripFromPalm() {
    // Seat frame: +X right, +Y up, -Z forward. Both hands flat, palms down,
    // fingers forward: the palm joint's +Y is up, +Z back, so +X is right.
    const auto flat = Frame({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.1f, -0.2f, -0.3f});
    // OpenXR's grip: +X into the right palm (up here) and away from the left
    // (down); -Z from the little finger to the thumb, which is left on the
    // right hand and right on the left; the fingers along -Y.
    const auto right = GripFromPalm(flat, 1);
    CheckAxis(right, 0, {0, 1, 0}, "right grip +X goes into the palm");
    CheckAxis(right, 2, {1, 0, 0}, "right grip -Z runs from the little finger to the thumb");
    CheckAxis(right, 1, {0, 0, 1}, "right grip: the fingers point along -Y");
    CheckAxis(right, 3, {0.1f, -0.2f, -0.3f}, "the grip keeps the palm's origin");
    const auto left = GripFromPalm(flat, 0);
    CheckAxis(left, 0, {0, -1, 0}, "left grip +X leaves the palm");
    CheckAxis(left, 2, {-1, 0, 0}, "left grip -Z runs from the little finger to the thumb");
    CheckAxis(left, 1, {0, 0, 1}, "left grip: the fingers point along -Y");
    // Hands held mirrored, palms facing each other with the fingers forward,
    // give both grips the same orientation, as controllers held that way do.
    const auto right_inward = GripFromPalm(Frame({0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {0.2f, 0, 0}), 1);
    const auto left_inward = GripFromPalm(Frame({0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, {-0.2f, 0, 0}), 0);
    for (int column = 0; column < 3; ++column) {
        CheckAxis(left_inward, column, {right_inward[column], right_inward[4 + column], right_inward[8 + column]},
                  "mirrored hands carry the same grip orientation");
    }
    // A rotation either way: X x Y = Z.
    for (const auto& grip : {right, left, right_inward, left_inward}) {
        const Vec3 x{grip[0], grip[4], grip[8]}, y{grip[1], grip[5], grip[9]};
        CheckAxis(grip, 2, {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]},
                  "the grip is right-handed");
    }
}

JointPositions Transformed(const JointPositions& joints, float scale, float yaw, const Vec3& offset) {
    JointPositions out{};
    const float c = std::cos(yaw), s = std::sin(yaw);
    for (size_t j = 0; j < kJointCount; ++j) {
        const Vec3& p = joints[j];
        out[j] = {scale * (c * p[0] + s * p[2]) + offset[0], scale * p[1] + offset[1],
                  scale * (-s * p[0] + c * p[2]) + offset[2]};
    }
    return out;
}

void TestGrasp() {
    CheckNear(GraspFromJoints(Hand(0.05f, 0.05f, 0.05f)), 0.0f, "an open hand does not grasp");
    Check(GraspFromJoints(Hand(0.3f, 0.5f, 0.3f)) < 0.15f, "a relaxed hand is under the wheel's release");
    const float grip = GraspFromJoints(Hand(1.0f, 1.3f, 0.6f));
    Check(grip > 0.55f, "a hand closed on a rim is over the wheel's press");
    CheckNear(GraspFromJoints(Hand(1.4f, 1.6f, 1.0f)), 1.0f, "a fist grasps fully");
    CheckNear(GraspFromJoints(Hand(1.0f, 1.3f, 0.6f, -1.0f)), grip, "the other hand grasps the same");
    CheckNear(GraspFromJoints(Transformed(Hand(1.0f, 1.3f, 0.6f), 1.3f, 0.7f, {0.2f, -0.3f, -0.4f})), grip,
              "a bigger, turned hand elsewhere grasps the same");
    JointPositions broken = Hand(1.0f, 1.3f, 0.6f);
    broken[13][1] = std::numeric_limits<float>::quiet_NaN();
    CheckNear(GraspFromJoints(broken), 0.0f, "a NaN joint reads as open");
    JointPositions collapsed = Hand(1.0f, 1.3f, 0.6f);
    collapsed[14] = collapsed[13];
    CheckNear(GraspFromJoints(collapsed), 0.0f, "a collapsed bone reads as open");
}

void TestLatch() {
    constexpr float dt = 1.0f / 72.0f, grace = 0.2f;
    BareLatch latch;
    Check(!latch.Update(false, true, false, 0.0f, dt, grace), "a controller hand is not bare");
    Check(!latch.Update(true, false, false, 0.9f, dt, grace), "a hand-driven hand needs camera joints");
    Check(latch.Update(true, false, true, 0.9f, dt, grace) && latch.Tracked(), "camera joints make it bare");
    CheckNear(latch.Grasp(), 0.9f, "the latch keeps the grasp");
    bool held = true;
    for (int frame = 0; frame < 10; ++frame) {
        held = held && latch.Update(false, false, false, 0.0f, dt, grace);
    }
    Check(held && !latch.Tracked(), "a short loss keeps it bare but untracked");
    CheckNear(latch.Grasp(), 0.9f, "the last grasp is kept through the loss");
    for (int frame = 0; frame < 10; ++frame) {
        latch.Update(false, false, false, 0.0f, dt, grace);
    }
    Check(!latch.Bare(), "a loss past the grace lets go");
    latch.Update(true, false, true, 0.9f, dt, grace);
    Check(!latch.Update(false, true, true, 0.9f, dt, grace), "picking a controller up clears it at once");
    Check(!latch.Update(false, false, true, 0.9f, dt, grace), "joints alone, not hand-driven, are not bare");
}

void TestPinchGate() {
    constexpr float dt = 1.0f / 72.0f;
    PinchGate gate;
    Check(!gate.Update(true, true, dt), "a hand on the wheel never uses an item");
    bool fired = false;
    for (int frame = 0; frame < 5; ++frame) {
        fired = gate.Update(true, false, dt) || fired;
    }
    Check(!fired, "a pinch carried off the rim does not use an item");
    gate.Update(false, false, dt);
    for (int frame = 0; frame < 12; ++frame) {
        gate.Update(false, false, dt);
    }
    Check(gate.Update(true, false, dt), "a pinch from a hand free for a moment uses an item");
    Check(gate.Update(true, false, dt), "holding the pinch holds the button");
    Check(!gate.Update(false, false, dt), "letting go releases it");
    Check(!gate.Update(true, true, dt), "grabbing the wheel stops it");

    Check(ItemPinch(true, GraspFromJoints(Hand(0.3f, 0.5f, 0.3f))), "a pinch from a relaxed hand is an item");
    Check(!ItemPinch(true, GraspFromJoints(Hand(1.0f, 1.3f, 0.6f))), "a hand closing on the rim is not an item");
    Check(!ItemPinch(true, std::numeric_limits<float>::quiet_NaN()), "an unknown grasp is not an item");
    Check(!ItemPinch(false, 0.0f), "no pinch, no item");
}

void TestGestures() {
    Check(GesturesOf(true, true, false, false, false, false).pinch, "the runtime's pinch counts");
    Check(!GesturesOf(true, true, false, true, true, false).pinch, "a pinch in the system gesture is the menu");
    Check(GesturesOf(true, false, true, true, false, false).menu, "the runtime's menu gesture counts");
    Check(GesturesOf(false, false, false, false, true, false).pinch, "without the aim state, select is the pinch");
    Check(GesturesOf(false, false, false, false, false, true).menu, "without the aim state, the menu action");
    wii_remote::HandInputs left{}, right{};
    left.secondary = true;
    right.primary = true;
    Check(SelectOf(left, 0) && SelectOf(right, 1), "simple_controller's select per hand");

    Check(!HandDriven(true, true, true), "a hand holding a controller is not bare");
    Check(HandDriven(false, true, false), "a hand driving simple_controller is bare");
    Check(HandDriven(false, false, true), "a camera-tracked hand with no profile of ours is bare");
    Check(!HandDriven(false, false, false), "a hand with nothing active is not");
}

std::array<wii_remote::HandInputs, 2> PinchingHands() {
    std::array<wii_remote::HandInputs, 2> hands{};
    hands[0].secondary = true; // left select
    hands[0].menu = true;
    hands[1].primary = true;   // right select
    return hands;
}

void TestHandDrivenButtons() {
    auto hands = PinchingHands();
    ApplyHandDrivenButtons(hands, {true, true}, {true, true}, true);
    Check(hands[1].primary, "option on: a right pinch is A");
    Check(!hands[0].secondary && !hands[0].primary, "option on: a left pinch no longer toggles the panel");
    Check(hands[0].menu, "the menu gesture is kept");

    hands = PinchingHands();
    ApplyHandDrivenButtons(hands, {true, true}, {true, true}, false);
    Check(!hands[1].primary && !hands[0].secondary, "option off: hand-driven hands press nothing");
    Check(hands[0].menu, "option off: the menu gesture still pauses");

    std::array<wii_remote::HandInputs, 2> controllers{};
    controllers[0].secondary = true;
    controllers[1].primary = true;
    controllers[1].squeeze = 0.8f;
    const auto before = controllers;
    ApplyHandDrivenButtons(controllers, {false, false}, {false, false}, true);
    Check(controllers[0].secondary == before[0].secondary && controllers[1].primary == before[1].primary &&
              controllers[1].squeeze == before[1].squeeze,
          "controller hands are left alone");
}

void TestBareHandRace() {
    std::array<wii_remote::HandInputs, 2> hands{};
    hands[1].primary = true; // a right pinch, A from the menus' mapping
    Check(!ApplyBareHandRace(hands, {false, false}, {false, false}) && hands[1].primary,
          "with no hand on the wheel a right pinch stays A (the pause menu, the results)");
    hands = {};
    Check(ApplyBareHandRace(hands, {true, false}, {false, false}) && hands[1].primary,
          "a bare hand on the wheel holds the gas");
    Check(hands[0].trigger == 0.0f && hands[0].squeeze == 0.0f && hands[1].squeeze == 0.0f,
          "holding presses no item and no shoulder");
    hands = {};
    ApplyBareHandRace(hands, {false, true}, {true, false});
    Check(hands[1].primary && hands[0].trigger == 1.0f, "a free hand's pinch uses an item while the other drives");
    hands = {};
    ApplyBareHandRace(hands, {true, false}, {false, true});
    Check(hands[1].primary && hands[0].trigger == 1.0f, "a free right hand's pinch is an item, not A, while driving");
    std::array<wii_remote::HandInputs, 2> controllers{};
    controllers[1].primary = true;
    controllers[0].trigger = 0.3f;
    ApplyBareHandRace(controllers, {false, false}, {false, false});
    Check(controllers[1].primary && controllers[0].trigger == 0.3f, "controller hands keep their own buttons");
}

// Runs the detector over `frames` frames of both hands moving at the given
// vertical speeds; returns how many flicks fired.
int Flicks(FlickDetector& detector, std::array<FlickHand, 2> hands, float left_speed, float right_speed,
           int frames, float dt = 1.0f / 72.0f) {
    int fired = 0;
    for (int frame = 0; frame < frames; ++frame) {
        fired += detector.Update(hands, dt) ? 1 : 0;
        hands[0].height += left_speed * dt;
        hands[1].height += right_speed * dt;
    }
    return fired;
}

void TestFlick() {
    const std::array<FlickHand, 2> holding{{{true, true, -0.3f}, {true, true, -0.3f}}};
    FlickDetector detector;
    Check(Flicks(detector, holding, 0.0f, 0.0f, 20) == 0, "still hands do not flick");
    Check(Flicks(detector, holding, 2.0f, 2.0f, 12) == 1, "both hands rising together flick once");

    detector.Reset();
    Check(Flicks(detector, holding, 2.0f, -2.0f, 20) == 0, "a turn (one up, one down) never flicks");
    detector.Reset();
    Check(Flicks(detector, holding, 2.5f, 0.2f, 20) == 0, "one hand rising on the wheel is a turn");

    detector.Reset();
    const std::array<FlickHand, 2> free_right{{{true, true, -0.3f}, {true, false, -0.2f}}};
    Check(Flicks(detector, free_right, 0.0f, 2.0f, 12) == 1, "a free hand rising fast flicks");
    detector.Reset();
    Check(Flicks(detector, free_right, 0.0f, 1.0f, 30) == 0, "a free hand lifted slowly does not");

    detector.Reset();
    const std::array<FlickHand, 2> lone{{{false, false, 0.0f}, {true, true, -0.3f}}};
    Check(Flicks(detector, lone, 0.0f, 2.5f, 20) == 0, "a lone hand on the wheel does not flick");

    detector.Reset();
    std::array<FlickHand, 2> jump = holding;
    Flicks(detector, jump, 0.0f, 0.0f, 5);
    jump[0].height += 0.2f;
    jump[1].height += 0.2f;
    Check(Flicks(detector, jump, 0.0f, 0.0f, 10) == 0, "a pose jumping back into tracking does not flick");

    detector.Reset();
    Check(Flicks(detector, holding, 2.0f, 2.0f, 12) == 1, "a flick");
    Check(Flicks(detector, holding, 0.0f, 0.0f, 5) + Flicks(detector, holding, 2.0f, 2.0f, 12) == 0,
          "a second flick inside the cooldown does not fire");
    Check(Flicks(detector, holding, 0.0f, 0.0f, 40) + Flicks(detector, holding, 2.0f, 2.0f, 12) == 1,
          "after the cooldown it fires again");

    detector.Reset();
    std::array<FlickHand, 2> nan = holding;
    nan[0].height = std::numeric_limits<float>::quiet_NaN();
    Check(Flicks(detector, nan, 2.0f, 2.0f, 12) == 0, "a NaN height never flicks");
    Check(!detector.Update(holding, std::numeric_limits<float>::quiet_NaN()), "a NaN step never flicks");
}

void TestPulse() {
    bool active = false;
    float peak = 0.0f, low = 0.0f;
    int64_t last_active = -1;
    for (int64_t t = 0; t <= 200'000'000; t += 1'000'000) {
        const auto acc = FlickPulse(t, &active);
        Check(std::fabs(acc[1]) <= wii_remote::kAccelRangeG, "the pulse stays within the accelerometer's range");
        peak = std::max(peak, acc[1]);
        low = std::min(low, acc[1]);
        if (active) {
            last_active = t;
        }
    }
    Check(last_active >= 50'000'000, "the pulse lasts at least three guest frames");
    Check(!active, "the pulse ends");
    Check(peak > 1.0f && low < -3.0f, "the pulse swings up then down, well past rest");
    const auto rest = FlickPulse(-1, &active);
    Check(!active && rest[1] == -1.0f, "before it starts the remote is at rest");
}

// A bare hand on the canonical VR wheel: palm position and grasp, as
// OpenXRInput::UpdateDriving feeds them.
void TestBareHandSteers() {
    constexpr float dt = 1.0f / 72.0f;
    SteeringWheel wheel;
    const float x = SteeringWheel::Radius, y = SteeringWheel::Height, z = SteeringWheel::Depth;
    const float open = GraspFromJoints(Hand(0.2f, 0.3f, 0.2f));
    const float closed = GraspFromJoints(Hand(1.0f, 1.3f, 0.6f));
    WheelState state = wheel.Update({WheelHand{}, WheelHand{x, y, z, open, true}}, true, dt);
    Check(!state.held[1], "an open hand at the rim does not hold");
    state = wheel.Update({WheelHand{}, WheelHand{x, y, z, closed, true}}, true, dt);
    Check(state.held[1], "closing the hand on the rim takes hold");
    for (int frame = 0; frame < 30; ++frame) {
        const float angle = -0.02f * static_cast<float>(frame + 1);
        state = wheel.Update({WheelHand{}, WheelHand{x * std::cos(angle), y + x * std::sin(angle), z, closed, true}},
                             true, dt);
    }
    Check(state.held[1] && state.steering > 0.1f, "turning the hand down the right of the rim steers right");
    for (int frame = 0; frame < 7; ++frame) {
        state = wheel.Update({WheelHand{}, WheelHand{0.0f, 0.0f, 0.0f, closed, false}}, true, dt);
    }
    Check(state.held[1], "a short loss with the latched grasp keeps hold");
    state = wheel.Update({WheelHand{}, WheelHand{x, y, z, open, true}}, true, dt);
    Check(!state.held[1], "opening the hand lets go");
}

} // namespace

int main() {
    TestGrasp();
    TestLatch();
    TestPinchGate();
    TestGestures();
    TestHandDrivenButtons();
    TestBareHandRace();
    TestFlick();
    TestPulse();
    TestBareHandSteers();
    TestGripFromPalm();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "mkw_vr_hand_tracking_tests passed\n";
    return 0;
}
