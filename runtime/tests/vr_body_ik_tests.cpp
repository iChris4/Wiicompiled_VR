// SPDX-License-Identifier: GPL-3.0-or-later
//
// Character body IK (vr/body_ik.h, vr/body_hand_profiles.h), tested without a
// guest: the leg animation gate, seated limbs, the per-character hand profiles
// and their keys, arm reach and the two-bone solve, the wrist behind the palm,
// skinned-segment fitting, joint rotation and the skeleton's regions. Ported
// from heurazy's Wiicompiled_VR-PLUS fork (GPL-3.0-or-later).

#include "vr/body_hand_profiles.h"
#include "vr/body_ik.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mkw::vr;
using namespace mkw::vr::body_ik;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

// A NaN read from memory, the way bad guest or tracking data arrives. The body
// IK ships in a -ffast-math translation unit (and is tested that way), where a
// NaN constant is itself undefined and would let the compiler drop the very
// guards it is meant to exercise.
volatile uint32_t g_nan_bits = 0x7FC00000u;

float LoadedNaN() {
    const uint32_t bits = g_nan_bits;
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool IsFiniteVec(const Vec& v) {
    return detail::IsFiniteFloat(&v.x) && detail::IsFiniteFloat(&v.y) && detail::IsFiniteFloat(&v.z);
}

bool Near(float a, float b, float tolerance = 0.0005f) {
    return std::fabs(a - b) < tolerance;
}

bool NearVec(Vec a, Vec b, float tolerance = 0.0005f) {
    return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance);
}

Vec Column(const Mtx34& m, int column) {
    return {m[column], m[column + 4], m[column + 8]};
}

float Determinant(const Mtx34& m) {
    return detail::Dot(Column(m, 0), detail::Cross(Column(m, 1), Column(m, 2)));
}

// A proper rotation in the 3x3 part: unit, perpendicular columns, no mirror.
bool IsRotation(const Mtx34& m, float tolerance = 0.002f) {
    for (int column = 0; column < 3; ++column) {
        if (!Near(Length(Column(m, column)), 1.0f, tolerance)) return false;
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            if (!Near(detail::Dot(Column(m, a), Column(m, b)), 0.0f, tolerance)) return false;
        }
    }
    return Near(Determinant(m), 1.0f, 2.5f * tolerance);
}

void TestLegAnimationGate() {
    for (uint16_t id : {0, 2, 3, 4, 5, 6, 7, 15, 16, 17, 18, 19}) {
        Check(IsDrivingLegAnimation(id), "drive, drift, wheelie, wait and steering are driving poses");
    }
    for (uint16_t id : {1, 8, 9, 10, 11, 12, 13, 14, 20, 21, 27, 28, 40, 65535}) {
        Check(!IsDrivingLegAnimation(id), "dash and every other animation are not driving poses");
    }

    // A character action (a celebration) holds the legs past its own end.
    LegAnimationGate celebration;
    Check(celebration.AllowNative(false, false, true, 1000000000ull, false), "a drift allows native legs");
    Check(!celebration.AllowNative(false, false, true, 1010000000ull, true), "a character action blocks them");
    Check(!celebration.AllowNative(false, false, false, 4000000000ull, true), "the action blocks with the boost over");
    Check(!celebration.AllowNative(false, false, false, 4100000000ull, false), "the action blends out");
    Check(celebration.AllowNative(false, false, false, 4300000000ull, false), "the blend-out ends after 250 ms");
    Check(celebration.AllowNative(false, false, true, 4400000000ull, false), "a later drift allows native legs");

    LegAnimationGate gate;
    Check(gate.AllowNative(false, false, false, 1000000000ull), "plain driving allows native legs");
    Check(gate.AllowNative(false, false, true, 1010000000ull), "a drift allows native legs");
    Check(!gate.AllowNative(false, true, false, 1020000000ull), "a mini-turbo release blocks them");
    Check(!gate.AllowNative(false, true, false, 1100000000ull), "the boost's reward animation blocks them");
    Check(!gate.AllowNative(false, false, false, 1200000000ull), "the boost settles for 250 ms");
    Check(gate.AllowNative(false, false, false, 1400000000ull), "native legs return after the boost");
    Check(!gate.AllowNative(false, true, false, 1500000000ull), "another boost blocks them");
    Check(gate.AllowNative(false, true, true, 1510000000ull), "the next drift during a boost allows them");
    Check(!gate.AllowNative(true, true, true, 1520000000ull), "a trick blocks them");
    Check(!gate.AllowNative(false, true, true, 1600000000ull), "the trick hold still applies while drifting");
    Check(gate.AllowNative(false, true, true, 1800000000ull), "the trick hold ends after 250 ms");
}

