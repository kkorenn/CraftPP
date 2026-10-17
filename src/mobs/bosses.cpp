// Boss mobs built on the Mob framework (the End fight and the wither live in src/boss/, included via game.hpp).
// ElderGuardian: stats, sounds and the mining fatigue curse (customServerAiStep + MobEffectUtil.addEffectToPlayersAround).
// ponytail: Guardian's swimming / laser AI (mobs-roster) is not here yet; the elder guardian only looks around.
// Warden: AngerManagement (suspects, decay, agitated 40 / angry 80), melee, SonicBoom, darkness pulses, digging.
// ponytail: no game event / vibration system yet: a moving, non-sneaking survival player within 16 blocks counts as
// one vibration per 40 ticks (VIBRATION_COOLDOWN_TICKS); no sniffing, roaring or emerging from shriekers.
#include "mobs.hpp"

namespace {

struct Curse { int amplifier; int64_t end; };
std::unordered_map<int, Curse> mining_fatigue;  // player eid -> what we last applied (the server keeps no effects yet)

struct ElderGuardian : Monster {
    void init() override {
        Monster::init();
        xp_reward = 10;
        set_persistence_required();
    }
    void register_goals() override {
        goal_selector.add(8, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(9, std::make_unique<RandomLookAroundGoal>(*this));
    }
    bool remove_when_far_away(double) override { return false; }
    std::string ambient_sound() const override { return is_in_water() ? "minecraft:entity.elder_guardian.ambient" : "minecraft:entity.elder_guardian.ambient_land"; }
    std::string hurt_sound(const DamageSource&) const override { return is_in_water() ? "minecraft:entity.elder_guardian.hurt" : "minecraft:entity.elder_guardian.hurt_land"; }
    std::string death_sound() const override { return is_in_water() ? "minecraft:entity.elder_guardian.death" : "minecraft:entity.elder_guardian.death_land"; }
    void custom_server_ai_step() override {
        Monster::custom_server_ai_step();
        if ((tick_count + id) % 1200 != 0) return;
        static const int fatigue = registry_index(*registry("minecraft:mob_effect"), "minecraft:mining_fatigue");
        for (auto& pp : players) {
            Player& p = *pp;
            if (!survival_like(p) || p.g.dead || pos.dist_sqr(Vec3{p.x, p.y, p.z}) >= 50.0 * 50.0) continue;
            auto it = mining_fatigue.find(p.eid);
            bool has = it != mining_fatigue.end() && it->second.end > server_tick;
            if (has && it->second.amplifier >= 2 && it->second.end - server_tick > 1200 - 1) continue;  // endsWithin(1199)
            mining_fatigue[p.eid] = {2, server_tick + 6000};
            p.send(Writer().varint(play_cb::update_mob_effect).varint(p.eid).varint(fatigue).varint(2).varint(6000).u8(0x02 | 0x04));
            p.send(Writer().varint(play_cb::game_event).u8(10).f32(silent() ? 0.0f : 1.0f));  // GUARDIAN_ELDER_EFFECT
        }
    }
};

void send_effect(Player& p, const char* effect, int amp, int ticks) {
    int id = registry_index(*registry("minecraft:mob_effect"), std::string("minecraft:") + effect);
    p.send(Writer().varint(play_cb::update_mob_effect).varint(p.eid).varint(id).varint(amp).varint(ticks).u8(0x02 | 0x04));
}

struct Warden : Monster {
    std::unordered_map<int, int> anger;  // AngerManagement: entity id -> anger (0..150)
    std::unordered_map<int, Vec3> heard_at;  // player eid -> position at the last vibration check
    int64_t boom_cooldown_until = 0, dig_after = 0;
    int boom_tick = -1, dig_tick = -1;
    void init() override {
        Monster::init();
        xp_reward = 5;
        dig_after = server_tick + 1200;
    }
    void register_goals() override {
        goal_selector.add(1, std::make_unique<MeleeAttackGoal>(*this, 1.2, true));
        goal_selector.add(8, std::make_unique<RandomLookAroundGoal>(*this));
    }
    bool remove_when_far_away(double) override { return false; }
    int active_anger() const {
        int best = 0;
        for (auto& [e, a] : anger) best = std::max(best, a);
        return best;
    }
    std::string ambient_sound() const override {
        int a = active_anger();
        return a >= 80 ? "minecraft:entity.warden.angry" : a >= 40 ? "minecraft:entity.warden.agitated" : "minecraft:entity.warden.ambient";
    }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.warden.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.warden.death"; }
    static bool can_target(Entity* e) {  // Warden.canTargetEntity
        LivingEntity* l = living_of(e);
        if (!l || l->removed || l->is_dead_or_dying() || l->type == entity_types::warden || l->type == entity_types::armor_stand) return false;
        if (Player* p = e->as_player()) return p->gamemode == 0 || p->gamemode == 2;
        return !e->invulnerable;
    }
    void increase_anger(Entity* e, int amount, bool sound) {
        if (!can_target(e) || dig_tick >= 0) return;
        dig_after = server_tick + 1200;  // WardenAi.setDigCooldown
        int& a = anger[e->id];
        a = std::min(150, a + amount);
        if (sound) play_sound(active_anger() >= 40 ? "minecraft:entity.warden.listening_angry" : "minecraft:entity.warden.listening", 1.0f, 1.0f);
    }
    bool hurt_server(const DamageSource& s, float dmg) override {
        bool r = Monster::hurt_server(s, dmg);
        if (dig_tick < 0 && s.causing) {
            increase_anger(s.causing, 80 + 20, false);
            if (!target() && s.causing->as_living() && (s.direct == s.causing || dist_sqr(*s.causing) < 25.0)) {
                set_target(s.causing->as_living());
                boom_cooldown_until = server_tick + 200;
            }
        }
        return r;
    }
    bool in_boom_range(const Entity& t) const {  // closerThan(target, 15, 20): horizontal 15, vertical 20
        double dx = t.pos.x - pos.x, dz = t.pos.z - pos.z;
        return dx * dx + dz * dz < 15.0 * 15.0 && std::abs(t.pos.y - pos.y) < 20.0;
    }
    void custom_server_ai_step() override {
        Monster::custom_server_ai_step();
        if (dig_tick >= 0) {  // Digging: 100 ticks, then gone
            if (++dig_tick >= 100) discard();
            return;
        }
        if (tick_count % 40 == 0)  // vibrations (see the header note)
            for (auto& pp : players) {
                Player& p = *pp;
                Vec3 at{p.x, p.y, p.z};
                auto it = heard_at.find(p.eid);
                bool moved = it != heard_at.end() && it->second.dist_sqr(at) > 0.01;
                heard_at[p.eid] = at;
                if (moved && !p.g.sneaking && pos.dist_sqr(at) <= 16.0 * 16.0) {
                    broadcast_event(61);  // tendrils
                    increase_anger(entity_by_id(p.eid), 35, true);
                }
            }
        if ((tick_count + id) % 120 == 0)  // applyDarknessAround
            for (auto& pp : players)
                if (survival_like(*pp) && pos.dist_sqr(Vec3{pp->x, pp->y, pp->z}) < 20.0 * 20.0) send_effect(*pp, "darkness", 0, 260);
        if (tick_count % 20 == 0) {  // AngerManagement.tick: decay, forget invalid suspects
            for (auto it = anger.begin(); it != anger.end();) {
                if (!can_target(entity_by_id(it->first)) || --it->second <= 0) it = anger.erase(it);
                else ++it;
            }
            data.set_int(edata::Warden::CLIENT_ANGER_LEVEL, active_anger());
        }
        int best_id = 0, best = 0;  // getEntityAngryAt
        for (auto& [e, a] : anger)
            if (a > best) best = a, best_id = e;
        if (best >= 80 && (!target() || target()->id != best_id)) set_target(living_of(entity_by_id(best_id)));
        if (best < 80 && target() && !anger.count(target()->id)) set_target(nullptr);
        LivingEntity* t = target();
        if (boom_tick >= 0) {  // SonicBoom: charge 34 ticks, hit, 60 total, cooldown 40
            boom_tick++;
            if (boom_tick == 34 && t && in_boom_range(*t)) {
                play_sound("minecraft:entity.warden.sonic_boom", 3.0f, 1.0f);
                Vec3 d = (t->eye_pos() - Vec3{pos.x, pos.y + 1.6, pos.z}).normalize();
                if (t->hurt_server(DamageSource::of("sonic_boom", this), 10.0f)) {
                    double kr = t->attr(attr::knockback_resistance);
                    Vec3 k{d.x * 2.5 * (1.0 - kr), d.y * 0.5 * (1.0 - kr), d.z * 2.5 * (1.0 - kr)};
                    if (Player* pl = t->as_player()) pl->send(set_entity_motion_packet(pl->eid, k.x, k.y, k.z));  // hurtMarked impulse
                    else t->push(k.x, k.y, k.z), t->sync_velocity = true;
                }
            }
            if (boom_tick >= 60) boom_tick = -1, boom_cooldown_until = server_tick + 40;
        } else if (t && server_tick >= boom_cooldown_until && in_boom_range(*t) && sensing.has_line_of_sight(*this, *t)) {
            boom_tick = 0;
            broadcast_event(62);
            play_sound("minecraft:entity.warden.sonic_charge", 3.0f, 1.0f);
        }
        if (!t && active_anger() == 0 && server_tick >= dig_after) {  // Digging after the dig cooldown
            dig_tick = 0;
            set_pose(Pose::DIGGING);
            play_sound("minecraft:entity.warden.dig", 5.0f, 1.0f);
            navigation->stop();
        }
    }
};

MobRegistration reg([] {
    register_entity_factory(entity_types::elder_guardian, make_entity<ElderGuardian>);
    register_entity_factory(entity_types::warden, make_entity<Warden>);
});

}  // namespace
