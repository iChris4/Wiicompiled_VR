// SPDX-License-Identifier: GPL-3.0-or-later
//
// The online room and race as Mario Kart Wii holds them, read for voice chat: who is in the RKNet
// room (their RWFC profile ids), the friend roster, team assignments, and where each online
// racer's kart is. Read-only; every pointer is checked and any fault yields "nothing".
//
// Addresses by PAL RMCP01 identity (MKW_GADDR maps them to NTSC-U and NTSC-J). The room offsets
// are the ones MKW-VoiceChat reads (retro_rewind_voice_bridge.cpp, from Retro Rewind's own
// RKNet::Controller layout), cross-checked against the generated translation:
//   RKNet::Controller instance   0x809C20D8 (Controller::CreateInstance 0x80655B24, 0x29C8 bytes)
//     +0xE8   room type, 0 = none
//     +0x291C current match-info slot (0/1); match infos at +0x38, 0x58 bytes each
//             +0x08 connected consoles, +0x10 AID bitmap, +0x18 group id, +0x21 local AID
//     +0x2920 player id -> AID, 12 bytes, 0xFF = none (ResetAidsBelongingToPlayerIds 0x80659D20)
//   DWC match control            0x8038630C: 32 nodes at +0x38, 0x30 bytes; pid +0, AID +0x16
//   FriendMgr instance           0x809C2110: 30 friend pids at +0x36C
//   Racedata instance            0x809BD728: race scenario at +0x20 (count +0x4, players +0x8,
//             0xF0 each, team +0xCC), menu scenario at +0xC10, settings +0xB48 (mode flags +0x28,
//             0x2 = teams)
//   Kart::Manager instance       0x809C18F8: +0x20 players[i] -> proxy -> accessor; position =
//             accessor +0x08 body, +0x90 physics, +0x4 dynamics, +0x68 (Kart::Link::GetKartPosition
//             0x8059020C, the walk mkw_vr_first_person.cpp uses)

#pragma once

#include "guest_abi/guest_region.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mkw::voicechat::detail {

inline constexpr std::uint32_t kMaxAids = 12;

struct RkNetRoomRead {
    bool valid = false;   // the controller and the current match info could be read
    bool inRoom = false;  // room type set, our AID present, another console connected
    std::uint32_t controller = 0;
    std::uint32_t aidBitmap = 0;
    std::uint32_t groupId = 0;
    std::uint8_t localAid = 0xFF;
};

template <typename GuestMemory>
RkNetRoomRead ReadRkNetRoom() noexcept {
    const std::uint32_t kControllerInstance = MKW_GADDR(809C20D8);
    constexpr std::uint32_t kRoomType = 0xE8u;
    constexpr std::uint32_t kMatchInfos = 0x38u;
    constexpr std::uint32_t kMatchInfoSize = 0x58u;
    constexpr std::uint32_t kCurrentMatchInfo = 0x291Cu;
    constexpr std::uint32_t kPlayerAidMap = 0x2920u;
    constexpr std::uint32_t kConnectedConsoles = 0x08u;
    constexpr std::uint32_t kAidBitmap = 0x10u;
    constexpr std::uint32_t kGroupId = 0x18u;
    constexpr std::uint32_t kLocalAid = 0x21u;

    RkNetRoomRead read;
    try {
        std::uint32_t controller = 0;
        if (!GuestMemory::TryRead32(kControllerInstance, controller) || controller == 0 ||
            !GuestMemory::Contains(controller, kPlayerAidMap + kMaxAids)) {
            return read;
        }
        const std::uint32_t current = GuestMemory::Read32(controller + kCurrentMatchInfo);
        if (current > 1) {
            return read;
        }
        const std::uint32_t match = controller + kMatchInfos + current * kMatchInfoSize;
        read.controller = controller;
        read.aidBitmap = GuestMemory::Read32(match + kAidBitmap);
        read.groupId = GuestMemory::Read32(match + kGroupId);
        read.localAid = GuestMemory::Read8(match + kLocalAid);
        read.valid = true;
        const std::uint32_t roomType = GuestMemory::Read32(controller + kRoomType);
        const std::uint32_t connected = GuestMemory::Read32(match + kConnectedConsoles);
        // The game's own "match found": our AID present and another console connected.
        read.inRoom = roomType != 0 && read.localAid < kMaxAids && connected > 1 &&
                      (read.aidBitmap & (1u << read.localAid)) != 0;
    } catch (const typename GuestMemory::AccessViolation&) {
        read = {};
    }
    return read;
}