void TestSeatedLimb() {
    // Turning and drifting leg motion survives, but an animated root's
    // rotation and jump translation do not move the seated pelvis or chest.
    auto seated_parent = kIdentityMtx34;
    seated_parent[7] = 0.4f;
    auto animated_parent = Align({}, {0, 1, 0}, {1, 0, 0});
    animated_parent[3] = 4;
    animated_parent[7] = 8;
    animated_parent[11] = -2;
    auto leg_local = Align({}, {0, -1, 0}, {0, -1, -1});
    leg_local[7] = -0.3f;
    leg_local[11] = -0.1f;
    const auto animated_leg = ComposeMtx(animated_parent, leg_local);
    Mtx34 seated_leg{};
    Check(AnimateSeatedLimb(seated_parent, animated_parent, animated_leg, seated_leg),
          "a limb under an invertible parent is reseated");
    const auto expected_leg = ComposeMtx(seated_parent, leg_local);
    bool same = true;
    for (unsigned i = 0; i < 12; ++i) same = same && Near(seated_leg[i], expected_leg[i]);
    Check(same, "the limb keeps its pose relative to the seated parent");
    Check(!AnimateSeatedLimb(seated_parent, Mtx34{}, animated_leg, seated_leg), "a singular parent is rejected");
    auto broken_parent = animated_parent;
    broken_parent[3] = LoadedNaN();
    Check(!AnimateSeatedLimb(seated_parent, broken_parent, animated_leg, seated_leg),
          "a non-finite parent is rejected");
    auto broken_joint = animated_leg;
    broken_joint[7] = LoadedNaN();
    Check(!AnimateSeatedLimb(seated_parent, animated_parent, broken_joint, seated_leg),
          "a non-finite joint is rejected");
    auto broken_seat = seated_parent;
    broken_seat[0] = LoadedNaN();
    Check(!AnimateSeatedLimb(broken_seat, animated_parent, animated_leg, seated_leg),
          "a non-finite seated parent is rejected");
}

void TestProfileKey() {
    Check(ProfileKey("mario_body__x") == "mario_body", "a model's array name is cut at the double underscore");
    Check(ProfileKey("body__mario_body") == "mario_body", "a body__ prefix is dropped");
    Check(ProfileKey("body__bb_peach_all_tx") == "bb_peach_all_tx", "the body__ prefix wins over the cut");
    Check(ProfileKey("k_hair_model") == "kino_girl_body", "Toadette's hair names her body");
    Check(ProfileKey("k_hair_model__pos") == "kino_girl_body", "Toadette's hair array names her body");
    Check(ProfileKey("mario-body.x") == "mario_body_x", "other characters become underscores");
    Check(ProfileKey("a b@c") == "a_b_c", "spaces and symbols become underscores");
    Check(ProfileKey("luigi_body") == "luigi_body", "a plain key is kept");
    Check(ProfileKey("").empty(), "an empty name stays empty");
}

void TestHandProfiles() {
    Check(std::size(kHandProfiles) == 24, "24 standard characters have hand profiles");
    std::set<std::string> keys;
    for (const auto& profile : kHandProfiles) {
        Check(profile.key != nullptr && profile.key[0] != '\0', "every profile has a key");
        if (profile.key == nullptr) continue;
        keys.insert(profile.key);
        Check(ProfileKey(profile.key) == profile.key, "every key is its own ProfileKey");
        Check(FindHandProfile(profile.key) == &profile, "every profile is found by its key");
        for (int hand = 0; hand < 2; ++hand) {
            Mtx34 pose{};
            std::copy_n(profile.wrist_from_grip.begin() + hand * 12, 12, pose.begin());
            Check(detail::IsFiniteMtx34(pose), "every wrist-from-grip is finite");
            Check(Length(Position(pose)) < 0.18f, "every wrist sits within 18 cm of the grip");
            Check(IsRotation(pose), "every wrist-from-grip rotation is orthonormal and unmirrored");
        }
    }
    Check(keys.size() == std::size(kHandProfiles), "profile keys are unique");

    Check(FindHandProfile("mario_body") == &kHandProfiles[0], "a key is found");
    const auto* toadette = FindHandProfile(ProfileKey("k_hair_model__pos").c_str());
    Check(toadette != nullptr && std::strcmp(toadette->key, "kino_girl_body") == 0,
          "Toadette is found through her hair's array name");
    const auto* peach = FindHandProfile(ProfileKey("body__bb_peach_all_tx").c_str());
    Check(peach != nullptr && std::strcmp(peach->key, "bb_peach_all_tx") == 0, "Baby Peach is found by her body__ name");
    Check(FindHandProfile("unknown_body") == nullptr, "an unknown key is not found");
    Check(FindHandProfile("mario_body__x") == nullptr, "a raw array name is not a key");
    Check(FindHandProfile("Mario_body") == nullptr, "keys are case-sensitive");
    Check(FindHandProfile("") == nullptr, "an empty key is not found");
}

