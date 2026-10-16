#pragma once
// Boss-fight entities: a small entity base with tracking and synched data, EndCrystal, DragonFireball,
// AreaEffectCloud (dragon breath) and ExperienceOrb. Tick thread only.
// ponytail: stand-in for the mobs-core entity framework; port these onto it once it lands.
#include "level.hpp"

#include <cfloat>
#include <memory>

namespace boss {

// ---------- synched entity data (EntityDataSerializers ids) ----------
namespace meta {
inline void byte(Writer& w, int i, int v) { w.u8(uint8_t(i)).varint(0).u8(uint8_t(v)); }
inline void i32(Writer& w, int i, int v) { w.u8(uint8_t(i)).varint(1).varint(v); }
inline void f32(Writer& w, int i, float v) { w.u8(uint8_t(i)).varint(3).f32(v); }
inline void boolean(Writer& w, int i, bool v) { w.u8(uint8_t(i)).varint(8).boolean(v); }
inline void opt_pos(Writer& w, int i, const std::optional<std::array<int, 3>>& p) {
    w.u8(uint8_t(i)).varint(11).boolean(bool(p));
    if (p) w.i64(pack_pos((*p)[0], (*p)[1], (*p)[2]));
}
inline void particle(Writer& w, int i, int type, float power) {  // PowerParticleOption (dragon_breath)
    w.u8(uint8_t(i)).varint(16).varint(type).f32(power);
}
}  // namespace meta

// DamageSource, reduced to what the bosses look at.
struct Entity;
struct Dmg {
    std::string type = "generic";
    Player* player = nullptr;      // getEntity() when a player is responsible
    Entity* causing = nullptr;     // getEntity() when a (boss) entity is responsible
    Entity* direct = nullptr;      // getDirectEntity()
    bool explosion = false, bypass_invul = false;  // IS_EXPLOSION / BYPASSES_INVULNERABILITY (generic_kill)
    bool always_hurts_dragon = false;  // ALWAYS_HURTS_ENDER_DRAGONS (explosions)
};

struct Entity {
    int eid = 0, type = 0;
    int64_t uh = 0, ul = 0;
    double x = 0, y = 0, z = 0;
    Vec3 vel;
    float yaw = 0, pitch = 0, width = 0, height = 0;
    int tick_count = 0;
    bool removed = false, invulnerable = false, sync_motion = false;
    int extra_ids = 0;  // ids after eid owned by this entity (dragon parts)
    double sx = 0, sy = 0, sz = 0;
    float syaw = 0, spitch = 0;
    std::vector<int> watchers;
    Writer dirty;  // synched-data entries changed this tick (no terminator)

    virtual ~Entity() = default;
    virtual void tick() {}
    virtual void write_meta(Writer&) {}  // every entry that differs from the client default
    virtual int spawn_data() const { return 0; }
    virtual bool hurt(const Dmg&, float, int part = -1) { return (void)part, false; }
    virtual void kill() { removed = true; }  // Entity.kill (/kill)
    virtual bool living() const { return false; }
    virtual std::string type_name() const = 0;
    virtual void on_removed() {}

