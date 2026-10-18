// Mob roster translation unit: every vanilla mob the framework (mobs-core) and the boss branch don't own.
// Per-type tuning lives in the static spec tables below (shared by all instances of a type).
#include "horses.hpp"
#include "monsters.hpp"

// ---------- tables ----------
using namespace entity_types;
static const AnimalSpec ANIMALS[] = {
    // type, food tag, panic, tempt, follow parent, volume, fights
    {armadillo, "armadillo_food", 2.0, 1.25, 1.25},     {mooshroom, "cow_food", 2.0, 1.25, 1.25, 0.4f},
    {goat, "goat_food", 2.0, 1.25, 1.25},               {rabbit, "rabbit_food", 2.2, 1.0, 1.25, 0.6f},
    {fox, "fox_food", 2.2, 1.0, 1.25},                  {frog, "frog_food", 1.25, 1.25, 1.25},
    {turtle, "turtle_food", 1.2, 1.1, 1.0},             {panda, "panda_food", 2.0, 1.0, 1.25, 1.0f, 1},
    {polar_bear, "", 2.0, 1.0, 1.25, 1.0f, 1},          {sniffer, "sniffer_food", 2.0, 1.25, 1.1},
    {strider, "strider_food", 1.65, 1.4, 1.1},          {hoglin, "hoglin_food", 1.0, 1.0, 1.0, 1.0f, 2},
    {axolotl, "axolotl_food", 2.0, 1.1, 1.1},           {tadpole, "", 2.0, 1.0, 1.0},
    {happy_ghast, "happy_ghast_food", 1.5, 1.1, 1.1},   {nautilus, "nautilus_food", 1.5, 1.1, 1.1},
    {bee, "bee_food", 1.25, 1.25, 1.25},                {camel, "camel_food", 2.0, 1.4, 1.0},
};
inline const AnimalSpec& animal_spec(int type) {
    for (auto& s : ANIMALS)
        if (s.type == type) return s;
    return ANIMALS[0];
}
static const MonsterSpec MONSTERS[] = {
    // type, speed, hostile, burns in daylight, weapon
    {silverfish, 1.0, true, false},      {endermite, 1.0, true, false},          {vindicator, 1.0, true, false, "iron_axe"},
    {pillager, 1.0, true, false, "crossbow"}, {evoker, 1.0, true, false},        {illusioner, 1.0, true, false, "bow"},
    {ravager, 1.0, true, false},         {witch, 1.0, true, false},              {giant, 1.0, true, false},
    {piglin, 1.0, true, false, "golden_sword"}, {piglin_brute, 1.0, true, false, "golden_axe"},
    {zoglin, 1.0, true, false},          {breeze, 1.0, true, false},             {creaking, 1.0, true, false},
    {zombified_piglin, 1.0, false, false, "golden_sword"}, {zombie_nautilus, 1.0, true, false},
};
inline const MonsterSpec& monster_spec(int type) {
    for (auto& s : MONSTERS)
        if (s.type == type) return s;
    return MONSTERS[0];
}

