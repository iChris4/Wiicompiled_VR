// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace mkw::vr {

struct HeldItem {
    uint8_t id = 0x14;
    uint8_t count = 0;
    bool valid = false;
    // Items already out, trailed behind or circling the kart (a triple, or a
    // shell or banana held behind): the next press of the item button uses one.
    bool tethered = false;
    uint64_t race_generation = 0;
};

namespace detail {

// PAL RMCP01 Item::Manager and Item::Player. Item::PlayerInventory is at
// Player+0x88. Read the inventory, never PlayerRoulette::nextItemId: roulette
// teardown clears the latter, and an interrupted roulette may predict an item
// the player never receives. Using a triple, or trailing an item, empties the
// inventory (Item::PlayerObj::UseItem, 0x80791910) and moves the items to the
// Item::PlayerObj at Player+0xB4, whose object type (+0x14) is 0x10 while it
// has none (UpdateTetheredItems, 0x8079231C, restores that after the last).
// Item::Player::Update (0x80797928) gives a press to those first, then to the
// inventory; they stay out while the roulette spins for the next item.
template <typename GuestMemory>
HeldItem ReadHeldItem(uint32_t local_racer, uint64_t race_generation) noexcept {
    HeldItem result{};
    result.race_generation = race_generation;
    if (local_racer >= 12) return result;
    uint32_t manager = 0, players = 0;
    if (!GuestMemory::TryRead32(0x809C3618u, manager) || !manager ||
        !GuestMemory::TryRead32(manager + 0x14u, players) || !players) return result;
    const uint32_t player = players + local_racer * 0x248u;
    if (player < players || !GuestMemory::Contains(player, 0xCCu)) return result;
    try {
        if (GuestMemory::Read8(player + 0x18u) != local_racer) return result;
        result.tethered = GuestMemory::Read32(player + 0xC8u) < 0x10u;
        if (GuestMemory::Read32(player + 0x58u) != 0) return result;
        const uint32_t id = GuestMemory::Read32(player + 0x8Cu);
        const uint32_t count = GuestMemory::Read32(player + 0x90u);
        if (id > 0x12u || count == 0 || count > 3) return result;
        result.id = static_cast<uint8_t>(id);
        result.count = static_cast<uint8_t>(count);
        result.valid = true;
    } catch (const typename GuestMemory::AccessViolation&) {
        return result;
    }
    return result;
}

} // namespace detail
} // namespace mkw::vr
