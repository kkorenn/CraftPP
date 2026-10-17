#pragma once
// LivingEntity (attributes, health, damage, armour, death, loot/xp, travel physics, jumping, pushing) and
// PlayerEntity, the per-player proxy that lets mobs target, hit and look at players through the same API.
// Mobs TU only (src/mobs/*.cpp). Overview: src/mobs/mobs.hpp.
#include "entity.hpp"
#include "../progress/api.hpp"

// ---------- raycast (Level.clip, ClipContext.Block.COLLIDER) ----------
struct BlockHit { bool hit = false; Vec3 pos{}; int bx = 0, by = 0, bz = 0, face = 0; };
// Segment vs AABB slab test; returns the entry fraction in [0, 1] or -1.
inline double clip_box(const AABB& b, const Vec3& a, const Vec3& d, int& face) {
    double t0 = 0, t1 = 1;
    const double o[3] = {a.x, a.y, a.z}, v[3] = {d.x, d.y, d.z};
    face = -1;
    for (int ax = 0; ax < 3; ax++) {
        double lo = b.min(ax), hi = b.max(ax);
        if (std::abs(v[ax]) < 1e-12) {
            if (o[ax] < lo || o[ax] > hi) return -1;
            continue;
        }
        double ta = (lo - o[ax]) / v[ax], tb = (hi - o[ax]) / v[ax];
        int f = v[ax] > 0 ? ax * 2 : ax * 2 + 1;  // entered through min face (moving +) or max face
        if (ta > tb) std::swap(ta, tb);
        if (ta > t0) t0 = ta, face = f;
        t1 = std::min(t1, tb);
        if (t0 > t1) return -1;
    }
    return t0;
}
// Level.clip with collision shapes (fluids ignored): first block hit along from -> to.
inline BlockHit clip_blocks(const Vec3& from, const Vec3& to) {
    BlockHit h;
    Vec3 d = to - from;
    // BlockGetter.traverseBlocks: voxel DDA from slightly inside `from` to `to`
    double sx = mth::lerp(-1.0E-7, to.x, from.x), sy = mth::lerp(-1.0E-7, to.y, from.y), sz = mth::lerp(-1.0E-7, to.z, from.z);
    double ex = mth::lerp(-1.0E-7, from.x, to.x), ey = mth::lerp(-1.0E-7, from.y, to.y), ez = mth::lerp(-1.0E-7, from.z, to.z);
    int x = mth::floor(sx), y = mth::floor(sy), z = mth::floor(sz);
    auto test = [&](int bx, int by, int bz) {
        int s = block_at(bx, by, bz);
        double best = 2;
        int bf = 0;
        for (const AABB* b = shape_begin(s); b != shape_end(s); b++) {
            int f;
            double t = clip_box(b->move(bx, by, bz), from, d, f);
            if (t >= 0 && t < best) best = t, bf = f;
        }
        if (best <= 1) { h = {true, from + d * best, bx, by, bz, bf}; return true; }
        return false;
    };
    if (test(x, y, z)) return h;
    double dx = ex - sx, dy = ey - sy, dz = ez - sz;
    int stx = dx > 0 ? 1 : dx < 0 ? -1 : 0, sty = dy > 0 ? 1 : dy < 0 ? -1 : 0, stz = dz > 0 ? 1 : dz < 0 ? -1 : 0;
    double tdx = stx == 0 ? 1e300 : stx / dx, tdy = sty == 0 ? 1e300 : sty / dy, tdz = stz == 0 ? 1e300 : stz / dz;
    auto frac = [](double v) { return v - std::floor(v); };
    double tx = tdx * (stx > 0 ? 1.0 - frac(sx) : frac(sx)), ty = tdy * (sty > 0 ? 1.0 - frac(sy) : frac(sy)),
           tz = tdz * (stz > 0 ? 1.0 - frac(sz) : frac(sz));
    while (tx <= 1.0 || ty <= 1.0 || tz <= 1.0) {
        if (tx < ty) {
            if (tx < tz) x += stx, tx += tdx;
            else z += stz, tz += tdz;
        } else if (ty < tz) y += sty, ty += tdy;
        else z += stz, tz += tdz;
        if (test(x, y, z)) return h;
    }
    return h;
}

// ---------- light (until the lighting branch lands) ----------
// Real light (src/light) when the column is lit; before that a sky-light heuristic (15 at or above the column's
// highest motion-blocking block, else 0, no block light). Every caller goes through these two functions.
inline int surface_y_cached(int x, int z);
inline int sky_light_at(int x, int y, int z) {
    if (!cur_level->skylight) return 0;
    int l = light::get(x, y, z, true);
    return l >= 0 ? l : y >= surface_y_cached(x, z) ? 15 : 0;
}
inline int block_light_at(int x, int y, int z) { return std::max(0, light::get(x, y, z, false)); }
inline int sky_darken() {  // Level.updateSkyBrightness: 15 - sky_light_level (timeline/day.json keyframes, linear, cyclic)
    static const double kt[] = {133, 11867, 13670, 22330}, kv[] = {1.0, 1.0, 0.26666668, 0.26666668};
    double t = double(((clocks[0].total % 24000) + 24000) % 24000), v = kv[3];
    for (int i = 0; i < 4; i++) {
        double t0 = kt[i], t1 = i == 3 ? kt[0] + 24000 : kt[i + 1], tt = t < kt[0] ? t + 24000 : t;
        if (tt >= t0 && tt < t1) { v = kv[i] + (kv[(i + 1) % 4] - kv[i]) * (tt - t0) / (t1 - t0); break; }
    }
    float level = float(15.0 * v);  // ponytail: rain/thunder darkening of the sky light not applied
    return int(15.0f - level);
}
inline int raw_brightness(int x, int y, int z, int darken) {  // Level.getRawBrightness
    return std::max(sky_light_at(x, y, z) - darken, block_light_at(x, y, z));
}
inline int max_local_raw_brightness(int x, int y, int z) { return raw_brightness(x, y, z, sky_darken()); }
inline bool can_see_sky(int x, int y, int z) { return y >= surface_y_cached(x, z); }
inline bool is_day() { return sky_darken() < 4; }  // Level.isBrightOutside
// MOTION_BLOCKING heightmap: one above the highest block that blocks motion or holds fluid. Cached per column for
// 20 ticks (no block-change hook yet).
inline int surface_y_scan(int x, int z) {
    Chunk* c = loaded_chunk(x >> 4, z >> 4);
    if (!c) return world.min_y();
    for (int y = world.max_y() - 1; y >= world.min_y(); y--) {
        int s = c->get(x & 15, y, z & 15);
        if (s != blocks::air && (has_collision(s) || fluid_type(s))) return y + 1;
    }
    return world.min_y();
}
inline int surface_y_cached(int x, int z) {
    struct E { int64_t key = INT64_MIN; int y = 0; int64_t tick = -100; World* level = nullptr; };
    static std::array<E, 4096> cache;
    int64_t key = (int64_t(x) << 32) ^ uint32_t(z);
    E& e = cache[size_t(uint32_t(x * 73856093 ^ z * 19349663) & 4095)];
    if (e.level != cur_level || e.key != key || server_tick - e.tick > 20) e = {key, surface_y_scan(x, z), server_tick, cur_level};
    return e.y;
}

