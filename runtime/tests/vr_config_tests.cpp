// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_config.h"
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>

static void Require(bool condition) {
    if (!condition) std::abort();
}

static RuntimeUserConfig Parse(const std::string& text) {
    std::istringstream input(text);
    return RuntimeConfigFile::ParseConfig(input);
}

int main() {
    // [vr] foveation: the Quest's foveated rendering level, index-matched to
    // aurora_set_stereo_foveation.
    for (std::string_view level : RuntimeConfigFile::kVrFoveationLevels) {
        Require(Parse("[vr]\nfoveation = \"" + std::string(level) + "\"\n").vrFoveation == std::string(level));
    }
    Require(!Parse("[vr]\nfoveation = \"ultra\"\n").vrFoveation.has_value());
    Require(!Parse("[vr]\nfoveation = 2\n").vrFoveation.has_value());
    Require(!Parse("[vr]\n").vrFoveation.has_value());
    Require(std::string_view(RuntimeConfigFile::kVrFoveationDefault) == "off");
    Require(RuntimeConfigFile::VrFoveationLevelIndex("off") == 0);
    Require(RuntimeConfigFile::VrFoveationLevelIndex("low") == 1);
    Require(RuntimeConfigFile::VrFoveationLevelIndex("medium") == 2);
    Require(RuntimeConfigFile::VrFoveationLevelIndex("high") == 3);
    Require(RuntimeConfigFile::VrFoveationLevelIndex("ultra") == 0);

    // [vr] single_pass_eyes: each eye replayed in one render pass.
    Require(Parse("[vr]\nsingle_pass_eyes = true\n").vrSinglePassEyes == true);
    Require(Parse("[vr]\nsingle_pass_eyes = false\n").vrSinglePassEyes == false);
    Require(!Parse("[vr]\n").vrSinglePassEyes.has_value());

    // [vr] immersive_window and flat_screen: one race view in two keys, Flat
    // Screen mode winning, so a file that predates the window reads as before.
    using RuntimeConfigFile::VrRaceView;
    using RuntimeConfigFile::VrRaceViewOf;
    Require(Parse("[vr]\nimmersive_window = true\n").vrImmersiveWindow == true);
    Require(!Parse("[vr]\n").vrImmersiveWindow.has_value());
    Require(VrRaceViewOf(Parse("[vr]\n")) == VrRaceView::Immersive);
    Require(VrRaceViewOf(Parse("[vr]\nflat_screen = false\n")) == VrRaceView::Immersive);
    Require(VrRaceViewOf(Parse("[vr]\nflat_screen = true\n")) == VrRaceView::FlatScreen);
    Require(VrRaceViewOf(Parse("[vr]\nimmersive_window = true\n")) == VrRaceView::ImmersiveWindow);
    Require(VrRaceViewOf(Parse("[vr]\nflat_screen = true\nimmersive_window = true\n")) == VrRaceView::FlatScreen);
    Require(VrRaceViewOf(Parse("[vr]\nflat_screen = false\nimmersive_window = false\n")) == VrRaceView::Immersive);
    return 0;
}
