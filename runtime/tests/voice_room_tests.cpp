// SPDX-License-Identifier: GPL-3.0-or-later
// The voice chat's room and racer reads against a synthetic PAL guest: who is in the RKNet room,
// friends, teams, and each online racer's kart position, with a read fault at every link.

#include "voicechat/voice_room_mkw.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// The reads name guest addresses by PAL identity (MKW_GADDR); these tests run as PAL RMCP01.
extern "C" const uint8_t kMkwGuestRegion = 0;

namespace {

namespace detail = mkw::voicechat::detail;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

struct GuestMemory {
    using AccessViolation = std::out_of_range;
    inline static std::unordered_map<uint32_t, uint8_t> bytes;
    inline static uint32_t fault_address = 0;

    static bool Contains(uint32_t address, uint32_t size) {
        for (uint32_t i = 0; i < size; ++i) {
            if (bytes.count(address + i) == 0) return false;
        }
        return true;
    }
    static uint8_t Read8(uint32_t address) {
        if (address == fault_address) throw AccessViolation("injected read fault");
        return bytes.at(address);
    }
    static uint32_t Read32(uint32_t address) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i) value = (value << 8) | Read8(address + i);
        return value;
    }
    static float ReadFloat32(uint32_t address) {
        const uint32_t raw = Read32(address);
        float value;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }
    static bool TryRead32(uint32_t address, uint32_t& out) {
        try {
            out = Read32(address);
            return true;
        } catch (const AccessViolation&) {
            return false;
        }
    }
    static void Write8(uint32_t address, uint8_t value) { bytes[address] = value; }
    static void Write32(uint32_t address, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) bytes[address + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
    }
    static void WriteFloat(uint32_t address, float value) {
        uint32_t raw;
        std::memcpy(&raw, &value, sizeof(raw));
        Write32(address, raw);
    }
    static void Fill(uint32_t address, uint32_t size) {
        for (uint32_t i = 0; i < size; ++i) bytes.emplace(address + i, 0);
    }
};

constexpr uint32_t kController = 0x81000000u;
constexpr uint32_t kMatchControl = 0x81100000u;
constexpr uint32_t kFriendMgr = 0x81200000u;
constexpr uint32_t kRacedata = 0x81300000u;
constexpr uint32_t kKartManager = 0x81400000u;
constexpr uint32_t kKartPlayers = 0x81400100u;
constexpr uint32_t kKartObjects = 0x81500000u;  // per player: proxy, accessor, body, physics, dynamics

constexpr uint32_t kOwnPid = 500000000u;

void BuildRoom() {
    GuestMemory::bytes.clear();
    GuestMemory::fault_address = 0;
    // Room: our console is AID 1; AIDs 0, 1 and 3 present (bitmap 0b1011), slot 1 current.
    GuestMemory::Write32(0x809C20D8u, kController);
    GuestMemory::Fill(kController, 0x2920 + 12);
    GuestMemory::Write32(kController + 0xE8, 2);
    GuestMemory::Write32(kController + 0x291C, 1);
    const uint32_t match = kController + 0x38 + 0x58;
    GuestMemory::Write32(match + 0x08, 3);
    GuestMemory::Write32(match + 0x10, 0b1011u);
    GuestMemory::Write32(match + 0x18, 0xCAFEu);
    GuestMemory::Write8(match + 0x21, 1);
    // Player -> AID: players 0..3 on AIDs 3, 1, 0, 1 (a guest on our console).
    const uint8_t aids[12] = {3, 1, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (uint32_t i = 0; i < 12; ++i) GuestMemory::Write8(kController + 0x2920 + i, aids[i]);

    // DWC nodes: AID 0 -> 600000000, AID 1 -> us, AID 3 -> 700000000, AID 2 (absent) -> 800000000.
    GuestMemory::Write32(0x8038630Cu, kMatchControl);
    GuestMemory::Fill(kMatchControl + 0x38, 32 * 0x30);
    const struct {
        uint32_t pid;
        uint8_t aid;
    } nodes[] = {{600000000u, 0}, {kOwnPid, 1}, {800000000u, 2}, {700000000u, 3}};
    for (uint32_t i = 0; i < 4; ++i) {
        GuestMemory::Write32(kMatchControl + 0x38 + i * 0x30, nodes[i].pid);
        GuestMemory::Write8(kMatchControl + 0x38 + i * 0x30 + 0x16, nodes[i].aid);
    }
    for (uint32_t i = 4; i < 32; ++i) GuestMemory::Write8(kMatchControl + 0x38 + i * 0x30 + 0x16, 0xFF);

    GuestMemory::Write32(0x809C2110u, kFriendMgr);
    GuestMemory::Fill(kFriendMgr + 0x36C, 30 * 4);
    GuestMemory::Write32(kFriendMgr + 0x36C + 4, 700000000u);

    // Racedata: 4 racers, race scenario team mode with players 0 and 1 on team 1.
    GuestMemory::Write32(0x809BD728u, kRacedata);
    GuestMemory::Fill(kRacedata, 0xC10 + 0xB48 + 0x2C);
    GuestMemory::Write8(kRacedata + 0x20 + 0x4, 4);
    GuestMemory::Write32(kRacedata + 0x20 + 0xB48 + 0x28, 0x2);
    const uint32_t teams[4] = {1, 1, 0, 1};
    for (uint32_t i = 0; i < 4; ++i) GuestMemory::Write32(kRacedata + 0x20 + 0x8 + i * 0xF0 + 0xCC, teams[i]);

    // Karts: player i at (100 * (i + 1), 10, -50 * i).
    GuestMemory::Write32(0x809C18F8u, kKartManager);
    GuestMemory::Write32(kKartManager + 0x20, kKartPlayers);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t base = kKartObjects + i * 0x1000;
        const uint32_t proxy = base, accessor = base + 0x100, body = base + 0x200, physics = base + 0x300,
                       dynamics = base + 0x400;
        GuestMemory::Write32(kKartPlayers + i * 4, proxy);
        GuestMemory::Write32(proxy, accessor);
        GuestMemory::Write32(accessor + 0x08, body);
        GuestMemory::Write32(body + 0x90, physics);
        GuestMemory::Write32(physics + 0x4, dynamics);
        GuestMemory::WriteFloat(dynamics + 0x68, 100.0f * static_cast<float>(i + 1));
        GuestMemory::WriteFloat(dynamics + 0x6C, 10.0f);
        GuestMemory::WriteFloat(dynamics + 0x70, -50.0f * static_cast<float>(i));
    }
}

