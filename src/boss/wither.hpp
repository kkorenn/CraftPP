#pragma once
// WitherBoss + WitherSkull + WitherSkullBlock.checkSpawn (soul sand T with three wither skeleton skulls).
// ponytail: no Mob framework yet (goals, navigation, FlyingMoveControl): the wither targets players only, flies with
// its own aiStep pull towards the target (no WaterAvoidingRandomFlyingGoal wandering), and the wither effect of
// skulls is not applied (no mob effects on players yet).
#include "fight.hpp"
#include "pattern.hpp"

#include <cfloat>

namespace boss {

struct WitherSkull : BossEntity {
    int owner_id = 0;
    bool dangerous = false;
    WitherSkull() { type = entity_types::wither_skull; }
    int add_entity_data() const override { return owner_id; }
    void set_dangerous(bool d) { dangerous = d, data.set_bool(edata::WitherSkull::DANGEROUS, d); }
    void tick() override;
    void save_extra(Nbt& o) const override { o.set("dangerous", Nbt::num(Nbt::Byte, dangerous)); }
    void load_extra(const Nbt& in) override { set_dangerous(nbt_num(in, "dangerous", 0) != 0); }
};

struct WitherBoss : BossEntity {
    float health = 300, max_health = 300, last_hurt = 0;
    int invul = 0, hurt_time = 0, cooldown = 0, death_time = 0, ambient_time = 0, destroy_blocks_tick = 0, attack_time = -1, seen_time = 0;
    int target = 0;  // player eid (alternative target 0 = Mob.getTarget)
    int heads[2] = {0, 0}, next_head_update[2] = {0, 0}, idle_head_updates[2] = {0, 0};
    float body_yaw = 0;
    int64_t last_hurt_by_player = -1000;
    BossEvent bar;
    WitherBoss() {
        type = entity_types::wither;
        bar.name = tr("entity.minecraft.wither"), bar.color = 5, bar.flags = 1;  // purple, progress, darken screen
    }
    bool fire_immune() const override { return true; }
    bool is_alive() const override { return !removed && health > 0; }
    int sound_source() const override { return SRC_HOSTILE; }
    double default_gravity() const override { return 0.08; }
    float body_yaw_now() const { return body_yaw; }
    float head_yaw() const override { return yaw; }
    bool powered() const { return health <= max_health / 2.0F; }
    void set_health(float h) {
        h = std::clamp(h, 0.0f, max_health);
        if (h == health) return;
        health = h;
        data.set_float(edata::LivingEntity::HEALTH, health);
    }
    void set_invul(int t) { invul = t, data.set_int(edata::WitherBoss::ID_INV, t); }
    void set_alt(int head, int eid) {
        static const int slots[3] = {edata::WitherBoss::TARGET_A, edata::WitherBoss::TARGET_B, edata::WitherBoss::TARGET_C};
        data.set_int(slots[head], eid);
        if (head > 0) heads[head - 1] = eid;
    }
    void make_invulnerable() {  // WitherBoss.makeInvulnerable
        set_invul(220);
        bar.progress = 0.0f;
        set_health(max_health / 3.0F);
    }
    double head_x(int i) const {
        if (i <= 0) return pos.x;
        return pos.x + emth::cos((body_yaw + 180 * (i - 1)) * float(M_PI / 180.0)) * 1.3;
    }
    double head_y(int i) const { return pos.y + (i <= 0 ? 3.0F : 2.2F); }
    double head_z(int i) const {
        if (i <= 0) return pos.z;
        return pos.z + emth::sin((body_yaw + 180 * (i - 1)) * float(M_PI / 180.0)) * 1.3;
    }
    bool sees(const Player& p) const { return !clip_hits(eye_pos(), Vec3{p.x, p.y + 1.62, p.z}); }
    void shoot(int head, double tx, double ty, double tz, bool dangerous) {  // performRangedAttack
        level_event(1024, block_x(), block_y(), block_z(), 0);
        double hx = head_x(head), hy = head_y(head), hz = head_z(head);
        auto s = std::make_unique<WitherSkull>();
        s->owner_id = id;
        if (dangerous) s->set_dangerous(true);
        s->snap_to(hx, hy, hz, yaw, pitch);
        s->vel = Vec3{tx - hx, ty - hy, tz - hz}.normalize() * 0.1;
        add_entity(std::move(s));
    }
    void shoot_at(int head, const Player& p) { shoot(head, p.x, p.y + 1.62 * 0.5, p.z, head == 0 && rand_float() < 0.001F); }

