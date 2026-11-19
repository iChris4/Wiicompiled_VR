// SPDX-License-Identifier: GPL-3.0-or-later
// [shaker]: the secondary (bass shaker) audio device settings.
#include "runtime_config.h"
#include <cstdlib>
#include <sstream>
#include <string>

static void Require(bool condition) {
    if (!condition) std::abort();
}

static RuntimeUserConfig Parse(const std::string& text) {
    std::istringstream input(text);
    return RuntimeConfigFile::ParseConfig(input);
}

int main() {
    // Nothing set: every key reads as absent, so the getters' defaults apply.
    const auto defaults = Parse("[shaker]\n");
    Require(!defaults.shakerEnabled && !defaults.shakerDevice && !defaults.shakerMono);
    Require(!defaults.shakerLowpassHz && !defaults.shakerGainDb && !defaults.shakerIncludeMusic);

    const auto set = Parse(
        "[shaker]\nenabled = true\ndevice = \"Bass Amp (USB)\"\nmono = false\n"
        "lowpass_hz = 63.5\ngain_db = -4\ninclude_music = true\n");
    Require(set.shakerEnabled == true && set.shakerMono == false && set.shakerIncludeMusic == true);
    Require(set.shakerDevice == std::string("Bass Amp (USB)"));
    Require(set.shakerLowpassHz == 63.5f && set.shakerGainDb == -4.0f);

    // A device name with quotes and a backslash survives the round trip through the writer.
    const std::string tricky = "Amp \"Out\" \\ one";
    Require(Parse("[shaker]\ndevice = " + RuntimeConfigFile::FormatString(tricky) + "\n").shakerDevice == tricky);

    // Out-of-range numbers are ignored instead of trusted.
    const auto bounded = Parse("[shaker]\nlowpass_hz = 9999\ngain_db = 500\n");
    Require(!bounded.shakerLowpassHz && !bounded.shakerGainDb);
    const auto negative = Parse("[shaker]\nlowpass_hz = -1\ngain_db = -500\n");
    Require(!negative.shakerLowpassHz && !negative.shakerGainDb);
    // 0 is valid: it turns the filter off.
    Require(Parse("[shaker]\nlowpass_hz = 0\n").shakerLowpassHz == 0.0f);

    // The new section does not disturb the other audio settings.
    const auto mixed = Parse("[audio]\nmix_worker = false\n[shaker]\nenabled = true\n");
    Require(mixed.shakerEnabled == true);

    return 0;
}
