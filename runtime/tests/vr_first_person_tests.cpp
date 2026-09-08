// SPDX-License-Identifier: GPL-3.0-or-later
//
// The first-person VR camera's transform, tested without a guest. Everything
// here exercises ComputeFirstPersonAnchor, which turns the game's own view and
// kart matrices into the relocation Aurora composes onto each eye.

#include "vr/mkw_vr_first_person.h"

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>

namespace {

using mkw::vr::ComputeFirstPersonAnchor;
using mkw::vr::kIdentityMtx34;
using mkw::vr::FirstPersonRotation;
using mkw::vr::Mtx34;

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

// out = matrix * (x, y, z, 1)
void Apply(const Mtx34& matrix, float x, float y, float z, float out[3]) {
    out[0] = matrix[0] * x + matrix[1] * y + matrix[2] * z + matrix[3];
    out[1] = matrix[4] * x + matrix[5] * y + matrix[6] * z + matrix[7];
    out[2] = matrix[8] * x + matrix[9] * y + matrix[10] * z + matrix[11];
}

// A view matrix for a camera at `eye` looking along -Z with no pitch or roll.
Mtx34 LevelViewAt(float x, float y, float z) {
    Mtx34 view = kIdentityMtx34;
    view[3] = -x;
    view[7] = -y;
    view[11] = -z;
    return view;
}

// The same, pitched down by `radians` about the view's X axis. Rows are the
// camera's axes in world space, which is what a world -> view matrix holds.
Mtx34 PitchedViewAt(float x, float y, float z, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mtx34 view{};
    view[0] = 1.0f;
    view[5] = c;
    view[6] = s;
    view[9] = -s;
    view[10] = c;
    view[3] = -(view[0] * x + view[1] * y + view[2] * z);
    view[7] = -(view[4] * x + view[5] * y + view[6] * z);
    view[11] = -(view[8] * x + view[9] * y + view[10] * z);
    return view;
}

Mtx34 KartAt(float x, float y, float z) {
    Mtx34 pose = kIdentityMtx34;
    pose[3] = x;
    pose[7] = y;
    pose[11] = z;
    return pose;
}

void TestNeutralInputsProduceIdentity() {
    Mtx34 anchor{};
    Check(ComputeFirstPersonAnchor(kIdentityMtx34, kIdentityMtx34, 0.0f, 0.0f, 0.0f,
                                   FirstPersonRotation::YawOnly, anchor),
          "a camera already at the head must produce an anchor");
    for (size_t i = 0; i < anchor.size(); ++i) {
        CheckNear(anchor[i], kIdentityMtx34[i], "neutral inputs must produce the identity anchor");
    }
}

void TestLevelCameraGivesPureTranslation() {
    // Camera 5 m behind and 2 m above the origin, kart at the origin, head 1 m up.
    // A camera that is already level needs no rotation, so the anchor reduces to
    // the translation and the head-placement math is visible on its own.
    const Mtx34 view = LevelViewAt(0.0f, 2.0f, 5.0f);
    const Mtx34 kart = KartAt(0.0f, 0.0f, 0.0f);
    Mtx34 anchor{};
    Check(ComputeFirstPersonAnchor(view, kart, 0.0f, 1.0f, 0.0f, FirstPersonRotation::YawOnly,
                                   anchor),
          "a level camera must produce an anchor");

    // The head sits at (0, -1, -5) in view space, so the anchor's translation
    // is its negation.
    CheckNear(anchor[3], 0.0f, "no lateral offset");
    CheckNear(anchor[7], 1.0f, "the anchor cancels the head's -1 view-space height");
    CheckNear(anchor[11], 5.0f, "the anchor cancels the head's -5 view-space depth");

    // Rotation untouched, so a world point keeps its orientation and only shifts.
    float moved[3];
    Apply(anchor, 0.0f, -1.0f, -5.0f, moved);
    CheckNear(moved[0], 0.0f, "the head lands at the eye origin (x)");
    CheckNear(moved[1], 0.0f, "the head lands at the eye origin (y)");
    CheckNear(moved[2], 0.0f, "the head lands at the eye origin (z)");
}

void TestLevellingRemovesCameraPitch() {
    // A chase camera looking down at the kart, which is the ordinary Mario Kart
    // Wii case: first person must not inherit that downward tilt.
    const float pitch = 0.35f;
    const Mtx34 view = PitchedViewAt(0.0f, 2.0f, 5.0f, pitch);
    const Mtx34 kart = KartAt(0.0f, 0.0f, 0.0f);
    Mtx34 anchor{};
    Check(ComputeFirstPersonAnchor(view, kart, 0.0f, 1.0f, 0.0f, FirstPersonRotation::YawOnly, anchor),
          "a pitched camera must still produce an anchor");

    // The anchored camera's axes, expressed in world space: rows of A_rot times
    // the view rotation. Its forward is -row2, and it must be horizontal.
    const float worldUp[3]{0.0f, 1.0f, 0.0f};
    float rowInWorld[3][3];
    for (size_t row = 0; row < 3; ++row) {
        for (size_t axis = 0; axis < 3; ++axis) {
            // view's rows are the camera axes in world space, so a view-space
            // vector returns to world space through view's transpose.
            rowInWorld[row][axis] = anchor[row * 4 + 0] * view[0 * 4 + axis] +
                                    anchor[row * 4 + 1] * view[1 * 4 + axis] +
                                    anchor[row * 4 + 2] * view[2 * 4 + axis];
        }
    }
    const float forwardDotUp = -(rowInWorld[2][0] * worldUp[0] + rowInWorld[2][1] * worldUp[1] +
                                 rowInWorld[2][2] * worldUp[2]);
    CheckNear(forwardDotUp, 0.0f, "the levelled forward axis must be horizontal");
    const float rightDotUp = rowInWorld[0][0] * worldUp[0] + rowInWorld[0][1] * worldUp[1] +
                             rowInWorld[0][2] * worldUp[2];
    CheckNear(rightDotUp, 0.0f, "the levelled right axis must be horizontal");
    const float upDotUp = rowInWorld[1][0] * worldUp[0] + rowInWorld[1][1] * worldUp[1] +
                          rowInWorld[1][2] * worldUp[2];
    CheckNear(upDotUp, 1.0f, "the levelled up axis must be world up");

    // The head still lands exactly at the eye origin.
    float head[3];
    Apply(view, 0.0f, 1.0f, 0.0f, head);
    float moved[3];
    Apply(anchor, head[0], head[1], head[2], moved);
    CheckNear(moved[0], 0.0f, "the head lands at the eye origin under levelling (x)");
    CheckNear(moved[1], 0.0f, "the head lands at the eye origin under levelling (y)");
    CheckNear(moved[2], 0.0f, "the head lands at the eye origin under levelling (z)");
}

void TestAnchorRotationStaysOrthonormal() {
    // Straight down at the kart: the camera's own forward projects to nothing on
    // the horizon plane, so the heading has to be recovered from its up axis.
    const float kHalfPi = 1.57079632679f;
    for (const float pitch : {0.0f, 0.35f, kHalfPi, -kHalfPi, 3.0f}) {
        const Mtx34 view = PitchedViewAt(3.0f, 12.0f, -7.0f, pitch);
        Mtx34 anchor{};
        Check(ComputeFirstPersonAnchor(view, KartAt(3.0f, 0.0f, -20.0f), 0.1f, 1.0f, 0.2f,
                                       FirstPersonRotation::YawOnly, anchor),
              "every camera pitch must produce an anchor");
        for (size_t row = 0; row < 3; ++row) {
            for (size_t other = row; other < 3; ++other) {
                float dot = 0.0f;
                for (size_t axis = 0; axis < 3; ++axis) {
                    dot += anchor[row * 4 + axis] * anchor[other * 4 + axis];
                }
                CheckNear(dot, row == other ? 1.0f : 0.0f,
                          "the anchor's rotation must stay orthonormal");
            }
        }
    }
}

void TestNonFiniteInputIsRejected() {
    Mtx34 broken = kIdentityMtx34;
    broken[3] = std::numeric_limits<float>::infinity();
    Mtx34 anchor = kIdentityMtx34;
    anchor[3] = 1234.0f;
    Check(!ComputeFirstPersonAnchor(broken, kIdentityMtx34, 0.0f, 1.0f, 0.0f, FirstPersonRotation::YawOnly, anchor),
          "a non-finite view matrix must be rejected");
    CheckNear(anchor[3], 1234.0f, "a rejected anchor must leave the output untouched");
}

void TestDegenerateKartPoseIsRejected() {
    Mtx34 collapsed{};
    Mtx34 anchor{};
    // A zeroed view matrix has no world up to level against.
    Check(!ComputeFirstPersonAnchor(collapsed, kIdentityMtx34, 0.0f, 1.0f, 0.0f, FirstPersonRotation::YawOnly, anchor),
          "a collapsed view matrix must be rejected");
}

// A kart pitched up by `pitch` and rolled by `roll`, heading toward -Z so it
// points away from a level camera. Only columns 1 and 2 are read by the anchor.
Mtx34 KartPitchedAndRolled(float pitch, float roll) {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    const float forward[3]{0.0f, sp, -cp};
    const float upUnrolled[3]{0.0f, cp, sp};
    const float rightUnrolled[3]{1.0f, 0.0f, 0.0f};
    Mtx34 pose{};
    for (size_t row = 0; row < 3; ++row) {
        pose[row * 4 + 1] = -rightUnrolled[row] * sr + upUnrolled[row] * cr;
        pose[row * 4 + 2] = forward[row];
    }
    return pose;
}

void TestYawPitchKeepsClimbAndDropsRoll() {
    const float pitch = 0.4f, roll = 0.5f;
    const Mtx34 view = LevelViewAt(0.0f, 2.0f, 5.0f);
    Mtx34 anchor{};
    Check(ComputeFirstPersonAnchor(view, KartPitchedAndRolled(pitch, roll), 0.0f, 0.0f, 0.0f,
                                   FirstPersonRotation::YawPitch, anchor),
          "yaw+pitch must be computable");

    // The climb survives: the anchor's forward is the kart's forward.
    CheckNear(-anchor[9], std::sin(pitch), "yaw+pitch keeps the kart's climb (y)");
    CheckNear(-anchor[10], -std::cos(pitch), "yaw+pitch keeps the kart's heading (z)");
    // The roll does not: the right axis stays horizontal.
    CheckNear(anchor[1], 0.0f, "yaw+pitch leaves the right axis horizontal");

    // Full rotation on the same kart does keep the roll, so the two differ.
    Mtx34 full{};
    Check(ComputeFirstPersonAnchor(view, KartPitchedAndRolled(pitch, roll), 0.0f, 0.0f, 0.0f,
                                   FirstPersonRotation::Full, full),
          "full rotation must be computable");
    Check(std::fabs(full[1]) > 0.1f, "full rotation keeps the roll yaw+pitch drops");

    for (size_t row = 0; row < 3; ++row) {
        for (size_t other = row; other < 3; ++other) {
            float dot = 0.0f;
            for (size_t axis = 0; axis < 3; ++axis) {
                dot += anchor[row * 4 + axis] * anchor[other * 4 + axis];
            }
            CheckNear(dot, row == other ? 1.0f : 0.0f, "yaw+pitch stays orthonormal");
        }
    }
}

// A kart yawed by `yaw` and rolled by `roll`, as a kart-local -> world pose.
Mtx34 KartOriented(float yaw, float roll) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cr = std::cos(roll), sr = std::sin(roll);
    // Columns are the kart's right, up and forward axes in world space.
    const float right[3]{cy * cr, sr, -sy * cr};
    const float up[3]{-cy * sr, cr, sy * sr};
    const float forward[3]{sy, 0.0f, cy};
    Mtx34 pose{};
    for (size_t row = 0; row < 3; ++row) {
        pose[row * 4 + 0] = right[row];
        pose[row * 4 + 1] = up[row];
        pose[row * 4 + 2] = forward[row];
    }
    return pose;
}

