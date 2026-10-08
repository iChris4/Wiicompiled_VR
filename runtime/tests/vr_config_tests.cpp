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
    for (std::string_view hand : {"left", "right", "off"}) {
        Require(Parse("[vr]\ncockpit_item_hand = \"" + std::string(hand) + "\"\n")
                    .vrCockpitItemHand == std::string(hand));
    }
    Require(std::string_view(RuntimeConfigFile::kVrCockpitItemHandDefault) == "left");
    Require(!Parse("[vr]\n").vrCockpitItemHand.has_value());
    Require(!Parse("[vr]\ncockpit_item_hand = 1\n").vrCockpitItemHand.has_value());
    Require(RuntimeConfigFile::kVrCockpitItemThrowDefault);
    Require(Parse("[vr]\ncockpit_item_throw = false\n").vrCockpitItemThrow == false);
    Require(Parse("[vr]\ncockpit_item_throw = true\n").vrCockpitItemThrow == true);
    Require(!Parse("[vr]\n").vrCockpitItemThrow.has_value());
    // [vr] placeholder_steering_wheel: the separate VR wheel or handlebar is opt-in.
    Require(!RuntimeConfigFile::kVrPlaceholderSteeringWheelDefault);
    Require(Parse("[vr]\nplaceholder_steering_wheel = true\n").vrPlaceholderSteeringWheel == true);
    Require(Parse("[vr]\nplaceholder_steering_wheel = false\n").vrPlaceholderSteeringWheel == false);
    Require(!Parse("[vr]\n").vrPlaceholderSteeringWheel.has_value());
    // [vr] foveation: the Quest's foveated rendering level, index-matched to
    // aurora_set_stereo_foveation.
    for (std::string_view level : RuntimeConfigFile::kVrFoveationLevels) {
        Require(Parse("[vr]\nfoveation = \"" + std::string(level) + "\"\n").vrFoveation == std::string(level));
    }
    Require(!Parse("[vr]\nfoveation = \"ultra\"\n").vrFoveation.has_value());
    Require(!Parse("[vr]\nfoveation = 2\n").vrFoveation.has_value());
    Require(!Parse("[vr]\n").vrFoveation.has_value());
#if defined(__ANDROID__)
    Require(std::string_view(RuntimeConfigFile::kVrFoveationDefault) == "medium");
#else
    Require(std::string_view(RuntimeConfigFile::kVrFoveationDefault) == "off");
#endif
    Require(RuntimeConfigFile::VrFoveationLevelIndex("off") == 0);
    Require(RuntimeConfigFile::VrFoveationLevelIndex("low") == 1);
    Require(RuntimeConfigFile::VrFoveationLevelIndex("medium") == 2);
    Require(RuntimeConfigFile::VrFoveationLevelIndex("high") == 3);
    Require(RuntimeConfigFile::VrFoveationLevelIndex("ultra") == 0);

    // [vr] hand_tracking: the cockpit hands follow the headset's hand tracking.
    Require(Parse("[vr]\nhand_tracking = true\n").vrHandTracking == true);
    Require(Parse("[vr]\nhand_tracking = false\n").vrHandTracking == false);
    Require(!Parse("[vr]\n").vrHandTracking.has_value());
    Require(!Parse("[vr]\nhand_tracking = 1\n").vrHandTracking.has_value());
    Require(!RuntimeConfigFile::kVrHandTrackingDefault);

    // [vr] object_culling: false draws what the game camera culls. The PC
    // defaults to that; the Quest keeps the game's culling.
    Require(Parse("[vr]\nobject_culling = true\n").vrObjectCulling == true);
    Require(Parse("[vr]\nobject_culling = false\n").vrObjectCulling == false);
    Require(!Parse("[vr]\n").vrObjectCulling.has_value());
    Require(!Parse("[vr]\nobject_culling = 0\n").vrObjectCulling.has_value());
#if defined(__ANDROID__)
    Require(RuntimeConfigFile::kVrObjectCullingDefault);
    Require(std::string_view(MKW_VR_OBJECT_CULLING_DEFAULT_TOML) == "true");
#else
    Require(!RuntimeConfigFile::kVrObjectCullingDefault);
    Require(std::string_view(MKW_VR_OBJECT_CULLING_DEFAULT_TOML) == "false");
#endif

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

    // [vr] body_ik and body_ik_hands_only: the player's own character in the
    // cockpit seat, or only its hands. The getters read the user's file, so
    // their fallback is checked the way they apply it.
    Require(RuntimeConfigFile::kVrBodyIkDefault);
    Require(!RuntimeConfigFile::kVrBodyIkHandsOnlyDefault);
    Require(Parse("[vr]\nbody_ik = true\n").vrBodyIk == true);
    Require(Parse("[vr]\nbody_ik = false\n").vrBodyIk == false);
    Require(!Parse("[vr]\n").vrBodyIk.has_value());
    Require(!Parse("[vr]\nbody_ik = 1\n").vrBodyIk.has_value());
    Require(Parse("[vr]\n").vrBodyIk.value_or(RuntimeConfigFile::kVrBodyIkDefault));
    Require(Parse("[vr]\nbody_ik_hands_only = true\n").vrBodyIkHandsOnly == true);
    Require(Parse("[vr]\nbody_ik_hands_only = false\n").vrBodyIkHandsOnly == false);
    Require(!Parse("[vr]\n").vrBodyIkHandsOnly.has_value());
    Require(!Parse("[vr]\nbody_ik_hands_only = \"yes\"\n").vrBodyIkHandsOnly.has_value());
    Require(!Parse("[vr]\n").vrBodyIkHandsOnly.value_or(RuntimeConfigFile::kVrBodyIkHandsOnlyDefault));

    // [vr] bullet_bill_camera: ride inside the bullet, or show the game's camera.
    Require(std::string_view(RuntimeConfigFile::kVrBulletBillCameraDefault) == "inside");
    for (std::string_view camera : {"inside", "game"}) {
        Require(Parse("[vr]\nbullet_bill_camera = \"" + std::string(camera) + "\"\n").vrBulletBillCamera ==
                std::string(camera));
    }
    Require(!Parse("[vr]\n").vrBulletBillCamera.has_value());
    Require(!Parse("[vr]\nbullet_bill_camera = \"chase\"\n").vrBulletBillCamera.has_value());
    Require(!Parse("[vr]\nbullet_bill_camera = \"Game\"\n").vrBulletBillCamera.has_value());
    Require(!Parse("[vr]\nbullet_bill_camera = 1\n").vrBulletBillCamera.has_value());
    Require(Parse("[vr]\nbullet_bill_camera = \"chase\"\n")
                .vrBulletBillCamera.value_or(RuntimeConfigFile::kVrBulletBillCameraDefault) == "inside");
    return 0;
}
