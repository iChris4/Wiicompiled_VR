// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Character body IK, ported from heurazy's Wiicompiled_VR-PLUS fork
// (GPL-3.0-or-later): the player's own character drawn in the first-person
// cockpit, its arms reaching for the tracked controllers. The guest side is
// runtime/src/vr/character_body_ik.inl; everything here is plain math on the
// character's world matrices, tested headlessly (tests/vr_body_ik_tests.cpp).
// Positions and lengths are in the scaled world units of the character.

#include "vr/mkw_vr_first_person.h"

#include <cctype>
#include <string>
#include <vector>

namespace mkw::vr::body_ik {

using Vec = detail::Vec3;
inline Vec Add(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec Sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec Mul(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Length(Vec a) { return std::sqrt(detail::Dot(a, a)); }
inline Vec Position(const Mtx34& m) { return {m[3], m[7], m[11]}; }

// The hand-profile key of a driver model, from the name of its first
// vertex-position array ("mario_body__..." or "body__mario_body"). Toadette's
// first array is her hair, which names her body.
inline std::string ProfileKey(std::string name) {
    const auto delimiter = name.find("__");
    if (name.compare(0, 6, "body__") == 0) {
        name = name.substr(6);
    } else if (delimiter != std::string::npos) {
        name.resize(delimiter);
    }
    if (name == "k_hair_model") name = "kino_girl_body";
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    }
    return name;
}

// The controller grip tracks the palm, not the character's wrist joint. Rotate
// the wrist-to-palm offset with the hand so it works in every orientation.
inline Vec WristForPalm(Vec palm, const Mtx34& hand, Vec palm_axis, float distance) {
    auto axis = detail::TransformDirection(hand, palm_axis);
    if (!detail::Normalize(axis)) return palm;
    return Sub(palm, Mul(axis, distance));
}

// Driver animation IDs from the native 24-byte animation table: drive=0,
// dash=1, drift_l/r=2/3, wheelie=4, wait/back=5..7, steering=15..19.
// Both driver animation layers must be driving poses before exposing legs.
inline bool IsDrivingLegAnimation(uint16_t animation) {
    return animation == 0 || (animation >= 2 && animation <= 7) || (animation >= 15 && animation <= 19);
}

struct LegAnimationGate {
    uint64_t trick_hold_until = 0, turbo_hold_until = 0;
    bool AllowNative(bool trick, bool mini_turbo, bool drifting, uint64_t now, bool character_action = false) {
        // A new drift may start before the previous boost finishes. Keep its
        // leg articulation usable, but do not release a trick hold early.
        if (trick) trick_hold_until = now + 250000000ull;
        if (character_action || (mini_turbo && !drifting)) turbo_hold_until = now + 250000000ull;
        return !trick && !character_action && now >= trick_hold_until &&
               (drifting || (!mini_turbo && now >= turbo_hold_until));
    }
};

// Keep a little bend without forcing a long, tightly folded arm when the
// controller is close to the chest. Distances scale with the character/world.
inline float FitArmReach(float natural, float requested, float units_per_meter) {
    if (natural < 0.001f || units_per_meter <= 0) return 1.0f;
    return std::clamp(std::max(0.30f * units_per_meter, requested * 1.04f) / natural, 0.25f, 3.5f);
}

// Keep joint articulation relative to a stable seated parent. Native root
// translation/rotation (drift lean or a rider jump) cannot move the torso.
inline bool AnimateSeatedLimb(const Mtx34& seated_parent, const Mtx34& animated_parent, const Mtx34& animated_joint,
                              Mtx34& result) {
    Mtx34 inverse{};
    if (!InvertMtx(animated_parent, inverse)) return false;
    result = ComposeMtx(seated_parent, ComposeMtx(inverse, animated_joint));
    return detail::IsFiniteMtx34(result);
}

struct Arm {
    Vec elbow{}, wrist{};
    bool valid = false;
};

// Analytic two-bone IK, with a stable elbow pole and no bone stretching.
inline Arm Solve(Vec shoulder, Vec elbow, Vec wrist, Vec target, Vec pole) {
    const float upper = Length(Sub(elbow, shoulder)), lower = Length(Sub(wrist, elbow));
    if (!detail::IsFiniteFloat(&upper) || !detail::IsFiniteFloat(&lower) || upper < 0.001f || lower < 0.001f) return {};
    Vec axis = Sub(target, shoulder);
    float distance = Length(axis);
    if (!detail::IsFiniteFloat(&distance)) return {};
    if (!detail::Normalize(axis)) {
        axis = Sub(wrist, shoulder);
        if (!detail::Normalize(axis)) return {};
    }
    const float epsilon = std::min(upper, lower) * 0.001f;
    distance = std::clamp(distance, std::abs(upper - lower) + epsilon, upper + lower - epsilon);
    Vec bend = Sub(pole, Mul(axis, detail::Dot(pole, axis)));
    if (!detail::Normalize(bend)) {
        bend = Sub(Sub(elbow, shoulder), Mul(axis, detail::Dot(Sub(elbow, shoulder), axis)));
        if (!detail::Normalize(bend)) {
            bend = detail::Cross(axis, {0, 1, 0});
            if (!detail::Normalize(bend)) {
                bend = detail::Cross(axis, {1, 0, 0});
                detail::Normalize(bend);
            }
        }
    }
    const float along = (upper * upper - lower * lower + distance * distance) / (2 * distance);
    const float height = std::sqrt(std::max(0.0f, upper * upper - along * along));
    return {Add(shoulder, Add(Mul(axis, along), Mul(bend, height))), Add(shoulder, Mul(axis, distance)), true};
}

// Rotate about a joint to align its old segment to the solved one. Opposite
// vectors use a perpendicular axis rather than an undefined cross product.
inline Mtx34 Align(Vec origin, Vec from, Vec to) {
    if (!detail::Normalize(from) || !detail::Normalize(to)) return kIdentityMtx34;
    const float c = std::clamp(detail::Dot(from, to), -1.0f, 1.0f);
    Vec axis = detail::Cross(from, to);
    float s = Length(axis);
    if (s < 1e-5f) {
        if (c > 0) return kIdentityMtx34;
        axis = detail::Cross(from, {0, 1, 0});
        if (!detail::Normalize(axis)) {
            axis = detail::Cross(from, {1, 0, 0});
            detail::Normalize(axis);
        }
        s = 0;
    } else {
        axis = Mul(axis, 1 / s);
    }
    const float x = axis.x, y = axis.y, z = axis.z, k = 1 - c;
    Mtx34 m{x * x * k + c,     x * y * k - z * s, x * z * k + y * s, 0,
            y * x * k + z * s, y * y * k + c,     y * z * k - x * s, 0,
            z * x * k - y * s, z * y * k + x * s, z * z * k + c,     0};
    const auto rotated = detail::TransformDirection(m, origin);
    m[3] = origin.x - rotated.x;
    m[7] = origin.y - rotated.y;
    m[11] = origin.z - rotated.z;
    return m;
}

// Fit a skinned limb to a new segment, scaling its cross section by
// cross_section_scale (1 keeps the original thickness).
inline Mtx34 MapSegment(Vec old_origin, Vec new_origin, Vec old_segment, Vec new_segment,
                        float cross_section_scale = 1.0f) {
    const float old_length = Length(old_segment), new_length = Length(new_segment);
    if (old_length < 0.0001f || new_length < 0.0001f) return kIdentityMtx34;
    Vec axis = Mul(old_segment, 1 / old_length);
    const float k = new_length / old_length - cross_section_scale;
    Mtx34 stretch = kIdentityMtx34;
    stretch[0] = stretch[5] = stretch[10] = cross_section_scale;
    const float v[3]{axis.x, axis.y, axis.z};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) stretch[row * 4 + col] += k * v[row] * v[col];
    }
    const auto shifted = detail::TransformDirection(stretch, old_origin);
    stretch[3] = old_origin.x - shifted.x;
    stretch[7] = old_origin.y - shifted.y;
    stretch[11] = old_origin.z - shifted.z;
    auto result = ComposeMtx(Align(old_origin, old_segment, new_segment), stretch);
    const auto delta = Sub(new_origin, old_origin);
    result[3] += delta.x;
    result[7] += delta.y;
    result[11] += delta.z;
    return result;
}