// ---------- attributes ----------
struct AttrModifier { std::string id; double amount; uint8_t op; };  // AttributeModifier (op 0 add, 1 mult base, 2 mult total)
struct AttrOverride { uint16_t attr; double base; std::vector<AttrModifier> mods; };

struct LivingEntity;
struct PlayerEntity;
struct DamageSource;
// src/effects/effects.hpp
inline float damage_after_magic_absorb(LivingEntity& e, const DamageSource& s, float dmg);
inline void tick_effects(LivingEntity& e);
inline void trigger_on_death_effects(LivingEntity& e);
inline void effects_on_hurt(LivingEntity& e);
inline void save_effects(const LivingEntity& e, Nbt& o);
inline float enchant_armor_effectiveness(const ItemStack& w, Entity* victim, const DamageSource& s, float v);
inline bool enchant_damage_immune(LivingEntity& victim, const DamageSource& s);
inline float enchant_modify_damage(const ItemStack& w, Entity* victim, const DamageSource& s, float v);
inline float enchant_modify_knockback(const ItemStack& w, Entity* victim, const DamageSource& s, float v);
inline void enchant_post_attack(Entity& victim, const DamageSource& s, const ItemStack* weapon);
inline void enchant_location_changed(LivingEntity& e);
inline bool item_has_comp(const ItemStack&);
template <class F> inline void enchant_attribute_mods(const ItemStack& it, int slot, F f);
inline void load_effects(LivingEntity& e, const Nbt& in);
inline bool has_water_breathing(const LivingEntity& e);
inline bool effects_refill_air(const LivingEntity& e);
inline LivingEntity* living_of(Entity* e) { return e ? e->as_living() : nullptr; }

struct LivingEntity : Entity {
    // ---- attributes (AttributeMap): type defaults + per-entity overrides + equipment modifiers ----
    std::vector<AttrOverride> attrs;
    uint64_t attr_dirty = 0;  // attribute ids (< 64) to resend
    std::array<ItemStack, 8> equipment{};  // EquipmentSlot order: mainhand offhand feet legs chest head body saddle
    uint8_t equip_dirty = 0;
    // ---- LivingEntity state ----
    float absorption = 0, last_hurt = 0;
    int hurt_time = 0, death_time = 0, damage_cooldown = 0, no_jump_delay = 0, no_action_time = 0;
    bool dead = false, jumping = false;
    float xxa = 0, yya = 0, zza = 0, speed = 0;
    float y_head_rot = 0, y_body_rot = 0, y_head_rot_o = 0, y_body_rot_o = 0;
    int last_hurt_by_mob = 0, last_hurt_by_mob_time = 0;        // entity id / tickCount
    int last_hurt_by_player = 0, last_hurt_by_player_time = 0;  // entity id / memory ticks left
    int last_hurt_mob = 0, last_hurt_mob_time = 0;
    int last_damage_type = -1;
    int64_t last_damage_stamp = 0;
    // ---- mob effects (src/effects/effects.hpp) ----
    std::vector<EffectInstance> own_effects;
    std::array<int, 3> last_block{INT32_MIN, 0, 0};  // lastPos (onChangedBlock)
    bool effects_dirty = true;
    virtual std::vector<EffectInstance>& effects() { return own_effects; }
    const std::vector<EffectInstance>& effects() const { return const_cast<LivingEntity*>(this)->effects(); }
    const EffectInstance* effect(int id) const {
        for (auto& e : effects()) if (e.id == id) return &e;
        return nullptr;
    }
    bool has_effect(int id) const { return effect(id) != nullptr; }
    double effective_gravity() const;  // getEffectiveGravity (slow falling)
    float jump_boost_power() const;

    LivingEntity* as_living() override { return this; }
    void init() override { Entity::init(); set_health(max_health()); }
    float head_yaw() const override { return y_head_rot; }
    float body_yaw() const override { return y_body_rot; }