void TestReach() {
    for (float scale : {0.2f, 1.0f, 2.5f}) {
        // A close controller must not create the old 65 cm folded sleeve.
        const float reach_fit = FitArmReach(0.5f * scale, 0.2f * scale, scale);
        Check(Near(0.5f * scale * reach_fit, 0.30f * scale), "a close controller keeps a 30 cm arm");
        const auto close = Solve({}, {0.25f * scale * reach_fit, 0, 0}, {0.5f * scale * reach_fit, 0, 0},
                                 {0, 0, -0.2f * scale}, {-0.7f, -1, 0.25f});
        Check(close.valid && Near(close.wrist.z, -0.2f * scale), "a close controller is reached");
        Check(Length(close.elbow) < 0.16f * scale, "a close controller does not fold a long arm");
        const auto extended = FitArmReach(0.5f * scale, 0.8f * scale, scale);
        Check(0.5f * scale * extended > 0.8f * scale, "a far controller stretches the arm past it");

        const Vec s{0, 0, 0}, e{scale, 0, 0}, w{2 * scale, 0, 0};
        for (Vec target : {Vec{scale, scale, 0}, Vec{100, 0, 0}, Vec{0, 0, 0}, Vec{-scale, 0, 0}}) {
            const auto arm = Solve(s, e, w, target, {0, -1, 0});
            Check(arm.valid, "every target solves");
            Check(Near(Length(Sub(arm.elbow, s)), scale), "the upper arm keeps its length");
            Check(Near(Length(Sub(arm.wrist, arm.elbow)), scale), "the forearm keeps its length");
            Check(IsFiniteVec(arm.elbow), "the elbow is finite");
        }
        const auto reached = Solve(s, e, w, {scale, scale, 0}, {0, -1, 0});
        Check(NearVec(reached.wrist, {scale, scale, 0}), "a target within reach is reached");
        const auto straight = Solve(s, e, w, {scale, 0, 0}, {0, -1, 0});
        Check(straight.valid && straight.elbow.y < 0, "the elbow bends toward the pole");
        const auto far = Solve(s, e, w, {100, 0, 0}, {0, -1, 0});
        Check(far.valid && far.wrist.x <= 2 * scale && far.wrist.x > 1.99f * scale,
              "a target out of reach stops at a full arm without stretching");
        const auto parallel_pole = Solve(s, e, w, {scale, 0, 0}, {1, 0, 0});
        Check(parallel_pole.valid && Near(Length(Sub(parallel_pole.elbow, s)), scale) &&
                  detail::IsFiniteFloat(&parallel_pole.elbow.z),
              "a pole along the reach falls back to another bend plane");
    }
    Check(FitArmReach(0.0f, 0.5f, 1.0f) == 1.0f, "a zero-length arm is not refit");
    Check(FitArmReach(0.5f, 0.5f, 0.0f) == 1.0f, "a zero world scale does not refit");
    Check(Near(FitArmReach(0.01f, 100.0f, 1.0f), 3.5f), "the stretch is capped");
    Check(Near(FitArmReach(10.0f, 0.0f, 1.0f), 0.25f), "the shrink is capped");
    Check(!Solve({}, {}, {}, {1, 0, 0}, {0, -1, 0}).valid, "a zero-length arm does not solve");

    // Lost tracking or a corrupt pose never reaches the skinning palette.
    const float nan = LoadedNaN();
    Check(!Solve({}, {1, 0, 0}, {2, 0, 0}, {nan, 0, 0}, {0, -1, 0}).valid, "a non-finite target does not solve");
    Check(!Solve({}, {1, 0, 0}, {2, 0, 0}, {1, 1, nan}, {0, -1, 0}).valid, "a target with a NaN in it does not solve");
    Check(!Solve({nan, 0, 0}, {1, 0, 0}, {2, 0, 0}, {1, 1, 0}, {0, -1, 0}).valid, "a non-finite shoulder does not solve");
    Check(!Solve({}, {1, nan, 0}, {2, 0, 0}, {1, 1, 0}, {0, -1, 0}).valid, "a non-finite elbow does not solve");
    Check(!Solve({}, {1, 0, 0}, {2, 0, nan}, {1, 1, 0}, {0, -1, 0}).valid, "a non-finite wrist does not solve");
    const auto nan_pole = Solve({}, {1, 0, 0}, {2, 0, 0}, {1, 1, 0}, {nan, -1, 0});
    Check(!nan_pole.valid || (IsFiniteVec(nan_pole.elbow) && IsFiniteVec(nan_pole.wrist)),
          "a non-finite pole never yields a non-finite arm");
}