// ---------------------------------------------------------------------------
// The skeleton's regions, worked out once per driver model so a frame touches
// each bone a fixed number of times instead of walking the tree per bone.
// ---------------------------------------------------------------------------

struct RigBone {
    int parent = -1;
    std::string name;
};

inline bool DescendsFrom(const std::vector<RigBone>& bones, int bone, int ancestor) {
    for (size_t steps = 0; steps < bones.size() && bone >= 0; ++steps) {
        if (bone == ancestor) return true;
        bone = bones[static_cast<size_t>(bone)].parent;
    }
    return false;
}

struct RigRegions {
    bool valid = false;
    // face_1/head/head1; -1 for a character whose head cannot be separated
    // (King Boo), which is then drawn as hands only.
    int head = -1;
    // "spin": the chest leans about it.
    int spine = -1;
    // Shoulder, elbow and wrist bones of the left and right arm.
    std::array<std::array<int, 3>, 2> arms{{{{-1, -1, -1}}, {{-1, -1, -1}}}};
    // Every list holds bone indices and includes its subtree's root.
    std::vector<int> head_bones;
    std::vector<int> spine_bones;
    struct Leg {
        int parent = -1;
        std::vector<int> bones;
    };
    // leg_l1 and leg_r1 with everything below them.
    std::vector<Leg> legs;
    // The arm's regions: below the wrist, below the elbow but not the wrist,
    // and below the shoulder but not the elbow.
    std::array<std::vector<int>, 2> hand_bones, lower_bones, upper_bones;
    // What is drawn as the hand: the wrist's subtree, or the elbow's when the
    // character has no separate head.
    std::array<std::vector<int>, 2> visible_hand_bones;
    // With hands only, every other bone collapses: onto its arm's wrist (0, 1)
    // when it belongs to an arm, otherwise onto the root bone (-1).
    std::vector<std::pair<int, int>> collapsed;
};