    // attribute value: AttributeInstance.calculateValue over base + modifiers (+ equipped item modifiers)
    virtual double attr_base(int a) const {
        for (auto& o : attrs) if (o.attr == a) return o.base;
        return info().attr_base(a);
    }
    virtual bool has_attr(int a) const {
        for (auto& o : attrs) if (o.attr == a) return true;
        return info().has_attr(a);
    }
    virtual const ItemStack& item_in_slot(int slot) const { return equipment[size_t(slot)]; }
    double attr(int a) const {
        double base = attr_base(a);
        const AttrOverride* ov = nullptr;
        for (auto& o : attrs) if (o.attr == a) ov = &o;
        double add = 0, mb = 0, mt = 1;
        auto apply = [&](double amount, int op) {
            if (op == 0) add += amount;
            else if (op == 1) mb += amount;
            else mt *= 1.0 + amount;
        };
        if (ov) for (auto& m : ov->mods) apply(m.amount, m.op);
        for (int s = 0; s < 8; s++) {
            const ItemStack& it = item_in_slot(s);
            if (it.empty()) continue;
            if (item_has_comp(it))  // enchantment minecraft:attributes (efficiency, respiration, sweeping edge...)
                enchant_attribute_mods(it, s, [&](int ea, double amount, int op) { if (ea == a) apply(amount, op); });
            auto c = item_combat.find(it.item);
            if (c == item_combat.end()) continue;
            for (auto& m : c->second.mods)
                if (m.attr == a && slot_matches(m.slot, s)) apply(m.amount, m.op);
        }
        double b = base + add;
        double r = (b + b * mb) * mt;
        const AttributeInfo& ai = attribute_info[size_t(a)];
        return std::isnan(r) ? ai.min : std::clamp(r, ai.min, ai.max);
    }
    AttrOverride& attr_override(int a) {
        for (auto& o : attrs) if (o.attr == a) return o;
        attrs.push_back({uint16_t(a), info().attr_base(a), {}});
        return attrs.back();
    }
    void set_attr_base(int a, double v) { attr_override(a).base = v; if (a < 64) attr_dirty |= 1ull << a; }
    void add_attr_modifier(int a, const std::string& id, double amount, int op) {
        auto& o = attr_override(a);
        for (auto& m : o.mods) if (m.id == id) { m.amount = amount, m.op = uint8_t(op); goto done; }
        o.mods.push_back({id, amount, uint8_t(op)});
    done:
        if (a < 64) attr_dirty |= 1ull << a;
    }
    void remove_attr_modifier(int a, const std::string& id) {
        for (auto& o : attrs) if (o.attr == a) std::erase_if(o.mods, [&](auto& m) { return m.id == id; });
        if (a < 64) attr_dirty |= 1ull << a;
    }
    bool has_attr_modifier(int a, const std::string& id) const {
        for (auto& o : attrs) if (o.attr == a) for (auto& m : o.mods) if (m.id == id) return true;
        return false;
    }

    // ---- equipment ----
    void set_item_slot(int slot, const ItemStack& s) { equipment[size_t(slot)] = s; equip_dirty |= uint8_t(1 << slot); }
    const ItemStack& main_hand() const { return item_in_slot(ES_MAINHAND); }

    // ---- health ----
    float health() const override { return data.get_float(edata::LivingEntity::HEALTH, 1.0f); }
    virtual void set_health(float h) { data.set_float(edata::LivingEntity::HEALTH, std::clamp(h, 0.0f, max_health())); }
    float max_health() const override { return float(attr(attr::max_health)); }
    void heal(float a) { float h = health(); if (h > 0) set_health(h + a); }
    bool is_dead_or_dying() const { return health() <= 0.0f || dead; }
    bool is_alive() const override { return !removed && health() > 0.0f; }
    virtual bool is_baby() const { return false; }
    float age_scale() const { return is_baby() ? 0.5f : 1.0f; }
    int armor_value() const { return mth::floor(attr(attr::armor)); }

    // ---- physics ----
    double default_gravity() const override { return attr(attr::gravity); }
    float max_up_step() const override { return float(attr(attr::step_height)); }
    bool is_pushable() const override { return is_alive() && !is_spectator(); }
    bool is_pickable() const override { return !removed; }
    float block_speed_factor() const override {
        return mth::lerp(float(attr(attr::movement_efficiency)), Entity::block_speed_factor(), 1.0f);
    }
    virtual bool is_affected_by_fluids() const { return true; }
    virtual bool can_breathe_underwater() const { return in_tag("can_breathe_under_water"); }
    virtual bool is_sensitive_to_water() const { return false; }
    virtual float water_slow_down() const { return 0.8f; }
    virtual float flying_speed() const { return 0.02f; }
    virtual int max_fall_distance() const {  // getMaxFallDistance (Mob overrides with its target)
        return 3;
    }
    float jump_power() const {  // getJumpPower
        int here = block_at(block_x(), block_y(), block_z());
        float jf = phys_of(here).jump;
        if (jf == 1.0f) jf = phys_of(block_below_affecting_movement()).jump;
        return float(attr(attr::jump_strength)) * jf + jump_boost_power();
    }
    virtual void jump_from_ground() {
        float jp = jump_power();
        if (jp <= 1.0E-5f) return;
        vel.y = std::max(double(jp), vel.y);
        if (shared_flag(3)) {
            float a = yaw * mth::DEG;
            vel.x += -mth::sin(a) * 0.2, vel.z += mth::cos(a) * 0.2;
        }
        needs_sync = true;
    }
    virtual void jump_in_liquid() { vel.y += 0.04f; }
    virtual void go_down_in_water() { vel.y -= 0.04f; }
    bool on_climbable() const {
        if (is_spectator()) return false;
        return in_sorted(tag_climbable, block_index(block_at(block_x(), block_y(), block_z())));
    }
    float friction_influenced_speed(float f) const {
        if (on_ground) return f > 0.6 ? speed * (0.21600002f / (f * f * f)) : speed;
        return flying_speed();
    }
    static float modified_friction(float f, float m) { return std::clamp(1.0f - (1.0f - f) * m, 0.0f, 1.0f); }
    void travel(const Vec3& input);
    void travel_in_air(const Vec3& input);
    void travel_in_fluid(const Vec3& input);
    Vec3 fluid_falling_adjusted(double g, bool falling, Vec3 m) const {
        if (g != 0.0 && !shared_flag(3)) {
            double yd = falling && std::abs(m.y - 0.005) >= 0.003 && std::abs(m.y - g / 16.0) < 0.003 ? -0.003 : m.y - g / 16.0;
            return {m.x, yd, m.z};
        }
        return m;
    }
    void push_entities();
    bool cause_fall_damage(double dist, float mod, const DamageSource& src) override {
        if (in_tag("fall_damage_immune")) return false;
        int dmg = mth::floor((dist + 1.0E-6 - attr(attr::safe_fall_distance)) * mod * attr(attr::fall_damage_multiplier));
        if (dmg <= 0) return false;
        play_sound(dmg > 4 ? "minecraft:entity.generic.big_fall" : "minecraft:entity.generic.small_fall", 1, 1);
        hurt_server(src, float(dmg));
        return true;
    }

    // ---- ticking ----
    void tick() override;
    void base_tick() override;
    virtual void ai_step();
    virtual void server_ai_step() {}
    virtual void tick_head_turn(float target_body);
    virtual void tick_death();
    virtual float max_head_rotation() const { return 50.0f; }

