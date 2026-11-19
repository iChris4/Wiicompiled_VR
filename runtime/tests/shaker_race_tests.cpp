// SPDX-License-Identifier: GPL-3.0-or-later
// When the secondary (bass shaker) output counts a race as running (shaker_race.h), checked
// against synthetic PAL race state.

#include "shaker_race.h"

#include <array>
#include <iostream>
#include <unordered_map>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

// Unmapped addresses are not readable, like addresses outside guest memory.
struct GuestMemory {
    inline static std::unordered_map<uint32_t, uint8_t> bytes;

    static bool TryRead8(uint32_t address, uint32_t& out) {
        const auto it = bytes.find(address);
        if (it == bytes.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    static bool TryRead32(uint32_t address, uint32_t& out) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i) {
            uint32_t byte = 0;
            if (!TryRead8(address + i, byte)) {
                return false;
            }
            value = (value << 8) | byte;
        }
        out = value;
        return true;
    }

    static void Write8(uint32_t address, uint8_t value) { bytes[address] = value; }

    static void Write32(uint32_t address, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) {
            bytes[address + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
        }
    }
};

constexpr uint32_t kRaceInfo = 0x80900000u;
constexpr uint32_t kRaceData = 0x80A00000u;
constexpr uint32_t kTypeCpu = 1;
constexpr uint32_t kTypeLocal = 0;
constexpr uint32_t kTypeGhost = 3;
constexpr uint32_t kTypeOnline = 4;

struct Scene {
    bool raceInfo = true;
    uint32_t stage = 2;
    bool raceData = true;
    uint32_t racerCount = 12;
    std::array<uint32_t, 12> types{};
};

Scene AllCpuScene() {
    Scene scene;
    scene.types.fill(kTypeCpu);
    return scene;
}

bool Running(const Scene& scene) {
    GuestMemory::bytes.clear();
    GuestMemory::Write32(0x809BD730u, scene.raceInfo ? kRaceInfo : 0);
    if (scene.raceInfo) {
        GuestMemory::Write32(kRaceInfo + 0x28u, scene.stage);
    }
    GuestMemory::Write32(0x809BD728u, scene.raceData ? kRaceData : 0);
    if (scene.raceData) {
        GuestMemory::Write8(kRaceData + 0x24u, static_cast<uint8_t>(scene.racerCount));
        for (uint32_t i = 0; i < scene.racerCount && i < scene.types.size(); ++i) {
            GuestMemory::Write32(kRaceData + 0x38u + i * 0xF0u, scene.types[i]);
        }
    }
    return shaker_race::IsHumanRaceRunning<GuestMemory>();
}

void TestHumanRaces() {
    Scene scene = AllCpuScene();
    scene.types[0] = kTypeLocal;
    Check(Running(scene), "a race you drive");

    scene = AllCpuScene();
    scene.types[7] = kTypeLocal;
    Check(Running(scene), "the human can be any racer");

    scene = AllCpuScene();
    scene.types[0] = kTypeLocal;
    scene.types[1] = kTypeLocal;
    Check(Running(scene), "split screen");

    scene = AllCpuScene();
    scene.types[3] = kTypeOnline;
    Check(Running(scene), "online race");

    scene = AllCpuScene();
    scene.racerCount = 2;
    scene.types[0] = kTypeLocal;
    scene.types[1] = kTypeGhost;
    Check(Running(scene), "time trial against a ghost");

    for (uint32_t stage = 1; stage <= 4; ++stage) {
        scene = AllCpuScene();
        scene.types[0] = kTypeLocal;
        scene.stage = stage;
        Check(Running(scene), "every stage from 1 to 4 counts");
    }
}

void TestNotRacing() {
    Check(!Running(AllCpuScene()), "the title-screen demo race has no human");

    Scene scene = AllCpuScene();
    scene.types[0] = kTypeLocal;
    scene.raceInfo = false;
    Check(!Running(scene), "menus: no RaceInfo object, even with stale racer data");

    scene.raceInfo = true;
    scene.stage = 0;
    Check(!Running(scene), "before the race starts");
    scene.stage = 5;
    Check(!Running(scene), "after the race");

    scene = AllCpuScene();
    scene.types[0] = kTypeLocal;
    scene.raceData = false;
    Check(!Running(scene), "no race data");

    scene.raceData = true;
    scene.racerCount = 0;
    Check(!Running(scene), "no racers");
    scene.racerCount = 200;
    Check(!Running(scene), "racer count out of range");
}

void TestUnreadableMemory() {
    GuestMemory::bytes.clear();
    Check(!shaker_race::IsHumanRaceRunning<GuestMemory>(), "nothing readable");

    Scene scene = AllCpuScene();
    scene.types[0] = kTypeLocal;
    Check(Running(scene), "baseline for the unreadable cases");
    GuestMemory::bytes.erase(kRaceInfo + 0x28u);
    Check(!shaker_race::IsHumanRaceRunning<GuestMemory>(), "unreadable stage");

    Check(Running(scene), "baseline restored");
    GuestMemory::bytes.erase(kRaceData + 0x24u);
    Check(!shaker_race::IsHumanRaceRunning<GuestMemory>(), "unreadable racer count");

    Check(Running(scene), "baseline restored");
    GuestMemory::bytes.erase(kRaceData + 0x38u);
    Check(!shaker_race::IsHumanRaceRunning<GuestMemory>(), "unreadable racer type");
}

} // namespace

int main() {
    TestHumanRaces();
    TestNotRacing();
    TestUnreadableMemory();
    if (g_failures == 0) {
        std::cout << "shaker_race_tests passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