void TestWristForPalm() {
    const Vec pivot{4, 5, 6};
    for (float scale : {0.2f, 1.0f, 2.5f}) {
        for (Vec direction : {Vec{1, 0, 0}, Vec{-1, 0, 0}, Vec{0, 0, -1}, Vec{0, 1, 0}}) {
            auto rotation = Align({}, {1, 0, 0}, direction);
            // The hand's translation does not move the offset.
            rotation[3] = 7;
            rotation[7] = -3;
            rotation[11] = 2;
            const auto wrist = WristForPalm(pivot, rotation, {1, 0, 0}, 0.055f * scale);
            const auto palm = Add(wrist, Mul(direction, 0.055f * scale));
            Check(NearVec(palm, pivot), "the wrist sits behind the palm along the hand's palm axis");
        }
    }
    Check(NearVec(WristForPalm(pivot, Mtx34{}, {1, 0, 0}, 0.055f), pivot), "a degenerate hand keeps the palm");
    Check(NearVec(WristForPalm(pivot, kIdentityMtx34, {}, 0.055f), pivot), "a zero palm axis keeps the palm");
}

void TestMapSegment() {
    const Vec pivot{4, 5, 6};
    const auto fit = MapSegment(pivot, {1, 2, 3}, {1, 0, 0}, {0, 2, 0});
    Check(NearVec(detail::TransformPoint(fit, pivot.x, pivot.y, pivot.z), {1, 2, 3}),
          "the segment's origin moves to the new origin");
    const auto endpoint = detail::TransformPoint(fit, 5, 5, 6);
    Check(NearVec(endpoint, {1, 4, 3}), "the segment's end lands on the new segment's end");
    Check(Near(Length(detail::TransformDirection(fit, {0, 0, 1})), 1), "the cross section keeps its thickness");
    Check(Near(Length(detail::TransformDirection(fit, {0, 1, 0})), 1), "the cross section keeps its width");
    const auto thin = MapSegment(pivot, {1, 2, 3}, {1, 0, 0}, {0, 2, 0}, 0.65f);
    Check(NearVec(detail::TransformPoint(thin, 5, 5, 6), endpoint), "thinning keeps the segment's end");
    Check(Near(Length(detail::TransformDirection(thin, {0, 0, 1})), 0.65f), "thinning scales the thickness");
    Check(Near(Length(detail::TransformDirection(thin, {0, 1, 0})), 0.65f), "thinning scales the width");
    Check(MapSegment(pivot, {1, 2, 3}, {}, {0, 2, 0}) == kIdentityMtx34, "a zero-length old segment is not fit");
    Check(MapSegment(pivot, {1, 2, 3}, {1, 0, 0}, {}) == kIdentityMtx34, "a zero-length new segment is not fit");
}

void TestAlign() {
    const Vec pivot{4, 5, 6};
    for (Vec to : {Vec{0, 1, 0}, Vec{-1, 0, 0}, Vec{1, 0, 0}, Vec{0, 0, -3}}) {
        const auto m = Align(pivot, {1, 0, 0}, to);
        Vec unit = to;
        detail::Normalize(unit);
        Check(NearVec(detail::TransformPoint(m, pivot.x, pivot.y, pivot.z), pivot), "the joint stays in place");
        Check(NearVec(detail::TransformDirection(m, {1, 0, 0}), unit), "the old segment turns onto the new one");
        Check(IsRotation(m), "the turn is a proper rotation");
    }
    // Opposite vectors along +Y have no perpendicular through +Y itself.
    const auto flip = Align(pivot, {0, 1, 0}, {0, -1, 0});
    Check(NearVec(detail::TransformPoint(flip, pivot.x, pivot.y, pivot.z), pivot), "a flip keeps the joint in place");
    Check(NearVec(detail::TransformDirection(flip, {0, 1, 0}), {0, -1, 0}), "a flip reverses the segment");
    Check(IsRotation(flip), "a flip is a proper rotation");
    Check(Align(pivot, {2, 0, 0}, {5, 0, 0}) == kIdentityMtx34, "an unchanged direction is the identity");
    Check(Align(pivot, {}, {1, 0, 0}) == kIdentityMtx34, "a zero old segment is the identity");
    Check(Align(pivot, {1, 0, 0}, {}) == kIdentityMtx34, "a zero new segment is the identity");
}

