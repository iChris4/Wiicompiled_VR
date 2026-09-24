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
    return 0;
}
