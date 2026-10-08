// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which camera a race shows in VR (vr/vr_controls.h): the game's camera
// through the intro, then the configured default, a choice that lasts only for
// the race in progress, and the Bullet Bill override.

#include "vr/vr_controls.h"

#include <cstdint>
#include <iostream>

namespace {

using mkw::vr::CameraMode;
using mkw::vr::NextCameraMode;
using mkw::vr::RaceCameraChoice;
using mkw::vr::RaceIntroOver;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

void TestNextCameraMode() {
    Check(NextCameraMode(CameraMode::Game) == CameraMode::FirstPerson, "the game camera toggles to first person");
    Check(NextCameraMode(CameraMode::FirstPerson) == CameraMode::Game, "first person toggles to the game camera");
    Check(NextCameraMode(NextCameraMode(CameraMode::Game)) == CameraMode::Game, "two toggles come back");
}

void TestRaceIntroOver() {
    Check(!RaceIntroOver(0), "the course preview is the intro");
    Check(RaceIntroOver(1), "the countdown ends the intro");
    Check(RaceIntroOver(2), "the race ends the intro");
    Check(RaceIntroOver(3), "the finish ends the intro");
    Check(!RaceIntroOver(4), "an unknown stage does not end the intro");
    Check(!RaceIntroOver(UINT32_MAX), "a garbage stage does not end the intro");
}

void TestOutsideRace() {
    RaceCameraChoice choice;
    Check(!choice.InRace(), "a new choice is outside a race");
    Check(choice.IntroPending(), "a new choice waits for the intro");
    Check(choice.Selected() == CameraMode::Game, "a new choice selects the game camera");
    Check(choice.Effective(false) == CameraMode::Game, "outside a race the game camera shows");
    Check(choice.Effective(true) == CameraMode::Game, "outside a race a Bullet Bill shows the game camera");
    Check(!choice.Select(CameraMode::FirstPerson), "a choice outside a race is refused");
    Check(choice.Selected() == CameraMode::Game, "a refused choice changes nothing");
}

void TestIntro() {
    RaceCameraChoice choice;
    Check(!choice.Observe(false, 2, true), "a failed stage read does not end the intro");
    Check(choice.InRace() && choice.IntroPending(), "a race frame enters the race, intro pending");
    Check(!choice.Select(CameraMode::FirstPerson), "a choice during the intro is refused");
    Check(!choice.Observe(true, 0, true), "the course preview keeps the intro");
    Check(!choice.Observe(true, 4, true), "an unknown stage keeps the intro");
    Check(choice.IntroPending() && choice.Selected() == CameraMode::Game, "the intro keeps the game camera");
    Check(choice.Effective(false) == CameraMode::Game, "the intro shows the game camera");

    Check(choice.Observe(true, 1, true), "the countdown ends the intro");
    Check(!choice.IntroPending(), "the intro is over");
    Check(choice.Selected() == CameraMode::FirstPerson, "the race starts on the default, first person");
    Check(choice.Effective(false) == CameraMode::FirstPerson, "the race shows first person");
    Check(choice.Effective(true) == CameraMode::Game, "a Bullet Bill with bullet_bill_camera = game shows the game camera");
    Check(choice.Selected() == CameraMode::FirstPerson, "the Bullet Bill override keeps the player's choice");
    Check(!choice.Observe(true, 2, true), "the intro ends once");
    Check(!choice.Observe(true, 2, false), "a later frame does not apply the default again");
    Check(choice.Selected() == CameraMode::FirstPerson, "the default is applied only when the intro ends");
    Check(!choice.Observe(true, 0, true) && !choice.IntroPending(), "a stage read back at zero does not restart the intro");
    Check(!choice.Observe(false, 0, true) && choice.Selected() == CameraMode::FirstPerson,
          "a failed stage read mid-race keeps the choice");

    Check(choice.Select(CameraMode::Game), "a choice during the race is taken");
    Check(choice.Selected() == CameraMode::Game && choice.Effective(false) == CameraMode::Game,
          "the race shows the chosen game camera");
    Check(choice.Select(NextCameraMode(choice.Selected())), "the toggle is taken");
    Check(choice.Selected() == CameraMode::FirstPerson, "the toggle returns to first person");
}

void TestGameDefault() {
    RaceCameraChoice choice;
    choice.Observe(true, 0, false);
    Check(choice.Observe(true, 1, false), "the countdown ends the intro");
    Check(choice.Selected() == CameraMode::Game, "a race starts on the default, the game camera");
    Check(choice.Effective(false) == CameraMode::Game, "the race shows the game camera");
    Check(choice.Select(CameraMode::FirstPerson), "first person can be chosen for the race");
    Check(choice.Effective(false) == CameraMode::FirstPerson, "the race shows the chosen first person");

    RaceCameraChoice joined;
    Check(joined.Observe(true, 3, true), "a race first seen at its finish has no intro left");
    Check(joined.Selected() == CameraMode::FirstPerson, "it still starts on the default");
}

void TestReset() {
    // The race before the reset is on first person, not the initial game
    // camera, so the reset is seen to clear the choice.
    RaceCameraChoice choice;
    choice.Observe(true, 1, true);
    Check(choice.Selected() == CameraMode::FirstPerson, "the race before the reset is on first person");
    choice.Reset();
    Check(!choice.InRace() && choice.IntroPending() && choice.Selected() == CameraMode::Game,
          "a reset returns to the initial state");
    Check(!choice.Select(CameraMode::FirstPerson), "a choice between races is refused");
    choice.Observe(true, 0, true);
    Check(choice.Effective(false) == CameraMode::Game, "the next race shows its intro on the game camera");
    Check(!choice.Select(CameraMode::Game), "the next race's intro refuses a choice");
    Check(choice.Observe(true, 1, true), "the next race's intro ends");
    Check(choice.Selected() == CameraMode::FirstPerson, "the next race starts on the default, not the last choice");

    // This race is on first person; the next one starts on a changed default.
    choice.Reset();
    choice.Observe(true, 1, false);
    Check(choice.Selected() == CameraMode::Game, "a changed default applies from the next race");
}

} // namespace

int main() {
    TestNextCameraMode();
    TestRaceIntroOver();
    TestOutsideRace();
    TestIntro();
    TestGameDefault();
    TestReset();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "vr camera mode tests passed\n";
    return 0;
}
