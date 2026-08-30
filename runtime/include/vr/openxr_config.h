// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <openxr/openxr.h>

namespace mkw::vr {

// Kept independent from RuntimeUserConfig so the OpenXR core can be initialized
// before Aurora chooses a graphics adapter. The application-facing TOML values
// should be translated into this structure by the runtime integration layer.
struct OpenXRConfig {
    std::string application_name = "WiiCompiled";
    uint32_t application_version = 1;
    std::string engine_name = "Aurora";
    uint32_t engine_version = 1;

    // OpenXR 1.0 is sufficient for the lifecycle implemented by this class and
    // remains compatible with desktop runtimes that do not advertise 1.1 yet.
    XrVersion api_version = XR_API_VERSION_1_0;
    XrFormFactor form_factor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrViewConfigurationType view_configuration =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    // LOCAL is the natural default for a seated racing game. STAGE can be
    // requested by the integration layer and falls back to LOCAL when absent.
    XrReferenceSpaceType reference_space = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrEnvironmentBlendMode preferred_blend_mode =
        XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    // Applied to the runtime-recommended dimensions and clamped to the
    // runtime-advertised maximum for each view.
    float resolution_scale = 1.0f;

    // The graphics backend must put its binding extension in required_extensions
    // (for example XR_KHR_D3D12_enable or XR_KHR_vulkan_enable2). Optional
    // extensions/layers are enabled only when the active runtime advertises them.
    std::vector<std::string> required_extensions;
    std::vector<std::string> optional_extensions;
    std::vector<std::string> required_api_layers;
    std::vector<std::string> optional_api_layers;

    // Destruction never waits indefinitely for a runtime to acknowledge an exit.
    uint32_t shutdown_timeout_ms = 500;
};

} // namespace mkw::vr
