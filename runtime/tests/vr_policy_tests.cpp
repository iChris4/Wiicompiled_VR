// SPDX-License-Identifier: GPL-3.0-or-later
#include "vr/mkw_vr_policy.h"

#include <cstring>
#include <iostream>

using namespace mkw::vr;

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << message << '\n';
            ++failures;
        }
    };
    MkwVRPolicyReset();
    MkwVRPolicyConfig config{};
    config.enabled = true;
    MkwVRPolicyConfigure(config);
    MkwVRPolicySetSessionActive(true);
    MkwVRPolicySetAvailableBindings(kMkwVRRequiredImmersiveBindings);
    MkwVRSceneObservation scene{VRSceneMode::Race, 1, 10};
    MkwVRCameraObservation camera{};
    camera.view_from_world = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    camera.guest_camera_address = 0x81000000;
    camera.guest_frame_index = 10;
    camera.valid = true;
    MkwVRPolicyPublishScene(scene);
    MkwVRPolicyPublishRaceCamera(camera);
    uint64_t previous_tag = 0;
    for (uint32_t players : {1u, 2u, 3u, 4u, 2u, 1u}) {
        scene.local_player_count = players;
        MkwVRPolicyPublishScene(scene);
        const auto snapshot = MkwVRPolicyGetSnapshot();
        check(snapshot.presentation == VRPresentationMode::ImmersiveRace, "1-4 players should be immersive");
        check(snapshot.content_tag != previous_tag, "layout changes must invalidate retained packets");
        previous_tag = snapshot.content_tag;
        MkwVRPolicyPublishScene(scene);
        MkwVRPolicyPublishRaceCamera(camera);
        check(MkwVRPolicyGetSnapshot().content_tag == previous_tag, "steady frames must keep the same tag");
    }
    camera.guest_frame_index = 11;
    MkwVRPolicyPublishRaceCamera(camera);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::ImmersiveRace,
          "adjacent camera publication is coherent");
    camera.guest_frame_index = 12;
    MkwVRPolicyPublishRaceCamera(camera);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "stale camera fails safe");
    camera.guest_frame_index = 10;
    MkwVRPolicyPublishRaceCamera(camera);
    for (uint32_t players : {0u, 5u, UINT32_MAX}) {
        scene.local_player_count = players;
        MkwVRPolicyPublishScene(scene);
        check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "invalid counts fail safe");
    }
    scene.local_player_count = 4;
    MkwVRPolicyPublishScene(scene);
    const uint32_t nan_bits = 0x7fc00000;
    std::memcpy(&camera.view_from_world[0], &nan_bits, sizeof(nan_bits));
    MkwVRPolicyPublishRaceCamera(camera);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "NaN camera fails under fast math");
    camera.view_from_world[0] = 1;
    MkwVRPolicyPublishRaceCamera(camera);
    MkwVRPolicySetAvailableBindings(MkwVRBindingRaceCamera);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "partial hooks fail safe");
    MkwVRPolicySetAvailableBindings(kMkwVRRequiredImmersiveBindings);
    scene.mode = VRSceneMode::FrontEnd;
    MkwVRPolicyPublishScene(scene);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "menus use virtual screen");
    scene.mode = VRSceneMode::Race;
    MkwVRPolicyPublishScene(scene);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen, "race entry needs fresh camera");
    MkwVRPolicyPublishRaceCamera(camera);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::ImmersiveRace, "race resumes with fresh camera");
    {
        const uint64_t immersive_tag = MkwVRPolicyGetSnapshot().content_tag;
        MkwVRPolicySetFirstPersonEngaged(true);
        MkwVRPolicySetImmersiveRaces(false);
        const auto flat = MkwVRPolicyGetSnapshot();
        check(flat.presentation == VRPresentationMode::VirtualScreen, "Flat Screen mode keeps races on the screen");
        check(flat.content_tag != immersive_tag, "Flat Screen mode must invalidate immersive packets");
        check(!flat.first_person_engaged, "Flat Screen mode never engages first person");
        check(MkwVRPolicyRouteDraw(VRDrawClass::PerspectiveWorld, flat) == VRDrawRoute::VirtualScreen,
              "Flat Screen mode routes the race world to the screen");
        MkwVRPolicySetFirstPersonEngaged(false);
        MkwVRPolicySetImmersiveRaces(true);
        const auto resumed = MkwVRPolicyGetSnapshot();
        check(resumed.presentation == VRPresentationMode::ImmersiveRace, "leaving Flat Screen mode resumes the race");
        check(resumed.content_tag != flat.content_tag, "leaving Flat Screen mode must invalidate screen packets");
    }
    {
        // [vr] flat_race_intro: the intro stays on the screen until the countdown.
        const auto before = MkwVRPolicyGetSnapshot();
        check(before.race_intro_pending, "a race starts in its intro");
        check(before.presentation == VRPresentationMode::ImmersiveRace,
              "without flat_race_intro the intro is immersive");
        MkwVRPolicySetRaceIntroOnScreen(true);
        const auto intro = MkwVRPolicyGetSnapshot();
        check(intro.presentation == VRPresentationMode::VirtualScreen, "flat_race_intro keeps the intro on the screen");
        check(intro.content_tag != before.content_tag, "flat_race_intro must invalidate immersive packets");
        check(MkwVRPolicyRouteDraw(VRDrawClass::PerspectiveWorld, intro) == VRDrawRoute::VirtualScreen,
              "flat_race_intro routes the intro's world to the screen");
        MkwVRPolicyPublishScene(scene);
        MkwVRPolicyPublishRaceCamera(camera);
        check(MkwVRPolicyGetSnapshot().content_tag == intro.content_tag, "steady intro frames keep the same tag");
        MkwVRPolicySetRaceIntroPending(false);
        const auto countdown = MkwVRPolicyGetSnapshot();
        check(countdown.presentation == VRPresentationMode::ImmersiveRace, "the race turns immersive at the countdown");
        check(countdown.content_tag != intro.content_tag, "the countdown must invalidate screen packets");
        MkwVRPolicySetRaceIntroPending(false);
        check(MkwVRPolicyGetSnapshot().content_tag == countdown.content_tag, "a repeated intro end changes nothing");
        MkwVRPolicySetImmersiveRaces(false);
        check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen,
              "Flat Screen mode still keeps the race on the screen after the intro");
        MkwVRPolicySetImmersiveRaces(true);
        MkwVRPolicySetRaceIntroPending(true);
        check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::VirtualScreen,
              "the next race's intro is on the screen again");
        MkwVRPolicySetRaceIntroOnScreen(false);
        const auto live = MkwVRPolicyGetSnapshot();
        check(live.presentation == VRPresentationMode::ImmersiveRace, "turning flat_race_intro off mid-intro is live");
        MkwVRPolicySetRaceIntroPending(false);
        check(MkwVRPolicyGetSnapshot().content_tag == live.content_tag,
              "without flat_race_intro the countdown changes nothing");
    }
    MkwVRPolicySetSessionActive(false);
    check(MkwVRPolicyGetSnapshot().presentation == VRPresentationMode::Desktop, "inactive session uses desktop");
    return failures == 0 ? 0 : 1;
}
