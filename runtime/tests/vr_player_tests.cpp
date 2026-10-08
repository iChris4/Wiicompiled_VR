// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production local-kart resolver against a synthetic PAL roster.

#include "vr/mkw_vr_player.h"

#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace {

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
            if (bytes.count(address + i) == 0) {
                return false;
            }
        }
        return true;
    }

    static uint8_t Read8(uint32_t address) {
        if (address == fault_address) {
            throw AccessViolation("injected read fault");
        }
        return bytes.at(address);
    }

    static uint32_t Read32(uint32_t address) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i) {
            value = (value << 8) | Read8(address + i);
        }
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

    static void Write32(uint32_t address, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) {
            bytes[address + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
        }
    }
};

constexpr uint32_t kRaceData = 0x81000000u;
constexpr uint32_t kManager = 0x81100000u;
constexpr uint32_t kKarts = 0x81100100u;
constexpr uint32_t kProxies = 0x81200000u;
constexpr uint32_t kAccessors = 0x81300000u;

void SetLocalRacer(uint8_t player) {
    GuestMemory::bytes[kRaceData + 0xB84] = player;
    for (uint32_t i = 0; i < 12; ++i) {
        GuestMemory::Write32(kRaceData + 0x38 + i * 0xF0, i == player ? 0 : 4);
    }
}

void MakeRace(uint8_t local_player) {
    GuestMemory::bytes.clear();
    GuestMemory::fault_address = 0;
    for (uint32_t i = 0; i < 0xB90; ++i) {
        GuestMemory::bytes[kRaceData + i] = 0;
    }
    GuestMemory::Write32(0x809BD728u, kRaceData);
    GuestMemory::Write32(0x809C18F8u, kManager);
    GuestMemory::Write32(kManager + 0x20, kKarts);
    GuestMemory::bytes[kRaceData + 0x24] = 12;
    GuestMemory::bytes[kRaceData + 0x26] = 1;
    for (uint32_t i = 0; i < 12; ++i) {
        GuestMemory::Write32(kKarts + i * 4, kProxies + i * 0x100);
        GuestMemory::Write32(kProxies + i * 0x100, kAccessors + i * 0x100);
    }
    SetLocalRacer(local_player);
}

auto Resolve() {
    return mkw::vr::detail::ReadLocalPlayerKart<GuestMemory>();
}

void TestSinglePlayer() {
    MakeRace(0);
    for (uint32_t i = 1; i < 12; ++i) {
        GuestMemory::Write32(kRaceData + 0x38 + i * 0xF0, 1); // CPU
    }
    const auto read = Resolve();
    Check(read.failed_step == nullptr && read.player_index == 0 &&
              read.accessor == kAccessors,
          "offline racing still selects the local kart in slot zero");
}

void TestEveryOnlineSlot() {
    for (uint8_t player = 0; player < 12; ++player) {
        MakeRace(player);
        const auto read = Resolve();
        Check(read.failed_step == nullptr && read.player_index == player &&
                  read.proxy == kProxies + player * 0x100u &&
                  read.accessor == kAccessors + player * 0x100u,
              "an all-human online roster selects only the screen's local racer");
    }
}

void TestRosterRemapping() {
    MakeRace(5);
    Check(Resolve().accessor == kAccessors + 0x500, "initial online assignment");
    SetLocalRacer(11);
    Check(Resolve().accessor == kAccessors + 0xB00, "a new roster assignment is read afresh");
}

void TestNoOpponentFallback() {
    for (uint32_t type : {1u, 2u, 3u, 4u, 5u}) {
        MakeRace(5);
        GuestMemory::Write32(kRaceData + 0x38 + 5 * 0xF0, type);
        GuestMemory::Write32(kRaceData + 0x38, 0); // Another valid kart is not a fallback.
        const auto read = Resolve();
        Check(read.failed_step != nullptr && read.accessor == 0,
              "a mapped CPU, unused, ghost, remote or absent racer cannot be an anchor");
    }
    MakeRace(5);
    GuestMemory::Write32(kKarts + 5 * 4, 0);
    Check(Resolve().failed_step != nullptr && Resolve().accessor == 0,
          "a missing local kart never falls back to another valid kart");
}