bool Contains(const std::vector<uint32_t>& values, uint32_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void TestRoom() {
    BuildRoom();
    const auto room = detail::ReadVoiceRoom<GuestMemory>(kOwnPid);
    Check(room.inRoom && room.roomKey == 0xCAFEu, "in the room, keyed by group");
    Check(room.roomProfileIds == std::vector<uint32_t>({kOwnPid, 600000000u, 700000000u}),
          "room pids: present AIDs only, sorted, ours included");
    Check(room.friendProfileIds == std::vector<uint32_t>({700000000u}), "friend roster");
    Check(room.teamMode, "team mode from racedata");
    // We (AID 1) are on team 1 with player 3 (also AID 1) and player 0 (AID 3, pid 700000000).
    Check(Contains(room.teammateProfileIds, 700000000u) && !Contains(room.teammateProfileIds, 600000000u),
          "teammates by AID");
    Check(Contains(room.teamProfileIds, 600000000u), "opponent's team is known");

    GuestMemory::Write32(kController + 0x38 + 0x58 + 0x08, 1);
    Check(!detail::ReadVoiceRoom<GuestMemory>(kOwnPid).inRoom, "alone is not a room");
    BuildRoom();
    GuestMemory::Write32(kController + 0xE8, 0);
    Check(!detail::ReadVoiceRoom<GuestMemory>(kOwnPid).inRoom, "room type 0 is no room");
    BuildRoom();
    GuestMemory::Write32(kController + 0x291C, 2);
    Check(!detail::ReadVoiceRoom<GuestMemory>(kOwnPid).inRoom, "bad match slot is no room");
    BuildRoom();
    GuestMemory::Write32(kRacedata + 0x20 + 0xB48 + 0x28, 0);
    Check(!detail::ReadVoiceRoom<GuestMemory>(kOwnPid).teamMode, "no team flag, no teams");
}

void TestRacers() {
    BuildRoom();
    const auto room = detail::ReadRkNetRoom<GuestMemory>();
    const auto racers = detail::ReadRacerPositions<GuestMemory>(room, kOwnPid);
    // Players 1 and 3 are ours (AID 1); players 0 and 2 are the two remote consoles.
    Check(racers.size() == 2, "two remote racers");
    if (racers.size() == 2) {
        Check(racers[0].profileId == 700000000u && racers[0].position[0] == 100.0f && racers[0].position[2] == 0.0f,
              "player 0 is AID 3's kart");
        Check(racers[1].profileId == 600000000u && racers[1].position[0] == 300.0f &&
                  racers[1].position[2] == -100.0f,
              "player 2 is AID 0's kart");
    }
}

void TestFaults() {
    // A fault anywhere along a walk degrades to "nothing", never a throw.
    const uint32_t faults[] = {
        0x809C20D8u, kController + 0x291C, kController + 0x38 + 0x58 + 0x21, 0x8038630Cu,
        kMatchControl + 0x38 + 0x16, 0x809C2110u, kFriendMgr + 0x36C, 0x809BD728u,
        kRacedata + 0x20 + 0xB48 + 0x28, kKartPlayers, kKartObjects + 0x400 + 0x68,
    };
    for (const uint32_t fault : faults) {
        BuildRoom();
        GuestMemory::fault_address = fault;
        const auto room = detail::ReadVoiceRoom<GuestMemory>(kOwnPid);
        const auto rk = detail::ReadRkNetRoom<GuestMemory>();
        const auto racers = detail::ReadRacerPositions<GuestMemory>(rk, kOwnPid);
        Check(!room.inRoom || !room.roomProfileIds.empty(), "fault leaves a consistent room");
        (void)racers;
    }
    BuildRoom();
    GuestMemory::Write32(0x809C20D8u, 0);
    Check(!detail::ReadRkNetRoom<GuestMemory>().valid, "null controller");
    BuildRoom();
    GuestMemory::Write32(0x809C18F8u, 0);
    Check(detail::ReadRacerPositions<GuestMemory>(detail::ReadRkNetRoom<GuestMemory>(), kOwnPid).empty(),
          "no kart manager, no racers");
}

} // namespace

int main() {
    TestRoom();
    TestRacers();
    TestFaults();
    if (g_failures == 0) std::cout << "voice room tests passed\n";
    return g_failures == 0 ? 0 : 1;
}
