// SPDX-License-Identifier: GPL-3.0-or-later
//
// The first-person cockpit's pure geometry, tested without a guest: where the
// seated eye comes from, how the vehicle's wheel and handlebar land in the
// seated frame, the level seat through spins, and which of the vehicle's own
// vertices the wheel animation turns. The math is ported from heurazy's
// mario-kart-wii-VR-port.

#include "vr/cockpit_stabilizer.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/native_wheel_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using namespace mkw::vr;

int g_failures = 0;

void Check(bool condition, const char *what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

void CheckNear(float actual, float expected, const char *what, float tolerance = 1.0e-3f) {
    if (!(std::fabs(actual - expected) <= tolerance)) {
        ++g_failures;
        std::cerr << "FAILED: " << what << " (expected " << expected << ", got " << actual << ")\n";
    }
}

Mtx34 Translation(float x, float y, float z) {
    Mtx34 m = kIdentityMtx34;
    m[3] = x;
    m[7] = y;
    m[11] = z;
    return m;
}

Mtx34 YawAt(float yaw, float x, float y, float z) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {c, 0, s, x, 0, 1, 0, y, -s, 0, c, z};
}

void TestMatrixHelpers() {
    const Mtx34 a = YawAt(0.7f, 1.0f, 2.0f, 3.0f);
    Mtx34 inverse{};
    Check(InvertMtx(a, inverse), "a rigid transform inverts");
    const Mtx34 identity = ComposeMtx(a, inverse);
    for (int i = 0; i < 12; ++i) {
        CheckNear(identity[i], kIdentityMtx34[i], "a * inverse(a) is identity", 1e-5f);
    }
    Mtx34 singular{};
    Check(!InvertMtx(singular, inverse), "a singular matrix does not invert");
    const Mtx34 scaled = ScaleModelBasis(kIdentityMtx34, {2.0f, 3.0f, 4.0f});
    CheckNear(scaled[0], 2.0f, "basis X scaled");
    CheckNear(scaled[5], 3.0f, "basis Y scaled");
    CheckNear(scaled[10], 4.0f, "basis Z scaled");
    CheckNear(scaled[3], 0.0f, "translation untouched");
}

void TestSeatHelpers() {
    CheckNear(CharacterCockpitScale(80.0f), 1.0f, "short characters keep the base scale");
    CheckNear(CharacterCockpitScale(150.0f), 1.5f, "tall characters grow the scale with eye height");
    CheckNear(CharacterCockpitScale(1000.0f), 2.5f, "the scale is capped");
    CheckNear(CharacterCockpitScale(std::nanf("")), 1.0f, "a bad eye height keeps the base scale");
    CheckNear(ValidPlayerScale(2.0f), 2.0f, "mega mushroom scale kept");
    CheckNear(ValidPlayerScale(0.0f), 1.0f, "an implausible scale is ignored");
    Check(NeutralPlayerScale({1.0f, 1.0f, 1.0f}), "unit scale is neutral");
    Check(!NeutralPlayerScale({0.5f, 0.5f, 0.5f}), "lightning scale is not neutral");
    // 100 units per metre, controls 60 units ahead: the eye stays at least 0.45 m behind.
    CheckNear(EyeBehindControls(50.0f, 60.0f, 100.0f, 0.0f), 60.0f - 45.0f, "eye pulled behind the wheel");
    CheckNear(EyeBehindControls(50.0f, 60.0f, 100.0f, 50.0f), 60.0f - 55.0f, "a wider wheel keeps more clearance");
    CheckNear(EyeBehindControls(-10.0f, 60.0f, 100.0f, 18.0f), -10.0f, "an eye already behind stays put");
    CheckNear(EyeAboveControls(69.0f, 37.0f, 100.0f), 69.0f, "normal seated height is unchanged");
    CheckNear(EyeAboveControls(126.0f, 58.0f, 126.0f), 108.4f, "long neck does not put the wheel at knee height");
    CheckNear(EyeAboveControls(180.0f, 45.0f, 180.0f), 117.0f, "tall driver's reach uses character scale");
    CheckNear(EyeAboveControls(69.0f, NAN, 100.0f), 69.0f, "invalid controls leave the eye alone");
    CheckNear(EyeAboveControls(69.0f, -100.0f, 100.0f), 69.0f, "bad controls cannot push the eye below the seat");
}

