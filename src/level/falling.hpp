#pragma once
// FallingBlockEntity (sand, gravel, concrete powder, anvils...), self-contained: its own list, tracking and a
// small Entity.move over the real collision boxes. Tick thread only.
// ponytail: separate from the item/mob entity lists; fold into the entity framework (mobs-core) once it lands.
#include "behaviours.hpp"

namespace lvl {

struct FallingBlock {
    int eid;
    int64_t uh, ul;
    double x, y, z, vx = 0, vy = 0, vz = 0;
    double ox, oy, oz;
    int state, time = 0;
    bool on_ground = false, removed = false, hurt_entities = false, cancel_drop = false;
    float damage_per_distance = 0;
    int damage_max = 40;
    double fall_distance = 0;
    std::vector<int> watchers;
};
inline std::vector<FallingBlock> falling_blocks;
inline constexpr double FB_HALF = 0.49, FB_H = 0.98;

// Entity.collide for an AABB against block collision boxes, one axis at a time (y, then x/z by size).
inline double clip_axis(double x0, double y0, double z0, double x1, double y1, double z1, int ax, double d) {
    if (d == 0) return 0;
    double lo[3] = {x0, y0, z0}, hi[3] = {x1, y1, z1};
    double elo[3] = {x0, y0, z0}, ehi[3] = {x1, y1, z1};
    if (d > 0) ehi[ax] += d; else elo[ax] += d;
    for (int bx = int(std::floor(elo[0] - 1e-7)); bx <= int(std::floor(ehi[0] + 1e-7)); bx++)
        for (int by = int(std::floor(elo[1] - 1e-7)) - 1; by <= int(std::floor(ehi[1] + 1e-7)); by++)
            for (int bz = int(std::floor(elo[2] - 1e-7)); bz <= int(std::floor(ehi[2] + 1e-7)); bz++) {
                if (!in_height(by)) continue;
                int s = get(bx, by, bz);
                for (auto& b : collision(s)) {
                    double blo[3] = {bx + b.x0, by + b.y0, bz + b.z0}, bhi[3] = {bx + b.x1, by + b.y1, bz + b.z1};
                    bool overlap = true;
                    for (int k = 0; k < 3 && overlap; k++)
                        if (k != ax) overlap = bhi[k] - 1e-7 > lo[k] && blo[k] + 1e-7 < hi[k];
                    if (!overlap) continue;
                    if (d > 0 && blo[ax] >= hi[ax] - 1e-7) d = std::min(d, blo[ax] - hi[ax]);
                    else if (d < 0 && bhi[ax] <= lo[ax] + 1e-7) d = std::max(d, bhi[ax] - lo[ax]);
                }
            }
    return d;
}

// FallingBlockEntity.fall
inline void start_falling(const Pos& p, int state) {
    FallingBlock f;
    f.eid = next_entity_id++;
    f.uh = int64_t((game_rng() & ~0xF000ull) | 0x4000), f.ul = int64_t((game_rng() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull);
    f.x = f.ox = p.x + 0.5, f.y = f.oy = p.y, f.z = f.oz = p.z + 0.5;
    f.state = has(state, "waterlogged") ? with(state, "waterlogged", false) : state;
    if (kind(state) == K_ANVIL) f.hurt_entities = true, f.damage_per_distance = 2.0f, f.damage_max = 40;  // AnvilBlock.falling
    set(p, legacy_fluid_block(state));
    falling_blocks.push_back(f);
}

inline void fb_drop(FallingBlock& f) {  // spawnAtLocation(block): the block's item
    if (!rule("entity_drops")) return;
    int item = pick_item(f.state);
    if (item >= 0) spawn_item({item, 1, 0}, f.x, f.y, f.z);
}

inline void fb_land_damage(FallingBlock& f) {  // causeFallDamage (anvils)
    if (!f.hurt_entities) return;
    int dist = int(std::ceil(f.fall_distance - 1.0));
    if (dist < 0) return;
    float dmg = std::min(float(std::floor(dist * f.damage_per_distance)), float(f.damage_max));
    for (auto& pl : players)
        if (pl->gamemode != 1 && pl->gamemode != 3 && std::abs(pl->x - f.x) < 0.3 + FB_HALF && std::abs(pl->z - f.z) < 0.3 + FB_HALF &&
            pl->y < f.y + FB_H && pl->y + 1.8 > f.y)
            hurt(*pl, dmg, "falling_anvil");
    if (dmg > 0 && rand_float() < 0.05f + dist * 0.05f) {  // AnvilBlock.damage
        std::string n = block_of(f.state).name;
        int facing = getdir(f.state);
        if (n == "minecraft:anvil") f.state = withdir(def("chipped_anvil"), "facing", facing);
        else if (n == "minecraft:chipped_anvil") f.state = withdir(def("damaged_anvil"), "facing", facing);
        else f.cancel_drop = true;
    }
}

inline void tick_falling(FallingBlock& f) {
    if (is_air(f.state)) { f.removed = true; return; }
    f.time++;
    f.ox = f.x, f.oy = f.y, f.oz = f.z;
    f.vy -= 0.04;  // applyGravity
    // move(SELF, delta)
    double dy = clip_axis(f.x - FB_HALF, f.y, f.z - FB_HALF, f.x + FB_HALF, f.y + FB_H, f.z + FB_HALF, 1, f.vy);
    f.y += dy;
    double dx = clip_axis(f.x - FB_HALF, f.y, f.z - FB_HALF, f.x + FB_HALF, f.y + FB_H, f.z + FB_HALF, 0, f.vx);
    f.x += dx;
    double dz = clip_axis(f.x - FB_HALF, f.y, f.z - FB_HALF, f.x + FB_HALF, f.y + FB_H, f.z + FB_HALF, 2, f.vz);
    f.z += dz;
    bool vertical = dy != f.vy;
    f.on_ground = vertical && f.vy < 0;
    if (dx != f.vx) f.vx = 0;
    if (dz != f.vz) f.vz = 0;
    if (f.on_ground) {
        fb_land_damage(f);
        f.fall_distance = 0;
    } else if (dy < 0) {
        f.fall_distance -= dy;
    }
    if (vertical) f.vy = 0;  // Block.updateEntityMovementAfterFallOn
    Pos p{int(std::floor(f.x)), int(std::floor(f.y)), int(std::floor(f.z))};
    bool concrete = kind(f.state) == K_CONCRETE_POWDER;
    bool stuck_in_water = concrete && is_water(get(p));
    // ponytail: the fast-concrete clip (moveVec > 1) checks only the end position.
    if (!f.on_ground && !stuck_in_water) {
        if ((f.time > 100 && (p.y <= world.min_y() || p.y > world.max_y() - 1)) || f.time > 600) {
            fb_drop(f);
            f.removed = true;
        }
    } else {
        int cur = get(p);
        f.vx *= 0.7, f.vy *= -0.5, f.vz *= 0.7;
        if (!f.cancel_drop) {
            bool may_replace = block(cur) == ID.snow ? geti(cur, "layers") == 1 : can_be_replaced(cur);
            bool continue_falling = falling_is_free(get(p.below())) && (!concrete || !stuck_in_water);
            bool survive = can_survive(f.state, p) && !continue_falling;
            if (may_replace && survive) {
                if (has(f.state, "waterlogged") && fluid_type(get(p)) == WATER) f.state = with(f.state, "waterlogged", true);
                if (set(p, f.state)) {
                    f.removed = true;
                    // Fallable.onLand
                    if (concrete && (concrete_can_solidify(cur) || concrete_touches_liquid(p))) set(p, concrete_of(f.state));
                    if (kind(f.state) == K_ANVIL) level_event(1031, p, 0);
                } else {
                    f.removed = true;
                    if (kind(f.state) == K_ANVIL) level_event(1029, p, 0);
                    fb_drop(f);
                }
            } else {
                f.removed = true;
                if (kind(f.state) == K_ANVIL) level_event(1029, p, 0);
                fb_drop(f);
            }
        } else {
            f.removed = true;
            if (kind(f.state) == K_ANVIL) level_event(1029, p, 0);
        }
    }
    f.vx *= 0.98, f.vy *= 0.98, f.vz *= 0.98;
}

// Tracking: add_entity (data = block state) / teleport every 20 ticks (updateInterval) / remove.
inline void track_falling() {
    for (auto& f : falling_blocks) {
        int cx = int(std::floor(f.x)) >> 4, cz = int(std::floor(f.z)) >> 4;
        bool sync = f.time % 20 == 0;
        for (auto& pp : players) {
            Player& p = *pp;
            auto w = std::find(f.watchers.begin(), f.watchers.end(), p.eid);
            bool in = !f.removed && sees_chunk(p, cx, cz);
            if (in && w == f.watchers.end()) {
                p.send(add_entity_packet(f.eid, f.uh, f.ul, entity_types::falling_block, f.x, f.y, f.z, f.vx, f.vy, f.vz, 0, 0, f.state));
                f.watchers.push_back(p.eid);
            } else if (!in && w != f.watchers.end()) {
                p.send(remove_entities_packet({f.eid}));
                f.watchers.erase(w);
            } else if (in && sync) {
                p.send(teleport_entity_packet(f.eid, f.x, f.y, f.z, f.vx, f.vy, f.vz, 0, 0, f.on_ground));
            }
        }
    }
    std::erase_if(falling_blocks, [](const FallingBlock& f) { return f.removed; });
}

inline void tick_falling_blocks() {
    for (size_t i = 0; i < falling_blocks.size(); i++) {
        FallingBlock& f = falling_blocks[i];
        if (!f.removed && loaded(int(std::floor(f.x)) >> 4, int(std::floor(f.z)) >> 4)) tick_falling(falling_blocks[i]);
    }
    track_falling();
}

}  // namespace lvl