// ---------- small types ----------
struct Fish : SwimMob {
    float sound_volume() const override { return 0.4f; }
    std::string ambient_sound() const override { return ""; }  // fish are silent but for hurt/death/flop
};
struct Squid : SwimMob {
    std::string ambient_sound() const override { return entity_sound(*this, "ambient"); }
};
struct Dolphin : SwimMob {
    bool breathes_air() const override { return true; }
    double swim_speed() const override { return 1.2; }
};
struct Guardian : SwimMob {  // ponytail: no laser attack yet
    int sound_source() const override { return SRC_HOSTILE; }
    std::string ambient_sound() const override { return is_in_water() ? "minecraft:entity.guardian.ambient" : "minecraft:entity.guardian.ambient_land"; }
};
struct Bat : FlyMob {
    int sound_source() const override { return SRC_AMBIENT; }
    float sound_volume() const override { return 0.1f; }
    int wander_range() const override { return 6; }
};
struct Allay : FlyMob {  // ponytail: no item collecting / note block dancing
    std::string ambient_sound() const override { return "minecraft:entity.allay.ambient_without_item"; }
    void register_goals() override { goal_selector.add(1, std::make_unique<LookAtPlayerGoal>(*this, 6.0f)); }
};
struct HostileFlyer : FlyMob {  // ghast, phantom, blaze, vex
    int attack_cooldown = 0;
    int sound_source() const override { return SRC_HOSTILE; }
    bool remove_when_far_away(double) override { return !is_persistence_required(); }
    void register_goals() override {
        goal_selector.add(7, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        target_selector.add(1, std::make_unique<HurtByTargetGoal>(*this));
        target_selector.add(2, std::make_unique<NearestAttackableTargetGoal>(*this, true));
    }
    void ai_step() override {
        FlyMob::ai_step();
        if (attack_cooldown > 0) attack_cooldown--;
        if (LivingEntity* t = target(); t && attack_cooldown == 0) {
            double dist_sq = dist_sqr(*t);
            if (type == entity_types::ghast && dist_sq < 4096.0) {
                // Shoot large fireball
                std::string err;
                Vec3 dir = (t->pos + Vec3{0, t->eye_height * 0.5, 0} - (pos + Vec3{0, 1.5, 0})).normalize();
                if (Entity* fb = summon_entity(entity_types::fireball, pos.x + dir.x * 2.0, pos.y + 1.5, pos.z + dir.z * 2.0, nullptr, err)) {
                    fb->vel = dir * 0.5;
                    fb->sync_velocity = true;
                    mark_entity_sync(*fb);
                }
                attack_cooldown = 50; // 2.5s cooldown
            } else if (type == entity_types::blaze && dist_sq < 2304.0) {
                // Shoot small fireball
                std::string err;
                Vec3 dir = (t->pos + Vec3{0, t->eye_height * 0.5, 0} - (pos + Vec3{0, 1.0, 0})).normalize();
                if (Entity* fb = summon_entity(entity_types::small_fireball, pos.x + dir.x * 1.0, pos.y + 1.0, pos.z + dir.z * 1.0, nullptr, err)) {
                    fb->vel = dir * 0.7;
                    fb->sync_velocity = true;
                    mark_entity_sync(*fb);
                }
                attack_cooldown = 35; // burst cooldown
            } else if (is_within_melee_attack_range(*t)) {
                do_hurt_target(*t);
                attack_cooldown = 20;
            }
        }
    }
};
struct Shulker : Mob {
    int shoot_cooldown = 40;
    int peek_cycle = 0;

    double default_gravity() const override { return 0.0; }
    bool is_pushable() const override { return false; }
    int sound_source() const override { return SRC_HOSTILE; }
    std::string ambient_sound() const override { return "minecraft:entity.shulker.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.shulker.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.shulker.death"; }
    void init() override {
        Mob::init();
        data.define(edata::Shulker::ATTACH_FACE, 0); // DOWN
        data.define(edata::Shulker::PEEK, uint8_t(0));
        data.define(edata::Shulker::COLOR, uint8_t(16)); // default purple
    }
    void register_goals() override {
        goal_selector.add(7, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        target_selector.add(1, std::make_unique<HurtByTargetGoal>(*this));
        target_selector.add(2, std::make_unique<NearestAttackableTargetGoal>(*this, true));
    }
    bool hurt_server(const DamageSource& src, float d) override {
        uint8_t peek = data.get(edata::Shulker::PEEK, uint8_t(0));
        if (peek == 0) d *= 0.2f; // 80% damage reduction when closed
        return Mob::hurt_server(src, d);
    }
    void ai_step() override {
        Mob::ai_step();
        peek_cycle++;
        if (peek_cycle % 80 == 0) {
            uint8_t next_peek = (data.get(edata::Shulker::PEEK, uint8_t(0)) == 0 ? 100 : 0);
            data.set_byte(edata::Shulker::PEEK, next_peek);
            mark_entity_sync(*this);
        }
        if (shoot_cooldown > 0) shoot_cooldown--;
        if (LivingEntity* t = target(); t && shoot_cooldown == 0 && dist_sqr(*t) < 256.0) {
            std::string err;
            if (Entity* b = summon_entity(entity_types::shulker_bullet, pos.x, pos.y + 0.5, pos.z, nullptr, err)) {
                b->vel = Vec3{0, 0.2, 0};
                mark_entity_sync(*b);
            }
            shoot_cooldown = 70;
        }
    }
};
struct Golem : PathfinderMob {  // iron golem defends villagers; snow / copper golems wander
    std::string ambient_sound() const override { return type == iron_golem ? "" : entity_sound(*this, "ambient"); }
    std::string hurt_sound(const DamageSource&) const override { return entity_sound(*this, "hurt"); }
    std::string death_sound() const override { return entity_sound(*this, "death"); }
    bool remove_when_far_away(double) override { return false; }
    void register_goals() override {
        if (type == iron_golem) {  // IronGolem.registerGoals (minus offering flowers / village patrols)
            goal_selector.add(1, std::make_unique<MeleeAttackGoal>(*this, 1.0, true));
            goal_selector.add(6, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 0.6));
            target_selector.add(2, std::make_unique<HurtByTargetGoal>(*this));
            target_selector.add(3, std::make_unique<NearestAttackableTargetGoal>(*this, false, [](LivingEntity& e) {
                return e.as_mob() && e.info().cat == CAT_MONSTER && e.type != creeper; }, 5));
        } else {
            goal_selector.add(2, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0, 1.0000001E-5f));
        }
        goal_selector.add(7, std::make_unique<LookAtPlayerGoal>(*this, 6.0f));
        goal_selector.add(8, std::make_unique<RandomLookAroundGoal>(*this));
    }
    bool do_hurt_target(Entity& t) override {
        bool r = PathfinderMob::do_hurt_target(t);
        if (r && type == iron_golem) {  // IronGolem.doHurtTarget: fling upwards
            t.vel = t.vel + Vec3{0, 0.4, 0};
            t.needs_sync = true;
            play_sound("minecraft:entity.iron_golem.attack", 1.0f, 1.0f);
        }
        return r;
    }
};
struct Camel : AbstractHorse {
    bool is_food(const ItemStack& s) const override { return item_in_tag(s.item, info().id + "_food"); }
};
struct SulfurCube : CubeMob {
    const char* sound(const char* what) const override {
        static thread_local std::string s;
        s = std::string("minecraft:entity.slime.") + what + (tiny() ? "_small" : "");
        return s.c_str();
    }
};

