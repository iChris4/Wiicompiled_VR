// SPDX-License-Identifier: GPL-3.0-or-later
#include "vr/frame_interpolation_pacing.h"
#include "runtime_config.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

using mkw::vr::FrameInterpolationPacing;

static void Require(bool condition) {
    if (!condition) std::abort();
}

int main() {
    for (uint32_t target : {0u, 1u, 72u, 90u, 120u}) {
        std::istringstream input("[vr]\nframe_interpolation_fps = " + std::to_string(target) + "\n");
        const auto config = RuntimeConfigFile::ParseConfig(input);
        Require(config.vrFrameInterpolationFps == target);
    }
    std::istringstream legacy("[vr]\nframe_interpolation = true\neager_frame_heartbeat = true\n");
    Require(RuntimeConfigFile::ParseConfig(legacy).vrFrameInterpolationFps == 1);
    std::istringstream explicitOff("[vr]\nframe_interpolation_fps = 0\nframe_interpolation = true\n");
    Require(RuntimeConfigFile::ParseConfig(explicitOff).vrFrameInterpolationFps == 0);
    std::istringstream missing("[vr]\n");
    Require(!RuntimeConfigFile::ParseConfig(missing).vrFrameInterpolationFps.has_value());
    for (uint32_t headset : {72u, 90u, 120u}) {
        for (uint32_t target : {1u, 72u, 90u, 120u}) {
            FrameInterpolationPacing pacing;
            uint32_t rendered = 0;
            // Ten seconds on the actual display grid, including noninteger ratios.
            for (uint32_t frame = 0; frame < headset * 10; ++frame) {
                const auto time = 1'000'000'000ll + static_cast<int64_t>(frame) * 1'000'000'000ll / headset;
                if (pacing.ShouldRender(time, target)) ++rendered;
            }
            const auto expected = std::min(headset, target == 1 ? headset : target) * 10;
            Require(std::abs(static_cast<int>(rendered) - static_cast<int>(expected)) <= 1);
        }
    }
    FrameInterpolationPacing pacing;
    Require(pacing.ShouldRender(1'000'000'000, 72));
    Require(!pacing.ShouldRender(1'008'333'333, 72));
    Require(pacing.ShouldRender(1'008'333'334, 120)); // Live target change.
    Require(pacing.ShouldRender(5'000'000'000, 120)); // Stall: no catch-up burst.
    Require(!pacing.ShouldRender(5'000'000'001, 120));
    Require(pacing.ShouldRender(1'000'000, 120)); // New session clock.
    pacing.Reset();
    Require(pacing.ShouldRender(1'000'001, 120));
    Require(mkw::vr::NormalizeFrameInterpolationFps(90) == 90);
    Require(mkw::vr::NormalizeFrameInterpolationFps(1) == 1);
    Require(mkw::vr::NormalizeFrameInterpolationFps(60) == 0);
    Require(mkw::vr::NormalizeFrameInterpolationFps(UINT32_MAX) == 0);
    std::cout << "VR interpolation pacing tests passed\n";
}
