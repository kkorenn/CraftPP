#pragma once
// Boss-fight entities on the mob framework (src/mobs): EndCrystal, DragonFireball, AreaEffectCloud (dragon
// breath) and ExperienceOrb, plus small query helpers. Tick thread only.
#include "level.hpp"

#include <memory>

namespace boss {

// Players are not framework entities yet, so damage that a player is responsible for carries the Player
// alongside the DamageSource (ponytail: fold into DamageSource.causing once PlayerEntity proxies exist).
struct BossEntity : ::Entity {
    virtual bool hurt_by(const DamageSource&, Player*, float, int part = -1) { return (void)part, false; }
    bool hurt_server(const DamageSource& s, float dmg) override {
        return hurt_by(s, s.causing ? s.causing->as_player() : nullptr, dmg);
    }
};

template <class T> inline T& spawn(double x, double y, double z, float yaw = 0) {
    auto e = std::make_unique<T>();
    e->snap_to(x, y, z, yaw, 0);
    return *add_entity_t(std::move(e));
}
template <class T> inline std::vector<T*> all_of() {
    std::vector<T*> v;
    for_each_entity([&](::Entity& e) {
        if (auto* t = dynamic_cast<T*>(&e)) v.push_back(t);
    });
    return v;
}
template <class T> inline std::vector<T*> in_box(const AABB& b) {
    std::vector<T*> v;
    for_each_entity_in(b, [&](::Entity& e) {
        if (auto* t = dynamic_cast<T*>(&e)) v.push_back(t);
    }, false);
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
        double d = dsq(Vec3{p.x, p.y, p.z}, x, y, z);
        if (range > 0 && d > range * range) continue;
        if (bd < 0 || d < bd) bd = d, best = &p;
    }
    return best;
}
inline std::vector<uint8_t> opt_pos_bytes(const std::optional<std::array<int, 3>>& p) {
    Writer w;
    w.boolean(bool(p));
    if (p) w.i64(pack_pos((*p)[0], (*p)[1], (*p)[2]));
    return w.b;
}

inline void award_xp(double x, double y, double z, int amount) { spawn_xp_orbs(x, y, z, amount); }  // ExperienceOrb.award

// ---------- EndCrystal ----------
inline bool fight_active();                                                          // fight.hpp
inline void fight_crystal_destroyed(::Entity& c, const DamageSource& d, Player* p);  // fight.hpp

struct EndCrystal : BossEntity {
    std::optional<std::array<int, 3>> beam;
    bool show_bottom = true;
    int time = rand_int(100000);
    EndCrystal() { type = entity_types::end_crystal; }
    bool is_pickable() const override { return true; }
    void set_beam(std::optional<std::array<int, 3>> b) {
        if (b == beam && data.find(edata::EndCrystal::BEAM_TARGET)) return;
        beam = b;
        data.set_raw(edata::EndCrystal::BEAM_TARGET, edser::OPTIONAL_BLOCK_POS, opt_pos_bytes(beam));
    }
    void set_show_bottom(bool s) { show_bottom = s, data.set_bool(edata::EndCrystal::SHOW_BOTTOM, s); }
    void tick() override {
        time++;
        Entity::tick();
        int bx = block_x(), by = block_y(), bz = block_z();
        if (fight_active() && chunk_loaded_at(bx, bz) && is_air(get_block(bx, by, bz))) set_block(bx, by, bz, blocks::fire);
    }
    bool hurt_by(const DamageSource& d, Player* p, float, int) override;
    void kill() override {
        fight_crystal_destroyed(*this, DamageSource::of("generic"), nullptr);
        remove(Removal::KILLED);
    }
    void save_extra(Nbt& o) const override {
        if (beam) {
            Nbt a;
            a.t = Nbt::IntArray;
            a.arr = {(*beam)[0], (*beam)[1], (*beam)[2]};
            o.set("beam_target", a);
        }
        o.set("ShowBottom", Nbt::num(Nbt::Byte, show_bottom));
    }
    void load_extra(const Nbt& in) override {
        if (const Nbt* a = in.get("beam_target"); a && a->t == Nbt::IntArray && a->arr.size() == 3)
            set_beam(std::array<int, 3>{int(a->arr[0]), int(a->arr[1]), int(a->arr[2])});
        if (!nbt_num(in, "ShowBottom", 1)) set_show_bottom(false);
    }
};

