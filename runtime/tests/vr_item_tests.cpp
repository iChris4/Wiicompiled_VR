#include "vr/mkw_vr_item.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace {
struct GuestMemory {
    using AccessViolation = std::out_of_range;
    inline static std::unordered_map<uint32_t, uint8_t> bytes;
    inline static uint32_t fault = 0;
    static bool Contains(uint32_t address, uint32_t length) {
        for (uint32_t i = 0; i < length; ++i) if (!bytes.count(address + i)) return false;
        return true;
    }
    static uint8_t Read8(uint32_t address) {
        if (address == fault) throw AccessViolation("fault");
        return bytes.at(address);
    }
    static uint32_t Read32(uint32_t address) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i) value = (value << 8) | Read8(address + i);
        return value;
    }
    static bool TryRead32(uint32_t address, uint32_t& out) {
        try { out = Read32(address); return true; } catch (const AccessViolation&) { return false; }
    }
    static void Write32(uint32_t address, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) bytes[address + i] = uint8_t(value >> (24 - i * 8));
    }
};

constexpr uint32_t manager = 0x81000000u, players = 0x82000000u;
constexpr uint32_t player = players + 7 * 0x248u;
int failures = 0;
void Check(bool value, const char* name) {
    if (!value) { ++failures; std::cerr << "FAILED: " << name << '\n'; }
}
void Init() {
    GuestMemory::bytes.clear(); GuestMemory::fault = 0;
    GuestMemory::Write32(0x809C3618u, manager);
    GuestMemory::Write32(manager + 0x14u, players);
    for (uint32_t i = 0; i < 0xCCu; ++i) GuestMemory::bytes[player + i] = 0;
    GuestMemory::bytes[player + 0x18u] = 7;
    GuestMemory::Write32(player + 0x90u, 1);
    GuestMemory::Write32(player + 0xC8u, 0x10); // nothing out behind or around the kart
}
}

int main() {
    using mkw::vr::detail::ReadHeldItem;
    for (uint32_t id = 0; id <= 0x12u; ++id) {
        Init(); GuestMemory::Write32(player + 0x8cu, id);
        const auto item = ReadHeldItem<GuestMemory>(7, 19);
        Check(item.valid && item.id == id && item.count == 1 && item.race_generation == 19,
              "all 19 IDs, nonzero racer, generation");
    }
    Init(); GuestMemory::Write32(player + 0x8cu, 0x10); GuestMemory::Write32(player + 0x90u, 3);
    Check(ReadHeldItem<GuestMemory>(7, 1).count == 3, "triple full");
    GuestMemory::Write32(player + 0x90u, 2);
    Check(ReadHeldItem<GuestMemory>(7, 1).count == 2, "triple remaining");
    GuestMemory::Write32(player + 0x90u, 0);
    Check(!ReadHeldItem<GuestMemory>(7, 1).valid, "inventory removed on use or damage");
    GuestMemory::Write32(player + 0x90u, 1);
    GuestMemory::Write32(player + 0x58u, 1);
    Check(!ReadHeldItem<GuestMemory>(7, 1).valid, "roulette spinning");
    GuestMemory::Write32(player + 0x58u, 0);
    for (uint32_t id : {0x13u, 0x14u, 0xffu}) {
        GuestMemory::Write32(player + 0x8cu, id);
        Check(!ReadHeldItem<GuestMemory>(7, 1).valid, "empty or unsupported");
    }
    Init(); GuestMemory::Write32(player + 0x8cu, 0x0au);
    Check(ReadHeldItem<GuestMemory>(7, 1).valid, "golden mushroom held");
    Check(ReadHeldItem<GuestMemory>(7, 1).use_mark == 0, "golden mushroom not used yet");
    GuestMemory::Write32(player + 0xACu, 438); // the first use: 450 - 12
    Check(ReadHeldItem<GuestMemory>(7, 1).use_mark == 438, "golden mushroom first use");
    GuestMemory::Write32(player + 0xACu, 401);
    Check(ReadHeldItem<GuestMemory>(7, 1).use_mark == 401, "golden mushroom next use");
    GuestMemory::Write32(player + 0x8cu, 0x04);
    Check(ReadHeldItem<GuestMemory>(7, 1).use_mark == 0, "no use mark for another item");
    GuestMemory::Write32(player + 0x8cu, 0x0au);
    GuestMemory::Write32(player + 0x90u, 0);
    Check(!ReadHeldItem<GuestMemory>(7, 1).valid, "golden mushroom expiry");
    Init(); GuestMemory::fault = player + 0x8cu;
    const auto faulted = ReadHeldItem<GuestMemory>(7, 2);
    Check(!faulted.valid && faulted.race_generation == 2, "read fault preserves generation");
    Init(); GuestMemory::Write32(0x809C3618u, 0);
    Check(!ReadHeldItem<GuestMemory>(7, 1).valid, "invalid manager");
    Init(); GuestMemory::bytes[player + 0x18u] = 0;
    Check(!ReadHeldItem<GuestMemory>(7, 1).valid, "racer identity mismatch");

    // A triple used, or an item trailed: the inventory is empty and the items
    // are out, which the item button still uses.
    Init(); GuestMemory::Write32(player + 0x8cu, 0x14); GuestMemory::Write32(player + 0x90u, 0);
    Check(!ReadHeldItem<GuestMemory>(7, 1).tethered, "nothing out");
    GuestMemory::Write32(player + 0xC8u, 0x00); // green shells circling
    auto out = ReadHeldItem<GuestMemory>(7, 1);
    Check(out.tethered && !out.valid, "triple out, hand empty");
    GuestMemory::Write32(player + 0xC8u, 0x02); // bananas trailing
    Check(ReadHeldItem<GuestMemory>(7, 1).tethered, "bananas trailing");
    GuestMemory::Write32(player + 0x58u, 1);
    out = ReadHeldItem<GuestMemory>(7, 1);
    Check(out.tethered && !out.valid, "still out while the roulette spins");
    GuestMemory::Write32(player + 0x58u, 0);
    GuestMemory::Write32(player + 0x8cu, 0x04); GuestMemory::Write32(player + 0x90u, 1);
    out = ReadHeldItem<GuestMemory>(7, 1);
    Check(out.tethered && out.valid && out.id == 0x04, "out with a new item in the hand");
    for (uint32_t type : {0x10u, 0x11u, 0xffffffffu}) {
        GuestMemory::Write32(player + 0xC8u, type);
        Check(!ReadHeldItem<GuestMemory>(7, 1).tethered, "no object type or garbage");
    }
    Init(); GuestMemory::Write32(player + 0xC8u, 0x00); GuestMemory::bytes[player + 0x18u] = 0;
    Check(!ReadHeldItem<GuestMemory>(7, 1).tethered, "tethered racer identity mismatch");
    Init(); GuestMemory::Write32(player + 0xC8u, 0x00); GuestMemory::fault = player + 0xC8u;
    Check(!ReadHeldItem<GuestMemory>(7, 1).tethered, "tethered read fault");
    Init(); GuestMemory::Write32(player + 0xC8u, 0x00); GuestMemory::bytes.erase(player + 0xCBu);
    Check(!ReadHeldItem<GuestMemory>(7, 1).tethered, "player struct out of reach");
    return failures ? 1 : 0;
}
