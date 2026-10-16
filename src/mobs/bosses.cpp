// Boss mobs built on the Mob framework (the End fight and the wither live in src/boss/, included via game.hpp).
// ElderGuardian: stats, sounds and the mining fatigue curse (customServerAiStep + MobEffectUtil.addEffectToPlayersAround).
// ponytail: Guardian's swimming / laser AI (mobs-roster) is not here yet; the elder guardian only looks around.
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

MobRegistration reg([] { register_entity_factory(entity_types::elder_guardian, make_entity<ElderGuardian>); });

}  // namespace
