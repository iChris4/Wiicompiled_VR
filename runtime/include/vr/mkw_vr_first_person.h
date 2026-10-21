// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vr/steering_wheel.h"
#include "vr/mkw_vr_item.h"
#include "vr/vr_controls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mkw::vr {

// A row-major affine 3x4, the same shape and convention as an NW4R/GX Mtx and
// as Aurora's Mat3x4: a point is transformed as out = M * (p, 1).
using Mtx34 = std::array<float, 12>;

inline constexpr Mtx34 kIdentityMtx34{
    1.0f, 0.0f, 0.0f, 0.0f, //
    0.0f, 1.0f, 0.0f, 0.0f, //
    0.0f, 0.0f, 1.0f, 0.0f,
};

// Where the driver's head sits in the kart's own frame, in metres. The kart
// frame is the EGG convention: +x right, +y up, +z forward.
struct FirstPersonHeadOffsets {
    float right = 0.0f;
    float up = 3.0f;
    float forward = 0.0f;
};

// Where the anchored camera's orientation comes from, mirroring DolphinXR's
// camera-anchor modes. The headset always adds free look on top of whichever
// is chosen; this only decides the frame it looks around from.
enum class FirstPersonRotation : uint8_t {
    // The horizon is kept level and only a heading is taken. Comfort default.
    YawOnly,
    // The kart's heading and its climb, with roll dropped: slopes and wheelies
    // tip the view, but a banked corner never rolls the horizon.
    YawPitch,
    // The kart's whole orientation, so the view banks and pitches with it.
    Full,
};

// Where the first-person head is placed.
enum class FirstPersonSeat : uint8_t {
    // At the driver's own eyes, measured from the character's model and kept
    // behind the steering wheel, at a life-size cockpit scale. The wheel or
    // handlebar is then within reach of the player's hands.
    Cockpit,
    // The free first_person_head_*_meters offsets at first_person_units_per_meter.
    Custom,
};

// The camera relocation published to Aurora for one guest frame: a transform
// from the game's recorded view space into the space the headset renders from.
struct FirstPersonAnchor {
    Mtx34 anchor_from_scene = kIdentityMtx34;
    bool valid = false;
    uint64_t guest_frame_index = 0;
    // The rest describes the cockpit seat and is left empty by the custom seat.
    bool cockpit = false;
    // World units per metre the anchor was built with (character and player
    // scale included).
    float units_per_meter = 0.0f;
    // The vehicle's steering wheel or handlebar, in metres in the seated frame
    // (+X right, +Y up, -Z forward, origin at the head).
    WheelGeometry native_wheel{};
    bool bike = false;
    // The vehicle's own wheel is being animated in the scene this frame.
    bool native_mesh_prepared = false;
    // Changes whenever the player's vehicle object does.
    uint64_t vehicle_identity = 0;
    // The steering the vehicle turns with this frame, -1..1 with positive to
    // the right, from whatever controller the game reads (mirrored in mirror
    // cups, like the scene).
    bool steering_valid = false;
    float steering = 0.0f;
};

// ---------------------------------------------------------------------------
// Pure math. Header-only and free of guest access, so it is directly testable.
// ---------------------------------------------------------------------------