void TestDriverEye() {
    std::array<float, 3> eye{};
    // Face bone at (0, 80, 10) in the character, placed 5 units up in the vehicle.
    const Mtx34 face = Translation(0.0f, 80.0f, 10.0f);
    const Mtx34 placement = Translation(0.0f, 5.0f, 0.0f);
    Check(ComputeDriverEyeFromBounds(face, placement, {-2, 8, 0}, {2, 12, 4}, eye), "eye from bounds");
    CheckNear(eye[1], 95.0f, "bounds centre through bind and placement (up)");
    CheckNear(eye[2], 12.0f, "bounds centre through bind and placement (forward)");
    Check(!ComputeDriverEyeFromBounds(face, placement, {2, 8, 0}, {-2, 12, 4}, eye), "inverted bounds rejected");
    Check(!ComputeDriverEyeFromBounds(Translation(0, -50, 0), placement, {0, 0, 0}, {1, 1, 1}, eye),
          "an eye below the seat is rejected");
    CheckNear(eye[1], 95.0f, "a rejected measurement cannot corrupt the fallback height");

    // The same eye through the animated world matrices: the body's own motion
    // must not leak into the seat.
    const Mtx34 body = YawAt(1.2f, 500.0f, 20.0f, -300.0f);
    const Mtx34 faceWorld = ComposeMtx(body, Translation(0.0f, 90.0f, 15.0f));
    Check(ComputeSeatedEye(faceWorld, body, {0, 0, 0}, eye), "seated eye from world matrices");
    CheckNear(eye[0], 0.0f, "seated eye right", 1e-3f);
    CheckNear(eye[1], 90.0f, "seated eye up", 1e-3f);
    CheckNear(eye[2], 15.0f, "seated eye forward", 1e-3f);

    // Custom models may have no _eye group and a small/rotated bind head with
    // a large negative bike placement. Only the evaluated head includes the
    // animation's scale and riding posture. Do not apply placement twice.
    Mtx34 customHead{0, 0, 1, 0, 0, -1, 0, 60, 1, 0, 0, 12};
    Check(ComputeDriverEyeFromHead(ComposeMtx(body, customHead), body, eye), "custom animated head fallback");
    CheckNear(eye[1], 68.0f, "custom head fallback stays above the bike");
    CheckNear(eye[2], 20.0f, "fallback offset uses vehicle axes despite rotated head");
    Check(!ComputeDriverEyeFromHead(ComposeMtx(body, Translation(0, -20, 0)), body, eye),
          "an invalid head is rejected");
    CheckNear(eye[1], 68.0f, "failed head fallback leaves caller output intact");

    SeatedEyeReference reference;
    for (int i = 0; i < 7; ++i) {
        reference.Observe({0, 90, 15}, true, true);
    }
    Check(!reference.valid, "seven samples are not enough");
    reference.Observe({0, 90, 15}, true, true);
    Check(reference.valid, "eight stable samples calibrate the seat");
    reference.Observe({0, 200, 15}, true, true);
    CheckNear(reference.value[1], 90.0f, "a calibrated seat is frozen");
    for (int i = 0; i < 8; ++i) {
        reference.Observe({0, 75, -60}, true, true);
    }
    CheckNear(reference.value[2], 15.0f, "a stable later pose cannot move a calibrated seat");
    SeatedEyeReference drifting;
    for (int i = 0; i < 30; ++i) {
        drifting.Observe({0, 60.0f + float(i), 15}, true, true);
    }
    Check(!drifting.valid, "a slowly changing start animation is not a stable seat");
    for (int i = 0; i < 7; ++i) {
        drifting.Observe({0, 100, 15}, true, true);
    }
    drifting.Observe({0, NAN, 15}, true, true);
    drifting.Observe({0, 100, 15}, true, true);
    Check(!drifting.valid, "invalid samples break calibration continuity");
    SeatedEyeReference interrupted;
    for (int i = 0; i < 5; ++i) {
        interrupted.Observe({0, 90, 15}, true, true);
    }
    interrupted.Observe({0, 90, 15}, false, true);
    for (int i = 0; i < 5; ++i) {
        interrupted.Observe({0, 90, 15}, true, true);
    }
    Check(!interrupted.valid, "an unsafe sample restarts calibration");
}