void TestRollAboutForearm() {
    // A right grip held palm down: +X (into the palm) up, +Y back along the
    // forearm towards the player, +Z right.
    const Mtx34 grip{0, 0, 1, 0.1f, 1, 0, 0, -0.2f, 0, 1, 0, -0.3f};
    const auto turned = RollAboutForearm(grip, 90.0f);
    // Looking forward along the arm from the seat, up turning to the left is
    // counter-clockwise.
    Check(NearVec(detail::TransformDirection(turned, {1, 0, 0}), {-1, 0, 0}),
          "+90 turns the hand counter-clockwise as the player sees it");
    Check(NearVec(detail::TransformDirection(turned, {0, 1, 0}), {0, 0, 1}), "the forearm axis stays");
    Check(NearVec(Position(turned), Position(grip)), "the palm stays in place");
    Check(IsRotation(turned), "the turned grip is a proper rotation");
    Check(NearVec(detail::TransformDirection(RollAboutForearm(grip, -90.0f), {1, 0, 0}), {1, 0, 0}),
          "-90 turns it clockwise");
    Check(NearVec(detail::TransformDirection(RollAboutForearm(grip, 180.0f), {1, 0, 0}), {0, -1, 0}),
          "180 turns it over");
    Check(RollAboutForearm(grip, 0.0f) == grip, "no turn leaves the grip as it is");
    Check(RollAboutForearm(grip, LoadedNaN()) == grip, "a non-finite turn leaves the grip as it is");
}

// root -> spin -> {arm_l1 -> arm_l2 -> wrist_l1 -> finger, arm_r1 -> arm_r2 ->
// wrist_r1, face_1 -> hair}; root -> leg_l1 -> foot_l; root -> leg_r1.
std::vector<RigBone> Skeleton() {
    return {
        {-1, "root"},    //  0
        {0, "spin"},     //  1
        {1, "arm_l1"},   //  2
        {2, "arm_l2"},   //  3
        {3, "wrist_l1"}, //  4
        {4, "finger"},   //  5
        {1, "arm_r1"},   //  6
        {6, "arm_r2"},   //  7
        {7, "wrist_r1"}, //  8
        {1, "face_1"},   //  9
        {9, "hair"},     // 10
        {0, "leg_l1"},   // 11
        {11, "foot_l"},  // 12
        {0, "leg_r1"},   // 13
    };
}

using Bones = std::vector<int>;
using Collapsed = std::vector<std::pair<int, int>>;

void CheckArmRegions(const RigRegions& rig) {
    Check(rig.arms[0] == std::array<int, 3>{2, 3, 4}, "the left arm's shoulder, elbow and wrist");
    Check(rig.arms[1] == std::array<int, 3>{6, 7, 8}, "the right arm's shoulder, elbow and wrist");
    Check(rig.hand_bones[0] == Bones{4, 5}, "the left hand is the wrist's subtree");
    Check(rig.lower_bones[0] == Bones{3}, "the left forearm is below the elbow, above the wrist");
    Check(rig.upper_bones[0] == Bones{2}, "the left upper arm is below the shoulder, above the elbow");
    Check(rig.hand_bones[1] == Bones{8}, "the right hand is the wrist's subtree");
    Check(rig.lower_bones[1] == Bones{7}, "the right forearm is below the elbow, above the wrist");
    Check(rig.upper_bones[1] == Bones{6}, "the right upper arm is below the shoulder, above the elbow");
}

void CheckLegs(const RigRegions& rig) {
    Check(rig.legs.size() == 2, "both legs are found");
    if (rig.legs.size() != 2) return;
    Check(rig.legs[0].parent == 0 && rig.legs[0].bones == Bones{11, 12}, "the left leg and its foot");
    Check(rig.legs[1].parent == 0 && rig.legs[1].bones == Bones{13}, "the right leg");
}

