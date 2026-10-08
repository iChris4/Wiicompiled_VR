// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Which camera a race shows in VR (camera modes, after heurazy's fork). Kept
// free of guest memory so the rules are tested headlessly
// (tests/vr_camera_mode_tests.cpp).

#include <cstdint>

namespace mkw::vr {

enum class CameraMode : uint8_t {
    // The game's own camera, behind the kart.
    Game,
    // The anchor at the driver's head (the cockpit or the custom seat).
    FirstPerson,
};

inline CameraMode NextCameraMode(CameraMode mode) noexcept {
    return mode == CameraMode::FirstPerson ? CameraMode::Game : CameraMode::FirstPerson;
}

// RaceManager::stage (mkw-master src/system/RaceManager.hpp): 0 is the course
// preview, 1 the countdown (with the pan down to the kart), 2 the race and 3 the
// finish. Anything past the preview ends the intro.
inline bool RaceIntroOver(uint32_t stage) noexcept {
    return stage >= 1 && stage <= 3;
}

// The camera of the race in progress. Each race shows the game's own camera
// through its intro, then starts on the configured default camera; a choice
// made during the race (the thumbstick click, F10 "This race") lasts until the
// race ends and is never saved. Outside races and during the intro a choice is
// refused, so it cannot leak into the next race.
class RaceCameraChoice {
public:
    // RaceScene entry and exit.
    void Reset() noexcept { *this = {}; }

    // Once per race frame, with the stage read from the guest (or none when the
    // read failed). Returns true on the frame the intro ends.
    bool Observe(bool stage_valid, uint32_t stage, bool default_first_person) noexcept {
        in_race_ = true;
        if (!intro_pending_ || !stage_valid || !RaceIntroOver(stage)) {
            return false;
        }
        intro_pending_ = false;
        selected_ = default_first_person ? CameraMode::FirstPerson : CameraMode::Game;
        return true;
    }

    bool Select(CameraMode mode) noexcept {
        if (!in_race_ || intro_pending_) {
            return false;
        }
        selected_ = mode;
        return true;
    }

    bool InRace() const noexcept { return in_race_; }
    bool IntroPending() const noexcept { return intro_pending_; }
    // The player's choice for this race, whatever is overriding it right now.
    CameraMode Selected() const noexcept { return selected_; }
    // What the race shows: the game's camera through the intro and, when asked
    // for, while the player is a Bullet Bill.
    CameraMode Effective(bool bullet_game_camera) const noexcept {
        return intro_pending_ || bullet_game_camera ? CameraMode::Game : selected_;
    }

private:
    bool in_race_ = false;
    bool intro_pending_ = true;
    CameraMode selected_ = CameraMode::Game;
};

} // namespace mkw::vr
