#pragma once
// Level helpers the boss fights need (tick thread): block edits with client updates, heightmaps, block clip,
// level events, sounds, particles and ServerExplosion.
// ponytail: single World (`world`); the dimensions branch splits Overworld/End and boss_level() follows it.
#include "emth.hpp"
#include "end_features.hpp"

#include <cfloat>

namespace boss {
// Vec3::dist_sqr against loose coordinates
inline double dsq(const Vec3& v, double x, double y, double z) { return v.dist_sqr(Vec3{x, y, z}); }

inline World& boss_level() { return world; }
inline int64_t level_seed() {
    static int64_t s = [] {
        auto kv = read_level_data();
        return kv.count("level-seed") ? parse_seed(kv["level-seed"]) : int64_t(0);
    }();
    return s;
}

inline int get_block(int x, int y, int z) { return block_at(x, y, z); }  // loaded chunks only (unloaded = air)
inline void set_block(int x, int y, int z, int state) {  // Level.setBlockAndUpdate (client update; no neighbour updates)
    if (get_block(x, y, z) == state) return;
    change_block(x, y, z, state);
}
inline bool is_air(int state) { return state_flags()[size_t(state)] & STATE_AIR; }
inline bool has_tag(const std::vector<uint16_t>& tag, int state) { return in_sorted(tag, block_index(state)); }

// State of `state`'s block with property `prop` = `value` (no-op when the block lacks it).
inline int with_prop(int state, const std::string& prop, const std::string& value) {
    const BlockInfo& b = block_of(state);
    int off = state - b.first, stride = 1;
    for (int k = int(b.props.size()) - 1; k >= 0; k--) {
        auto& p = b.props[size_t(k)];
        int n = int(p.values.size());
        if (p.name == prop) {
            auto it = std::find(p.values.begin(), p.values.end(), value);
            if (it == p.values.end()) return state;
            int cur = off / stride % n;
            return state + (int(it - p.values.begin()) - cur) * stride;
        }
        stride *= n;
    }
    return state;
}

inline const endgen::States& end_states() {
    static const endgen::States st = [] {
        endgen::States s{};
        s.air = blocks::air, s.obsidian = blocks::obsidian, s.bedrock = blocks::bedrock, s.end_stone = blocks::end_stone;
        s.end_portal = blocks::end_portal, s.end_gateway = blocks::end_gateway, s.fire = blocks::fire;
        s.wall_torch_n = with_prop(blocks::wall_torch, "facing", "north");
        s.wall_torch_e = with_prop(blocks::wall_torch, "facing", "east");
        s.wall_torch_s = with_prop(blocks::wall_torch, "facing", "south");
        s.wall_torch_w = with_prop(blocks::wall_torch, "facing", "west");
        for (int i = 0; i < 16; i++) {
            int b = blocks::iron_bars;
            const char* t[2] = {"false", "true"};
            b = with_prop(b, "north", t[i & 1]);
            b = with_prop(b, "south", t[i >> 1 & 1]);
            b = with_prop(b, "west", t[i >> 2 & 1]);
            b = with_prop(b, "east", t[i >> 3 & 1]);
            s.iron_bars[i] = b;
        }
        return s;
    }();
    return st;
}

// Heightmap.Types MOTION_BLOCKING / MOTION_BLOCKING_NO_LEAVES: y above the highest motion-blocking block or
// fluid. ponytail: blocksMotion approximated by "has collision".
inline int heightmap(int x, int z, bool no_leaves) {
    if (!chunk_loaded_at(x, z)) return MIN_Y;  // unloaded columns read as barrier
    for (int y = MIN_Y + HEIGHT - 1; y >= MIN_Y; y--) {
        int s = get_block(x, y, z);
        if (is_air(s)) continue;
        bool motion = block_of(s).collision || (state_flags()[size_t(s)] & STATE_FLUID);
        if (no_leaves && block_of(s).name.ends_with("_leaves")) motion = false;
        if (motion) return y + 1;
    }
    return MIN_Y;
}

// Level.clip with ClipContext.Block.COLLIDER: does the segment a->b cross a solid block? (BlockGetter.traverseBlocks)
// ponytail: every colliding block is a full cube, like move_box.
inline bool clip_hits(const Vec3& a, const Vec3& b) {
    if (a.x == b.x && a.y == b.y && a.z == b.z) return false;
    double x0 = a.x - 1.0E-7 * (b.x - a.x), y0 = a.y - 1.0E-7 * (b.y - a.y), z0 = a.z - 1.0E-7 * (b.z - a.z);
    double x1 = b.x - 1.0E-7 * (a.x - b.x), y1 = b.y - 1.0E-7 * (a.y - b.y), z1 = b.z - 1.0E-7 * (a.z - b.z);
    int bx = int(std::floor(x0)), by = int(std::floor(y0)), bz = int(std::floor(z0));
    auto solid = [](int x, int y, int z) { return y >= MIN_Y && y < MIN_Y + HEIGHT && block_of(get_block(x, y, z)).collision; };
    if (solid(bx, by, bz)) return true;
    double dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
    int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0, sy = dy > 0 ? 1 : dy < 0 ? -1 : 0, sz = dz > 0 ? 1 : dz < 0 ? -1 : 0;
    double tdx = sx == 0 ? DBL_MAX : sx / dx, tdy = sy == 0 ? DBL_MAX : sy / dy, tdz = sz == 0 ? DBL_MAX : sz / dz;
    auto frac = [](double v) { return v - std::floor(v); };
    double tx = tdx * (sx > 0 ? 1 - frac(x0) : frac(x0)), ty = tdy * (sy > 0 ? 1 - frac(y0) : frac(y0)),
           tz = tdz * (sz > 0 ? 1 - frac(z0) : frac(z0));
    while (tx <= 1 || ty <= 1 || tz <= 1) {
        if (tx < ty) {
            if (tx < tz) bx += sx, tx += tdx;
            else bz += sz, tz += tdz;
        } else if (ty < tz) by += sy, ty += tdy;
        else bz += sz, tz += tdz;
        if (solid(bx, by, bz)) return true;
    }
    return false;
}

inline AABB player_box(const Player& p) { return {p.x - 0.3, p.y, p.z - 0.3, p.x + 0.3, p.y + 1.8, p.z + 0.3}; }

// ServerExplosion.getSeenPercent
inline float seen_percent(const Vec3& center, const AABB& bb) {
    double xs = 1.0 / ((bb.x1 - bb.x0) * 2.0 + 1.0), ys = 1.0 / ((bb.y1 - bb.y0) * 2.0 + 1.0), zs = 1.0 / ((bb.z1 - bb.z0) * 2.0 + 1.0);
    double xo = (1.0 - std::floor(1.0 / xs) * xs) / 2.0, zo = (1.0 - std::floor(1.0 / zs) * zs) / 2.0;
    if (xs < 0 || ys < 0 || zs < 0) return 0;
    int hits = 0, count = 0;
    for (double xx = 0.0; xx <= 1.0; xx += xs)
        for (double yy = 0.0; yy <= 1.0; yy += ys)
            for (double zz = 0.0; zz <= 1.0; zz += zs) {
                Vec3 from{bb.x0 + xx * (bb.x1 - bb.x0) + xo, bb.y0 + yy * (bb.y1 - bb.y0), bb.z0 + zz * (bb.z1 - bb.z0) + zo};
                hits += !clip_hits(from, center);
                count++;
            }
    return float(hits) / count;
}

// Player.hurtServer difficulty scaling for sources that scale (explosions; mob attacks by non-players).
inline bool hurt_player(Player& p, float dmg, const std::string& type, bool scales) {
    if (scales && survival_like(p)) {
        int d = difficulty();
        if (d == 0) dmg = 0;
        else if (d == 1) dmg = std::min(dmg / 2.0F + 1.0F, dmg);
        else if (d == 3) dmg = dmg * 3.0F / 2.0F;
    }
    return dmg > 0 && hurt(p, dmg, type);
}
inline void push_player(Player& p, double x, double y, double z) {  // Entity.push + hurtMarked: client adds the impulse
    p.send(set_entity_motion_packet(p.eid, x, y, z));
}

// ---------- client effects ----------
inline int64_t pack_pos(int x, int y, int z) {
    return int64_t((uint64_t(int64_t(x) & 0x3FFFFFF) << 38) | (uint64_t(int64_t(z) & 0x3FFFFFF) << 12) | uint64_t(int64_t(y) & 0xFFF));
}
// Level.levelEvent (64-block radius) / globalLevelEvent (everyone in the level).
inline void level_event(int type, int x, int y, int z, int data, bool global = false) {
    Writer w;
    w.varint(play_cb::level_event).i32(type).i64(pack_pos(x, y, z)).i32(data).boolean(global);
    for (auto& p : players) {
        double dx = p->x - x, dy = p->y - y, dz = p->z - z;
        if (global || dx * dx + dy * dy + dz * dz < 64.0 * 64.0) p->send(w);
    }
}
inline void play_sound(const std::string& id, int source, double x, double y, double z, float vol, float pitch) {
    double range = vol > 1 ? 16.0 * vol : 16.0;
    Writer w;
    w.varint(play_cb::sound).varint(0).str(id).boolean(false).varint(source).i32(int32_t(x * 8)).i32(int32_t(y * 8))
        .i32(int32_t(z * 8)).f32(vol).f32(pitch).i64(int64_t(game_rng()));
    for (auto& p : players) {
        double dx = p->x - x, dy = p->y - y, dz = p->z - z;
        if (dx * dx + dy * dy + dz * dz < range * range) p->send(w);
    }
}
inline int particle_id(const char* name) { return registry_index(*registry("minecraft:particle_type"), std::string("minecraft:") + name); }
inline int sound_source_hostile = 5, sound_source_blocks = 4;

// ---------- explosions (ServerExplosion) ----------
// Non-player entities (entities.hpp): hurt/knock every entity near the blast; `source_eid` is skipped.
inline void (*explosion_entities)(const Vec3& center, float radius, int source_eid, int attacker_player_eid) = nullptr;

// Level.explode(source, x, y, z, r, interaction): interaction 0 NONE (keep blocks), 1 BLOCK, 2 MOB (mob_griefing).
// `fire`: createFire. attacker_player_eid: player credited for the explosion (-1 none).
inline void explode(double x, double y, double z, float radius, int interaction, bool fire = false, int source_eid = -1,
                    int attacker_player_eid = -1, bool silent = false) {
    bool destroy = interaction == 1 || (interaction == 2 && rule("mob_griefing"));
    Vec3 c{x, y, z};
    std::vector<std::tuple<int, int, int>> blow;
    std::set<std::tuple<int, int, int>> seen;
    for (int xx = 0; xx < 16; xx++)
        for (int yy = 0; yy < 16; yy++)
            for (int zz = 0; zz < 16; zz++) {
                if (!(xx == 0 || xx == 15 || yy == 0 || yy == 15 || zz == 0 || zz == 15)) continue;
                double xd = xx / 15.0F * 2.0F - 1.0F, yd = yy / 15.0F * 2.0F - 1.0F, zd = zz / 15.0F * 2.0F - 1.0F;
                double d = std::sqrt(xd * xd + yd * yd + zd * zd);
                xd /= d, yd /= d, zd /= d;
                float power = radius * (0.7F + rand_float() * 0.6F);
                double xp = x, yp = y, zp = z;
                for (; power > 0.0F; power -= 0.22500001F) {
                    int bx = int(std::floor(xp)), by = int(std::floor(yp)), bz = int(std::floor(zp));
                    if (by < MIN_Y || by >= MIN_Y + HEIGHT) break;
                    int s = get_block(bx, by, bz);
                    if (!is_air(s)) power -= (block_of(s).resistance + 0.3F) * 0.3F;
                    if (power > 0.0F && !is_air(s) && seen.insert({bx, by, bz}).second) blow.push_back({bx, by, bz});
                    xp += xd * 0.3F, yp += yd * 0.3F, zp += zd * 0.3F;
                }
            }
    if (explosion_entities) explosion_entities(c, radius, source_eid, attacker_player_eid);
    std::map<int, Vec3> knock;  // ServerExplosion.hurtEntities for players
    float r2 = radius * 2.0F;
    for (auto& pp : players) {
        Player& p = *pp;
        if (p.gamemode == 3 || p.g.dead || radius < 1.0E-5F) continue;
        double dist = std::sqrt(dsq(Vec3{p.x, p.y, p.z}, x, y, z)) / r2;
        if (dist > 1.0) continue;
        Vec3 dir = (Vec3{p.x, p.y + 1.62, p.z} - c).normalize();
        float exposure = seen_percent(c, player_box(p));
        double pw = (1.0 - dist) * exposure;
        hurt_player(p, float((pw * pw + pw) / 2.0 * 7.0 * r2 + 1.0), attacker_player_eid >= 0 ? "player_explosion" : "explosion", true);
        if (p.gamemode != 1) knock[p.eid] = dir * ((1.0 - dist) * exposure);  // ponytail: no knockback resistance
    }
    if (destroy) {
        for (size_t i = blow.size(); i > 1; i--) std::swap(blow[i - 1], blow[size_t(rand_int(int(i)))]);  // Util.shuffle
        bool decay = interaction == 1 ? rule("block_explosion_drop_decay") : rule("mob_explosion_drop_decay");
        std::vector<ItemStack> drops;
        for (auto& [bx, by, bz] : blow) {
            int s = get_block(bx, by, bz);
            if (is_air(s) || block_of(s).resistance >= 3600000.0f) continue;  // ponytail: no TNT priming / per-block onExplosionHit
            drops.clear();
            if (rule("block_drops") && block_of(s).loot >= 0) {
                LootContext ctx{s, bx, by, bz, {}, decay ? radius : 0};
                roll_loot(loot_tables[size_t(block_of(s).loot)], ctx, drops);
            }
            boss_level().set_block(bx, by, bz, blocks::air);
            for (auto& d : drops) pop_resource(bx, by, bz, d);  // ponytail: vanilla merges drops into 16-stacks first
        }
        // one section update per block is simplest; clients batch them fine
        for (auto& [bx, by, bz] : blow) broadcast(block_update(bx, by, bz, get_block(bx, by, bz)));
    }
    if (fire)
        for (auto& [bx, by, bz] : blow)
            if (rand_int(3) == 0 && is_air(get_block(bx, by, bz)) && block_of(get_block(bx, by - 1, bz)).collision)
                set_block(bx, by, bz, blocks::fire);
    // ClientboundExplodePacket (knockback is applied server-side through set_entity_motion instead).
    static const int p_explosion = particle_id("explosion"), p_emitter = particle_id("explosion_emitter"),
                     p_poof = particle_id("poof"), p_smoke = particle_id("smoke");
    bool small = radius < 2.0F || !destroy;
    for (auto& p : players) {
        double dx = p->x - x, dy = p->y - y, dz = p->z - z;
        if (dx * dx + dy * dy + dz * dz >= 4096.0) continue;
        Writer w;
        w.varint(play_cb::explode).f64(x).f64(y).f64(z).f32(radius).i32(int32_t(blow.size()));
        auto k = knock.find(p->eid);
        w.boolean(k != knock.end());
        if (k != knock.end()) w.f64(k->second.x).f64(k->second.y).f64(k->second.z);
        w.varint(small ? p_explosion : p_emitter).varint(0).str("minecraft:entity.generic.explode").boolean(false)
            .varint(2).varint(p_poof).f32(0.5F).f32(1.0F).varint(1).varint(p_smoke).f32(1.0F).f32(1.0F).varint(1).boolean(!silent);
        p->send(w);
    }
}

}  // namespace boss