void CheckInvalid(const RigRegions& rig, const char* what) {
    Check(!rig.valid && rig.head == -1 && rig.spine == -1 && rig.arms[0] == std::array<int, 3>{-1, -1, -1} &&
              rig.arms[1] == std::array<int, 3>{-1, -1, -1} && rig.head_bones.empty() && rig.spine_bones.empty() &&
              rig.legs.empty() && rig.hand_bones[0].empty() && rig.visible_hand_bones[0].empty() &&
              rig.collapsed.empty(),
          what);
}

void TestClassifyRig() {
    const auto bones = Skeleton();
    const auto rig = ClassifyRig(bones);
    Check(rig.valid, "a complete rig is valid");
    Check(rig.head == 9, "face_1 is the head");
    Check(rig.spine == 1, "spin is the spine");
    Check(rig.head_bones == Bones{9, 10}, "the head's subtree");
    Check(rig.spine_bones == Bones{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, "the spine's subtree");
    CheckArmRegions(rig);
    Check(rig.visible_hand_bones[0] == Bones{4, 5}, "the left hand is drawn from the wrist");
    Check(rig.visible_hand_bones[1] == Bones{8}, "the right hand is drawn from the wrist");
    Check(rig.collapsed ==
              Collapsed{{0, -1}, {1, -1}, {2, 0}, {3, 0}, {6, 1}, {7, 1}, {9, -1}, {10, -1}, {11, -1}, {12, -1}, {13, -1}},
          "hands only collapses arm bones onto their arm and the rest onto the root");
    CheckLegs(rig);

    for (const char* head : {"head", "head1"}) {
        auto named = bones;
        named[9].name = head;
        Check(ClassifyRig(named).head == 9, "head and head1 are heads too");
    }

    // King Boo: no separable head, so his hands are drawn from the elbows.
    auto king_boo = bones;
    king_boo[9].name = "crown";
    const auto boo = ClassifyRig(king_boo);
    Check(boo.valid, "a rig without a head is still valid");
    Check(boo.head == -1 && boo.head_bones.empty(), "a rig without a head has no head bones");
    Check(boo.spine_bones == Bones{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, "the headless spine's subtree");
    CheckArmRegions(boo);
    Check(boo.visible_hand_bones[0] == Bones{3, 4, 5}, "the headless left hand is drawn from the elbow");
    Check(boo.visible_hand_bones[1] == Bones{7, 8}, "the headless right hand is drawn from the elbow");
    Check(boo.collapsed == Collapsed{{0, -1}, {1, -1}, {2, 0}, {6, 1}, {9, -1}, {10, -1}, {11, -1}, {12, -1}, {13, -1}},
          "a headless rig collapses everything above the elbows");
    CheckLegs(boo);

    auto no_spine = bones;
    no_spine[1].name = "chest";
    const auto chest = ClassifyRig(no_spine);
    Check(chest.valid && chest.spine == -1 && chest.spine_bones.empty(), "a rig without a spine has no spine bones");

    auto floating_leg = bones;
    floating_leg[13].parent = -1;
    const auto floating = ClassifyRig(floating_leg);
    Check(floating.valid && floating.legs.size() == 1 && floating.legs[0].bones == Bones{11, 12},
          "a leg without a parent is not a seated leg");

    auto broken_wrist = bones;
    broken_wrist[4].parent = 2;
    CheckInvalid(ClassifyRig(broken_wrist), "a wrist that does not hang from the elbow invalidates the rig");
    auto broken_elbow = bones;
    broken_elbow[7].parent = 1;
    CheckInvalid(ClassifyRig(broken_elbow), "an elbow that does not hang from the shoulder invalidates the rig");
    auto missing_arm = bones;
    missing_arm[6].name = "shoulder_r";
    CheckInvalid(ClassifyRig(missing_arm), "a missing shoulder invalidates the rig");
    auto missing_wrist = bones;
    missing_wrist[4].name = "hand_l";
    CheckInvalid(ClassifyRig(missing_wrist), "a missing wrist invalidates the rig");
    CheckInvalid(ClassifyRig({}), "an empty skeleton is not a rig");
}

} // namespace

int main() {
    TestLegAnimationGate();
    TestSeatedLimb();
    TestProfileKey();
    TestHandProfiles();
    TestReach();
    TestWristForPalm();
    TestMapSegment();
    TestAlign();
    TestRollAboutForearm();
    TestClassifyRig();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "vr body IK tests passed\n";
    return 0;
}