inline RigRegions ClassifyRig(const std::vector<RigBone>& bones) {
    RigRegions rig;
    static const char* const kArmNames[2][3] = {{"arm_l1", "arm_l2", "wrist_l1"}, {"arm_r1", "arm_r2", "wrist_r1"}};
    for (size_t i = 0; i < bones.size(); ++i) {
        const auto& name = bones[i].name;
        if (name == "face_1" || name == "head" || name == "head1") rig.head = static_cast<int>(i);
        if (name == "spin") rig.spine = static_cast<int>(i);
        for (int hand = 0; hand < 2; ++hand) {
            for (int joint = 0; joint < 3; ++joint) {
                if (name == kArmNames[hand][joint]) rig.arms[hand][joint] = static_cast<int>(i);
            }
        }
    }
    for (const auto& arm : rig.arms) {
        if (arm[0] < 0 || arm[1] < 0 || arm[2] < 0 || bones[static_cast<size_t>(arm[1])].parent != arm[0] ||
            bones[static_cast<size_t>(arm[2])].parent != arm[1]) {
            return {};
        }
    }
    const int count = static_cast<int>(bones.size());
    const int hand_root = rig.head < 0 ? 1 : 2;
    for (int i = 0; i < count; ++i) {
        if (rig.head >= 0 && DescendsFrom(bones, i, rig.head)) rig.head_bones.push_back(i);
        if (rig.spine >= 0 && DescendsFrom(bones, i, rig.spine)) rig.spine_bones.push_back(i);
        bool visible_hand = false;
        int pivot = -1;
        for (int hand = 0; hand < 2; ++hand) {
            const auto& arm = rig.arms[hand];
            if (DescendsFrom(bones, i, arm[2])) {
                rig.hand_bones[hand].push_back(i);
            } else if (DescendsFrom(bones, i, arm[1])) {
                rig.lower_bones[hand].push_back(i);
            } else if (DescendsFrom(bones, i, arm[0])) {
                rig.upper_bones[hand].push_back(i);
            }
            if (DescendsFrom(bones, i, arm[hand_root])) {
                rig.visible_hand_bones[hand].push_back(i);
                visible_hand = true;
            } else if (DescendsFrom(bones, i, arm[0])) {
                pivot = hand;
            }
        }
        if (!visible_hand) rig.collapsed.emplace_back(i, pivot);
    }
    for (int root = 0; root < count; ++root) {
        const auto& leg = bones[static_cast<size_t>(root)];
        if ((leg.name != "leg_l1" && leg.name != "leg_r1") || leg.parent < 0) continue;
        RigRegions::Leg region;
        region.parent = leg.parent;
        for (int i = 0; i < count; ++i) {
            if (DescendsFrom(bones, i, root)) region.bones.push_back(i);
        }
        rig.legs.push_back(std::move(region));
    }
    rig.valid = true;
    return rig;
}

} // namespace mkw::vr::body_ik