// Profile id per AID (0 = unknown), from DWC's node list, limited to AIDs in the room.
template <typename GuestMemory>
std::array<std::uint32_t, kMaxAids> ReadAidProfileIds(std::uint32_t aidBitmap) noexcept {
    const std::uint32_t kMatchControlInstance = MKW_GADDR(8038630C);
    constexpr std::uint32_t kNodes = 0x38u;
    constexpr std::uint32_t kNodeSize = 0x30u;
    constexpr std::uint32_t kNodeCount = 32u;
    constexpr std::uint32_t kNodePid = 0x0u;
    constexpr std::uint32_t kNodeAid = 0x16u;

    std::array<std::uint32_t, kMaxAids> pids{};
    try {
        std::uint32_t control = 0;
        if (!GuestMemory::TryRead32(kMatchControlInstance, control) || control == 0 ||
            !GuestMemory::Contains(control + kNodes, kNodeSize * kNodeCount)) {
            return pids;
        }
        for (std::uint32_t i = 0; i < kNodeCount; ++i) {
            const std::uint32_t node = control + kNodes + i * kNodeSize;
            const std::uint32_t pid = GuestMemory::Read32(node + kNodePid);
            const std::uint8_t aid = GuestMemory::Read8(node + kNodeAid);
            if (pid != 0 && aid < kMaxAids && (aidBitmap & (1u << aid)) != 0 && pids[aid] == 0) {
                pids[aid] = pid;
            }
        }
    } catch (const typename GuestMemory::AccessViolation&) {
        pids = {};
    }
    return pids;
}

template <typename GuestMemory>
std::array<std::uint8_t, kMaxAids> ReadPlayerAids(std::uint32_t controller) noexcept {
    constexpr std::uint32_t kPlayerAidMap = 0x2920u;
    std::array<std::uint8_t, kMaxAids> aids;
    aids.fill(0xFF);
    try {
        for (std::uint32_t player = 0; player < kMaxAids; ++player) {
            aids[player] = GuestMemory::Read8(controller + kPlayerAidMap + player);
        }
    } catch (const typename GuestMemory::AccessViolation&) {
        aids.fill(0xFF);
    }
    return aids;
}

template <typename GuestMemory>
std::vector<std::uint32_t> ReadFriendProfileIds() noexcept {
    const std::uint32_t kFriendMgrInstance = MKW_GADDR(809C2110);
    constexpr std::uint32_t kFriendPids = 0x36Cu;
    constexpr std::uint32_t kFriendCount = 30u;
    std::vector<std::uint32_t> friends;
    try {
        std::uint32_t manager = 0;
        if (!GuestMemory::TryRead32(kFriendMgrInstance, manager) || manager == 0 ||
            !GuestMemory::Contains(manager + kFriendPids, kFriendCount * 4u)) {
            return friends;
        }
        for (std::uint32_t i = 0; i < kFriendCount; ++i) {
            const std::uint32_t pid = GuestMemory::Read32(manager + kFriendPids + i * 4u);
            if (pid != 0) friends.push_back(pid);
        }
    } catch (const typename GuestMemory::AccessViolation&) {
        friends.clear();
    }
    return friends;
}

