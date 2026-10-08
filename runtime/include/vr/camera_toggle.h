// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The right thumbstick click that switches the race in progress between first
// person and the game's camera, on the VR controllers and on any gamepad while
// VR runs (F10 > VR > first_person_toggle_click). Kept free of OpenXR and SDL so it is tested
// headlessly (tests/vr_camera_toggle_tests.cpp).

namespace mkw::vr {

// One clean click of a button: it fires on release, and only if its partner
// (the other thumbstick, which with it opens the headset settings panel on a
// gamepad) stayed up and nothing owned the controllers at any point during the
// press. Firing on release is what lets a two-stick chord pass untouched.
class ClickToggle {
public:
    bool Update(bool held, bool partner_held, bool blocked) noexcept {
        if (held) {
            if (!held_) {
                held_ = true;
                spoiled_ = partner_held || blocked;
            } else if (partner_held || blocked) {
                spoiled_ = true;
            }
            return false;
        }
        const bool fire = held_ && !spoiled_ && !blocked;
        held_ = false;
        spoiled_ = false;
        return fire;
    }
    void Reset() noexcept {
        held_ = false;
        spoiled_ = false;
    }
    bool Held() const noexcept { return held_; }

private:
    bool held_ = false;
    bool spoiled_ = false;
};

} // namespace mkw::vr