// ---------- merchant lookup + packet handlers ----------
static MerchantState* lookup_merchant(int eid, double& x, double& y, double& z) {
    auto* v = dynamic_cast<AbstractVillager*>(entity_by_id(eid));
    if (!v || !v->is_alive()) return nullptr;
    x = v->pos.x, y = v->pos.y, z = v->pos.z;
    return &v->m;
}

template <class T> static std::unique_ptr<Entity> make() { return std::make_unique<T>(); }
static MobRegistration roster_registration([] {
    struct { int type; EntityFactory f; } reg[] = {
        {wolf, make<Wolf>}, {cat, make<Cat>}, {ocelot, make<Ocelot>}, {parrot, make<Parrot>},
        {horse, make<Horse>}, {donkey, make<ChestedHorse>}, {mule, make<ChestedHorse>}, {llama, make<Llama>},
        {trader_llama, make<Llama>}, {skeleton_horse, make<UndeadHorse>}, {zombie_horse, make<UndeadHorse>},
        {camel, make<Camel>}, {camel_husk, make<Camel>},
        {villager, make<Villager>}, {wandering_trader, make<WanderingTrader>}, {zombie_villager, make<ZombieVillager>},
        {husk, make<Husk>}, {drowned, make<Drowned>}, {stray, make<SkeletonVariant>}, {bogged, make<SkeletonVariant>},
        {parched, make<SkeletonVariant>}, {wither_skeleton, make<WitherSkeleton>}, {cave_spider, make<CaveSpider>},
        {slime, make<Slime>}, {magma_cube, make<MagmaCube>}, {sulfur_cube, make<SulfurCube>}, {enderman, make<Enderman>},
        {cod, make<Fish>}, {salmon, make<Fish>}, {tropical_fish, make<Fish>}, {pufferfish, make<Fish>},
        {squid, make<Squid>}, {glow_squid, make<Squid>}, {dolphin, make<Dolphin>}, {guardian, make<Guardian>},
        {bat, make<Bat>}, {allay, make<Allay>}, {ghast, make<HostileFlyer>}, {phantom, make<HostileFlyer>},
        {blaze, make<HostileFlyer>}, {vex, make<HostileFlyer>}, {shulker, make<Shulker>},
        {iron_golem, make<Golem>}, {snow_golem, make<Golem>}, {copper_golem, make<Golem>},
        {zombified_piglin, make<ZombifiedPiglin>},
    };
    for (auto& r : reg) register_entity_factory(r.type, r.f);
    for (auto& s : ANIMALS)
        if (s.type != camel) register_entity_factory(s.type, make<SimpleAnimal>);
    for (auto& s : MONSTERS)
        if (s.type != zombified_piglin) register_entity_factory(s.type, make<SimpleMonster>);
    merchant_lookup = lookup_merchant;
    on(play_sb::move_vehicle, [](Player& p, Reader& in, int) { handle_move_vehicle(p, in); });
});
