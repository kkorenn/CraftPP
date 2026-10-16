#pragma once
// Wires the level tick system into the server: tick hooks, change_block, player block breaking.
#include "run.hpp"
#include "placement.hpp"

namespace lvl {

inline void player_will_destroy(Player& p, const Pos& pos, int state);  // behaviours: other halves of doors/beds/plants

// ServerPlayerGameMode.destroyBlock: playerWillDestroy, then Level.removeBlock (leaves the block's fluid).
inline bool player_destroy(Player& p, int x, int y, int z, int state) {
    Pos pos{x, y, z};
    player_will_destroy(p, pos, state);
    return remove(pos);
}

inline void start() {
    init();
    on_tick(level_tick);
    on_tick_end(broadcast_changes);
    level_set_block = [](int x, int y, int z, int state, int flags) { return set(Pos{x, y, z}, state, flags); };
    on_save_all = pack_all;
    level_command_place = [](int x, int y, int z, int state) {
        Pos p{x, y, z};
        int old = get(p);
        int s = update_from_neighbour_shapes(state, p);
        if (is_air(s)) s = state;
        if (!set(p, s, UPDATE_CLIENTS | UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS)) return false;
        int now_state = get(p);  // ServerLevel.updateNeighboursOnBlockSet
        if (block(old) != block(now_state)) after_removal(old, p, false);
        update_neighbors(p, block(now_state));
        return true;
    };
}

}  // namespace lvl
