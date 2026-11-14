// SPDX-License-Identifier: GPL-3.0-or-later
// Positional voice gains: centred voices sound exactly as without the feature, panning keeps
// equal power, and distance never silences a player.

#include "voicechat/voice_spatial.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

using mkw::voicechat::spatial::GainsForPosition;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

bool Near(float a, float b) { return std::abs(a - b) < 1e-4f; }

} // namespace

int main() {
    auto gains = GainsForPosition(0.0f, 0.0f, -3.0f);
    Check(Near(gains.left, 1.0f) && Near(gains.right, 1.0f), "straight ahead and close is unchanged");
    gains = GainsForPosition(0.0f, 0.0f, 0.0f);
    Check(Near(gains.left, 1.0f) && Near(gains.right, 1.0f), "at the head is centred");

    gains = GainsForPosition(5.0f, 0.0f, 0.0f);
    Check(Near(gains.left, 0.0f) && Near(gains.right, std::sqrt(2.0f)), "hard right");
    gains = GainsForPosition(-5.0f, 0.0f, 0.0f);
    Check(Near(gains.right, 0.0f) && Near(gains.left, std::sqrt(2.0f)), "hard left");

    for (float angle = 0.0f; angle < 6.28f; angle += 0.3f) {
        gains = GainsForPosition(4.0f * std::sin(angle), 0.0f, -4.0f * std::cos(angle));
        Check(Near(gains.left * gains.left + gains.right * gains.right, 2.0f), "equal power at every angle");
    }
    gains = GainsForPosition(3.0f, 0.0f, 4.0f);  // behind and to the right
    auto front = GainsForPosition(3.0f, 0.0f, -4.0f);
    Check(Near(gains.left, front.left) && Near(gains.right, front.right), "behind mirrors in front");

    gains = GainsForPosition(0.0f, 0.0f, -16.0f);
    Check(Near(gains.left, 0.5f) && Near(gains.right, 0.5f), "twice the reference distance is half");
    gains = GainsForPosition(0.0f, 0.0f, -1000.0f);
    Check(Near(gains.left, 0.35f) && Near(gains.right, 0.35f), "far away stops at the floor");
    gains = GainsForPosition(0.0f, 30.0f, 0.0f);
    Check(Near(gains.left, gains.right) && gains.left < 1.0f, "straight above: centred, attenuated");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    gains = GainsForPosition(nan, 0.0f, 1.0f);
    Check(Near(gains.left, 1.0f) && Near(gains.right, 1.0f), "NaN falls back to centred");

    if (g_failures == 0) std::cout << "voice spatial tests passed\n";
    return g_failures == 0 ? 0 : 1;
}