namespace detail {

inline constexpr float kAnchorEpsilon = 1.0e-6f;

inline bool IsFiniteFloat(const float* value) noexcept {
    // The runtime is built with -ffast-math, which permits the compiler to fold
    // std::isfinite to true. Inspect the object representation instead, the way
    // the presentation policy validates its own floats.
    uint32_t bits = 0;
    std::memcpy(&bits, value, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

inline bool IsFiniteMtx34(const Mtx34& value) noexcept {
    for (const float& element : value) {
        if (!IsFiniteFloat(&element)) {
            return false;
        }
    }
    return true;
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline float Dot(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline bool Normalize(Vec3& value) noexcept {
    const float length_squared = Dot(value, value);
    if (!IsFiniteFloat(&length_squared) || !(length_squared > kAnchorEpsilon)) {
        return false;
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    return true;
}

// out = matrix's 3x3 * (x, y, z). Directions ignore the translation column.
inline Vec3 TransformDirection(const Mtx34& matrix, const Vec3& v) noexcept {
    return {
        matrix[0] * v.x + matrix[1] * v.y + matrix[2] * v.z,
        matrix[4] * v.x + matrix[5] * v.y + matrix[6] * v.z,
        matrix[8] * v.x + matrix[9] * v.y + matrix[10] * v.z,
    };
}

// Fills the three basis rows from a forward and an up that need not be exactly
// perpendicular, in the -Z-forward convention view space uses.
inline bool BasisFromForwardUp(const Vec3& forward_in, const Vec3& up_in, Vec3 rows[3]) noexcept {
    Vec3 forward = forward_in;
    if (!Normalize(forward)) {
        return false;
    }
    Vec3 right = Cross(forward, up_in);
    if (!Normalize(right)) {
        return false;
    }
    rows[0] = right;
    rows[1] = Cross(right, forward);
    rows[2] = {-forward.x, -forward.y, -forward.z};
    return true;
}

// out = matrix * (x, y, z, 1)
inline Vec3 TransformPoint(const Mtx34& matrix, float x, float y, float z) noexcept {
    return {
        matrix[0] * x + matrix[1] * y + matrix[2] * z + matrix[3],
        matrix[4] * x + matrix[5] * y + matrix[6] * z + matrix[7],
        matrix[8] * x + matrix[9] * y + matrix[10] * z + matrix[11],
    };
}

} // namespace detail

// ---------------------------------------------------------------------------
// Cockpit seat and steering-wheel geometry. Ported from heurazy's
// mario-kart-wii-VR-port (GPL-3.0-or-later).
// ---------------------------------------------------------------------------

inline Mtx34 ComposeMtx(const Mtx34& a, const Mtx34& b) noexcept {
    Mtx34 out{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            out[row * 4 + col] = col == 3 ? a[row * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                out[row * 4 + col] += a[row * 4 + k] * b[k * 4 + col];
            }
        }
    }
    return out;
}

inline bool InvertMtx(const Mtx34& m, Mtx34& out) noexcept {
    if (!detail::IsFiniteMtx34(m)) {
        return false;
    }
    const detail::Vec3 a{m[0], m[4], m[8]}, b{m[1], m[5], m[9]}, c{m[2], m[6], m[10]};
    const auto x = detail::Cross(b, c), y = detail::Cross(c, a), z = detail::Cross(a, b);
    const float det = detail::Dot(a, x);
    if (!detail::IsFiniteFloat(&det) || std::abs(det) < 1e-6f) {
        return false;
    }
    out = {x.x / det, x.y / det, x.z / det, 0, y.x / det, y.y / det, y.z / det, 0, z.x / det, z.y / det, z.z / det, 0};
    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 3] = -(out[row * 4] * m[3] + out[row * 4 + 1] * m[7] + out[row * 4 + 2] * m[11]);
    }
    return detail::IsFiniteMtx34(out);
}

// Keeps the eye behind the steering wheel or handlebar even when a long face or
// a leaned-forward riding animation puts the character's eyes over it. Units
// are the vehicle's; `radius` is the control's half width.
inline float EyeBehindControls(float eyeForward, float controlsForward, float units, float radius) noexcept {
    const float clearance = std::clamp(0.40f + radius / units * 0.3f, 0.45f, 0.65f) * units;
    return std::min(eyeForward, controlsForward - clearance);
}

// Once the eye is pulled behind the controls, a long neck or snout must not
// leave them down at the player's knees. Keep the measured character scale,
// but cap the seated eye at 40 cm above the neutral hand targets.
inline float EyeAboveControls(float eyeHeight, float controlsHeight, float units) noexcept {
    if (!detail::IsFiniteFloat(&controlsHeight) || !detail::IsFiniteFloat(&units) || units <= 0.0f) {
        return eyeHeight;
    }
    const float limit = controlsHeight + 0.40f * units;
    return limit >= 5.0f ? std::min(eyeHeight, limit) : eyeHeight;
}

// Tall characters sit higher; normalise them to a comfortable perceived cockpit
// height by growing the world scale with the measured eye height.
inline float CharacterCockpitScale(float eyeHeight) noexcept {
    if (!detail::IsFiniteFloat(&eyeHeight)) {
        return 1.0f;
    }
    return std::clamp(eyeHeight / 100.0f, 1.0f, 2.5f);
}

inline float ValidPlayerScale(float scale) noexcept {
    return detail::IsFiniteFloat(&scale) && scale >= 0.1f && scale <= 4.0f ? scale : 1.0f;
}

inline Mtx34 ScaleModelBasis(Mtx34 pose, const std::array<float, 3>& scale) noexcept {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            pose[row * 4 + col] *= scale[col];
        }
    }
    return pose;
}