void TestFullRotationFollowsTheKart() {
    // A level camera, and a kart yawed and rolled away from it. Full rotation
    // must adopt the kart's frame, not the camera's.
    const Mtx34 view = LevelViewAt(0.0f, 2.0f, 5.0f);
    const float yaw = 0.6f, roll = 0.4f;
    Mtx34 anchor{};
    Check(ComputeFirstPersonAnchor(view, KartOriented(yaw, roll), 0.0f, 0.0f, 0.0f,
                                   FirstPersonRotation::Full, anchor),
          "full rotation must be computable");

    // With an identity view rotation the anchor rows are the kart's axes
    // directly, so the third row is the kart's backward axis.
    CheckNear(anchor[8], -std::sin(yaw), "full rotation takes the kart's heading (x)");
    CheckNear(anchor[10], -std::cos(yaw), "full rotation takes the kart's heading (z)");
    // Roll survives: the anchor's up is the kart's up, not world up.
    CheckNear(anchor[5], std::cos(roll), "full rotation keeps the kart's roll");

    for (size_t row = 0; row < 3; ++row) {
        for (size_t other = row; other < 3; ++other) {
            float dot = 0.0f;
            for (size_t axis = 0; axis < 3; ++axis) {
                dot += anchor[row * 4 + axis] * anchor[other * 4 + axis];
            }
            CheckNear(dot, row == other ? 1.0f : 0.0f, "full rotation stays orthonormal");
        }
    }
}

