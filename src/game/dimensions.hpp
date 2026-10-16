#pragma once
// Dimensions: moving players between levels (ServerPlayer.teleport across levels, PlayerList.respawn), the end
// spawn platform, the end exit podium (EnderDragonFight without the dragon), the credits flow, and unloading
// levels nobody is in. Levels themselves: world.hpp (World, worlds[], cur_level). All on the tick thread.
#include "survival.hpp"

// ---------- moving between levels ----------
inline void move_to_level(Player& p, World* to, double x, double y, double z, float yaw, float pitch, uint8_t keep) {
    World* from = p.level;
    broadcast_level(remove_entities_packet({p.eid}), from, &p);   // removePlayerImmediately
    for (auto& e : item_entities) std::erase(e.watchers, p.eid);  // the client drops its entities on respawn
    if (p.g.container) close_container(p, false);                 // the client closes screens on respawn
    p.g.use_ticks = 0;                                             // stopUsingItem
    p.level = to;
    p.x = x, p.y = y, p.z = z, p.yaw = yaw, p.pitch = pitch;
    p.portal_time = 0;

    Writer w;
    w.varint(play_cb::respawn);
    write_spawn_info(w, p);
    p.send(w.u8(keep));
    p.send(difficulty_packet());
    p.send(pkt(play_cb::entity_event).i32(p.eid).u8(uint8_t(24 + perm_level(p))));  // sendPlayerPermissionLevel
    p.send(Writer().varint(play_cb::player_position).varint(0)
               .f64(p.x).f64(p.y).f64(p.z).f64(0).f64(0).f64(0).f32(p.yaw).f32(p.pitch).i32(0));
    // PlayerList.sendLevelInfo
    p.send(border_init_packet());
    p.send(set_time_packet());
    p.send(spawn_packet());
    if (to == &worlds[DIM_OVERWORLD] && weather.rain_level > 0.2f) {  // ponytail: weather lives on the overworld only
        p.send(game_event(1, 0));
        p.send(game_event(7, weather.rain_level));
        p.send(game_event(8, weather.thunder_level));
    }
    p.send(game_event(13, 0));  // LEVEL_CHUNKS_LOAD_START
    {
        to->joining++;  // keeps the level loaded while its chunks stream (unload_empty_levels)
        update_view(p, int(std::floor(p.x)) >> 4, int(std::floor(p.z)) >> 4, true);
        to->joining--;
    }
    // sendAllPlayerInfo + reset sent info
    p.g.remote_valid = false;  // full inventory resync at the end of the tick
    p.send(Writer().varint(play_cb::set_held_slot).varint(p.selected));
    send_health(p);
    send_experience(p);
    broadcast_level(spawn_entity(p), to, &p);
    for (auto& q : players)
        if (q.get() != &p && q->level == to) p.send(spawn_entity(*q));
}

// ---------- the end ----------
// EndPlatformFeature.createEndPlatform: 5x5 obsidian under `x, y, z`, air 3 high above it.
// ponytail: vanilla destroys (drops) what it replaces; blocks are simply replaced here.
inline void create_end_platform(World& w, int x, int y, int z) {
    InLevel in_level(&w);
    for (int dz = -2; dz <= 2; dz++)
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = -1; dy < 3; dy++) {
                int state = dy == -1 ? blocks::obsidian : blocks::air;
                if (world.get_block(x + dx, y + dy, z + dz) != state) change_block(x + dx, y + dy, z + dz, state);
            }
}

inline bool motion_blocking(uint16_t s) { return is_solid(s) || is_fluid(s); }  // ponytail: no shape data

// EndPodiumFeature(active) at origin: bedrock pillar + rim, end stone base, end_portal (active) or air in the rim.
inline void place_end_podium(World& w, int ox, int oy, int oz, bool active) {
    InLevel in_level(&w);
    for (int x = ox - 4; x <= ox + 4; x++)
        for (int y = oy - 1; y <= oy + 32; y++)
            for (int z = oz - 4; z <= oz + 4; z++) {
                int64_t d = int64_t(x - ox) * (x - ox) + int64_t(y - oy) * (y - oy) + int64_t(z - oz) * (z - oz);
                bool inside_rim = double(d) < 2.5 * 2.5;  // Vec3i.closerThan
                if (!inside_rim && !(double(d) < 3.5 * 3.5)) continue;
                int state;
                if (y < oy) state = inside_rim ? blocks::bedrock : blocks::end_stone;
                else if (y > oy) state = blocks::air;
                else if (!inside_rim) state = blocks::bedrock;
                else state = active ? blocks::end_portal : blocks::air;
                if (world.get_block(x, y, z) != state) change_block(x, y, z, state);
            }
    for (int y = 0; y < 4; y++) change_block(ox, oy + y, oz, blocks::bedrock);
    // Direction.Plane.HORIZONTAL: north, east, south, west; wall_torch states: north, south, west, east
    const int torch[4][3] = {{0, -1, blocks::wall_torch}, {1, 0, blocks::wall_torch + 3}, {0, 1, blocks::wall_torch + 1},
                             {-1, 0, blocks::wall_torch + 2}};
    for (auto& t : torch) change_block(ox + t[0], oy + 2, oz + t[1], t[2]);
}