void TestWheelGeometry() {
    // Grip targets 20 units either side of a wheel 60 units ahead and 50 up,
    // 100 units per metre, seat frame = the vehicle frame turned to face -Z
    // (vehicle +Z forward, +X to the driver's left).
    const Mtx34 seatFromBody{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0};
    const WheelGeometry wheel = ComputeNativeWheelGeometry(seatFromBody, {20, 50, 60}, {-20, 50, 60}, 100.0f);
    Check(wheel.valid, "wheel geometry from the grip targets");
    CheckNear(wheel.radius, 0.2f, "radius is half the grip span");
    CheckNear(wheel.center[1], 0.5f, "centre height in metres");
    CheckNear(wheel.center[2], -0.6f, "centre ahead in metres");
    CheckNear(wheel.right[0], 1.0f, "wheel right is the seated right");
    CheckNear(wheel.up[1], 1.0f, "wheel up is the vehicle's up");
    const auto swapped = ComputeNativeWheelGeometry(seatFromBody, {-20, 50, 60}, {20, 50, 60}, 100.0f);
    CheckNear(swapped.right[0], wheel.right[0], "grip order does not flip the wheel");
    Check(!ComputeNativeWheelGeometry(seatFromBody, {1, 50, 60}, {-1, 50, 60}, 100.0f).valid,
          "a wheel narrower than 4 cm is rejected");

    // A hand on the right of the rim maps onto the wheel's rim at angle zero.
    WheelHand hand{wheel.center[0] + 0.2f, wheel.center[1], wheel.center[2], 1.0f, true};
    const WheelHand local = wheel.ToWheel(hand);
    CheckNear(local.x, 0.2f, "right rim point is +radius along the wheel");
    CheckNear(local.y, SteeringWheel::Height, "wheel-local height matches the synthetic wheel");
    CheckNear(local.z, SteeringWheel::Depth, "wheel-local depth matches the synthetic wheel");

    // Handlebar: position from the (steered) handle, axes from the neutral body.
    const float steer = 0.4f, c = std::cos(steer), s = std::sin(steer);
    const Mtx34 steeredHandle{-c, 0, -s, 0, 0, 1, 0, 0, s, 0, -c, 0};
    const auto bar = ComputeNativeHandlebarGeometry(steeredHandle, seatFromBody, {20, 50, 60}, {-20, 50, 60}, 100.0f);
    Check(bar.valid, "handlebar geometry");
    CheckNear(bar.right[0], 1.0f, "handlebar axes ignore the steering already applied");
}

void TestStabilizer() {
    CockpitStabilizer stabilizer;
    const Mtx34 start = YawAt(0.5f, 10, 0, 20);
    auto seat = stabilizer.Update(start, false, 1.0f / 60.0f);
    CheckNear(seat[3], 10.0f, "position followed");
    CheckNear(std::atan2(seat[2], seat[10]), 0.5f, "heading followed");
    // Damage spins the chassis; the seat holds its heading but keeps position.
    seat = stabilizer.Update(YawAt(2.5f, 12, 0, 21), true, 1.0f / 60.0f);
    CheckNear(seat[3], 12.0f, "position exact while damaged");
    CheckNear(std::atan2(seat[2], seat[10]), 0.5f, "heading held while damaged");
    // Recovery eases back onto the real heading.
    for (int i = 0; i < 120; ++i) {
        seat = stabilizer.Update(YawAt(0.8f, 12, 0, 21), false, 1.0f / 60.0f);
    }
    CheckNear(std::atan2(seat[2], seat[10]), 0.8f, "heading recovered after damage", 5e-3f);
    CheckNear(seat[5], 1.0f, "the seat is always level");
}