    // ---- combat ----
    bool hurt_server(const DamageSource& s, float dmg) override;
    virtual void actually_hurt(const DamageSource& s, float dmg);
    virtual float damage_after_armor(const DamageSource& s, float dmg) {
        if (s.is(DT_BYPASSES_ARMOR)) return dmg;
        hurt_armor(s, dmg);
        float armor = float(armor_value()), tough = float(attr(attr::armor_toughness));
        float t = 2.0f + tough / 4.0f;
        float real = std::clamp(armor - dmg / t, armor * 0.2f, 20.0f);
        float frac = real / 25.0f;  // CombatRules.getDamageAfterAbsorb: the weapon's armor_effectiveness (breach)
        if (LivingEntity* w = s.direct ? s.direct->as_living() : nullptr)
            frac = std::clamp(enchant_armor_effectiveness(w->main_hand(), this, s, frac), 0.0f, 1.0f);
        return dmg * (1.0f - frac);
    }
    virtual void hurt_armor(const DamageSource&, float) {}
    virtual void knockback(double power, double xd, double zd);
    void deal_default_knockback(const DamageSource& s);
    virtual void die(const DamageSource& s);
    void kill() override { hurt_server(DamageSource::of("generic_kill"), 3.4028235E38f); }
    virtual void drop_all_death_loot(const DamageSource& s);
    virtual void drop_custom_death_loot(const DamageSource&, bool) {}
    virtual void drop_equipment() {}
    virtual int base_experience_reward() { return 0; }
    virtual bool should_drop_experience() const { return !is_baby(); }
    virtual bool should_drop_loot() const { return !is_baby() && rule("mob_drops"); }
    virtual bool is_always_experience_dropper() const { return false; }
    virtual int loot_table() const { return info().loot; }
    void drop_from_loot_table(const DamageSource& s, bool player_killed, int table);
    virtual void spawn_at_location(const ItemStack& s, float yoff = 0.0f);  // Entity.spawnAtLocation
    virtual bool can_attack(LivingEntity& target);
    virtual bool can_be_seen_as_enemy() { return !invulnerable && can_be_seen_by_anyone(); }
    virtual bool can_be_seen_by_anyone() { return !is_spectator() && is_alive(); }
    virtual bool is_allied_to(Entity&) { return false; }
    virtual double visibility_percent(Entity*) { return std::clamp(shared_flag(1) ? 0.8 : 1.0, 0.0, 10.0); }
    bool has_line_of_sight(Entity& t) const {
        Vec3 from{pos.x, eye_y(), pos.z}, to{t.pos.x, t.eye_y(), t.pos.z};
        if ((to - from).length() > 128.0) return false;
        return !clip_blocks(from, to).hit;
    }
    virtual void set_last_hurt_by_mob(LivingEntity* e) { last_hurt_by_mob = e ? e->id : 0; last_hurt_by_mob_time = tick_count; }
    LivingEntity* last_hurt_by() const { return living_of(entity_by_id(last_hurt_by_mob)); }
    LivingEntity* kill_credit() const {
        if (last_hurt_by_player) if (auto* p = living_of(entity_by_id(last_hurt_by_player))) return p;
        return last_hurt_by();
    }
    void set_last_hurt_mob(Entity& e) { last_hurt_mob = e.id; last_hurt_mob_time = tick_count; }
    virtual bool do_hurt_target(Entity& target) { set_last_hurt_mob(target); return false; }
    virtual float attack_knockback(Entity& t) {  // getKnockback: the weapon's knockback enchantment
        return enchant_modify_knockback(main_hand(), &t, DamageSource::of("mob_attack", this), float(attr(attr::attack_knockback))) / 2.0f;
    }
    void cause_extra_knockback(Entity& target, float kb) {
        if (kb > 0 && target.as_living()) {
            target.as_living()->knockback(kb, mth::sin(yaw * mth::DEG), -mth::cos(yaw * mth::DEG));
            vel = vel.multiply(0.6, 1.0, 0.6);
        }
    }
    void swing(int hand = 0) {  // LivingEntity.swing -> ClientboundAnimatePacket
        broadcast(std::move(Writer().varint(play_cb::animate).varint(id).u8(uint8_t(hand ? 3 : 0))));
    }

    // ---- sounds ----
    virtual std::string hurt_sound(const DamageSource&) const { return "minecraft:entity.generic.hurt"; }
    virtual std::string death_sound() const { return "minecraft:entity.generic.death"; }
    virtual float sound_volume() const { return 1.0f; }
    float voice_pitch() const { return is_baby() ? (rand_float() - rand_float()) * 0.2f + 1.5f : (rand_float() - rand_float()) * 0.2f + 1.0f; }
    void make_sound(const std::string& s) { if (!s.empty()) play_sound(s, sound_volume(), voice_pitch()); }

    // ---- persistence / network ----
    void save_extra(Nbt& o) const override;
    void load_extra(const Nbt& in) override;
    void send_pairing(Player& to) override;
    void send_extra_changes() override;
    Writer attributes_packet(uint64_t mask, bool all) const;
    Writer equipment_packet(uint8_t mask) const;
    bool loot_flag(const std::string& f) const override {
        if (f == "is_baby") return is_baby();
        return Entity::loot_flag(f);
    }
};

// ---------- LivingEntity bodies ----------
inline bool LivingEntity::can_attack(LivingEntity& t) {
    if (t.as_player() && difficulty() == 0) return false;
    return t.can_be_seen_as_enemy();
}

inline void LivingEntity::knockback(double power, double xd, double zd) {  // LivingEntity.knockback
    power *= 1.0 - attr(attr::knockback_resistance);
    if (power <= 0.0) return;
    needs_sync = true;
    while (xd * xd + zd * zd < 1.0E-5f) {
        xd = (rand_double() - rand_double()) * 0.01;
        zd = (rand_double() - rand_double()) * 0.01;
    }
    Vec3 dv = Vec3{xd, 0, zd}.normalize() * power;
    vel = {vel.x / 2.0 - dv.x, on_ground ? std::min(0.4, vel.y / 2.0 + power) : vel.y, vel.z / 2.0 - dv.z};
}
inline void LivingEntity::deal_default_knockback(const DamageSource& s) {
    double xd = 0, zd = 0;
    if (s.direct && s.direct != s.causing && s.is(DT_IS_PROJECTILE)) {  // Projectile.calculateHorizontalHurtKnockbackDirection
        knockback(0.4f, -s.direct->vel.x, -s.direct->vel.z);
        return;
    }
    if (auto p = s.position()) xd = p->x - pos.x, zd = p->z - pos.z;
    knockback(0.4f, xd, zd);
}