// EnderDragonFight state that matters without the dragon: where the exit portal is and whether it is open.
// Persisted in <end dir>/dragon_fight.txt.
struct DragonFight {
    bool scanned = false, killed = false;
    int px = 0, py = 0, pz = 0;  // exitPortalLocation
    std::string path() const { return worlds[DIM_END].dir + "/dragon_fight.txt"; }
    void load() { std::ifstream f(path()); if (f >> scanned >> killed >> px >> py >> pz) {} }
    void save() const {
        std::filesystem::create_directories(worlds[DIM_END].dir);
        std::ofstream(path()) << scanned << ' ' << killed << ' ' << px << ' ' << py << ' ' << pz << '\n';
    }
};
inline DragonFight dragon_fight;

// EnderDragonFight.spawnExitPortal: podium on top of the central island at (0, 0).
inline void spawn_exit_portal(bool active) {
    World& end = worlds[DIM_END];
    if (!dragon_fight.scanned) {
        int y = end.max_y() - 1;  // getHeightmapPos(MOTION_BLOCKING_NO_LEAVES, (0, 0)).below()
        while (y >= end.min_y && !motion_blocking(end.get_block(0, y, 0))) y--;
        while (end.get_block(0, y, 0) == blocks::bedrock && y > 63) y--;
        dragon_fight.px = 0, dragon_fight.py = std::max(end.min_y + 1, y), dragon_fight.pz = 0;
        dragon_fight.scanned = true;
    }
    place_end_podium(end, dragon_fight.px, dragon_fight.py, dragon_fight.pz, active);
    dragon_fight.save();
}

// Hook for the dragon fight (needs mobs): call when the ender dragon dies (EnderDragonFight.setDragonKilled).
// ponytail: no end gateways or dragon egg yet; add with the dragon.
inline void on_ender_dragon_killed() {
    dragon_fight.killed = true;
    spawn_exit_portal(true);
}

// EnderDragonFight.tick: once a player is within 192 blocks of (0, 128, 0) and the arena is loaded, scanState
// places the inactive exit podium. Hook: this is where vanilla also spawns the dragon (createNewDragon).
inline void dragon_fight_tick() {
    if (dragon_fight.scanned) return;
    for (auto& p : players) {
        if (p->level != &worlds[DIM_END] || p->g.dead) continue;
        double dy = p->y - 128;
        if (p->x * p->x + dy * dy + p->z * p->z < 192.0 * 192.0) {
            spawn_exit_portal(false);
            return;
        }
    }
}

// ServerPlayer.showEndCredits: the end portal in the end. The client shows the credits (or skips them when
// already seen) and answers with client_command PERFORM_RESPAWN.
inline void show_end_credits(Player& p) {
    if (p.won_game) return;
    p.won_game = true;
    broadcast_level(remove_entities_packet({p.eid}), p.level, &p);
    p.send(game_event(4, p.seen_credits ? 0.0f : 1.0f));  // WIN_GAME: 1 = roll the credits
    p.seen_credits = true;
}

// ---------- unloading ----------
// A level nobody is in (or joining) for 300 ticks (vanilla ServerLevel emptyTime) saves and drops its chunks; its
// entities stop ticking at once (tick_entities). The overworld is left to the chunk-unloading work.
inline void unload_empty_levels() {
    static int empty_ticks[DIM_COUNT] = {};
    for (int d = DIM_NETHER; d < DIM_COUNT; d++) {
        World& w = worlds[d];
        if (level_occupied(&w) || w.joining > 0) { empty_ticks[d] = 0; continue; }
        if (++empty_ticks[d] < 300) continue;
        empty_ticks[d] = 0;
        bool any;
        {
            std::lock_guard lock(w.m);
            any = !w.chunks.empty();
        }
        if (!any) continue;
        int n = w.unload_all();
        printf("Unloaded %d chunk(s) of %s (no players)\n", n, w.id.c_str());
        fflush(stdout);
    }
}

inline void dimensions_init() {
    dragon_fight.load();
    on_tick(unload_empty_levels);
    on_tick(dragon_fight_tick);
    // After the credits: PlayerList.respawn(player, keepAllPlayerData = true) at the world spawn.
    PacketHandler prev = handlers()[play_sb::client_command];
    on(play_sb::client_command, [prev](Player& p, Reader& in, int id) {
        Reader peek = in;
        if (peek.varint() == 0 && p.won_game) {
            p.won_game = false;
            World* from = p.level;
            place_new_player(p);
            World* spawn = p.level;
            p.level = from;
            move_to_level(p, spawn, p.x, p.y, p.z, p.yaw, p.pitch, 1);
            return;
        }
        prev(p, in, id);
    });
}
