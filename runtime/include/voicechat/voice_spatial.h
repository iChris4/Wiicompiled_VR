// SPDX-License-Identifier: GPL-3.0-or-later
//
// Positional voices: from where a player's kart is relative to the listener's head to a pair of
// left/right gains. Plain stereo, not HRTF: an equal-power pan on the horizontal angle plus a
// gentle distance roll-off that never goes silent, so a teammate across the track is still
// understood. Centred and close, both gains are 1, which is how a non-positional voice plays, so
// switching the feature on or off changes nothing for a voice straight ahead.

#pragma once

#include <algorithm>
#include <cmath>

namespace mkw::voicechat::spatial {

struct Gains {
    float left = 1.0f;
    float right = 1.0f;
};

struct Rolloff {
    float referenceMeters = 8.0f;  // full volume up to here
    float floor = 0.35f;           // never quieter than this
};

// `x`, `y`, `z`: head space in metres, +x right, +y up, -z ahead.
inline Gains GainsForPosition(float x, float y, float z, Rolloff rolloff = {}) noexcept {
    (void)y;
    Gains gains;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return gains;
    }
    const float horizontal = std::sqrt(x * x + z * z);
    // -1 hard left .. +1 hard right; behind the head mirrors in front (plain stereo cannot
    // tell them apart).
    const float pan = horizontal > 0.05f ? std::clamp(x / horizontal, -1.0f, 1.0f) : 0.0f;
    // Equal power, normalised so the centre stays at 1: L^2 + R^2 = 2 everywhere.
    gains.left = std::sqrt(1.0f - pan);
    gains.right = std::sqrt(1.0f + pan);

    const float distance = std::sqrt(x * x + y * y + z * z);
    const float reference = std::max(rolloff.referenceMeters, 0.01f);
    const float attenuation = std::clamp(reference / std::max(distance, reference), rolloff.floor, 1.0f);
    gains.left *= attenuation;
    gains.right *= attenuation;
    return gains;
}

// Game units to metres for a race's world. MKW's karts are about 1.5 m long at ~150 units; the
// VR cockpit's life-size default uses the same 100 units per metre.
inline constexpr float kWorldUnitsPerMeter = 100.0f;

} // namespace mkw::voicechat::spatial