// ---------- AreaEffectCloud (dragon breath) ----------
struct AreaEffectCloud : BossEntity {
    float radius = 3.0f, radius_per_tick = 0;
    int duration = -1, wait_time = 20, reapply = 20, amplifier = 0;
    bool waiting = true;
    bool owned_by_mob = false;  // owner is the dragon (difficulty scaling)
    std::map<int, int> victims;  // player eid -> tick when it may be hit again
    AreaEffectCloud() {
        type = entity_types::area_effect_cloud;
        static const int breath = particle_id("dragon_breath");
        Writer w;
        w.varint(breath).f32(1.0f);  // PowerParticleOption
        data.set_raw(edata::AreaEffectCloud::PARTICLE, edser::PARTICLE, w.b);
        data.set_bool(edata::AreaEffectCloud::WAITING, true);
    }
    bool fire_immune() const override { return true; }
    void set_radius(float r) {
        radius = std::clamp(r, 0.0f, 32.0f);
        set_size(radius * 2.0f, 0.5f, info().eye);
        data.set_float(edata::AreaEffectCloud::RADIUS, radius);
    }
    void tick() override {
        Entity::tick();
        if (duration != -1 && tick_count - wait_time >= duration) return discard();
        bool should_wait = tick_count < wait_time;
        if (waiting != should_wait) waiting = should_wait, data.set_bool(edata::AreaEffectCloud::WAITING, waiting);
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
            if (!alive_target(p) || victims.count(p.eid) || !player_box(p).intersects(bb)) continue;
            double dx = p.x - pos.x, dz = p.z - pos.z;
            if (dx * dx + dz * dz > double(r) * r) continue;
            victims[p.eid] = tick_count + reapply;
            // HealOrHarmMobEffect (instant damage, scale 0.5): indirect magic, scaled when the owner is a mob.
            int amount = int(0.5 * (6 << amplifier) + 0.5);
            hurt_player(p, float(amount), "indirect_magic", owned_by_mob);
        }
    }
    void save_extra(Nbt& o) const override {
        o.set("Radius", Nbt::real(Nbt::Float, radius));
        o.set("Duration", Nbt::num(Nbt::Int, duration));
        o.set("RadiusPerTick", Nbt::real(Nbt::Float, radius_per_tick));
        o.set("Amplifier", Nbt::num(Nbt::Int, amplifier));  // ponytail: potion_contents not modelled
    }
    void load_extra(const Nbt& in) override {
        set_radius(float(nbt_num(in, "Radius", 3)));
        duration = int(nbt_num(in, "Duration", -1));
        radius_per_tick = float(nbt_num(in, "RadiusPerTick", 0));
        amplifier = int(nbt_num(in, "Amplifier", 0));
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

struct DragonFireball : BossEntity {
    int owner_id = 0;
    double accel = 0.1;
    DragonFireball() { type = entity_types::dragon_fireball; }
    int add_entity_data() const override { return owner_id; }
    void on_hit() {  // DragonFireball.onHit
        auto& c = spawn<AreaEffectCloud>(pos.x, pos.y, pos.z);
        c.owned_by_mob = owner_id != 0;
        c.set_radius(3.0f);
        c.duration = 600;
        c.radius_per_tick = (7.0f - c.radius) / float(c.duration);
        c.amplifier = 1;
        for (auto& pp : players) {
            Player& p = *pp;
            if (!alive_target(p) || !player_box(p).intersects(bb.inflate(4.0, 2.0, 4.0))) continue;
            if (dsq(Vec3{p.x, p.y, p.z}, pos.x, pos.y, pos.z) < 16.0) {
                c.teleport_to(p.x, p.y, p.z);
                break;
            }
        }
        level_event(2006, block_x(), block_y(), block_z(), 1);
        discard();
    }
    void tick() override {  // AbstractHurtingProjectile.tick
        ::Entity* owner = owner_id ? entity_by_id(owner_id) : nullptr;
        if (owner_id && (!owner || owner->removed)) return discard();
        vel = (vel + vel.normalize() * accel) * double(0.95f);  // applyInertia
        Vec3 from = pos, to = from + vel;
        double best = 2;  // ProjectileUtil.getHitResultOnMoveVector: blocks, then the nearest player in the sweep
        if (clip_hits(from, to))
            for (int i = 1; i <= 16; i++)  // ponytail: hit point found by stepping, not exact face intersection
                if (clip_hits(from, from + vel * (i / 16.0))) { best = (i - 1) / 16.0; break; }
        for (auto& pp : players) {
            double t;
            if (alive_target(*pp) && segment_hits_box(from, to, player_box(*pp).inflate(0.3), t) && t < best) best = t;
        }
        set_pos(best <= 1 ? from + vel * best : to);
        Entity::tick();
        if (!removed && best <= 1) on_hit();
    }
};

// ---------- explosions -> framework entities ----------
inline void explosion_hits_entities(const Vec3& c, float radius, int source_eid, int attacker) {
    float r2 = radius * 2.0f;
    AABB box{std::floor(c.x - r2 - 1.0), std::floor(c.y - r2 - 1.0), std::floor(c.z - r2 - 1.0),
             std::floor(c.x + r2 + 1.0), std::floor(c.y + r2 + 1.0), std::floor(c.z + r2 + 1.0)};
    std::vector<::Entity*> hit;
    for_each_entity_in(box, [&](::Entity& e) {
        if (e.id != source_eid && !dynamic_cast<AreaEffectCloud*>(&e)) hit.push_back(&e);
    }, false);
    Player* p = player_by_eid(attacker);
    DamageSource d = DamageSource::of(p ? "player_explosion" : "explosion");
    for (::Entity* e : hit) {
        if (e->removed) continue;
        double dist = std::sqrt(e->pos.dist_sqr(c)) / r2;
        if (dist > 1.0) continue;
        float exposure = seen_percent(c, e->bb);
        double pw = (1.0 - dist) * exposure;
        float dmg = float((pw * pw + pw) / 2.0 * 7.0 * r2 + 1.0);
        if (auto* b = dynamic_cast<BossEntity*>(e)) b->hurt_by(d, p, dmg);
        else e->hurt_server(d, dmg);
        if (!e->removed) {  // pushFromExplosion
            Vec3 k = (e->eye_pos() - c).normalize() * ((1.0 - dist) * exposure);
            e->push(k.x, k.y, k.z);
            mark_entity_sync(*e);
        }
    }
}

inline bool EndCrystal::hurt_by(const DamageSource& d, Player* p, float, int) {  // EndCrystal.hurtServer
    if (invulnerable && !d.is(DT_BYPASSES_INVULNERABILITY) && !(p && p->gamemode == 1)) return false;
    if (d.causing && d.causing->type == entity_types::ender_dragon) return false;
    if (removed) return true;
    remove(Removal::KILLED);
    if (!d.is(DT_IS_EXPLOSION)) explode(pos.x, pos.y, pos.z, 6.0f, 1, false, id, p ? p->eid : -1);
    fight_crystal_destroyed(*this, d, p);
    return true;
}

}  // namespace boss
