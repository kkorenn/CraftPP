#pragma once
// Environment: beds, sleeping, night skipping, phantom insomnia reset, bed/anchor respawn points, explosions.
#include "boss/level.hpp"
#include "commands/state.hpp"
#include "level/placement.hpp"
#include "mobs/entity.hpp"
#include "mobs/manager.hpp"
#include "progress/progress.hpp"

namespace env {

inline void send_action_bar(Player& p, const std::string& key) {
    Writer w;
    w.varint(play_cb::set_action_bar_text);
    w.u8(0x0A).u8(0x08).u8(0).u8(9).bytes("translate", 9);
    w.u8(uint8_t(key.size() >> 8)).u8(uint8_t(key.size())).bytes(key.data(), key.size()).u8(0);
    p.send(w);
}

inline std::vector<uint8_t> opt_pos_bytes(const std::optional<std::array<int, 3>>& p) {
    Writer w;
    w.boolean(bool(p));
    if (p) w.i64(int64_t(pack_pos((*p)[0], (*p)[1], (*p)[2])));
    return w.b;
}

inline bool monsters_near(double x, double y, double z, double h, double v) {
    bool found = false;
    AABB box{x - h, y - v, z - h, x + h, y + v, z + h};
    for_each_entity_in(box, [&](Entity& e) {
        if (e.as_monster() && e.is_alive()) found = true;
    }, false);
    return found;
}

inline bool is_bed_occupied(const lvl::Pos& p) {
    for (auto& pl : players) {
        if (pl->g.sleeping && pl->g.bed_x == p.x && pl->g.bed_y == p.y && pl->g.bed_z == p.z) return true;
    }
    return false;
}

inline void wake_up(Player& p) {
    if (!p.g.sleeping) return;
    p.g.sleeping = false;
    p.g.sleep_ticks = 0;
    player_set_sleeping(p, false);
}

inline void start_sleeping(Player& p, const lvl::Pos& bed_pos) {
    p.g.sleeping = true;
    p.g.sleep_ticks = 0;
    p.g.bed_x = bed_pos.x;
    p.g.bed_y = bed_pos.y;
    p.g.bed_z = bed_pos.z;

    // Bed spawn point
    p.g.respawn_x = bed_pos.x;
    p.g.respawn_y = bed_pos.y;
    p.g.respawn_z = bed_pos.z;
    p.g.respawn_dim = "minecraft:overworld";
    p.g.has_respawn = true;

    player_set_sleeping(p, true, bed_pos.x, bed_pos.y, bed_pos.z);

    // Reset insomnia (time_since_rest)
    progress::award_stat(p, progress::CUSTOM, progress::custom_stat("time_since_rest"), 0);
    progress::award_stat(p, progress::CUSTOM, progress::custom_stat("sleep_in_bed"), 1);
    progress::slept_in_bed(p);
}

// Player interaction with a bed block
inline bool use_bed(Player& pl, const lvl::Pos& p, int s) {
    int dim = pl.level == &worlds[DIM_NETHER] ? DIM_NETHER : (pl.level == &worlds[DIM_END] ? DIM_END : DIM_OVERWORLD);
    if (dim != DIM_OVERWORLD) {
        // Bed explosion in Nether / End!
        pl.level->set_block(p.x, p.y, p.z, blocks::air);
        broadcast_level(block_update(p.x, p.y, p.z, blocks::air), pl.level);
        int dir = lvl::getdir(s);
        lvl::Pos head_or_foot = p.rel(dir);
        if (lvl::is(pl.level->get_block(head_or_foot.x, head_or_foot.y, head_or_foot.z), lvl::tag("beds"))) {
            pl.level->set_block(head_or_foot.x, head_or_foot.y, head_or_foot.z, blocks::air);
            broadcast_level(block_update(head_or_foot.x, head_or_foot.y, head_or_foot.z, blocks::air), pl.level);
        }
        boss::explode(p.x + 0.5, p.y + 0.5, p.z + 0.5, 5.0f, 1, true, -1, pl.eid);
        return true;
    }

    // Overworld: check range
    double dx = p.x + 0.5 - pl.x, dy = p.y + 0.5 - pl.y, dz = p.z + 0.5 - pl.z;
    if (dx * dx + dy * dy + dz * dz > 25.0) {
        send_action_bar(pl, "block.minecraft.bed.too_far_away");
        return true;
    }

    // Time check: only at night (12542..23458) or during thunderstorm
    int64_t t = clocks[0].total % 24000;
    bool night = t >= 12542 && t <= 23458;
    if (!night && !weather.thundering) {
        send_action_bar(pl, "block.minecraft.bed.no_sleep");
        return true;
    }

    // Monster check
    if (pl.gamemode != 1 && monsters_near(p.x + 0.5, p.y + 0.5, p.z + 0.5, 8.0, 5.0)) {
        send_action_bar(pl, "block.minecraft.bed.not_safe");
        return true;
    }

    // Occupied check
    if (is_bed_occupied(p)) {
        send_action_bar(pl, "block.minecraft.bed.occupied");
        return true;
    }

    start_sleeping(pl, p);
    return true;
}

// Player interaction with a respawn anchor
inline bool use_respawn_anchor(Player& pl, const lvl::Pos& p, int s, ItemStack& slot) {
    int dim = pl.level == &worlds[DIM_NETHER] ? DIM_NETHER : (pl.level == &worlds[DIM_END] ? DIM_END : DIM_OVERWORLD);
    int charges = lvl::has(s, "charges") ? lvl::geti(s, "charges") : 0;

    if (dim != DIM_NETHER) {
        // Explodes in Overworld or End if charged
        if (charges > 0) {
            pl.level->set_block(p.x, p.y, p.z, blocks::air);
            broadcast_level(block_update(p.x, p.y, p.z, blocks::air), pl.level);
            boss::explode(p.x + 0.5, p.y + 0.5, p.z + 0.5, 5.0f, 1, true, -1, pl.eid);
            return true;
        }
        return false;
    }

    // In Nether:
    if (lvl::item_name_is(slot.item, "glowstone") && charges < 4) {
        int next_state = lvl::with(s, "charges", charges + 1);
        pl.level->set_block(p.x, p.y, p.z, next_state);
        broadcast_level(block_update(p.x, p.y, p.z, next_state), pl.level);
        play_sound("minecraft:block.respawn_anchor.charge", 4, p.x + 0.5, p.y + 0.5, p.z + 0.5, 1.0f, 1.0f);
        if (pl.gamemode != 1) {
            if (--slot.count <= 0) slot = {};
        }
        return true;
    }

    if (charges > 0) {
        pl.g.respawn_x = p.x;
        pl.g.respawn_y = p.y;
        pl.g.respawn_z = p.z;
        pl.g.respawn_dim = "minecraft:the_nether";
        pl.g.has_respawn = true;
        play_sound("minecraft:block.respawn_anchor.set_spawn", 4, p.x + 0.5, p.y + 0.5, p.z + 0.5, 1.0f, 1.0f);
        send_action_bar(pl, "block.minecraft.respawn_anchor.set_spawn");
        return true;
    }

    return false;
}

// Tick hook for sleeping players and night skipping
inline void tick_sleeping() {
    int sleeping = 0, active = 0, deep_sleeping = 0;

    for (auto& pp : players) {
        if (!pp) continue;
        Player& p = *pp;
        if (p.gamemode == 3) continue;
        active++;
        if (p.g.sleeping) {
            // Check wake up conditions (damaged, sneaking)
            if (p.g.dead || p.g.last_hurt > 0 || p.g.sneaking) {
                wake_up(p);
                continue;
            }
            p.g.sleep_ticks++;
            sleeping++;
            if (p.g.sleep_ticks >= 100) deep_sleeping++;
        }
    }

    if (active == 0 || sleeping == 0) return;

    int pct = rule("players_sleeping_percentage");
    int needed = std::max(1, int(std::ceil(float(active * pct) / 100.0f)));

    if (sleeping >= needed && deep_sleeping >= needed) {
        // Skip night!
        int64_t current = clocks[0].total;
        int64_t day = current / 24000;
        clocks[0].total = (day + 1) * 24000;
        clocks[0].partial = 0;
        clock_changed(0);
        broadcast(set_time_packet());

        // Reset weather if raining
        if (rule("advance_weather") && weather.raining) {
            weather.clear_time = uniform(12000, 180000);
            weather.rain_time = 0;
            weather.thunder_time = 0;
            weather.raining = false;
            weather.thundering = false;
            weather.rain_level = 0.0f;
            weather.thunder_level = 0.0f;
            broadcast(game_event(2, 0));  // end rain
            broadcast(game_event(7, 0));
            broadcast(game_event(8, 0));
        }

        // Wake all sleepers
        for (auto& pp : players) {
            if (pp && pp->g.sleeping) wake_up(*pp);
        }
    }
}

}  // namespace env
