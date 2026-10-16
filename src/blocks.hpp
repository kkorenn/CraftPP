#pragma once
// Block interaction helpers: updates, placement, pick block.
#include "server.hpp"

#include <algorithm>

inline Writer block_update(int x, int y, int z, int state) {
    uint64_t pos = (uint64_t(int64_t(x) & 0x3FFFFFF) << 38) | (uint64_t(int64_t(z) & 0x3FFFFFF) << 12) |
                   uint64_t(int64_t(y) & 0xFFF);
    return std::move(Writer().varint(play_cb::block_update).i64(int64_t(pos)).varint(state));
}

inline void read_block_pos(Reader& in, int& x, int& y, int& z) {
    int64_t v = in.i64();
    x = int(v >> 38);
    y = int(v << 52 >> 52);
    z = int(v << 26 >> 38);
}

// Set by the level tick system (src/level/): Level.setBlock with UPDATE_ALL (neighbor updates, client sync).
inline bool (*level_set_block)(int x, int y, int z, int state, int flags) = nullptr;
inline void change_block(int x, int y, int z, int state) {
    if (level_set_block) level_set_block(x, y, z, state, 3);
    else if (world.set_block(x, y, z, uint16_t(state))) broadcast(block_update(x, y, z, state));
}
// /setblock, /fill: BlockInput.place(2 | 256) + updateNeighboursOnBlockSet (level sends the client updates).
inline bool (*level_command_place)(int x, int y, int z, int state) = nullptr;
// ServerGamePacketListenerImpl.ackBlockChangesUpTo: sent after this tick's block updates.
inline void ack_block_changes(Player& p, int seq) { p.g.ack_seq = std::max(p.g.ack_seq, seq); }

inline int pick_item(int state) {
    auto it = std::upper_bound(std::begin(BLOCK_FIRST_STATE), std::end(BLOCK_FIRST_STATE), state);
    return BLOCK_PICK_ITEM[it - std::begin(BLOCK_FIRST_STATE) - 1];
}

inline void send_slot(Player& p, int i) {
    Writer w;
    w.varint(play_cb::set_player_inventory).varint(i);
    if (p.inv[size_t(i)].item < 0) w.varint(0);
    else w.varint(p.inv[size_t(i)].count).varint(p.inv[size_t(i)].item).varint(0).varint(0);
    p.send(w);
}

// ServerGamePacketListenerImpl.tryPickItem + Inventory.pickSlot/addAndPickItem.
inline void pick_block(Player& p, int item) {
    auto& inv = p.inv;
    auto suitable_hotbar = [&] {
        for (int i = 0; i < 9; i++)
            if (inv[size_t((p.selected + i) % 9)].item < 0) return (p.selected + i) % 9;
        return p.selected;  // ponytail: vanilla skips enchanted items here; no enchantments tracked yet
    };
    int existing = -1;
    for (int i = 0; i < 36 && existing < 0; i++)
        if (inv[size_t(i)].item == item) existing = i;
    std::vector<int> changed;
    if (existing >= 0 && existing < 9) {
        p.selected = existing;
    } else if (existing >= 0) {
        p.selected = suitable_hotbar();
        std::swap(inv[size_t(p.selected)], inv[size_t(existing)]);
        changed = {p.selected, existing};
    } else if (p.gamemode == 1) {
        p.selected = suitable_hotbar();
        if (inv[size_t(p.selected)].item >= 0)
            for (int i = 0; i < 36; i++)
                if (inv[size_t(i)].item < 0) { inv[size_t(i)] = inv[size_t(p.selected)]; changed.push_back(i); break; }
        inv[size_t(p.selected)] = {item, 1};
        changed.push_back(p.selected);
    }
    for (int i : changed) send_slot(p, i);
    p.send(Writer().varint(play_cb::set_held_slot).varint(p.selected));
}

inline bool replaceable(int state) {
    return state == blocks::air || state == blocks::water || state == blocks::lava || state == blocks::short_grass;
}