inline bool NeutralPlayerScale(const std::array<float, 3>& scale) noexcept {
    for (float value : scale) {
        if (!detail::IsFiniteFloat(&value) || std::abs(value - 1.0f) > 0.001f) {
            return false;
        }
    }
    return true;
}

inline bool ValidSeatedEye(const std::array<float, 3>& eye) noexcept {
    for (const auto& value : eye) {
        if (!detail::IsFiniteFloat(&value) || std::abs(value) > 500.0f) {
            return false;
        }
    }
    return eye[1] >= 5.0f;
}

// Eye position resources are in the face bone's local coordinates, whose axes
// differ between characters. Transform their centre through the complete bind
// matrix before applying the vehicle-specific driver placement.
inline bool ComputeDriverEyeFromBounds(const Mtx34& face, const Mtx34& placement, detail::Vec3 minimum,
                                       detail::Vec3 maximum, std::array<float, 3>& eye) noexcept {
    if (!detail::IsFiniteMtx34(face) || !detail::IsFiniteMtx34(placement)) {
        return false;
    }
    const std::array<float, 6> bounds{minimum.x, minimum.y, minimum.z, maximum.x, maximum.y, maximum.z};
    for (const auto& value : bounds) {
        if (!detail::IsFiniteFloat(&value) || std::abs(value) > 500.0f) {
            return false;
        }
    }
    if (minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z) {
        return false;
    }
    const auto model = detail::TransformPoint(face, (minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f,
                                              (minimum.z + maximum.z) * 0.5f);
    const auto seat = detail::TransformPoint(placement, model.x, model.y, model.z);
    const std::array<float, 3> result{seat.x, seat.y, seat.z};
    if (!ValidSeatedEye(result)) {
        return false;
    }
    eye = result;
    return true;
}

// Removes the visible vehicle's world transform from the evaluated head pose.
// This retains the riding posture, but never imports kart motion into the seat.
inline bool ComputeSeatedEye(const Mtx34& faceWorld, const Mtx34& bodyWorld, detail::Vec3 eyeLocal,
                             std::array<float, 3>& eye) noexcept {
    if (!detail::IsFiniteMtx34(faceWorld) || !detail::IsFiniteMtx34(bodyWorld)) {
        return false;
    }
    const detail::Vec3 a{bodyWorld[0], bodyWorld[4], bodyWorld[8]}, b{bodyWorld[1], bodyWorld[5], bodyWorld[9]},
        c{bodyWorld[2], bodyWorld[6], bodyWorld[10]};
    const auto bc = detail::Cross(b, c), ca = detail::Cross(c, a), ab = detail::Cross(a, b);
    const float det = detail::Dot(a, bc);
    if (!detail::IsFiniteFloat(&det) || std::abs(det) < 1e-6f) {
        return false;
    }
    const auto world = detail::TransformPoint(faceWorld, eyeLocal.x, eyeLocal.y, eyeLocal.z);
    const detail::Vec3 delta{world.x - bodyWorld[3], world.y - bodyWorld[7], world.z - bodyWorld[11]};
    const std::array<float, 3> result{detail::Dot(bc, delta) / det, detail::Dot(ca, delta) / det,
                                      detail::Dot(ab, delta) / det};
    if (!ValidSeatedEye(result)) {
        return false;
    }
    eye = result;
    return true;
}

// Mods may have no separately named eye geometry. Estimate just above/ahead
// of the evaluated head, in vehicle axes, not the head bone's rotated axes.
// The animated pose includes the character animation's scale and placement;
// adding the driver's placement again would put some mods below the vehicle.
inline bool ComputeDriverEyeFromHead(const Mtx34& headWorld, const Mtx34& bodyWorld,
                                    std::array<float, 3>& eye) noexcept {
    std::array<float, 3> head{};
    if (!ComputeSeatedEye(headWorld, bodyWorld, {0, 0, 0}, head)) {
        return false;
    }
    head[1] += 8.0f;
    head[2] += 8.0f;
    if (!ValidSeatedEye(head)) {
        return false;
    }
    eye = head;
    return true;
}

// The neutral seated eye, accepted once eight consecutive safe samples agree
// within two units of the first sample, then frozen until the driver or the
// race changes, or a recenter asks for another calibration. Keep the previous
// seat while waiting for a safe replacement.
struct SeatedEyeReference {
    std::array<float, 3> value{}, candidate{};
    unsigned stable = 0;
    bool valid = false;
    bool recalibrating = false;
    void Recalibrate() noexcept {
        stable = 0;
        recalibrating = true;
    }
    void Observe(const std::array<float, 3>& sample, bool safe, bool freeze) {
        if (freeze && valid && !recalibrating) {
            return;
        }
        if (!safe || !ValidSeatedEye(sample)) {
            stable = 0;
            return;
        }
        float delta = 0.0f;
        for (int i = 0; i < 3; ++i) {
            delta = std::max(delta, std::abs(sample[i] - candidate[i]));
        }
        if (stable && delta < 2.0f) {
            ++stable;
        } else {
            stable = 1;
            candidate = sample;
        }
        if (stable >= 8) {
            value = sample;
            valid = true;
            recalibrating = false;
            stable = 8;
        }
    }
};

// Which calls latch the race frame's seat (MkwVRFirstPersonUpdate). The race
// draw observer latches once per draw, unless the character body's
// ScnRoot::CalcView wrapper already latched for that draw. Every CalcView call
// latches: a frame the game catches up on runs the simulation and CalcView
// twice before it draws once, and its draws show the second step. Keyed to
// draws, not to the display copy that commits the seat.
struct SeatLatchClock {
    uint64_t draws = 0;         // race draw observer calls so far
    uint64_t latched_draw = 0;  // the draw the current latch was made for
    uint64_t serial = 0;        // latches made, for work cached per latch
    // `armed`: a latch is waiting for its commit; `same_camera`: it was made
    // for the camera this call carries. True when the call latches.
    bool Observe(bool draw_observer, bool armed, bool same_camera) noexcept {
        const uint64_t draw = draws;
        if (draw_observer) {
            ++draws;
            if (armed && same_camera && latched_draw == draw) {
                return false;
            }
        }
        latched_draw = draw;
        ++serial;
        return true;
    }
};

// Neutral authored hand targets, transformed by the stabilised cockpit body. Do
// not use the animated hand IK targets: feeding their steering rotation back
// into the controller angle would make the input chase its own animation.
// `seat_from_body` maps vehicle-local units into the seated frame in units;
// the result is in metres.
inline WheelGeometry ComputeNativeWheelGeometry(const Mtx34& seat_from_body, detail::Vec3 left, detail::Vec3 right,
                                                float units) noexcept {
    WheelGeometry out{};
    if (!detail::IsFiniteMtx34(seat_from_body) || !detail::IsFiniteFloat(&units) || units <= 0.0f) {
        return out;
    }
    if (left.x > right.x) {
        std::swap(left, right);
    }
    const auto a = detail::TransformPoint(seat_from_body, left.x, left.y, left.z);
    const auto b = detail::TransformPoint(seat_from_body, right.x, right.y, right.z);
    detail::Vec3 x{b.x - a.x, b.y - a.y, b.z - a.z};
    const float radius = std::sqrt(detail::Dot(x, x)) / (2.0f * units);
    if (!detail::IsFiniteFloat(&radius) || radius < 0.04f || radius > 1.0f || !detail::Normalize(x)) {
        return out;
    }
    // Kart +X points left when looking along its +Z driving direction.
    // WheelHand uses headset +X (right), so reverse the authored lateral axis.
    x = {-x.x, -x.y, -x.z};
    detail::Vec3 y{seat_from_body[1], seat_from_body[5], seat_from_body[9]};
    const float projection = detail::Dot(x, y);
    y = {y.x - x.x * projection, y.y - x.y * projection, y.z - x.z * projection};
    if (!detail::Normalize(y)) {
        return out;
    }
    const auto z = detail::Cross(x, y);
    out.center = {(a.x + b.x) / (2.0f * units), (a.y + b.y) / (2.0f * units), (a.z + b.z) / (2.0f * units)};
    for (const auto& value : out.center) {
        if (!detail::IsFiniteFloat(&value) || std::abs(value) > 5.0f) {
            return {};
        }
    }
    out.right = {x.x, x.y, x.z};
    out.up = {y.x, y.y, y.z};
    out.normal = {z.x, z.y, z.z};
    out.radius = radius;
    out.valid = true;
    return out;
}

inline WheelGeometry ComputeNativeHandlebarGeometry(const Mtx34& seatFromHandle, const Mtx34& seatFromBody,
                                                    detail::Vec3 left, detail::Vec3 right, float units) noexcept {
    auto out = ComputeNativeWheelGeometry(seatFromHandle, left, right, units);
    if (!out.valid || !detail::IsFiniteMtx34(seatFromBody)) {
        return {};
    }
    // Use the body's neutral axes, not the already-steered handle's axes.
    // Otherwise the visual steering feeds back into the next input sample.
    detail::Vec3 x{-seatFromBody[0], -seatFromBody[4], -seatFromBody[8]},
        forward{seatFromBody[2], seatFromBody[6], seatFromBody[10]};
    if (!detail::Normalize(x)) {
        return {};
    }
    const float along = detail::Dot(forward, x);
    forward = {forward.x - along * x.x, forward.y - along * x.y, forward.z - along * x.z};
    if (!detail::Normalize(forward)) {
        return {};
    }
    const auto vertical = detail::Cross(x, forward);
    out.right = {x.x, x.y, x.z};
    out.up = {forward.x, forward.y, forward.z};
    out.normal = {vertical.x, vertical.y, vertical.z};
    return out;
}

// Builds the anchor from the game's view matrix (world -> recorded view space),
// the kart's pose (kart-local -> world), and head offsets already converted to
// world units.
//
// The translation always moves the camera onto the head; `rotation` decides the
// frame it looks around from. Returns false and leaves `out` untouched when the
// inputs cannot produce an orthonormal frame.
inline bool ComputeFirstPersonAnchor(const Mtx34& view_from_world, const Mtx34& kart_from_local,
                                     float head_right_units, float head_up_units,
                                     float head_forward_units, FirstPersonRotation rotation,
                                     Mtx34& out) noexcept {
    using namespace detail;
    if (!IsFiniteMtx34(view_from_world) || !IsFiniteMtx34(kart_from_local)) {
        return false;
    }
    const Vec3 head_world =
        TransformPoint(kart_from_local, head_right_units, head_up_units, head_forward_units);
    const Vec3 a = TransformPoint(view_from_world, head_world.x, head_world.y, head_world.z);
    if (!IsFiniteFloat(&a.x) || !IsFiniteFloat(&a.y) || !IsFiniteFloat(&a.z)) {
        return false;
    }

    // Rows of the anchor's rotation. Identity keeps the recorded camera's own
    // orientation and moves the eye only.
    // Every mode is the same construction from a forward and an up; they differ
    // only in which pair they take. Pairing a forward with world up is what
    // removes roll, since the resulting right axis is then always horizontal.
    Vec3 rows[3]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    // World +Y in view coordinates: the column of the view rotation that the
    // world up axis selects.
    Vec3 world_up{view_from_world[1], view_from_world[5], view_from_world[9]};
    const bool world_up_valid = Normalize(world_up);
    // Columns 2 and 1 of the kart pose are its forward and up. The pose may
    // carry scale, so the pair is re-orthonormalized rather than trusted.
    const Vec3 kart_forward = TransformDirection(
        view_from_world, {kart_from_local[2], kart_from_local[6], kart_from_local[10]});
    const Vec3 kart_up = TransformDirection(
        view_from_world, {kart_from_local[1], kart_from_local[5], kart_from_local[9]});

    if (rotation == FirstPersonRotation::YawOnly) {
        if (!world_up_valid) {
            return false;
        }
        // Level the recorded camera's forward (-Z in its own space) onto the
        // horizon plane. Looking near-straight up or down leaves nothing to
        // project, so recover the heading from the camera's up axis instead.
        const Vec3 camera_forward{0.0f, 0.0f, -1.0f};
        float along = Dot(camera_forward, world_up);
        Vec3 forward{camera_forward.x - world_up.x * along, camera_forward.y - world_up.y * along,
                     camera_forward.z - world_up.z * along};
        if (!Normalize(forward)) {
            const Vec3 camera_up{0.0f, 1.0f, 0.0f};
            along = Dot(camera_up, world_up);
            forward = {camera_up.x - world_up.x * along, camera_up.y - world_up.y * along,
                       camera_up.z - world_up.z * along};
            if (!Normalize(forward)) {
                return false;
            }
        }
        if (!BasisFromForwardUp(forward, world_up, rows)) {
            return false;
        }
    } else if (rotation == FirstPersonRotation::YawPitch) {
        // The kart's heading and climb, levelled against world up so no roll
        // survives. Pointing straight up or down leaves nothing to level
        // against, so that frame falls back to the kart's own up.
        if (!world_up_valid || !BasisFromForwardUp(kart_forward, world_up, rows)) {
            if (!BasisFromForwardUp(kart_forward, kart_up, rows)) {
                return false;
            }
        }
    } else if (!BasisFromForwardUp(kart_forward, kart_up, rows)) {
        return false;
    }

    Mtx34 anchor{};
    for (uint32_t row = 0; row < 3; ++row) {
        anchor[row * 4 + 0] = rows[row].x;
        anchor[row * 4 + 1] = rows[row].y;
        anchor[row * 4 + 2] = rows[row].z;
        anchor[row * 4 + 3] = -Dot(rows[row], a);
    }
    if (!IsFiniteMtx34(anchor)) {
        return false;
    }
    out = anchor;
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame observation. Called from the translated-code observers on the guest
// thread; the anchor is consumed by the producer at its Aurora frame seal.
// ---------------------------------------------------------------------------

// Enables anchor computation and sets the head offsets and world scale used to
// convert them. Called whenever the configuration or the F10 toggle changes.
void MkwVRFirstPersonConfigure(bool enabled, const FirstPersonHeadOffsets& offsets,
                               float units_per_meter, FirstPersonRotation rotation) noexcept;

// While the anchor is driving the view the player's own models can be removed,
// since the driver otherwise sits exactly where the eyes are. This uses the
// game's own visibility fields, and puts them back when it stops.
//
// Reads the current [vr] first-person settings and applies them here and to the
// presentation policy's world scale. The single place those settings are
// interpreted, shared by startup and the F10 settings bar.
void MkwVRFirstPersonApplyConfiguredSettings() noexcept;

// Arms the anchor for this guest frame. Call from the race draw boundary, with
// the frame's own RaceCamera, or zero if none was seen. This only latches; the
// anchor itself is computed by Commit below, because the scene's camera matrix
// for the frame is not set until the draws run. `draw_observer` is false for
// the character body's ScnRoot::CalcView call, which comes first and latches
// every time it runs (SeatLatchClock).
void MkwVRFirstPersonUpdate(uint64_t guest_frame_index, uint32_t race_camera_address,
                            bool draw_observer = true) noexcept;

// Computes and publishes the anchor from the values the frame was drawn with.
// Call from the producer's frame seal, after the draws and before the sealed
// frame reaches Aurora. Does nothing unless Update armed the frame, which is
// what keeps this to races.
void MkwVRFirstPersonCommit() noexcept;

// Drops every captured pointer and the held anchor. Call on race entry/exit.
void MkwVRFirstPersonReset() noexcept;

// Thread-safe request; the guest thread remeasures the cockpit on subsequent
// neutral frames. Keeps the current seat until a replacement is ready.
void MkwVRFirstPersonRecenter() noexcept;

// Producer-side read. Thread-safe. A valid anchor is also what marks the mode
// as engaged, and so what selects the first-person world scale: it is invalid
// whenever the mode is off, the race has not produced a usable anchor, or the
// anchor has been missing long enough to give up holding the last one.
FirstPersonAnchor MkwVRFirstPersonGetAnchor() noexcept;

// Checked guest-thread read of the camera that authored this frame's GX draws.
// Also available with first person off; render workers receive only its copy.
bool MkwVRReadSceneView(Mtx34& view) noexcept;

// Guest-frame inventory snapshot, sampled at the race draw boundary. A
// generation change invalidates any item retained by a prior race.
HeldItem MkwVRFirstPersonGetHeldItem() noexcept;

// Camera modes (vr_controls.h). Thread-safe.
// What the race shows now: the game's camera outside races, through the intro
// and, with bullet_bill_camera = "game", while the player is a Bullet Bill.
CameraMode MkwVRGetCameraMode() noexcept;
// The player's choice for the current race; through the intro, the camera the
// race will start on.
CameraMode MkwVRSelectedCameraMode() noexcept;
// Switches the current race's camera, never saved; the next race starts on
// [vr] first_person. Refused (false) outside races, during the intro and, for
// first person, without a single local racer to sit in (split screen, the
// attract demo, spectating). Call on the game thread: it reads the racer.
bool MkwVRSetCameraMode(CameraMode mode) noexcept;
// Whether a switch can show anything now (the same rules, from the last
// readings); for feedback on other threads, such as the controllers' tick.
bool MkwVRCameraSwitchAvailable() noexcept;
// The race draw observer's count of local screens (RaceScene::GetScreenCount).
void MkwVRFirstPersonObserveRaceScreens(uint32_t local_screens) noexcept;
bool MkwVRInRace() noexcept;
bool MkwVRRaceIntroActive() noexcept;

// Character body (character_body_ik.inl): the game's character model draws
// this hand (0 left, 1 right) for the current frame, so the cockpit draws no
// VR hand there. Thread-safe.
bool MkwVRCharacterHandActive(size_t hand) noexcept;

} // namespace mkw::vr