void TestYawOnlyIgnoresKartRoll() {
    // The same rolled kart, but yaw-only must leave the horizon level.
    const Mtx34 view = LevelViewAt(0.0f, 2.0f, 5.0f);
    Mtx34 rolled{};
    Mtx34 upright{};
    Check(ComputeFirstPersonAnchor(view, KartOriented(0.6f, 0.4f), 0.0f, 0.0f, 0.0f,
                                   FirstPersonRotation::YawOnly, rolled),
          "yaw-only must be computable for a rolled kart");
    Check(ComputeFirstPersonAnchor(view, KartOriented(0.6f, 0.0f), 0.0f, 0.0f, 0.0f,
                                   FirstPersonRotation::YawOnly, upright),
          "yaw-only must be computable for an upright kart");
    for (size_t i = 0; i < 3; ++i) {
        CheckNear(rolled[4 + i], upright[4 + i], "yaw-only ignores the kart's roll");
    }
    CheckNear(rolled[5], 1.0f, "yaw-only keeps the horizon level");
}

} // namespace

int main() {
    TestNeutralInputsProduceIdentity();
    TestLevelCameraGivesPureTranslation();
    TestLevellingRemovesCameraPitch();
    TestAnchorRotationStaysOrthonormal();
    TestFullRotationFollowsTheKart();
    TestYawPitchKeepsClimbAndDropsRoll();
    TestYawOnlyIgnoresKartRoll();
    TestNonFiniteInputIsRejected();
    TestDegenerateKartPoseIsRejected();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