void TestNativeWheelVertices() {
    // A 64-point disc of radius 20 in the vehicle's X/Y plane at z = 60, centred
    // at y = 50, plus two far vertices (the chassis) that must never move.
    std::vector<detail::Vec3> points;
    for (int i = 0; i < 64; ++i) {
        const float a = float(i) * 6.2831853f / 64.0f;
        points.push_back({20.0f * std::cos(a), 50.0f + 20.0f * std::sin(a), 60.0f});
    }
    points.push_back({100.0f, 0.0f, 0.0f});
    points.push_back({0.0f, 50.0f, 200.0f});
    const auto original = points;
    NativeWheelTopology topology(points.size());
    for (uint32_t i = 2; i < 64; ++i)
        topology.Triangle(0, i - 1, i);
    const unsigned changed = RotateNativeWheelVertices(points, topology, {0, 50, 60}, 20.0f, 0.5f);
    Check(changed == 64, "every disc vertex turns");
    CheckNear(points[64].x, original[64].x, "chassis vertex untouched");
    CheckNear(points[65].z, original[65].z, "vertex off the disc plane untouched");
    // Rotation keeps each disc point on the rim.
    for (int i = 0; i < 64; ++i) {
        CheckNear(std::hypot(points[i].x, points[i].y - 50.0f), 20.0f, "disc vertex stays on the rim", 1e-2f);
    }
    auto sparse = std::vector<detail::Vec3>(points.begin(), points.begin() + 4);
    NativeWheelTopology sparseTopology(sparse.size());
    Check(RotateNativeWheelVertices(sparse, sparseTopology, {0, 50, 60}, 20.0f, 0.5f) == 0,
          "too few candidates leaves the mesh");
    Check(RotateNativeWheelVertices(points, topology, {0, 50, 60}, 2.0f, 0.5f) == 0,
          "an implausible radius leaves the mesh");
}

void TestNativeWheelWithRaisedGrips() {
    // The same tilted wheel is gripped near its centre by one driver and near
    // its upper rim by another. Uneven spoke density must not move the pivot.
    constexpr float radius = 17.0f, slope = 0.3f;
    const float inv = 1.0f / std::sqrt(1.0f + slope * slope);
    const detail::Vec3 center{0, 28, -9};
    std::vector<detail::Vec3> original;
    for (int i = 0; i < 64; ++i) {
        const float a = float(i) * 6.2831853f / 64.0f;
        const float y = radius * std::sin(a);
        original.push_back({radius * std::cos(a), center.y + inv * y, center.z + slope * inv * y});
    }
    for (int i = 0; i < 12; ++i) {
        original.push_back({float(i % 3) - 1.0f, center.y + 5.0f, center.z + slope * 5.0f});
    }
    const auto wheelCount = original.size();
    // Inside the broad search box, but off the wheel plane: the chassis must
    // neither bias the fit nor be pulled along with the wheel.
    original.push_back({-18.0f, 8.0f, 6.0f});
    original.push_back(center); // A chassis triangle crosses the wheel volume.
    original.push_back({18.0f, 9.0f, 6.0f});
    NativeWheelTopology topology(original.size());
    for (uint32_t i = 2; i < 64; ++i)
        topology.Triangle(0, i - 1, i);
    for (uint32_t i = 66; i < wheelCount; ++i)
        topology.Triangle(64, i - 1, i);
    topology.Triangle(wheelCount, wheelCount + 1, wheelCount + 2);
    for (float angle : {-0.7f, 0.7f}) {
        auto lowerGrip = original, raisedGrip = original;
        Check(RotateNativeWheelVertices(lowerGrip, topology, {0, 27, -5}, 13.0f, angle) == wheelCount,
              "all wheel vertices turn with lower grips");
        Check(RotateNativeWheelVertices(raisedGrip, topology, {0, 36.7f, -5.9f}, 13.0f, angle) == wheelCount,
              "raised grips still turn the entire lower rim");
        for (size_t i = 0; i < wheelCount; ++i) {
            CheckNear(raisedGrip[i].x, lowerGrip[i].x, "driver hand height does not change wheel rotation X");
            CheckNear(raisedGrip[i].y, lowerGrip[i].y, "driver hand height does not change wheel rotation Y");
            CheckNear(raisedGrip[i].z, lowerGrip[i].z, "driver hand height does not change wheel rotation Z");
            const float x = original[i].x, y = (original[i].y - center.y) / inv;
            const float rx = std::cos(angle) * x - std::sin(angle) * y;
            const float ry = std::sin(angle) * x + std::cos(angle) * y;
            CheckNear(raisedGrip[i].x, rx, "wheel rotates rigidly about its geometric centre X");
            CheckNear(raisedGrip[i].y, center.y + inv * ry, "wheel rotates rigidly about its geometric centre Y");
            CheckNear(raisedGrip[i].z, center.z + slope * inv * ry, "wheel rotates rigidly in its tilted plane");
        }
        for (size_t i = wheelCount; i < original.size(); ++i) {
            CheckNear(raisedGrip[i].x, original[i].x, "nearby chassis X untouched");
            CheckNear(raisedGrip[i].y, original[i].y, "nearby chassis Y untouched");
            CheckNear(raisedGrip[i].z, original[i].z, "nearby chassis Z untouched");
        }
    }
    auto corrected = original;
    const auto correction = Translation(2, 3, 4);
    Check(RotateNativeWheelVertices(corrected, topology, {0, 36.7f, -5.9f}, 13.0f, 0.0f, &correction) == wheelCount,
          "the complete wheel also receives cockpit stabilization");
    for (size_t i = 0; i < wheelCount; ++i) {
        CheckNear(corrected[i].y, original[i].y + 3.0f, "lower rim receives body correction");
    }
    for (size_t i = wheelCount; i < original.size(); ++i) {
        CheckNear(corrected[i].y, original[i].y, "chassis does not receive wheel stabilization");
    }
    auto narrowGrip = original;
    Check(RotateNativeWheelVertices(narrowGrip, topology, {0, 36.7f, -5.9f}, 9.0f, 0.7f) == wheelCount,
          "hands inside a wide rim still select the entire wheel");

    // Baby Booster's root exchanges the authored lateral/vertical axes.
    const Mtx34 bodyFromVertices{0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0};
    Mtx34 verticesFromBody;
    Check(InvertMtx(bodyFromVertices, verticesFromBody), "authored body basis is invertible");
    auto authored = original, expected = original;
    for (auto &p : authored)
        p = detail::TransformPoint(verticesFromBody, p.x, p.y, p.z);
    Check(RotateNativeWheelVertices(authored, topology, {0, 36.7f, -5.9f}, 13.0f, 0.7f, &correction,
                                    bodyFromVertices) == wheelCount,
          "a rotated root bone does not hide the wheel");
    RotateNativeWheelVertices(expected, topology, {0, 36.7f, -5.9f}, 13.0f, 0.7f, &correction);
    for (size_t i = 0; i < original.size(); ++i) {
        const auto p = detail::TransformPoint(bodyFromVertices, authored[i].x, authored[i].y, authored[i].z);
        CheckNear(p.x, expected[i].x, "authored basis preserves rotation and stabilization X");
        CheckNear(p.y, expected[i].y, "authored basis preserves rotation and stabilization Y");
        CheckNear(p.z, expected[i].z, "authored basis preserves rotation and stabilization Z");
    }
    auto domed = original;
    for (size_t i = 64; i < wheelCount; ++i) {
        domed[i].y -= slope * inv * radius * 0.37f;
        domed[i].z += inv * radius * 0.37f;
    }
    Check(RotateNativeWheelVertices(domed, topology, {0, 36.7f, -5.9f}, 13.0f, 0.7f) == wheelCount,
          "a domed hub turns with the rim");
    topology.rootOwned[0] = false;
    auto foreignJoint = original;
    Check(RotateNativeWheelVertices(foreignJoint, topology, {0, 36.7f, -5.9f}, 13.0f, 0.7f) == 0,
          "geometry on another animated joint cannot be mistaken for the wheel");
}