// Team per race player (0/1, 0xFF unknown) when the race or the menu scenario is a team mode.
template <typename GuestMemory>
bool ReadRaceTeams(std::array<std::uint8_t, kMaxAids>& teams) noexcept {
    const std::uint32_t kRacedataInstance = MKW_GADDR(809BD728);
    constexpr std::uint32_t kScenarios[] = {0x20u, 0xC10u};
    constexpr std::uint32_t kPlayerCount = 0x4u;
    constexpr std::uint32_t kPlayers = 0x8u;
    constexpr std::uint32_t kPlayerSize = 0xF0u;
    constexpr std::uint32_t kPlayerTeam = 0xCCu;
    constexpr std::uint32_t kModeFlags = 0xB48u + 0x28u;
    constexpr std::uint32_t kTeamsEnabled = 0x2u;
    teams.fill(0xFF);
    try {
        std::uint32_t racedata = 0;
        if (!GuestMemory::TryRead32(kRacedataInstance, racedata) || racedata == 0) {
            return false;
        }
        for (const std::uint32_t offset : kScenarios) {
            const std::uint32_t scenario = racedata + offset;
            if (!GuestMemory::Contains(scenario, kModeFlags + 4u) ||
                (GuestMemory::Read32(scenario + kModeFlags) & kTeamsEnabled) == 0) {
                continue;
            }
            const std::uint32_t count = GuestMemory::Read8(scenario + kPlayerCount);
            if (count == 0 || count > kMaxAids) {
                continue;
            }
            bool any = false;
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t team = GuestMemory::Read32(scenario + kPlayers + i * kPlayerSize + kPlayerTeam);
                if (team < 2) {
                    teams[i] = static_cast<std::uint8_t>(team);
                    any = true;
                }
            }
            if (any) {
                return true;
            }
        }
    } catch (const typename GuestMemory::AccessViolation&) {
    }
    teams.fill(0xFF);
    return false;
}

struct VoiceRoomRead {
    bool inRoom = false;
    std::uint32_t roomKey = 0;
    std::vector<std::uint32_t> roomProfileIds;  // sorted, unique, includes ours
    std::vector<std::uint32_t> friendProfileIds;
    bool teamMode = false;
    std::vector<std::uint32_t> teamProfileIds;
    std::vector<std::uint32_t> teammateProfileIds;
};

template <typename GuestMemory>
VoiceRoomRead ReadVoiceRoom(std::uint32_t ownProfileId) noexcept {
    VoiceRoomRead result;
    result.friendProfileIds = ReadFriendProfileIds<GuestMemory>();
    const RkNetRoomRead room = ReadRkNetRoom<GuestMemory>();
    if (!room.inRoom) {
        return result;
    }
    result.inRoom = true;
    result.roomKey = room.groupId;
    const auto aidPids = ReadAidProfileIds<GuestMemory>(room.aidBitmap);
    if (ownProfileId != 0) result.roomProfileIds.push_back(ownProfileId);
    for (const auto pid : aidPids) {
        if (pid != 0) result.roomProfileIds.push_back(pid);
    }
    std::sort(result.roomProfileIds.begin(), result.roomProfileIds.end());
    result.roomProfileIds.erase(std::unique(result.roomProfileIds.begin(), result.roomProfileIds.end()),
                                result.roomProfileIds.end());

    std::array<std::uint8_t, kMaxAids> teams;
    if (!ReadRaceTeams<GuestMemory>(teams)) {
        return result;
    }
    const auto playerAids = ReadPlayerAids<GuestMemory>(room.controller);
    std::array<bool, 2> localTeams{};
    std::array<std::uint8_t, kMaxAids> aidTeam;
    aidTeam.fill(0xFF);
    for (std::uint32_t player = 0; player < kMaxAids; ++player) {
        const std::uint8_t aid = playerAids[player];
        if (teams[player] >= 2 || aid >= kMaxAids) continue;
        aidTeam[aid] = teams[player];
        if (aid == room.localAid) localTeams[teams[player]] = true;
    }
    if (!localTeams[0] && !localTeams[1]) {
        return result;
    }
    result.teamMode = true;
    for (std::uint32_t aid = 0; aid < kMaxAids; ++aid) {
        if (aidPids[aid] == 0 || aidTeam[aid] >= 2) continue;
        result.teamProfileIds.push_back(aidPids[aid]);
        if (localTeams[aidTeam[aid]]) result.teammateProfileIds.push_back(aidPids[aid]);
    }
    return result;
}