inline bool LivingEntity::hurt_server(const DamageSource& s, float dmg) {  // LivingEntity.hurtServer
    if (is_invulnerable_to(s) || is_dead_or_dying()) return false;
    if (enchant_damage_immune(*this, s)) return false;  // isInvulnerableTo: damage_immunity enchantments
    if (s.is(DT_IS_FIRE) && has_effect(eff::fire_resistance)) return false;
    no_action_time = 0;
    if (dmg < 0) dmg = 0;
    if (s.is(DT_IS_FREEZING) && in_tag("freeze_hurts_extra_types")) dmg *= 5.0f;
    if (s.is(DT_DAMAGES_HELMET) && !item_in_slot(ES_HEAD).empty()) dmg *= 0.75f;
    if (std::isnan(dmg) || std::isinf(dmg)) dmg = 3.4028235E38f;
    bool full = true;
    if (damage_cooldown > 10 && !s.is(DT_BYPASSES_COOLDOWN)) {
        if (dmg <= last_hurt) return false;
        actually_hurt(s, dmg - last_hurt);
        last_hurt = dmg;
        full = false;
    } else {
        last_hurt = dmg;
        damage_cooldown = 20;
        actually_hurt(s, dmg);
        hurt_time = 10;
    }
    if (LivingEntity* c = living_of(s.causing)) {  // resolveMobResponsibleForDamage / resolvePlayerResponsibleForDamage
        if (!s.is(DT_NO_ANGER) && !(c->as_player() && c->is_creative() && false)) set_last_hurt_by_mob(c);
        if (c->as_player()) last_hurt_by_player = c->id, last_hurt_by_player_time = 100;
    }
    if (full) {
        Writer w;  // level.broadcastDamageEvent
        w.varint(play_cb::damage_event).varint(id).varint(s.type).varint(s.causing ? s.causing->id + 1 : 0)
            .varint(s.direct ? s.direct->id + 1 : 0);
        if (s.source_pos) w.boolean(true).f64(s.source_pos->x).f64(s.source_pos->y).f64(s.source_pos->z);
        else w.boolean(false);
        broadcast(w);
        if (Player* self = as_player()) self->send(w);
        if (!s.is(DT_NO_IMPACT)) sync_velocity = true;  // markHurt
        if (!s.is(DT_NO_KNOCKBACK)) deal_default_knockback(s);
    }
    if (is_dead_or_dying()) {
        if (full) make_sound(death_sound());
        die(s);
    } else if (full) {
        make_sound(hurt_sound(s));
    }
    last_damage_type = s.type;
    last_damage_stamp = server_tick;
    effects_on_hurt(*this);
    return true;
}
inline void LivingEntity::actually_hurt(const DamageSource& s, float dmg) {
    if (is_invulnerable_to(s)) return;
    dmg = damage_after_armor(s, dmg);
    dmg = damage_after_magic_absorb(*this, s, dmg);
    float rest = std::max(dmg - absorption, 0.0f);
    absorption -= dmg - rest;
    if (rest != 0.0f) set_health(health() - rest);
}

inline std::vector<void (*)(LivingEntity&, const DamageSource&)> living_death_hooks;  // non-player deaths
inline void LivingEntity::die(const DamageSource& s) {  // LivingEntity.die
    if (removed || dead) return;
    dead = true;
    LivingEntity* k = kill_credit();
    if (!k && s.causing) k = s.causing->as_living();
    if (k) if (Player* p = k->as_player()) progress::player_killed_entity(*p, *this, s);
    if (!as_player())
        for (auto f : living_death_hooks) f(*this, s);
    drop_all_death_loot(s);
    broadcast_event(3);
    set_pose(Pose::DYING);
}
inline void LivingEntity::tick_death() {
    if (++death_time >= 20 && !removed) {
        broadcast_event(60);
        trigger_on_death_effects(*this);
        remove(Removal::KILLED);
    }
}
inline void LivingEntity::spawn_at_location(const ItemStack& s, float yoff) {
    if (s.empty()) return;
    auto& it = spawn_item(s, pos.x, pos.y + yoff, pos.z);
    it.pickup_delay = 10;  // ItemEntity.setDefaultPickUpDelay
}
inline void LivingEntity::drop_from_loot_table(const DamageSource& s, bool player_killed, int table) {
    if (table < 0) return;
    LootContext ctx;
    ctx.x = block_x(), ctx.y = block_y(), ctx.z = block_z();
    ctx.this_entity = this;
    ctx.attacker = s.causing;
    ctx.direct = s.direct;
    ctx.source = &s;
    if (player_killed) ctx.last_player = entity_by_id(last_hurt_by_player);
    static std::vector<ItemStack> out;
    out.clear();
    roll_loot(loot_tables[size_t(table)], ctx, out);
    std::vector<ItemStack> drops = out;
    for (auto& d : drops) {
        ItemStack s2 = d;
        while (!s2.empty()) {  // split into max stacks like LootTable.getRandomItems
            ItemStack part = s2;
            part.count = std::min(s2.count, max_stack(s2));
            s2.count -= part.count;
            spawn_at_location(part);
        }
    }
}
inline void LivingEntity::drop_all_death_loot(const DamageSource& s) {  // dropAllDeathLoot
    bool player_killed = last_hurt_by_player_time > 0 && last_hurt_by_player;
    if (should_drop_loot()) {
        drop_from_loot_table(s, player_killed, loot_table());
        drop_custom_death_loot(s, player_killed);
    }
    drop_equipment();
    if (is_always_experience_dropper() || (player_killed && should_drop_experience() && rule("mob_drops")))
        spawn_xp_orbs(pos.x, pos.y, pos.z, base_experience_reward());
}