void TestInvalidMapping() {
    for (uint8_t player : {12, 127, 255}) {
        MakeRace(5);
        GuestMemory::bytes[kRaceData + 0xB84] = player;
        Check(Resolve().failed_step != nullptr, "out-of-range and unassigned HUD racers fail");
    }
    MakeRace(5);
    GuestMemory::bytes[kRaceData + 0x24] = 5;
    Check(Resolve().failed_step != nullptr, "racer must be inside the active roster");
    for (uint8_t count : {0, 13}) {
        MakeRace(5);
        GuestMemory::bytes[kRaceData + 0x24] = count;
        Check(Resolve().failed_step != nullptr, "empty and corrupt rosters fail");
    }
    for (uint8_t count : {0, 2}) {
        MakeRace(5);
        GuestMemory::bytes[kRaceData + 0x26] = count;
        Check(Resolve().failed_step != nullptr, "spectating and split-screen have no anchor");
    }
}

void TestUnavailableGuestData() {
    MakeRace(5);
    GuestMemory::Write32(0x809BD728u, 0);
    Check(Resolve().failed_step != nullptr, "missing race data fails");
    MakeRace(5);
    GuestMemory::bytes.erase(kRaceData + 0xB84);
    Check(Resolve().failed_step != nullptr, "unmapped race data fails");
    MakeRace(5);
    GuestMemory::fault_address = kRaceData + 0x38 + 5 * 0xF0;
    Check(Resolve().failed_step != nullptr, "a fault after the bounds check is contained");
    MakeRace(5);
    GuestMemory::bytes.erase(kProxies + 5 * 0x100);
    Check(Resolve().failed_step != nullptr, "an unreadable local accessor fails");
}

// KartState+0xC bit 27, reached through accessor+4, marks a Bullet Bill.
void TestBulletBill() {
    const auto in_bullet = [](const mkw::vr::detail::LocalPlayerKartRead& player) {
        return mkw::vr::detail::LocalPlayerInBullet<GuestMemory>(player);
    };
    constexpr uint32_t kState = 0x81400000u;
    MakeRace(5);
    const auto local = Resolve();
    GuestMemory::Write32(local.accessor + 4, kState);
    GuestMemory::Write32(kState + 0xC, 0);
    Check(!in_bullet(local), "a normal kart is not a Bullet Bill");
    GuestMemory::Write32(kState + 0xC, 1u << 27);
    Check(in_bullet(local), "the local kart's Bullet Bill flag is read");
    GuestMemory::Write32(kState + 0xC, 0xFFFFFFFFu);
    Check(in_bullet(local), "other state flags do not hide the Bullet Bill");
    GuestMemory::Write32(kState + 0xC, ~(1u << 27));
    Check(!in_bullet(local), "other state flags are not a Bullet Bill");
    GuestMemory::Write32(kState + 0xC, 0);
    Check(!in_bullet(local), "the Bullet Bill ends when its flag clears");

    GuestMemory::Write32(kState + 0xC, 1u << 27);
    auto failed = local;
    failed.failed_step = "local kart accessor";
    Check(!in_bullet(failed), "a failed kart read is never a Bullet Bill");
    // Physical address 0 is MEM1, so low addresses read back: the null guards
    // must not depend on a fault. Address 0x4 holds a Bullet Bill's state and
    // 0xC a set flag, which a read through a null pointer would find.
    auto no_accessor = local;
    no_accessor.accessor = 0;
    GuestMemory::Write32(0x4u, kState);
    Check(!in_bullet(no_accessor), "a missing accessor is never a Bullet Bill");
    GuestMemory::fault_address = kState + 0xC;
    Check(!in_bullet(local), "an unreadable state is never a Bullet Bill");
    GuestMemory::fault_address = 0;
    GuestMemory::Write32(local.accessor + 4, 0);
    GuestMemory::Write32(0xCu, 1u << 27);
    Check(!in_bullet(local), "a null state pointer is never a Bullet Bill");
    GuestMemory::bytes.erase(local.accessor + 4);
    Check(!in_bullet(local), "an unreadable accessor is never a Bullet Bill");
}

} // namespace

int main() {
    TestSinglePlayer();
    TestEveryOnlineSlot();
    TestRosterRemapping();
    TestNoOpponentFallback();
    TestInvalidMapping();
    TestUnavailableGuestData();
    TestBulletBill();
    return g_failures == 0 ? 0 : 1;
}
