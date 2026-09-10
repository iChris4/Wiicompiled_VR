// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace mkw::vr::detail {

struct LocalPlayerKartRead {
    const char* failed_step = nullptr;
    uint32_t race_data = 0;
    uint32_t player_index = 0xFFu;
    uint32_t manager = 0;
    uint32_t players = 0;
    uint32_t proxy = 0;
    uint32_t accessor = 0;
};

// PAL RMCP01. Use the same screen-to-racer mapping as RaceCameraMgr::__ct
// (0x805A8468), not a kart-array slot or a search for a non-CPU racer.
// The memory provider is injectable so the actual pointer walk can be tested
// without a running guest. Production uses Memory's checked, big-endian reads.
template <typename GuestMemory>
LocalPlayerKartRead ReadLocalPlayerKart() noexcept {
    // Racedata::CreateInstance (0x8052FE58): 0x809C0000 - 10456.
    constexpr uint32_t kRaceDataInstanceAddress = 0x809BD728u;
    // Racedata::GetPlayerIdOfLocalPlayer (0x80531F70): byte +2948 + screen.
    constexpr uint32_t kHudPlayerIdsOffset = 0xB84u;
    // Active race scenario, not the menu scenario. GetRacePlayerCount
    // (0x8052DD30) reads +0x24; the local count is the byte at +0x26.
    constexpr uint32_t kPlayerCountOffset = 0x24u;
    constexpr uint32_t kLocalPlayerCountOffset = 0x26u;
    // Scenario::GetPlayer (0x8052DD20): scenario +8 + index*0xF0.
    // Race scenario starts at +0x20; Player::GetPlayerType (0x8052ED20)
    // reads +0x10. TYPE_REAL_LOCAL is 0; TYPE_REAL_ONLINE is 4.
    constexpr uint32_t kPlayerTypeOffset = 0x38u;
    constexpr uint32_t kPlayerStride = 0xF0u;
    constexpr uint32_t kMaxPlayers = 12;
    // Kart::Manager::CreateInstance (0x8058FAA8), GetKartPlayer (0x80590100),
    // and Kart::Link's shared proxy -> accessor link.
    constexpr uint32_t kKartManagerInstanceAddress = 0x809C18F8u;
    constexpr uint32_t kKartManagerPlayersOffset = 0x20u;

    LocalPlayerKartRead read{};
    const auto pointer = [](uint32_t address, uint32_t& out) {
        return GuestMemory::TryRead32(address, out) && out != 0;
    };
    if (!pointer(kRaceDataInstanceAddress, read.race_data)) {
        read.failed_step = "Racedata instance";
        return read;
    }
    read.failed_step = "local racer mapping";
    if (!GuestMemory::Contains(read.race_data, kHudPlayerIdsOffset + 1u)) {
        return read;
    }
    try {
        const uint32_t player_count = GuestMemory::Read8(read.race_data + kPlayerCountOffset);
        if (player_count == 0 || player_count > kMaxPlayers ||
            GuestMemory::Read8(read.race_data + kLocalPlayerCountOffset) != 1) {
            return read;
        }
        // Immersive first person supports one local screen. Its racer may be
        // anywhere in the online roster. 0xFF means no racer is assigned.
        read.player_index = GuestMemory::Read8(read.race_data + kHudPlayerIdsOffset);
        if (read.player_index >= player_count ||
            GuestMemory::Read32(read.race_data + kPlayerTypeOffset +
                                read.player_index * kPlayerStride) != 0) {
            return read;
        }
    } catch (const typename GuestMemory::AccessViolation&) {
        return read;
    }

    if (!pointer(kKartManagerInstanceAddress, read.manager)) {
        read.failed_step = "Kart::Manager instance";
    } else if (!pointer(read.manager + kKartManagerPlayersOffset, read.players)) {
        read.failed_step = "Kart::Manager players array";
    } else if (!pointer(read.players + read.player_index * 4u, read.proxy)) {
        read.failed_step = "local player kart object";
    } else if (!pointer(read.proxy, read.accessor)) {
        read.failed_step = "local kart accessor";
    } else {
        read.failed_step = nullptr;
    }
    return read;
}

} // namespace mkw::vr::detail