inline void LivingEntity::base_tick() {  // LivingEntity.baseTick
    Entity::base_tick();
    if (is_alive()) {
        if (is_in_wall()) hurt_server(DamageSource::of("in_wall"), 1.0f);
        if (was_eye_in_water && !can_breathe_underwater() && !has_water_breathing(*this) && !(as_player() && is_creative())) {
            double ob = attr(attr::oxygen_bonus);  // decreaseAirSupply: respiration
            int a = ob > 0.0 && rand_double() >= 1.0 / (ob + 1.0) ? air_supply() : air_supply() - 1;
            if (a <= -20) {
                a = 0;
                broadcast_event(67);
                hurt_server(DamageSource::of("drown"), 2.0f);
            }
            set_air_supply(a);
        } else if (air_supply() < 300 && (!was_eye_in_water || effects_refill_air(*this))) {
            set_air_supply(std::min(air_supply() + 4, 300));
        }
    }
    if (is_alive() && (block_x() != last_block[0] || block_y() != last_block[1] || block_z() != last_block[2])) {
        last_block = {block_x(), block_y(), block_z()};
        enchant_location_changed(*this);  // onChangedBlock: frost walker, soul speed
    }
    if (hurt_time > 0) hurt_time--;
    if (damage_cooldown > 0 && !as_player()) damage_cooldown--;
    if (is_dead_or_dying()) tick_death();
    if (last_hurt_by_player_time > 0) last_hurt_by_player_time--;
    else last_hurt_by_player = 0;
    if (last_hurt_mob && (!entity_by_id(last_hurt_mob) || !entity_by_id(last_hurt_mob)->is_alive())) last_hurt_mob = 0;
    if (last_hurt_by_mob) {
        Entity* h = entity_by_id(last_hurt_by_mob);
        if (!h || !h->is_alive() || tick_count - last_hurt_by_mob_time > 100) last_hurt_by_mob = 0;
    }
    tick_effects(*this);
    y_head_rot_o = y_head_rot, y_body_rot_o = y_body_rot;
}

inline void LivingEntity::tick_head_turn(float target) {  // LivingEntity.tickHeadTurn
    float d = mth::wrap_degrees(target - y_body_rot);
    y_body_rot += d * 0.3f;
    float hd = mth::wrap_degrees(yaw - y_body_rot);
    float mx = max_head_rotation();
    if (std::abs(hd) > mx) y_body_rot += hd - (hd > 0 ? 1.0f : hd < 0 ? -1.0f : 0.0f) * mx;
}

inline void LivingEntity::tick() {  // LivingEntity.tick
    Entity::tick();
    if (!removed) ai_step();
    double xd = pos.x - old.x, zd = pos.z - old.z;
    float side = float(xd * xd + zd * zd);
    float target = y_body_rot;
    if (side > 0.0025000002f) {
        float dir = float(std::atan2(zd, xd)) * (180.0f / float(M_PI)) - 90.0f;
        float diff = std::abs(mth::wrap_degrees(yaw) - dir);
        target = 95.0f < diff && diff < 265.0f ? dir - 180.0f : dir;
    }
    tick_head_turn(target);
    if (equip_dirty) needs_sync = true;
}

inline void LivingEntity::ai_step() {  // LivingEntity.aiStep
    if (no_jump_delay > 0) no_jump_delay--;
    if (std::abs(vel.x) < 0.003) vel.x = 0;
    if (std::abs(vel.z) < 0.003) vel.z = 0;
    if (std::abs(vel.y) < 0.003) vel.y = 0;
    xxa *= 0.98f, zza *= 0.98f;  // applyInput
    if (is_dead_or_dying()) {    // isImmobile
        jumping = false, xxa = 0, zza = 0;
    } else {
        server_ai_step();
    }
    if (jumping && is_affected_by_fluids()) {
        double fh = is_in_lava() ? lava_height : water_height;
        bool in_water_h = is_in_water() && fh > 0.0;
        double thr = fluid_jump_threshold();
        if (!in_water_h || (on_ground && !(fh > thr))) {
            if (!is_in_lava() || (on_ground && !(lava_height > thr))) {
                if ((on_ground || (in_water_h && fh <= thr)) && no_jump_delay == 0) {
                    jump_from_ground();
                    no_jump_delay = 10;
                }
            } else jump_in_liquid();
        } else jump_in_liquid();
    } else {
        no_jump_delay = 0;
    }
    if (has_effect(eff::slow_falling) || has_effect(eff::levitation)) reset_fall_distance();
    travel({xxa, yya, zza});
    push_entities();
    if (is_sensitive_to_water() && is_in_water_or_rain()) hurt_server(DamageSource::of("drown"), 1.0f);
}

