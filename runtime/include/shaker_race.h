// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace shaker_race {

// True while a race with at least one human racer is running. The secondary audio output
// uses it to stay silent in menus and in the title-screen demo race (all CPU racers).
//
// PAL RMCP01. Two checks, both on data the VR first-person code already relies on
// (src/vr/mkw_vr_first_person.cpp and include/vr/mkw_vr_player.h):
//   1. A race scene is up: the RaceInfo pointer at 0x809BD730 is set and its stage (+0x28)
//      is 1 to 4. The demo race passes this check too.
//   2. A human is racing: Racedata (pointer at 0x809BD728) holds the racer count at +0x24
//      and each racer's type at +0x38 + index * 0xF0, where 0 is a local human and 4 an
//      online human. CPU racers have other values.
//
// The memory provider is injectable so this can be tested without a running guest.
// `GuestMemory` needs `static bool TryRead32(uint32_t address, uint32_t& out)` and
// `static bool TryRead8(uint32_t address, uint32_t& out)`; both return false when the
// address is not readable.
template <typename GuestMemory>
bool IsHumanRaceRunning() noexcept {
    constexpr uint32_t kRaceInfoInstanceAddress = 0x809BD730u;
    constexpr uint32_t kRaceInfoStageOffset = 0x28u;
    constexpr uint32_t kFirstRaceStage = 1;
    constexpr uint32_t kLastRaceStage = 4;
    constexpr uint32_t kRaceDataInstanceAddress = 0x809BD728u;
    constexpr uint32_t kRacerCountOffset = 0x24u;
    constexpr uint32_t kRacerTypeOffset = 0x38u;
    constexpr uint32_t kRacerStride = 0xF0u;
    constexpr uint32_t kMaxRacers = 12;
    constexpr uint32_t kTypeRealLocal = 0;
    constexpr uint32_t kTypeRealOnline = 4;

    uint32_t raceInfo = 0;
    uint32_t stage = 0;
    if (!GuestMemory::TryRead32(kRaceInfoInstanceAddress, raceInfo) || raceInfo == 0 ||
        !GuestMemory::TryRead32(raceInfo + kRaceInfoStageOffset, stage) || stage < kFirstRaceStage ||
        stage > kLastRaceStage) {
        return false;
    }

    uint32_t raceData = 0;
    uint32_t racers = 0;
    if (!GuestMemory::TryRead32(kRaceDataInstanceAddress, raceData) || raceData == 0 ||
        !GuestMemory::TryRead8(raceData + kRacerCountOffset, racers) || racers < 1 || racers > kMaxRacers) {
        return false;
    }
    for (uint32_t i = 0; i < racers; ++i) {
        uint32_t type = 0;
        if (GuestMemory::TryRead32(raceData + kRacerTypeOffset + i * kRacerStride, type) &&
            (type == kTypeRealLocal || type == kTypeRealOnline)) {
            return true;
        }
    }
    return false;
}

} // namespace shaker_race
