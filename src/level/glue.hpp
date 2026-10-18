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

// A player's initial chunks are sent during join (network thread) before the player is in `players`, so
// block updates of that window (fluids flowing after post-processing) never reached it: resend the chunks of
// its view that changed recently. (Vanilla sends a chunk only after postProcessGeneration.)
inline void resend_changed_chunks(Player& p) {
    int v = view_distance();
    for (int x = p.cx - v; x <= p.cx + v; x++)
        for (int z = p.cz - v; z <= p.cz + v; z++) {
            auto it = chunk_changed_at.find(chunk_key(x, z));
            if (it == chunk_changed_at.end() || it->second < server_tick - 200) continue;
            if (ChunkPtr c = p.level->find(x, z)) push_chunk(p, *c);
        }
    std::erase_if(chunk_changed_at, [](auto& e) { return e.second < server_tick - 200; });
}

inline void start() {
    init();
    tnt_prime_hook = tnt_prime;
    on_tick(tick_all_levels);
    on_player_tick(plates_player_tick);
    on_tick_end(broadcast_all_changes);
    on_player_join(resend_changed_chunks);
    level_set_block = [](int x, int y, int z, int state, int flags) { return set(Pos{x, y, z}, state, flags); };
    on_save_all = pack_all_levels;
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