inline void LivingEntity::travel(const Vec3& input) {
    if (is_in_liquid() && is_affected_by_fluids()) travel_in_fluid(input);
    else travel_in_air(input);
}
inline void LivingEntity::travel_in_air(const Vec3& input) {  // LivingEntity.travelInAir
    float bf = on_ground ? modified_friction(phys_of(block_below_affecting_movement()).friction, float(attr(attr::friction_modifier))) : 1.0f;
    // handleRelativeFrictionAndCalculateMovement
    move_relative(friction_influenced_speed(bf), input);
    if (on_climbable()) {
        reset_fall_distance();
        vel = {std::clamp(vel.x, -0.15000000596046448, 0.15000000596046448), std::max(vel.y, -0.15000000596046448),
               std::clamp(vel.z, -0.15000000596046448, 0.15000000596046448)};
    }
    move(SELF, vel);
    Vec3 m = vel;
    if ((horizontal_collision || jumping) && on_climbable()) m.y = 0.2;
    double my = m.y;
    if (const EffectInstance* lev = effect(eff::levitation)) my += (0.05 * (lev->amp + 1) - m.y) * 0.2;
    else my -= effective_gravity();
    float drag_mod = float(attr(attr::air_drag_modifier));
    float air = modified_friction(0.91f, drag_mod);
    float fr = bf * air;
    float vf = modified_friction(0.98f, drag_mod);
    vel = {m.x * fr, my * vf, m.z * fr};
}
inline void LivingEntity::travel_in_fluid(const Vec3& input) {  // LivingEntity.travelInFluid
    bool falling = vel.y <= 0.0;
    double oy = pos.y;
    double g = effective_gravity();
    if (is_in_water()) {
        float slow = shared_flag(3) ? 0.9f : water_slow_down();
        float sp = 0.02f;
        float ww = float(attr(attr::water_movement_efficiency));
        if (!on_ground) ww *= 0.5f;
        if (ww > 0.0f) {
            slow += (0.54600006f - slow) * ww;
            sp += (speed - sp) * ww;
        }
        if (has_effect(eff::dolphins_grace)) slow = 0.96f;
        move_relative(sp, input);
        move(SELF, vel);
        Vec3 m = vel;
        if (horizontal_collision && on_climbable()) m.y = 0.2;
        m = m.multiply(slow, 0.8f, slow);
        vel = fluid_falling_adjusted(g, falling, m);
    } else {
        move_relative(0.02f, input);
        move(SELF, vel);
        if (lava_height <= fluid_jump_threshold()) {
            vel = vel.multiply(0.5, 0.8f, 0.5);
            vel = fluid_falling_adjusted(g, falling, vel);
        } else {
            vel = vel * 0.5;
        }
        if (g != 0.0) vel.y += -g / 4.0;
    }
    // jumpOutOfFluid
    Vec3 m = vel;
    if (horizontal_collision && is_free(m.x, m.y + 0.6f - pos.y + oy, m.z)) vel = {m.x, 0.3f, m.z};
}
inline void LivingEntity::push_entities() {  // LivingEntity.pushEntities (no maxEntityCramming damage yet)
    for_each_entity_in(bb, [&](Entity& e) {
        if (&e == this || !e.is_pushable() || e.as_player()) return;  // players push themselves client-side
        push_entity(e);
    }, true);
}

// ---------- persistence ----------
inline Nbt item_to_nbt(const ItemStack& s) {
    Nbt it = Nbt::compound();
    it.set("id", Nbt::str(item_info[size_t(s.item)].name));
    it.set("count", Nbt::num(Nbt::Int, s.count));
    if (s.damage) {
        Nbt c = Nbt::compound();
        c.set("minecraft:damage", Nbt::num(Nbt::Int, s.damage));
        it.set("components", c);
    }
    return it;
}
inline ItemStack item_from_nbt(const Nbt& it) {
    ItemStack s;
    if (const Nbt* id = it.get("id"); id && id->t == Nbt::String)
        if (auto f = item_by_name.find(id->s.find(':') == std::string::npos ? "minecraft:" + id->s : id->s); f != item_by_name.end())
            s = {f->second, int(nbt_num(it, "count", 1)), 0};
    if (const Nbt* c = it.get("components")) s.damage = int(nbt_num(*c, "minecraft:damage", 0));
    return s;
}
inline const char* equip_slot_name(int s) {
    static const char* n[] = {"mainhand", "offhand", "feet", "legs", "chest", "head", "body", "saddle"};
    return n[s];
}
inline void LivingEntity::save_extra(Nbt& o) const {
    o.set("Health", Nbt::real(Nbt::Float, health()));
    o.set("HurtTime", Nbt::num(Nbt::Short, hurt_time));
    o.set("DeathTime", Nbt::num(Nbt::Short, death_time));
    o.set("AbsorptionAmount", Nbt::real(Nbt::Float, absorption));
    save_effects(*this, o);
    if (!attrs.empty()) {
        Nbt l = Nbt::make_list();
        for (auto& a : attrs) {
            Nbt c = Nbt::compound();
            c.set("id", Nbt::str("minecraft:" + attribute_info[a.attr].id));
            c.set("base", Nbt::real(Nbt::Double, a.base));
            if (!a.mods.empty()) {
                Nbt ml = Nbt::make_list();
                for (auto& m : a.mods) {
                    Nbt mc = Nbt::compound();
                    mc.set("id", Nbt::str(m.id));
                    mc.set("amount", Nbt::real(Nbt::Double, m.amount));
                    static const char* ops[] = {"add_value", "add_multiplied_base", "add_multiplied_total"};
                    mc.set("operation", Nbt::str(ops[m.op]));
                    ml.list.push_back(mc);
                }
                c.set("modifiers", ml);
            }
            l.list.push_back(c);
        }
        o.set("attributes", l);
    }
    Nbt eq = Nbt::compound();
    for (int s = 0; s < 8; s++)
        if (!equipment[size_t(s)].empty()) eq.set(equip_slot_name(s), item_to_nbt(equipment[size_t(s)]));
    if (!eq.comp.empty()) o.set("equipment", eq);
}
inline void LivingEntity::load_extra(const Nbt& in) {
    absorption = float(nbt_num(in, "AbsorptionAmount", 0));
    load_effects(*this, in);
    if (const Nbt* l = in.get("attributes"); l && l->t == Nbt::List)
        for (auto& c : l->list) {
            const Nbt* id = c.get("id");
            if (!id || id->t != Nbt::String) continue;
            std::string n = id->s.rfind("minecraft:", 0) == 0 ? id->s.substr(10) : id->s;
            for (size_t a = 0; a < attribute_info.size(); a++) {
                if (attribute_info[a].id != n) continue;
                auto& ov = attr_override(int(a));
                ov.base = nbt_num(c, "base", ov.base);
                ov.mods.clear();
                if (const Nbt* ml = c.get("modifiers"); ml && ml->t == Nbt::List)
                    for (auto& m : ml->list) {
                        const Nbt* op = m.get("operation");
                        std::string ops = op && op->t == Nbt::String ? op->s : "add_value";
                        const Nbt* mid = m.get("id");
                        ov.mods.push_back({mid && mid->t == Nbt::String ? mid->s : "", nbt_num(m, "amount", 0),
                                           uint8_t(ops == "add_multiplied_base" ? 1 : ops == "add_multiplied_total" ? 2 : 0)});
                    }
            }
        }
    set_health(float(nbt_num(in, "Health", max_health())));
    hurt_time = int(nbt_num(in, "HurtTime", 0));
    death_time = int(nbt_num(in, "DeathTime", 0));
    if (const Nbt* eq = in.get("equipment"); eq && eq->t == Nbt::Compound)
        for (int s = 0; s < 8; s++)
            if (const Nbt* it = eq->get(equip_slot_name(s))) set_item_slot(s, item_from_nbt(*it));
}

