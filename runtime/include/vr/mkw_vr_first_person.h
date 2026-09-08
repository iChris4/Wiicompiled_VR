// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
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

// The camera relocation published to Aurora for one guest frame: a transform
// from the game's recorded view space into the space the headset renders from.
struct FirstPersonAnchor {
    Mtx34 anchor_from_scene = kIdentityMtx34;
    bool valid = false;
    uint64_t guest_frame_index = 0;
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

// Arms the anchor for this guest frame. Call once per frame from the race draw
// boundary, with the frame's own RaceCamera, or zero if none was seen. This
// only latches; the anchor itself is computed by Commit below, because the
// scene's camera matrix for the frame is not set until the draws run.
void MkwVRFirstPersonUpdate(uint64_t guest_frame_index, uint32_t race_camera_address) noexcept;

// Computes and publishes the anchor from the values the frame was drawn with.
// Call from the producer's frame seal, after the draws and before the sealed
// frame reaches Aurora. Does nothing unless Update armed the frame, which is
// what keeps this to races.
void MkwVRFirstPersonCommit() noexcept;

// Drops every captured pointer and the held anchor. Call on race entry/exit.
void MkwVRFirstPersonReset() noexcept;

// Producer-side read. Thread-safe. A valid anchor is also what marks the mode
// as engaged, and so what selects the first-person world scale: it is invalid
// whenever the mode is off, the race has not produced a usable anchor, or the
// anchor has been missing long enough to give up holding the last one.
FirstPersonAnchor MkwVRFirstPersonGetAnchor() noexcept;

} // namespace mkw::vr