    AABB box() const { return {x - width / 2, y, z - width / 2, x + width / 2, y + height, z + width / 2}; }
    Vec3 pos() const { return {x, y, z}; }
    double dist_sqr(const Entity& o) const { return pos().dist_sqr(o.x, o.y, o.z); }
    void discard() { removed = true; }
};

inline std::vector<std::unique_ptr<Entity>> entities;

template <class T, class... A> inline T& add(double x, double y, double z, A&&... args) {
    auto e = std::make_unique<T>(std::forward<A>(args)...);
    e->eid = next_entity_id.fetch_add(1 + e->extra_ids);
    e->uh = int64_t((game_rng() & ~0xF000ull) | 0x4000), e->ul = int64_t((game_rng() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull);
    e->x = e->sx = x, e->y = e->sy = y, e->z = e->sz = z;
    T& ref = *e;
    entities.push_back(std::move(e));
    return ref;
}
inline Entity* find(int eid) {  // Level.getEntityOrPart
    for (auto& e : entities)
        if (!e->removed && eid >= e->eid && eid <= e->eid + e->extra_ids) return e.get();
    return nullptr;
}
template <class T> inline std::vector<T*> all_of() {
    std::vector<T*> v;
    for (auto& e : entities)
        if (!e->removed)
            if (auto* t = dynamic_cast<T*>(e.get())) v.push_back(t);
    return v;
}
template <class T> inline std::vector<T*> in_box(const AABB& b) {
    std::vector<T*> v;
    for (auto* t : all_of<T>())
        if (t->box().intersects(b)) v.push_back(t);
    return v;
}
inline Player* player_by_eid(int eid) {
    for (auto& p : players)
        if (p->eid == eid) return p.get();
    return nullptr;
}
inline bool alive_target(const Player& p) { return !p.g.dead && p.gamemode != 3; }
inline bool attackable(const Player& p) { return alive_target(p) && p.gamemode != 1; }  // canBeSeenAsEnemy

// Level.getNearestPlayer(TargetingConditions.forCombat() [range], x, y, z): survival/adventure players.
inline Player* nearest_player(double x, double y, double z, double range = -1, bool (*extra)(const Player&) = nullptr) {
    Player* best = nullptr;
    double bd = -1;
    for (auto& pp : players) {
        Player& p = *pp;
        if (!attackable(p) || (extra && !extra(p))) continue;
        double d = Vec3{p.x, p.y, p.z}.dist_sqr(x, y, z);
        if (range > 0 && d > range * range) continue;
        if (bd < 0 || d < bd) bd = d, best = &p;
    }
    return best;
}

// ---------- tracking (ChunkMap.TrackedEntity / ServerEntity) ----------
inline void track() {
    for (auto& up : entities) {
        Entity& e = *up;
        int cx = int(std::floor(e.x)) >> 4, cz = int(std::floor(e.z)) >> 4;
        bool moved = e.x != e.sx || e.y != e.sy || e.z != e.sz || e.yaw != e.syaw || e.pitch != e.spitch;
        Writer meta_pkt;
        if (!e.dirty.b.empty()) {
            meta_pkt.varint(play_cb::set_entity_data).varint(e.eid).bytes(e.dirty.b).u8(0xFF);
            e.dirty.b.clear();
        }
        for (auto& pp : players) {
            Player& p = *pp;
            auto w = std::find(e.watchers.begin(), e.watchers.end(), p.eid);
            bool in = !e.removed && sees_chunk(p, cx, cz);
            if (in && w == e.watchers.end()) {
                p.send(add_entity_packet(e.eid, e.uh, e.ul, e.type, e.x, e.y, e.z, e.vel.x, e.vel.y, e.vel.z, e.yaw, e.pitch,
                                         e.spawn_data()));
                Writer m;
                e.write_meta(m);
                if (!m.b.empty()) p.send(Writer().varint(play_cb::set_entity_data).varint(e.eid).bytes(m.b).u8(0xFF));
                e.watchers.push_back(p.eid);
            } else if (!in && w != e.watchers.end()) {
                p.send(remove_entities_packet({e.eid}));
                e.watchers.erase(w);
            } else if (in) {
                if (!meta_pkt.b.empty()) p.send(meta_pkt);
                if (moved) p.send(teleport_entity_packet(e.eid, e.x, e.y, e.z, e.vel.x, e.vel.y, e.vel.z, e.yaw, e.pitch, false));
                if (e.sync_motion) p.send(set_entity_motion_packet(e.eid, e.vel.x, e.vel.y, e.vel.z));
            }
        }
        e.sx = e.x, e.sy = e.y, e.sz = e.z, e.syaw = e.yaw, e.spitch = e.pitch;
        e.sync_motion = false;
    }
    for (auto& e : entities)
        if (e->removed) e->on_removed();
    std::erase_if(entities, [](const std::unique_ptr<Entity>& e) { return e->removed; });
}
inline void forget_player(Player& p) {  // the client dropped its entities (left / respawned)
    for (auto& e : entities) std::erase(e->watchers, p.eid);
}

// ---------- ExperienceOrb ----------
inline int xp_needed(int level) { return level >= 30 ? 112 + (level - 30) * 9 : level >= 15 ? 37 + (level - 15) * 5 : 7 + level * 2; }
inline void give_xp_points(Player& p, int amount) {  // Player.giveExperiencePoints
    p.g.xp_progress += float(amount) / float(xp_needed(p.g.xp_level));
    p.g.xp_total = std::clamp(p.g.xp_total + amount, 0, INT32_MAX);
    while (p.g.xp_progress >= 1.0F) {
        p.g.xp_progress = (p.g.xp_progress - 1.0F) * float(xp_needed(p.g.xp_level));
        p.g.xp_level++;
        p.g.xp_progress /= float(xp_needed(p.g.xp_level));
    }
    send_experience(p);
}

inline std::map<int, int64_t> xp_taken;  // player eid -> tick of the last orb pickup
struct ExperienceOrb : Entity {
    int value = 1, age = 0, count = 1;
    Player* following = nullptr;
    ExperienceOrb(int v) : value(v) {
        type = entity_types::experience_orb, width = height = 0.5f;
        yaw = rand_float() * 360.0f;
        vel = {(rand_double() * 0.2 - 0.1) * 2.0, rand_double() * 0.2 * 2.0, (rand_double() * 0.2 - 0.1) * 2.0};
    }
    std::string type_name() const override { return "experience_orb"; }
    void write_meta(Writer& w) override { meta::i32(w, 8, value); }
    int spawn_data() const override { return 0; }
    void tick() override {
        tick_count++;
        vel.y -= 0.03;
        if (!following || following->g.dead || following->gamemode == 3 ||
            Vec3{following->x, following->y, following->z}.dist_sqr(x, y, z) > 64.0) {
            following = nullptr;
            double bd = 64.0;
            for (auto& p : players)
                if (alive_target(*p))
                    if (double d = Vec3{p->x, p->y, p->z}.dist_sqr(x, y, z); d < bd) bd = d, following = p.get();
        }
        if (following) {
            Vec3 d{(following->x - x) / 8.0, (following->y + 1.62 / 2.0 - y) / 8.0, (following->z - z) / 8.0};
            double power = 1.0 - d.length();
            if (power > 0.0) vel = vel + d.normalize().scale(power * power * 0.1), sync_motion = true;
        }
        bool og, hh;
        move_box(x, y, z, width / 2, height, vel.x, vel.y, vel.z, og, hh);
        double f = og ? 0.98 * 0.6 : 0.98;
        vel = {vel.x * f, vel.y * 0.98, vel.z * f};
        if (++age >= 6000) discard();
        for (auto& pp : players) {  // Player.touch -> playerTouch
            Player& p = *pp;
            if (removed || !alive_target(p) || !player_box(p).inflate(1.0, 0.5, 1.0).intersects(box())) continue;
            if (server_tick - xp_taken[p.eid] < 2) continue;  // Player.takeXpDelay
            xp_taken[p.eid] = server_tick;
            Writer t;
            t.varint(play_cb::take_item_entity).varint(eid).varint(p.eid).varint(1);
            broadcast_near(x, z, t);
            give_xp_points(p, value);  // ponytail: no Mending repair
            if (--count == 0) discard();
        }
    }
};
inline int orb_value(int amount) {  // ExperienceOrb.getExperienceValue
    for (int v : {2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3})
        if (amount >= v) return v;
    return 1;
}
inline void award_xp(double x, double y, double z, int amount) {  // ExperienceOrb.award (no merging)
    while (amount > 0) {
        int v = orb_value(amount);
        amount -= v;
        add<ExperienceOrb>(x, y, z, v);
    }
}

// ---------- EndCrystal ----------
inline bool fight_active();                               // fight.hpp
inline void fight_crystal_destroyed(Entity& c, const Dmg& d);  // fight.hpp
struct EnderDragon;

struct EndCrystal : Entity {
    std::optional<std::array<int, 3>> beam;
    bool show_bottom = true;
    int time = rand_int(100000);
    EndCrystal() { type = entity_types::end_crystal, width = height = 2.0f; }
    std::string type_name() const override { return "end_crystal"; }
    void write_meta(Writer& w) override {
        if (beam) meta::opt_pos(w, 8, beam);
        if (!show_bottom) meta::boolean(w, 9, false);
    }
    void set_beam(std::optional<std::array<int, 3>> b) {
        if (b == beam) return;
        beam = b;
        meta::opt_pos(dirty, 8, beam);
    }
    void set_show_bottom(bool s) {
        if (s == show_bottom) return;
        show_bottom = s;
        meta::boolean(dirty, 9, s);
    }
    void tick() override {
        tick_count++, time++;
        int bx = int(std::floor(x)), by = int(std::floor(y)), bz = int(std::floor(z));
        if (fight_active() && is_air(get_block(bx, by, bz))) set_block(bx, by, bz, blocks::fire);
    }
    bool hurt(const Dmg& d, float, int) override;
    void kill() override {
        Dmg d;
        fight_crystal_destroyed(*this, d);
        removed = true;
    }
};

// ---------- AreaEffectCloud (dragon breath) ----------
struct AreaEffectCloud : Entity {
    float radius = 3.0f, radius_per_tick = 0, duration_scale = 1;
    int duration = -1, wait_time = 20, reapply = 20, amplifier = 0;
    bool waiting = true;
    Entity* owner = nullptr;
    std::map<int, int> victims;  // player eid -> tick when it may be hit again
    AreaEffectCloud() { type = entity_types::area_effect_cloud, width = 6.0f, height = 0.5f; }
    std::string type_name() const override { return "area_effect_cloud"; }
    void set_radius(float r) {
        radius = std::clamp(r, 0.0f, 32.0f);
        width = radius * 2.0f;
        meta::f32(dirty, 8, radius);
    }
    void write_meta(Writer& w) override {
        static const int breath = particle_id("dragon_breath");
        meta::f32(w, 8, radius);
        meta::boolean(w, 9, waiting);
        meta::particle(w, 10, breath, 1.0f);
    }
    void tick() override {
        tick_count++;
        if (duration != -1 && tick_count - wait_time >= duration) return discard();
        bool should_wait = tick_count < wait_time;
        if (waiting != should_wait) waiting = should_wait, meta::boolean(dirty, 9, waiting);
        if (should_wait) return;
        float r = radius;
        if (radius_per_tick != 0) {
            r += radius_per_tick;
            if (r < 0.5f) return discard();
            set_radius(r);
        }
        if (tick_count % 5 != 0) return;
        std::erase_if(victims, [&](auto& v) { return tick_count >= v.second; });
        for (auto& pp : players) {
            Player& p = *pp;
            if (!alive_target(p) || victims.count(p.eid) || !player_box(p).intersects(box())) continue;
            double dx = p.x - x, dz = p.z - z;
            if (dx * dx + dz * dz > double(r) * r) continue;
            victims[p.eid] = tick_count + reapply;
            // HealOrHarmMobEffect (instant damage, scale 0.5): indirect magic, scaled when the owner is a mob.
            int amount = int(0.5 * (6 << amplifier) + 0.5);
            hurt_player(p, float(amount), "indirect_magic", owner != nullptr);
        }
    }
};

// ---------- DragonFireball ----------
inline bool segment_hits_box(const Vec3& a, const Vec3& b, const AABB& bb, double& t) {  // AABB.clip
    double t0 = 0, t1 = 1;
    double o[3] = {a.x, a.y, a.z}, d[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
    double lo[3] = {bb.x0, bb.y0, bb.z0}, hi[3] = {bb.x1, bb.y1, bb.z1};
    for (int i = 0; i < 3; i++) {
        if (std::abs(d[i]) < 1e-12) {
            if (o[i] < lo[i] || o[i] > hi[i]) return false;
            continue;
        }
        double ta = (lo[i] - o[i]) / d[i], tb = (hi[i] - o[i]) / d[i];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta), t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    t = t0;
    return true;
}

struct DragonFireball : Entity {
    Entity* owner = nullptr;
    double accel = 0.1;
    DragonFireball() { type = entity_types::dragon_fireball, width = height = 1.0f; }
    std::string type_name() const override { return "dragon_fireball"; }
    int spawn_data() const override { return owner ? owner->eid : 0; }
    bool hurt(const Dmg&, float, int) override { return false; }
    void tick() override;  // dragon.hpp (needs the owner's removal state)
    void on_hit() {  // DragonFireball.onHit
        auto& c = add<AreaEffectCloud>(x, y, z);
        c.owner = owner;
        c.radius = 3.0f, c.width = 6.0f;
        c.duration = 600;
        c.radius_per_tick = (7.0f - c.radius) / float(c.duration);
        c.amplifier = 1;
        for (auto& pp : players) {
            Player& p = *pp;
            if (!alive_target(p) || !player_box(p).intersects(box().inflate(4.0, 2.0, 4.0))) continue;
            if (Vec3{p.x, p.y, p.z}.dist_sqr(x, y, z) < 16.0) {
                c.x = p.x, c.y = p.y, c.z = p.z;
                break;
            }
        }
        level_event(2006, int(std::floor(x)), int(std::floor(y)), int(std::floor(z)), 1);
        discard();
    }
};
inline void DragonFireball::tick() {
    tick_count++;
    if (owner && owner->removed) return discard();
    vel = (vel + vel.normalize().scale(accel)).scale(0.95f);  // applyInertia
    Vec3 from = pos(), to = from + vel;
    // ProjectileUtil.getHitResultOnMoveVector: blocks first, then the nearest player in the swept box.
    double best = 2;
    if (clip_hits(from, to)) {
        for (int i = 1; i <= 16; i++)  // ponytail: hit point found by stepping, not exact face intersection
            if (clip_hits(from, from + vel.scale(i / 16.0))) { best = (i - 1) / 16.0; break; }
    }
    for (auto& pp : players) {
        if (!alive_target(*pp)) continue;
        double t;
        if (segment_hits_box(from, to, player_box(*pp).inflate(0.3, 0.3, 0.3), t) && t < best) best = t;
    }
    Vec3 np = best <= 1 ? from + vel.scale(best) : to;
    x = np.x, y = np.y, z = np.z;
    if (best <= 1) on_hit();
    if (y < MIN_Y - 64) discard();
}

// ---------- explosions -> boss entities ----------
inline void explosion_hits_entities(const Vec3& c, float radius, int source_eid, int attacker) {
    float r2 = radius * 2.0f;
    for (size_t i = 0; i < entities.size(); i++) {
        Entity& e = *entities[i];
        if (e.removed || e.eid == source_eid || dynamic_cast<ExperienceOrb*>(&e) || dynamic_cast<AreaEffectCloud*>(&e)) continue;
        double dist = std::sqrt(e.pos().dist_sqr(c.x, c.y, c.z)) / r2;
        if (dist > 1.0) continue;
        float exposure = seen_percent(c, e.box());
        double pw = (1.0 - dist) * exposure;
        Dmg d;
        d.type = attacker >= 0 ? "player_explosion" : "explosion";
        d.explosion = d.always_hurts_dragon = true;
        d.player = player_by_eid(attacker);
        e.hurt(d, float((pw * pw + pw) / 2.0 * 7.0 * r2 + 1.0));
    }
}

inline bool EndCrystal::hurt(const Dmg& d, float, int) {  // EndCrystal.hurtServer
    if (invulnerable && !d.bypass_invul && !(d.player && d.player->gamemode == 1)) return false;
    if (d.causing && d.causing->type == entity_types::ender_dragon) return false;
    if (removed) return true;
    removed = true;
    if (!d.explosion)
        explode(x, y, z, 6.0f, 1, false, eid, d.player ? d.player->eid : -1);
    fight_crystal_destroyed(*this, d);
    return true;
}

}  // namespace boss