// update_attributes: every syncable attribute the entity has (or `mask` ids when !all)
inline Writer LivingEntity::attributes_packet(uint64_t mask, bool all) const {
    Writer w;
    std::vector<int> list;
    if (all) {
        for (auto& [a, v] : info().attr) if (attribute_info[a].sync) list.push_back(a);
        for (auto& o : attrs)
            if (attribute_info[o.attr].sync && std::find(list.begin(), list.end(), o.attr) == list.end()) list.push_back(o.attr);
    } else {
        for (int a = 0; a < 64 && a < int(attribute_info.size()); a++)
            if ((mask >> a & 1) && attribute_info[size_t(a)].sync && has_attr(a)) list.push_back(a);
    }
    if (list.empty()) return w;
    w.varint(play_cb::update_attributes).varint(id).varint(int32_t(list.size()));
    for (int a : list) {
        const AttrOverride* ov = nullptr;
        for (auto& o : attrs) if (o.attr == a) ov = &o;
        w.varint(a).f64(attr_base(a));
        w.varint(ov ? int32_t(ov->mods.size()) : 0);
        if (ov)
            for (auto& m : ov->mods) w.str(m.id.find(':') == std::string::npos ? "minecraft:" + m.id : m.id).f64(m.amount).varint(m.op);
    }
    return w;
}
inline Writer LivingEntity::equipment_packet(uint8_t mask) const {
    Writer w;
    w.varint(play_cb::set_equipment).varint(id);
    int last = -1;
    for (int s = 0; s < 8; s++) if (mask >> s & 1) last = s;
    for (int s = 0; s <= last; s++) {
        if (!(mask >> s & 1)) continue;
        w.u8(uint8_t(s == last ? s : s | 0x80));
        write_slot(w, item_in_slot(s));
    }
    return w;
}
inline void LivingEntity::send_pairing(Player& to) {
    Entity::send_pairing(to);
    Writer a = attributes_packet(0, true);
    if (!a.b.empty()) to.send(a);
    uint8_t mask = 0;
    for (int s = 0; s < 8; s++) if (!item_in_slot(s).empty()) mask |= uint8_t(1 << s);
    if (mask) to.send(equipment_packet(mask));
}
inline void LivingEntity::send_extra_changes() {
    if (attr_dirty) {
        Writer a = attributes_packet(attr_dirty, false);
        if (!a.b.empty()) broadcast(a);
        if (Player* self = as_player(); self && !a.b.empty()) self->send(a);  // ServerEntity: a player's own attributes
        attr_dirty = 0;
    }
    if (equip_dirty) {
        broadcast(equipment_packet(equip_dirty));
        equip_dirty = 0;
    }
}

// ---------- players as entities ----------
inline bool hurt(Player& p, const DamageSource& s, float amount);  // survival.hpp
struct PlayerEntity : LivingEntity {
    Player* p;
    explicit PlayerEntity(Player* pl) : p(pl) { type = entity_types::player; }
    Player* as_player() override { return p; }
    const Player* as_player_const() const override { return p; }
    bool is_spectator() const override { return p->gamemode == 3; }
    bool is_creative() const override { return p->gamemode == 1; }
    bool is_alive() const override { return !removed && !p->g.dead && p->g.health > 0; }
    float health() const override { return p->g.health; }
    void set_health(float h) override { p->g.health = std::clamp(h, 0.0f, max_health()); }
    std::vector<EffectInstance>& effects() override { return p->g.effects; }
    const ItemStack& item_in_slot(int slot) const override {
        static const ItemStack none;
        switch (slot) {
            case ES_MAINHAND: return p->inv[size_t(p->selected)];
            case ES_OFFHAND: return p->inv[40];
            case ES_FEET: return p->inv[36];
            case ES_LEGS: return p->inv[37];
            case ES_CHEST: return p->inv[38];
            case ES_HEAD: return p->inv[39];
            default: return none;
        }
    }
    void sync() {  // copy the connection's state (tick thread owns both)
        old = pos;
        set_pos(p->x, p->y, p->z);
        yaw = p->yaw, pitch = p->pitch, y_head_rot = p->yaw, y_body_rot = p->yaw;
        on_ground = p->on_ground;
        fall_distance = p->g.fall_distance;
        set_shared_flag(1, p->g.sneaking);
        set_shared_flag(3, p->g.sprinting);
    }
    bool hurt_server(const DamageSource& s, float dmg) override {  // ServerPlayer.hurtServer: canHarmPlayer
        if (const Player* by = s.causing ? s.causing->as_player_const() : nullptr; by && by != p) {
            if (!rule("pvp")) return false;
            Team* a = team_of(by->name);
            if (a && a == team_of(p->name) && !a->friendly_fire) return false;  // doTeamsAllowDamage
        }
        return hurt(*p, s, dmg);
    }
    void knockback(double power, double xd, double zd) override {  // ServerPlayer: velocity goes to its own client
        power *= 1.0 - attr(attr::knockback_resistance);
        if (power <= 0.0) return;
        while (xd * xd + zd * zd < 1.0E-5f) xd = (rand_double() - rand_double()) * 0.01, zd = (rand_double() - rand_double()) * 0.01;
        Vec3 dv = Vec3{xd, 0, zd}.normalize() * power;
        Vec3 v{-dv.x, on_ground ? std::min(0.4, power) : 0.0, -dv.z};  // ponytail: server doesn't track player velocity
        vel = v;
        p->send(set_entity_motion_packet(p->eid, v.x, v.y, v.z));
    }
    void kill() override { ::kill(*p, "generic_kill"); }
    double visibility_percent(Entity*) override { return p->g.sneaking ? 0.8 : 1.0; }
    bool should_save() const override { return false; }
    void tick() override {}
};
inline PlayerEntity* player_entity(const Player& p) {
    for (Entity* e : player_entities())
        if (e->as_player() == &p) return static_cast<PlayerEntity*>(e);
    return nullptr;
}

#include "../effects/effects.hpp"