struct RacerPosition {
    std::uint32_t profileId = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};  // world units
};

// Each online racer's kart position, one entry per profile (a second player on the same console
// shares the console's AID and profile; the lower player index wins). Our own racers are skipped.
template <typename GuestMemory>
std::vector<RacerPosition> ReadRacerPositions(const RkNetRoomRead& room, std::uint32_t ownProfileId) noexcept {
    const std::uint32_t kRacedataInstance = MKW_GADDR(809BD728);
    const std::uint32_t kKartManagerInstance = MKW_GADDR(809C18F8);
    constexpr std::uint32_t kRacePlayerCount = 0x20u + 0x4u;
    constexpr std::uint32_t kKartManagerPlayers = 0x20u;
    constexpr std::uint32_t kAccessorBody = 0x08u;
    constexpr std::uint32_t kBodyPhysics = 0x90u;
    constexpr std::uint32_t kPhysicsDynamics = 0x4u;
    constexpr std::uint32_t kDynamicsPosition = 0x68u;

    std::vector<RacerPosition> racers;
    if (!room.inRoom) {
        return racers;
    }
    const auto aidPids = ReadAidProfileIds<GuestMemory>(room.aidBitmap);
    const auto playerAids = ReadPlayerAids<GuestMemory>(room.controller);
    try {
        std::uint32_t racedata = 0;
        std::uint32_t manager = 0;
        std::uint32_t players = 0;
        if (!GuestMemory::TryRead32(kRacedataInstance, racedata) || racedata == 0 ||
            !GuestMemory::TryRead32(kKartManagerInstance, manager) || manager == 0 ||
            !GuestMemory::TryRead32(manager + kKartManagerPlayers, players) || players == 0) {
            return racers;
        }
        const std::uint32_t count = GuestMemory::Read8(racedata + kRacePlayerCount);
        if (count == 0 || count > kMaxAids) {
            return racers;
        }
        for (std::uint32_t player = 0; player < count; ++player) {
            const std::uint8_t aid = playerAids[player];
            if (aid >= kMaxAids || aid == room.localAid) continue;
            const std::uint32_t pid = aidPids[aid];
            if (pid == 0 || pid == ownProfileId) continue;
            if (std::any_of(racers.begin(), racers.end(), [&](const RacerPosition& r) { return r.profileId == pid; })) {
                continue;
            }
            std::uint32_t proxy = 0, accessor = 0, body = 0, physics = 0, dynamics = 0;
            if (!GuestMemory::TryRead32(players + player * 4u, proxy) || proxy == 0 ||
                !GuestMemory::TryRead32(proxy, accessor) || accessor == 0 ||
                !GuestMemory::TryRead32(accessor + kAccessorBody, body) || body == 0 ||
                !GuestMemory::TryRead32(body + kBodyPhysics, physics) || physics == 0 ||
                !GuestMemory::TryRead32(physics + kPhysicsDynamics, dynamics) || dynamics == 0 ||
                !GuestMemory::Contains(dynamics + kDynamicsPosition, 12u)) {
                continue;
            }
            RacerPosition racer;
            racer.profileId = pid;
            for (std::uint32_t axis = 0; axis < 3; ++axis) {
                racer.position[axis] = GuestMemory::ReadFloat32(dynamics + kDynamicsPosition + axis * 4u);
            }
            racers.push_back(racer);
        }
    } catch (const typename GuestMemory::AccessViolation&) {
        racers.clear();
    }
    return racers;
}

} // namespace mkw::voicechat::detail