    void tick() override;
    void server_ai();
    bool hurt_by(const DamageSource& d, Player* p, float dmg, int part = -1) override;
    void kill() override { hurt_by(DamageSource::of("generic_kill"), nullptr, FLT_MAX); }
    void on_removed() override { bar.remove_all(); }
    void save_extra(Nbt& o) const override {
        o.set("Health", Nbt::real(Nbt::Float, health));
        o.set("Invul", Nbt::num(Nbt::Int, invul));
    }
    void load_extra(const Nbt& in) override {
        set_health(float(nbt_num(in, "Health", 300)));
        set_invul(int(nbt_num(in, "Invul", 0)));
    }
};

inline void WitherBoss::tick() {
    if (hurt_time > 0) hurt_time--;
    if (cooldown > 0) cooldown--;
    // the bar follows the tracking players (startSeenByPlayer / stopSeenByPlayer)
    for (Player* p : trk.watchers) bar.add(*p);
    for (size_t i = 0; i < bar.players.size();) {
        Player* p = player_by_eid(bar.players[i]);
        if (p && std::find(trk.watchers.begin(), trk.watchers.end(), p) == trk.watchers.end()) bar.remove(*p);
        else if (!p) bar.players.erase(bar.players.begin() + std::ptrdiff_t(i));
        else i++;
    }
    if (health <= 0) {  // LivingEntity.tickDeath
        if (++death_time >= 20) {
            broadcast_event(60);
            remove(Removal::KILLED);
        }
        return;
    }
    if (difficulty() == 0) return discard();  // checkDespawn: not allowed in peaceful
    // aiStep: pull towards the target
    Vec3 dm = vel.multiply(1.0, 0.6, 1.0);
    if (Player* t = player_by_eid(int(data.get(edata::WitherBoss::TARGET_A, 0)))) {  // getAlternativeTarget(0)
        double yd = dm.y;
        if (pos.y < t->y || (!powered() && pos.y < t->y + 5.0)) {
            yd = std::max(0.0, yd);
            yd += 0.3 - yd * 0.6F;
        }
        dm.y = yd;
        Vec3 delta{t->x - pos.x, 0.0, t->z - pos.z};
        if (delta.horizontal_sqr() > 9.0) {
            Vec3 sc = delta.normalize();
            dm = dm + Vec3{sc.x * 0.3 - dm.x * 0.6, 0.0, sc.z * 0.3 - dm.z * 0.6};
        }
    }
    vel = dm;
    if (vel.horizontal_sqr() > 0.05) yaw = float(emth::atan2(vel.z, vel.x)) * (180.0F / float(M_PI)) - 90.0F;
    server_ai();
    if (removed) return;
    // LivingEntity.travel in air (no move input: ponytail, no FlyingMoveControl)
    double ydm = vel.y - gravity();
    vel.y = ydm;
    move(SELF, vel);
    vel = {vel.x * 0.91F, vel.y * 0.98F, vel.z * 0.91F};
    body_yaw = yaw;
    if (rand_int(1000) < ambient_time++) {
        ambient_time = -80;
        play_sound("minecraft:entity.wither.ambient", 4.0f, rand_pitch());
    }
}

inline bool wither_target_ok(const WitherBoss& w, const Player& p, double range) {
    return attackable(p) && dsq(Vec3{p.x, p.y, p.z}, w.pos.x, w.pos.y, w.pos.z) <= range * range;
}

inline void WitherBoss::server_ai() {  // customServerAiStep + target / ranged-attack goals
    if (invul > 0) {
        int n = invul - 1;
        bar.set_progress(1.0F - n / 220.0F);
        if (n <= 0) {
            explode(pos.x, eye_y(), pos.z, 7.0F, 2, false, id);
            level_event(1023, block_x(), block_y(), block_z(), 0, true);
        }
        set_invul(n);
        if (tick_count % 10 == 0) set_health(health + 10.0F);
        return;
    }
    // NearestAttackableTargetGoal (follow range 40) / keep the current target while valid
    Player* t = player_by_eid(target);
    if (!t || !wither_target_ok(*this, *t, 40.0)) {
        t = nearest_player(pos.x, pos.y, pos.z, 40.0);
        target = t ? t->eid : 0;
    }
    // RangedAttackGoal(1.0, 40, 20): head 0 shoots every 40 ticks while the target is seen within 20 blocks
    if (t) {
        bool seen = sees(*t);
        seen_time = seen ? seen_time + 1 : 0;
        double d2 = dsq(Vec3{t->x, t->y, t->z}, pos.x, pos.y, pos.z);
        if (--attack_time == 0) {
            if (seen && d2 <= 400.0) shoot_at(0, *t);
            attack_time = 40;
        } else if (attack_time < 0) attack_time = 40;
    } else attack_time = -1;
    for (int i = 1; i < 3; i++) {
        if (tick_count < next_head_update[i - 1]) continue;
        next_head_update[i - 1] = tick_count + 10 + rand_int(10);
        if (difficulty() >= 2 && idle_head_updates[i - 1]++ > 15) {
            double xt = pos.x - 10.0 + rand_double() * 20.0, yt = pos.y - 5.0 + rand_double() * 10.0, zt = pos.z - 10.0 + rand_double() * 20.0;
            shoot(i + 1, xt, yt, zt, true);
            idle_head_updates[i - 1] = 0;
        }
        if (heads[i - 1] > 0) {
            Player* c = player_by_eid(heads[i - 1]);
            if (c && attackable(*c) && dsq(Vec3{c->x, c->y, c->z}, pos.x, pos.y, pos.z) <= 900.0 && sees(*c)) {
                shoot_at(i + 1, *c);
                next_head_update[i - 1] = tick_count + 40 + rand_int(20);
                idle_head_updates[i - 1] = 0;
            } else set_alt(i, 0);
        } else {
            std::vector<Player*> near;
            AABB box = bb.inflate(20.0, 8.0, 20.0);
            for (auto& pp : players)
                if (attackable(*pp) && wither_target_ok(*this, *pp, 20.0) && player_box(*pp).intersects(box)) near.push_back(pp.get());
            if (!near.empty()) set_alt(i, near[size_t(rand_int(int(near.size())))]->eid);
        }
    }
    set_alt(0, target);
    if (destroy_blocks_tick > 0 && --destroy_blocks_tick == 0 && rule("mob_griefing")) {
        bool destroyed = false;
        int w = Mth::floor(width / 2.0F + 1.0F), h = Mth::floor(height);
        for (int z = block_z() - w; z <= block_z() + w; z++)
            for (int y = block_y(); y <= block_y() + h; y++)
                for (int x = block_x() - w; x <= block_x() + w; x++) {
                    int s = get_block(x, y, z);
                    if (is_air(s) || has_tag(tag_wither_immune, s)) continue;
                    std::vector<ItemStack> drops;  // Level.destroyBlock(pos, true, this)
                    level_event(2001, x, y, z, s);
                    block_drops(s, x, y, z, {}, drops);
                    set_block(x, y, z, blocks::air);
                    for (auto& d : drops) pop_resource(x, y, z, d);
                    destroyed = true;
                }
        if (destroyed) level_event(1022, block_x(), block_y(), block_z(), 0);
    }
    if (tick_count % 20 == 0) set_health(health + 1.0F);
    bar.set_progress(health / max_health);
}

inline bool WitherBoss::hurt_by(const DamageSource& d, Player* p, float dmg, int) {  // WitherBoss.hurtServer
    if (is_invulnerable_to(d) || health <= 0) return false;
    if (d.is_type("drown") || (d.causing && d.causing->type == entity_types::wither)) return false;  // wither_immune_to
    if (invul > 0 && !d.is(DT_BYPASSES_INVULNERABILITY)) return false;
    if (d.causing && d.causing->in_tag("wither_friends")) return false;
    if (destroy_blocks_tick <= 0) destroy_blocks_tick = 20;
    for (int& h : idle_head_updates) h += 3;
    // LivingEntity.hurtServer: cooldown, armor (4), death
    if (std::isnan(dmg) || std::isinf(dmg)) dmg = FLT_MAX;
    auto apply = [&](float a) {
        if (!d.is(DT_BYPASSES_ARMOR)) {  // CombatRules.getDamageAfterAbsorb, armor 4, toughness 0
            float armor = 4.0F, real = std::clamp(armor - a / 2.0F, armor * 0.2F, 20.0F);
            a = a * (1.0F - real / 25.0F);
        }
        set_health(health - a);
    };
    bool full = true;
    if (cooldown > 10 && !d.is(DT_BYPASSES_COOLDOWN)) {
        if (dmg <= last_hurt) return false;
        apply(dmg - last_hurt);
        last_hurt = dmg, full = false;
    } else {
        last_hurt = dmg, cooldown = 20, hurt_time = 10;
        apply(dmg);
    }
    if (p) last_hurt_by_player = tick_count, target = p->eid;  // HurtByTargetGoal
    if (full) {
        Writer w;
        w.varint(play_cb::damage_event).varint(id).varint(d.type).varint(p ? p->eid + 1 : d.causing ? d.causing->id + 1 : 0)
            .varint(d.direct ? d.direct->id + 1 : p ? p->eid + 1 : 0).boolean(false);
        broadcast(w);
    }
    if (health <= 0) {  // die
        play_sound("minecraft:entity.wither.death", 4.0f, rand_pitch());
        broadcast_event(3);
        spawn_item({item_by_name["minecraft:nether_star"], 1, 0}, pos.x, pos.y, pos.z);  // ponytail: extended lifetime not set
        if (tick_count - last_hurt_by_player < 100 && rule("mob_drops")) award_xp(pos.x, pos.y, pos.z, 50);
    } else if (full) play_sound("minecraft:entity.wither.hurt", 4.0f, rand_pitch());
    return true;
}

inline void WitherSkull::tick() {  // AbstractHurtingProjectile.tick + WitherSkull hits
    ::Entity* owner = owner_id ? entity_by_id(owner_id) : nullptr;
    if (owner_id && (!owner || owner->removed)) return discard();
    vel = (vel + vel.normalize() * 0.1) * double(dangerous ? 0.73F : 0.95F);
    Vec3 from = pos, to = from + vel;
    double best = 2;
    Player* hit = nullptr;
    if (clip_hits(from, to))
        for (int i = 1; i <= 16; i++)
            if (clip_hits(from, from + vel * (i / 16.0))) { best = (i - 1) / 16.0; break; }
    for (auto& pp : players) {
        double t;
        if (alive_target(*pp) && segment_hits_box(from, to, player_box(*pp).inflate(0.3), t) && t < best) best = t, hit = pp.get();
    }
    set_pos(best <= 1 ? from + vel * best : to);
    Entity::tick();
    if (removed || best > 1) return;
    if (hit) {  // onHitEntity: 8 wither_skull damage from a living owner (5 magic otherwise)
        bool owned = owner && dynamic_cast<WitherBoss*>(owner);
        bool hurt = owned ? hurt_player(*hit, 8.0f, "wither_skull", true) : ::hurt(*hit, 5.0f, "magic");
        if (hurt && owned && hit->g.dead) static_cast<WitherBoss*>(owner)->set_health(static_cast<WitherBoss*>(owner)->health + 5.0F);
    }
    explode(pos.x, pos.y, pos.z, 1.0F, 2, false, id, -1, false, dangerous);
    discard();
}

// WitherSkullBlock.checkSpawn after a wither skeleton skull is placed at (x, y, z).
inline bool wither_check_spawn(int x, int y, int z) {
    int s = get_block(x, y, z);
    auto skull = [](int st) { return block_index(st) == block_index(blocks::wither_skeleton_skull) ||
                                     block_index(st) == block_index(blocks::wither_skeleton_wall_skull); };
    if (!skull(s) || y < MIN_Y || difficulty() == 0) return false;
    static const BlockPattern full{{{"^^^", "###", "~#~"}}, [](char c, int st) {
        if (c == '^') return block_index(st) == block_index(blocks::wither_skeleton_skull) ||
                             block_index(st) == block_index(blocks::wither_skeleton_wall_skull);
        if (c == '#') return has_tag(tag_wither_summon_base, st);
        return is_air(st);  // '~'
    }};
    auto m = full.find(x, y, z, get_block);
    if (!m) return false;
    for (int px = 0; px < 3; px++)  // CarvedPumpkinBlock.clearPatternBlocks
        for (int py = 0; py < 3; py++) {
            auto b = m->at(px, py, 0);
            int st = get_block(b[0], b[1], b[2]);
            boss_level().set_block(b[0], b[1], b[2], blocks::air);
            broadcast(block_update(b[0], b[1], b[2], blocks::air));
            level_event(2001, b[0], b[1], b[2], st);
        }
    auto sp = m->at(1, 2, 0);
    auto w = std::make_unique<WitherBoss>();
    float yr = m->forwards_x() ? 0.0F : 90.0F;
    w->snap_to(sp[0] + 0.5, sp[1] + 0.55, sp[2] + 0.5, yr, 0.0F);
    w->body_yaw = yr;
    w->make_invulnerable();
    add_entity(std::move(w));
    return true;
}

}  // namespace boss
