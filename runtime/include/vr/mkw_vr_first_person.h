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
    float up = 1.0f;
    float forward = 0.0f;
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
// The translation moves the camera onto the head. With level_horizon the
// rotation keeps the recorded camera's heading but drops its pitch and roll, so
// the headset owns pitch and roll outright; without it the recorded camera's
// orientation is kept whole and only the eye moves. Returns false and leaves
// `out` untouched when the inputs cannot produce an orthonormal frame.
inline bool ComputeFirstPersonAnchor(const Mtx34& view_from_world, const Mtx34& kart_from_local,
                                     float head_right_units, float head_up_units,
                                     float head_forward_units, bool level_horizon,
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
    Vec3 rows[3]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    if (level_horizon) {
        // World +Y in view coordinates: the column of the view rotation that
        // the world up axis selects.
        Vec3 up{view_from_world[1], view_from_world[5], view_from_world[9]};
        if (!Normalize(up)) {
            return false;
        }
        // Level the recorded camera's forward (-Z in its own space) onto the
        // horizon plane. Looking near-straight up or down leaves nothing to
        // project, so recover the heading from the camera's up axis instead.
        const Vec3 camera_forward{0.0f, 0.0f, -1.0f};
        float along = Dot(camera_forward, up);
        Vec3 forward{camera_forward.x - up.x * along, camera_forward.y - up.y * along,
                     camera_forward.z - up.z * along};
        if (!Normalize(forward)) {
            const Vec3 camera_up{0.0f, 1.0f, 0.0f};
            along = Dot(camera_up, up);
            forward = {camera_up.x - up.x * along, camera_up.y - up.y * along,
                       camera_up.z - up.z * along};
            if (!Normalize(forward)) {
                return false;
            }
        }
        Vec3 right = Cross(forward, up);
        if (!Normalize(right)) {
            return false;
        }
        // Re-derive up from the orthonormalized pair so a slightly non-rigid
        // view matrix cannot leave a skewed frame behind.
        rows[0] = right;
        rows[1] = Cross(right, forward);
        rows[2] = {-forward.x, -forward.y, -forward.z};
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
                               float units_per_meter) noexcept;

// Reads the current [vr] first-person settings and applies them here and to the
// presentation policy's world scale. The single place those settings are
// interpreted, shared by startup and the F10 settings bar.
void MkwVRFirstPersonApplyConfiguredSettings() noexcept;

// Reads the race camera and the player's kart and republishes the anchor. Call
// once per guest frame, after the kart and camera updates and before the draws.
// race_camera_address is the frame's own RaceCamera, or zero if none was seen.
void MkwVRFirstPersonUpdate(uint64_t guest_frame_index, uint32_t race_camera_address) noexcept;

// Drops every captured pointer and the held anchor. Call on race entry/exit.
void MkwVRFirstPersonReset() noexcept;

// Producer-side read. Thread-safe. A valid anchor is also what marks the mode
// as engaged, and so what selects the first-person world scale: it is invalid
// whenever the mode is off, the race has not produced a usable anchor, or the
// anchor has been missing long enough to give up holding the last one.
FirstPersonAnchor MkwVRFirstPersonGetAnchor() noexcept;

} // namespace mkw::vr
