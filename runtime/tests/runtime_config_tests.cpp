#include "runtime_config.h"

#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {
bool Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}
std::optional<toml::ordered_value> Update(const std::string& original, const char* section, const char* key,
                                          const char* value, std::string* text = nullptr) {
    const auto updated = RuntimeConfigFile::UpdateSettingText(original, section, key, value);
    if (!updated) return std::nullopt;
    if (text) *text = *updated;
    return toml::parse_str<toml::ordered_type_config>(*updated);
}
bool SetsFps(const std::string& original, const char* message) {
    const auto document = Update(original, "video", "show_fps", "true");
    return Require(document && document->at("video").at("show_fps").as_boolean() &&
                       document->at("video").as_table().size() == 1,
                   message);
}
bool SetsOffset(const std::string& original, const char* message) {
    const auto document = Update(original, "controller.wii_accel_offsets", "x", "0.5");
    return Require(document && document->at("controller").at("wii_accel_offsets").at("x").as_floating() == 0.5,
                   message);
}
} // namespace

int main() {
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = true\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "enabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{true},
                     "valid MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = false\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "disabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{false},
                     "false MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = \"yes\"\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "invalid.toml");
        if (!Require(!config.metalFxSpatialUpscaling, "invalid MetalFX setting was accepted")) return 1;
    }
    if (!SetsFps("[video]\nshow_fps = false\n", "bare key was not replaced")) return 1;
    if (!SetsFps("[video]\n\"show_fps\" = false\n", "basic-quoted key was not replaced")) return 1;
    if (!SetsFps("[video]\n'show_fps'=false\n", "literal-quoted key was not replaced")) return 1;
    if (!SetsFps("[video]\n\"show\\u005Ffps\" = false\n", "escaped key was not replaced")) return 1;
    if (!SetsFps("[\"video\"]\n  show_fps = false\n", "quoted table header was not matched")) return 1;
    if (!SetsFps("[video]\nshow_fps = [\n  1,\n]\n", "multi-line value was not replaced")) return 1;
    if (!SetsFps("[video]\n", "missing key was not created")) return 1;
    if (!SetsFps("", "missing table was not created")) return 1;
    {
        std::string text;
        const auto document = Update("# top\nfoo = 1\n\n[video]\nzzz = 2 # keep\nshow_fps = false # note\naaa = 3\n\n"
                                     "[audio]\nshow_fps = 1\n",
                                     "video", "show_fps", "true", &text);
        if (!Require(document && document->at("foo").as_integer() == 1 &&
                         document->at("audio").at("show_fps").as_integer() == 1 &&
                         document->at("video").at("zzz").as_integer() == 2 &&
                         document->at("video").at("aaa").as_integer() == 3 &&
                         document->at("video").at("show_fps").as_boolean(),
                     "unrelated settings were not preserved")) return 1;
        const size_t zzz = text.find("zzz"), fps = text.find("show_fps"), aaa = text.find("aaa");
        if (!Require(text.find("# top") != std::string::npos && text.find("# keep") != std::string::npos &&
                         text.find("# note") != std::string::npos && zzz < fps && fps < aaa,
                     "comments or key order were not preserved")) return 1;
    }
    if (!Require(!RuntimeConfigFile::UpdateSettingText("[video]\nshow_fps = 1\nshow_fps = 2\n", "video", "show_fps", "true"),
                 "a document with a duplicate key was rewritten")) return 1;
    if (!Require(!RuntimeConfigFile::UpdateSettingText("[video\nshow_fps = 1\n", "video", "show_fps", "true"),
                 "an unparseable document was rewritten")) return 1;
    if (!SetsOffset("[controller]\nrumble = true\n\n[controller.wii_accel_offsets]\nx = 0.1\ny = 2\n",
                    "key in an existing nested table was not replaced")) return 1;
    if (!SetsOffset("[controller.wii_accel_offsets]\ny = 2\n", "key was not added to a nested table")) return 1;
    if (!SetsOffset("[controller]\nrumble = true\n", "missing nested table was not created")) return 1;
    if (!SetsOffset("[controller.\"wii_accel_offsets\"]\nx = 0.1\n", "quoted nested header was not matched")) return 1;
    if (!SetsOffset("[controller]\nwii_accel_offsets.y = 2\n", "dotted-key table was not extended")) return 1;
    if (!SetsOffset("controller.wii_accel_offsets.y = 2\n", "root-level dotted table was not extended")) return 1;
    if (!Require(!RuntimeConfigFile::UpdateSettingText("controller = \"custom\"\n", "controller.wii_accel_offsets",
                                                       "x", "0.5"),
                 "a scalar was replaced by a table")) return 1;
    {
        const auto document = Update("[video.advanced]\nfoo = 1\n", "video", "show_fps", "true");
        if (!Require(document && document->at("video").at("show_fps").as_boolean() &&
                         document->at("video").at("advanced").at("foo").as_integer() == 1,
                     "key was not added to an implicit table")) return 1;
    }
    {
        // "Reset VR settings": every VR and Camera preference, each set off its default, and the
        // settings the reset keeps.
        const std::string original =
            "[video]\nshow_fps = true\n\n[vr]\nenabled = false\ncontroller_mode = \"gamepad\"\n"
            "direct_presentation = false\nmultiview = false\nworld_units_per_meter = 400.0\n"
            "# Race view\nflat_screen = true\nimmersive_window = true\nflat_race_intro = false\nrender_scale = 1.5\n"
            "performance_level = \"sustained_high\"\nfoveation = \"low\"\nframe_interpolation_fps = 90\n"
            "frame_interpolation = true\nrefresh_rate = 90\npassthrough = false\nmirror_view = \"left\"\n"
            "recenter_key = \"F8\"\nhud_virtual_screen = false\nhud_distance_meters = 3.0\nhud_width_meters = 3.0\n"
            "lean_back_degrees = 10.0\nfirst_person = true\nfirst_person_toggle_click = false\n"
            "bullet_bill_camera = \"game\"\nfirst_person_seat = \"custom\"\ncockpit_units_per_meter = 120.0\n"
            "first_person_units_per_meter = 60.0\nfirst_person_head_up_meters = 1.0\n"
            "first_person_head_forward_meters = 0.5\nfirst_person_head_right_meters = 0.5\n"
            "first_person_rotation = \"full\"\nbody_ik = false\nbody_ik_hands_only = true\n"
            "body_ik_left_hand_roll = 45.0\nbody_ik_right_hand_roll = -45.0\nfirst_person_hide_driver = false\n"
            "first_person_hidden_model = -1\nsteering_wheel = false\nnative_steering_wheel = false\n"
            "placeholder_steering_wheel = true\nhand_steering = false\nwheel_kart_degrees = 120.0\n"
            "wheel_bike_degrees = 60.0\nwheel_grab_distance = 0.3\nwheel_grab_assist = 1.0\nwheel_response = 1.0\n"
            "wheel_tracking_grace = 0.2\nwheel_haptics = false\ncockpit_item_hand = \"right\"\n"
            "cockpit_item_throw = false\ncockpit_item_size = 0.4\nhand_tracking = true\nobject_culling = true\n";
        const auto preferencesSet = [](const RuntimeUserConfig& c) {
            const bool set[] = {
                c.vrFlatScreen.has_value(), c.vrImmersiveWindow.has_value(), c.vrFlatRaceIntro.has_value(),
                c.vrRenderScale.has_value(), c.vrPerformanceLevel.has_value(), c.vrFoveation.has_value(),
                c.vrFrameInterpolationFps.has_value(), c.vrRefreshRate.has_value(), c.vrPassthrough.has_value(),
                c.vrMirrorView.has_value(), c.vrRecenterKey.has_value(), c.vrHudVirtualScreen.has_value(),
                c.vrHudDistanceMeters.has_value(), c.vrHudWidthMeters.has_value(), c.vrLeanBackDegrees.has_value(),
                c.vrFirstPerson.has_value(), c.vrFirstPersonToggleClick.has_value(), c.vrBulletBillCamera.has_value(),
                c.vrFirstPersonSeat.has_value(), c.vrCockpitUnitsPerMeter.has_value(),
                c.vrFirstPersonUnitsPerMeter.has_value(), c.vrFirstPersonHeadUpMeters.has_value(),
                c.vrFirstPersonHeadForwardMeters.has_value(), c.vrFirstPersonHeadRightMeters.has_value(),
                c.vrFirstPersonRotation.has_value(), c.vrBodyIk.has_value(), c.vrBodyIkHandsOnly.has_value(),
                c.vrBodyIkLeftHandRoll.has_value(), c.vrBodyIkRightHandRoll.has_value(),
                c.vrFirstPersonHideDriver.has_value(), c.vrFirstPersonHiddenModel.has_value(),
                c.vrSteeringWheel.has_value(), c.vrNativeSteeringWheel.has_value(),
                c.vrPlaceholderSteeringWheel.has_value(), c.vrHandSteering.has_value(),
                c.vrWheelKartDegrees.has_value(), c.vrWheelBikeDegrees.has_value(), c.vrWheelGrabDistance.has_value(),
                c.vrWheelGrabAssist.has_value(), c.vrWheelResponse.has_value(), c.vrWheelTrackingGrace.has_value(),
                c.vrWheelHaptics.has_value(), c.vrCockpitItemHand.has_value(), c.vrCockpitItemThrow.has_value(),
                c.vrCockpitItemSize.has_value(), c.vrHandTracking.has_value(), c.vrObjectCulling.has_value(),
            };
            size_t count = 0;
            for (const bool value : set) count += value ? 1 : 0;
            return std::pair{count, std::size(set)};
        };
        std::istringstream before(original);
        const auto [setBefore, total] = preferencesSet(RuntimeConfigFile::ParseConfig(before, "before.toml"));
        if (!Require(setBefore == total, "a VR preference in the reset test does not parse")) return 1;
        const auto reset = RuntimeConfigFile::RemoveSettingsText(original, "vr", RuntimeConfigFile::kVrPreferenceKeys);
        if (!Require(reset.has_value(), "the VR preferences were not removed")) return 1;
        std::istringstream after(*reset);
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(after, "after.toml");
        if (!Require(preferencesSet(config).first == 0, "a VR preference survived the reset")) return 1;
        if (!Require(config.vrEnabled == std::optional<bool>{false} && config.vrControllerMode == "gamepad" &&
                         config.vrDirectPresentation == std::optional<bool>{false} &&
                         config.vrMultiview == std::optional<bool>{false} && config.vrWorldUnitsPerMeter &&
                         config.showFps == std::optional<bool>{true},
                     "the reset removed a setting it keeps")) return 1;
        if (!Require(reset->find("# Race view") == std::string::npos, "a removed key's comment was left behind"))
            return 1;
        if (!Require(RuntimeConfigFile::RemoveSettingsText("[video]\nshow_fps = true\n", "vr",
                                                           RuntimeConfigFile::kVrPreferenceKeys) ==
                         std::optional<std::string>{"[video]\nshow_fps = true\n"},
                     "a document without [vr] was rewritten")) return 1;
        if (!Require(!RuntimeConfigFile::RemoveSettingsText("[vr\nfirst_person = true\n", "vr",
                                                            RuntimeConfigFile::kVrPreferenceKeys),
                     "an unparseable document was rewritten")) return 1;
    }
    return 0;
}