void TestNativeWheelTopology() {
    const uint8_t strip[]{0x98, 0, 8, 0, 1, 2, 2, 3, 3, 4, 5};
    NativeWheelTopology topology(6);
    Check(topology.AddPrimitives(strip, sizeof(strip), 2u << 9, 0), "decode an indexed strip");
    Check(topology.Root(0) == topology.Root(2) && topology.Root(3) == topology.Root(5),
          "strip triangles connect their positions");
    Check(topology.Root(0) != topology.Root(3), "degenerate strip connectors do not join pieces");
    Check(!topology.AddPrimitives(strip, sizeof(strip) - 1, 2u << 9, 0), "truncated primitive rejected");
    Check(!topology.AddPrimitives(strip, sizeof(strip), 1u << 9, 0), "unsupported direct positions rejected");
    NativeWheelTopology tooSmall(5);
    Check(!tooSmall.AddPrimitives(strip, sizeof(strip), 2u << 9, 0), "out-of-range position rejected");
    const uint8_t quads[]{0x80, 0, 4, 0, 0, 0, 1, 0, 2, 0, 3};
    NativeWheelTopology quad(4);
    Check(quad.AddPrimitives(quads, sizeof(quads), 3u << 9, 0) && quad.Root(0) == quad.Root(3),
          "16-bit quad positions connect both triangles");
    const uint8_t indexed[]{0x20, 0, 0, 0xb0, 0, 0x20, 0, 1, 0xb0, 12, 0x90, 0, 3, 0, 0, 0, 1, 3, 2};
    NativeWheelTopology joints(3);
    Check(joints.AddPrimitives(indexed, sizeof(indexed), (2u << 9) | 1u, 0), "decode indexed bone ownership");
    Check(joints.rootOwned[0] && joints.rootOwned[1] && !joints.rootOwned[2],
          "matrix loads distinguish the body from an animated child joint");

    // Minimal MDL0 exercising shape offsets, array IDs and bounds without game assets.
    std::vector<uint8_t> mdl(320, 0);
    const auto put32 = [&](size_t at, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            mdl[at + i] = uint8_t(value >> ((3 - i) * 8));
    };
    put32(0, 0x4d444c30);
    put32(4, uint32_t(mdl.size()));
    put32(8, 11);
    put32(0x38, 64);
    put32(68, 1);
    put32(100, 40);
    constexpr size_t shape = 104;
    put32(shape + 0x0c, 3u << 9);
    put32(shape + 0x28, sizeof(quads));
    put32(shape + 0x2c, 256 - (shape + 0x24));
    std::copy(std::begin(quads), std::end(quads), mdl.begin() + 256);
    NativeWheelTopology model(4);
    Check(ReadNativeWheelTopology(mdl.data(), mdl.size(), 0, model), "MDL0 shape topology decoded");
    Check(!ReadNativeWheelTopology(mdl.data(), mdl.size(), 1, model), "unrelated position array ignored");
    put32(shape + 0x2c, UINT32_MAX);
    Check(!ReadNativeWheelTopology(mdl.data(), mdl.size(), 0, model), "escaping primitive offset rejected");
    // The bone the wheel's positions are drawn through is found by node id:
    // the Flame Flyer and Cheep Charger list an nw4r_root bone (node 2)
    // before the node-0 bone, which used to leave them on the VR wheel.
    std::vector<uint8_t> bones(0x1c0, 0);
    const auto putBone32 = [&](size_t at, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            bones[at + i] = uint8_t(value >> ((3 - i) * 8));
    };
    const auto putBoneFloat = [&](size_t at, float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        putBone32(at, bits);
    };
    putBone32(0, 0x4d444c30);
    putBone32(4, uint32_t(bones.size()));
    putBone32(8, 11);
    putBone32(0x14, 0x40);
    putBone32(0x44, 2);
    putBone32(0x40 + 8 + 16 + 12, 0x40);
    putBone32(0x40 + 8 + 32 + 12, 0xe0);
    putBone32(0x80 + 0x10, 2);
    putBone32(0x120 + 0x10, 0);
    const float authored[12]{0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0};
    for (unsigned i = 0; i < 12; ++i) {
        putBoneFloat(0x80 + 0x70 + i * 4, i % 5 == 0 ? 1.0f : 0.0f);
        putBoneFloat(0x120 + 0x70 + i * 4, authored[i]);
    }
    float bone[12]{};
    Check(ReadNativeWheelNodeMatrix(bones.data(), bones.size(), 0, bone) &&
              std::equal(std::begin(authored), std::end(authored), bone),
          "node-0 bone found behind an nw4r_root bone");
    Check(ReadNativeWheelNodeMatrix(bones.data(), bones.size(), 2, bone) && bone[0] == 1.0f && bone[2] == 0.0f,
          "bones are told apart by node id");
    Check(!ReadNativeWheelNodeMatrix(bones.data(), bones.size(), 5, bone), "missing node rejected");
    Check(!ReadNativeWheelNodeMatrix(bones.data(), bones.size() - 1, 0, bone), "MDL0 size mismatch rejected");
    putBoneFloat(0x120 + 0x70, NAN);
    Check(!ReadNativeWheelNodeMatrix(bones.data(), bones.size(), 0, bone), "non-finite bone matrix rejected");
    putBone32(0x40 + 8 + 32 + 12, uint32_t(bones.size()));
    Check(!ReadNativeWheelNodeMatrix(bones.data(), bones.size(), 0, bone), "escaping bone offset rejected");
}

} // namespace

int main() {
    TestMatrixHelpers();
    TestSeatHelpers();
    TestDriverEye();
    TestWheelGeometry();
    TestStabilizer();
    TestNativeWheelVertices();
    TestNativeWheelWithRaisedGrips();
    TestNativeWheelTopology();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "vr cockpit tests passed\n";
    return 0;
}
