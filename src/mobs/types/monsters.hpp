#pragma once
// Zombie, Skeleton, Spider, Creeper (net.minecraft.world.entity.monster.*).
#include "misc.hpp"

inline int item_id(const char* name) {
    auto it = item_by_name.find(std::string("minecraft:") + name);
    return it == item_by_name.end() ? -1 : it->second;
}
// Mob.populateDefaultEquipmentSlots (armour) + getEquipmentForSlot
inline void populate_default_armor(Mob& m, const DifficultyInstance& d) {
    if (!(rand_float() < 0.15f * d.special_multiplier())) return;
    int type = rand_int(3);
    for (int i = 1; i <= 3; i++) if (rand_float() < 0.1087f) type++;
    static const char* mat[] = {"leather", "copper", "golden", "chainmail", "iron", "diamond"};
    static const char* piece[] = {"", "", "boots", "leggings", "chestplate", "helmet"};
    float partial = difficulty() == 3 ? 0.1f : 0.25f;
    bool first = true;
    for (int slot : {ES_HEAD, ES_CHEST, ES_LEGS, ES_FEET}) {
        if (!first && rand_float() < partial) break;
        first = false;
        if (!m.equipment[size_t(slot)].empty() || type > 5) continue;
        int it = item_id((std::string(mat[type]) + "_" + piece[slot]).c_str());
        if (it >= 0) m.set_item_slot(slot, {it, 1, 0});
    }
}

// ---------- Zombie ----------
struct Zombie : Monster {
    bool can_break_doors_ = false;
    bool is_baby() const override { return data.get(edata::Zombie::BABY, 0); }
    void set_baby(bool b) {
        data.set_bool(edata::Zombie::BABY, b);
        remove_attr_modifier(attr::movement_speed, "minecraft:baby");
        if (b) add_attr_modifier(attr::movement_speed, "minecraft:baby", 0.5, 1);
        if (b) set_size(0.49f, 0.98f, 0.775f);
        else set_size(info().w, info().h, info().eye);
    }
    std::string ambient_sound() const override { return "minecraft:entity.zombie.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.zombie.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.zombie.death"; }
    struct ZombieAttackGoal : MeleeAttackGoal {
        Zombie& z;
        int raise_arm = 0;
        ZombieAttackGoal(Zombie& zz, double s, bool t) : MeleeAttackGoal(zz, s, t), z(zz) {}
        void start() override { MeleeAttackGoal::start(); raise_arm = 0; }
        void stop() override { MeleeAttackGoal::stop(); z.set_aggressive(false); }
        void tick() override {
            MeleeAttackGoal::tick();
            raise_arm++;
            z.set_aggressive(raise_arm >= 5 && until_attack < adjusted_tick_delay(20) / 2);
        }
    };
    void register_goals() override {
        // ponytail: ZombieAttackTurtleEggGoal, SpearUseGoal, MoveThroughVillageGoal need turtle eggs/spears/villages
        goal_selector.add(8, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(8, std::make_unique<RandomLookAroundGoal>(*this));
        goal_selector.add(3, std::make_unique<ZombieAttackGoal>(*this, 1.0, false));
        goal_selector.add(7, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
        auto hurt = std::make_unique<HurtByTargetGoal>(*this);
        hurt->set_alert_others({entity_types::zombified_piglin});
        target_selector.add(1, std::move(hurt));
        target_selector.add(2, std::make_unique<NearestAttackableTargetGoal>(*this, true));
        target_selector.add(3, std::make_unique<NearestAttackableTargetGoal>(*this, false, [](LivingEntity& e) {
            return e.type == entity_types::villager || e.type == entity_types::wandering_trader; }));
        target_selector.add(3, std::make_unique<NearestAttackableTargetGoal>(*this, true, [](LivingEntity& e) { return e.type == entity_types::iron_golem; }));
    }
    int base_experience_reward() override {
        if (is_baby()) xp_reward = int(xp_reward * 2.5);
        return Monster::base_experience_reward();
    }
    bool do_hurt_target(Entity& t) override {
        bool r = Monster::do_hurt_target(t);
        if (r) {
            float d = difficulty_at(block_x(), block_y(), block_z()).effective;
            if (main_hand().empty() && is_on_fire() && rand_float() < d * 0.3f) t.ignite_for_ticks(2 * int(d) * 20);
        }
        return r;
    }
    bool hurt_server(const DamageSource& s, float dmg) override;
    void populate_default_equipment(const DifficultyInstance& d) override {
        populate_default_armor(*this, d);
        if (rand_float() < (difficulty() == 3 ? 0.05f : 0.01f)) {
            int r = rand_int(6);
            int it = item_id(r == 0 ? "iron_sword" : r == 1 ? "iron_spear" : "iron_shovel");
            if (it >= 0) set_item_slot(ES_MAINHAND, {it, 1, 0});
        }
    }
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override {
        g = Monster::finalize_spawn(r, g);
        DifficultyInstance d = difficulty_at(block_x(), block_y(), block_z());
        float mod = d.special_multiplier();
        if (r != SpawnReason::CONVERSION) can_pick_up_loot_ = rand_float() < 0.55f * mod;
        if (rand_float() < 0.05f) set_baby(true);  // ZombieGroupData(getSpawnAsBabyOdds); ponytail: chicken jockeys not ported
        can_break_doors_ = rand_float() < mod * 0.1f;
        if (r != SpawnReason::CONVERSION) populate_default_equipment(d);
        // handleAttributes
        set_attr_base(attr::spawn_reinforcements, rand_double() * 0.1f);
        add_attr_modifier(attr::knockback_resistance, "minecraft:random_spawn_bonus", rand_double() * 0.05f, 0);
        double fr = rand_double() * 1.5 * mod;
        if (fr > 1.0) add_attr_modifier(attr::follow_range, "minecraft:zombie_random_spawn_bonus", fr, 2);
        if (rand_float() < mod * 0.05f) {
            add_attr_modifier(attr::spawn_reinforcements, "minecraft:leader_zombie_bonus", rand_double() * 0.25 + 0.5, 0);
            add_attr_modifier(attr::max_health, "minecraft:leader_zombie_bonus", rand_double() * 3.0 + 1.0, 2);
            set_health(max_health());
            can_break_doors_ = true;
        }
        return g;
    }
    void save_extra(Nbt& o) const override {
        Monster::save_extra(o);
        o.set("IsBaby", Nbt::num(Nbt::Byte, is_baby()));
        o.set("CanBreakDoors", Nbt::num(Nbt::Byte, can_break_doors_));
    }
    void load_extra(const Nbt& in) override {
        Monster::load_extra(in);
        set_baby(nbt_num(in, "IsBaby", 0) != 0);
        can_break_doors_ = nbt_num(in, "CanBreakDoors", 0) != 0;
    }
};
inline bool Zombie::hurt_server(const DamageSource& s, float dmg) {  // reinforcements on hard
    if (!Monster::hurt_server(s, dmg)) return false;
    LivingEntity* t = target();
    if (!t) t = living_of(s.causing);
    if (t && difficulty() == 3 && rand_float() < attr(attr::spawn_reinforcements) && rule("spawn_monsters")) {
        int x = block_x(), y = block_y(), z = block_z();
        for (int i = 0; i < 50; i++) {
            int xt = x + rand_between(7, 40) * rand_between(-1, 1), yt = y + rand_between(7, 40) * rand_between(-1, 1),
                zt = z + rand_between(7, 40) * rand_between(-1, 1);
            if (!sflag(block_at(xt, yt - 1, zt), SF_STURDY_UP) || max_local_raw_brightness(xt, yt, zt) >= 10) continue;
            if (!check_spawn_rules_for(type, SpawnReason::REINFORCEMENT, xt, yt, zt)) continue;
            auto e = create_entity(type);
            auto* r = static_cast<Zombie*>(e.get());
            r->snap_to(xt + 0.5, yt, zt + 0.5, 0, 0);
            bool near_player = false;
            for (Entity* p : player_entities()) near_player |= p->dist_sqr(r->pos.x, r->pos.y, r->pos.z) < 49.0;
            if (near_player || !r->check_spawn_obstruction()) continue;
            r->set_target(t);
            r->finalize_spawn(SpawnReason::REINFORCEMENT, nullptr);
            add_attr_modifier(attr::spawn_reinforcements, "minecraft:reinforcement_caller_charge", -0.05f, 0);
            r->add_attr_modifier(attr::spawn_reinforcements, "minecraft:reinforcement_callee_charge", -0.05f, 0);
            add_entity(std::move(e));
            break;
        }
    }
    return true;
}

// ---------- Skeleton ----------
struct Skeleton : Monster {
    struct BowGoal : Goal {  // RangedBowAttackGoal
        Skeleton& mob;
        double speed;
        int min_interval, attack_time = -1, see_time = 0, strafing_time = -1;
        float radius_sqr;
        bool clockwise = false, backwards = false;
        BowGoal(Skeleton& m, double s, int mi, float r) : mob(m), speed(s), min_interval(mi), radius_sqr(r * r) { flags = GF_MOVE | GF_LOOK; }
        bool holding_bow() const { return mob.main_hand().item == item_id("bow") || mob.item_in_slot(ES_OFFHAND).item == item_id("bow"); }
        bool can_use() override { return mob.target() && holding_bow(); }
        bool can_continue_to_use() override { return (can_use() || !mob.navigation->is_done()) && holding_bow(); }
        void start() override { mob.set_aggressive(true); }
        void stop() override { mob.set_aggressive(false); see_time = 0; attack_time = -1; mob.stop_using_item(); }
        bool requires_update_every_tick() override { return true; }
        void tick() override {
            LivingEntity* t = mob.target();
            if (!t) return;
            double d = mob.dist_sqr(t->pos.x, t->pos.y, t->pos.z);
            bool los = mob.sensing.has_line_of_sight(mob, *t), had = see_time > 0;
            if (los != had) see_time = 0;
            see_time += los ? 1 : -1;
            if (!(d > radius_sqr) && see_time >= 20) { mob.navigation->stop(); strafing_time++; }
            else { mob.navigation->move_to(*t, speed); strafing_time = -1; }
            if (strafing_time >= 20) {
                if (rand_float() < 0.3) clockwise = !clockwise;
                if (rand_float() < 0.3) backwards = !backwards;
                strafing_time = 0;
            }
            if (strafing_time > -1) {
                if (d > radius_sqr * 0.75f) backwards = false;
                else if (d < radius_sqr * 0.25f) backwards = true;
                mob.move_control->strafe(backwards ? -0.5f : 0.5f, clockwise ? 0.5f : -0.5f);
                mob.look_at(*t, 30.0f, 30.0f);
            } else {
                mob.look_control->set_look_at(*t, 30.0f, 30.0f);
            }
            if (mob.using_item) {
                if (!los && see_time < -60) mob.stop_using_item();
                else if (los) {
                    int pull = mob.ticks_using;
                    if (pull >= 20) {
                        mob.stop_using_item();
                        float p = float(pull) / 20.0f;  // BowItem.getPowerForTime
                        p = (p * p + p * 2.0f) / 3.0f;
                        mob.perform_ranged_attack(*t, std::min(p, 1.0f));
                        attack_time = min_interval;
                    }
                }
            } else if (--attack_time <= 0 && see_time >= -60) {
                mob.start_using_item();
            }
        }
    };
    struct SkelMelee : MeleeAttackGoal {
        Skeleton& s;
        SkelMelee(Skeleton& m) : MeleeAttackGoal(m, 1.2, false), s(m) {}
        void stop() override { MeleeAttackGoal::stop(); s.set_aggressive(false); }
        void start() override { MeleeAttackGoal::start(); s.set_aggressive(true); }
    };
    BowGoal* bow_goal = nullptr;
    SkelMelee* melee_goal = nullptr;
    bool using_item = false;
    int ticks_using = 0;
    void start_using_item() {
        using_item = true, ticks_using = 0;
        data.set_byte(edata::LivingEntity::LIVING_ENTITY_FLAGS, 1);  // using item, main hand
    }
    void stop_using_item() {
        using_item = false, ticks_using = 0;
        data.set_byte(edata::LivingEntity::LIVING_ENTITY_FLAGS, 0);
    }
    std::string ambient_sound() const override { return "minecraft:entity.skeleton.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.skeleton.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.skeleton.death"; }
    void register_goals() override {
        goal_selector.add(2, std::make_unique<RestrictSunGoal>(*this));
        goal_selector.add(3, std::make_unique<FleeSunGoal>(*this, 1.0));
        goal_selector.add(3, std::make_unique<AvoidEntityGoal>(*this, [](LivingEntity& e) { return e.type == entity_types::wolf; }, 6.0f, 1.0, 1.2));
        goal_selector.add(5, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
        goal_selector.add(6, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(6, std::make_unique<RandomLookAroundGoal>(*this));
        target_selector.add(1, std::make_unique<HurtByTargetGoal>(*this));
        target_selector.add(2, std::make_unique<NearestAttackableTargetGoal>(*this, true));
        target_selector.add(3, std::make_unique<NearestAttackableTargetGoal>(*this, true, [](LivingEntity& e) { return e.type == entity_types::iron_golem; }));
        reassess_weapon_goal();
    }
    void reassess_weapon_goal() {  // AbstractSkeleton.reassessWeaponGoal
        if (bow_goal) goal_selector.remove(bow_goal), bow_goal = nullptr;
        if (melee_goal) goal_selector.remove(melee_goal), melee_goal = nullptr;
        if (main_hand().item == item_id("bow") || item_in_slot(ES_OFFHAND).item == item_id("bow"))
            bow_goal = goal_selector.add(4, std::make_unique<BowGoal>(*this, 1.0, difficulty() == 3 ? 20 : 40, 15.0f));
        else
            melee_goal = goal_selector.add(4, std::make_unique<SkelMelee>(*this));
    }
    void ai_step() override {
        if (using_item) ticks_using++;
        Monster::ai_step();
    }
    void perform_ranged_attack(LivingEntity& t, float power) {  // AbstractSkeleton.performRangedAttack
        auto e = create_entity(entity_types::arrow);
        auto* a = static_cast<Arrow*>(e.get());
        a->owner = id;
        a->snap_to(pos.x, eye_y() - 0.1f, pos.z, 0, 0);
        a->base_damage = power * 2.0f + rand_triangle(difficulty() * 0.11, 0.57425);  // setBaseDamageFromMob
        double xd = t.pos.x - pos.x, yd = t.y_at(0.3333333333333333) - a->pos.y, zd = t.pos.z - pos.z;
        double dist = std::sqrt(xd * xd + zd * zd);
        a->shoot(xd, yd + dist * 0.2f, zd, 1.6f, float(14 - difficulty() * 4));
        add_entity(std::move(e));
        play_sound("minecraft:entity.skeleton.shoot", 1.0f, 1.0f / (rand_float() * 0.4f + 0.8f));
    }
    void populate_default_equipment(const DifficultyInstance& d) override {
        populate_default_armor(*this, d);
        set_item_slot(ES_MAINHAND, {item_id("bow"), 1, 0});
    }
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override {
        g = Monster::finalize_spawn(r, g);
        DifficultyInstance d = difficulty_at(block_x(), block_y(), block_z());
        populate_default_equipment(d);
        reassess_weapon_goal();
        can_pick_up_loot_ = rand_float() < 0.55f * d.special_multiplier();
        return g;
    }
    void load_extra(const Nbt& in) override { Monster::load_extra(in); reassess_weapon_goal(); }
};

// ---------- Spider ----------
struct WallClimberNavigation : GroundPathNavigation {
    bool has_path_to = false;
    int ptx = 0, pty = 0, ptz = 0;
    explicit WallClimberNavigation(Mob& m) : GroundPathNavigation(m) {}
    std::unique_ptr<Path> create_path(int x, int y, int z, int r) override {
        has_path_to = true, ptx = x, pty = y, ptz = z;
        return GroundPathNavigation::create_path(x, y, z, r);
    }
    std::unique_ptr<Path> create_path_to(Entity& e, int r) override {
        auto p = GroundPathNavigation::create_path(e.block_x(), e.block_y(), e.block_z(), r);
        if (p) has_path_to = true, ptx = e.block_x(), pty = e.block_y(), ptz = e.block_z();
        return p;
    }
    void tick() override {
        if (!is_done()) { GroundPathNavigation::tick(); return; }
        if (!has_path_to) return;
        Vec3 c{ptx + 0.5, double(pty), ptz + 0.5};
        double w = mob.width;
        if (c.dist_sqr(mob.pos) < w * w ||
            (mob.pos.y > pty && Vec3{ptx + 0.5, mob.pos.y, ptz + 0.5}.dist_sqr(mob.pos) < w * w)) has_path_to = false;
        else mob.move_control->set_wanted(ptx, pty, ptz, speed_mod);
    }
};
struct Spider : Monster {
    std::unique_ptr<PathNavigation> create_navigation() override { return std::make_unique<WallClimberNavigation>(*this); }
    bool climbing() const { return data.get(edata::Spider::FLAGS, 0) & 1; }
    void set_climbing(bool v) { int f = int(data.get(edata::Spider::FLAGS, 0)); data.set_byte(edata::Spider::FLAGS, v ? f | 1 : f & ~1); }
    std::string ambient_sound() const override { return "minecraft:entity.spider.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.spider.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.spider.death"; }
    struct SpiderAttackGoal : MeleeAttackGoal {
        explicit SpiderAttackGoal(Spider& s) : MeleeAttackGoal(s, 1.0, true) {}
        bool can_continue_to_use() override {
            if (mob.light_magic_value() >= 0.5f && rand_int(100) == 0) { mob.set_target(nullptr); return false; }
            return MeleeAttackGoal::can_continue_to_use();
        }
    };
    struct SpiderTargetGoal : NearestAttackableTargetGoal {
        SpiderTargetGoal(Spider& s, std::function<bool(LivingEntity&)> t) : NearestAttackableTargetGoal(s, true, std::move(t)) {}
        bool can_use() override { return mob.light_magic_value() >= 0.5f ? false : NearestAttackableTargetGoal::can_use(); }
    };
    void register_goals() override {
        goal_selector.add(1, std::make_unique<FloatGoal>(*this));
        goal_selector.add(2, std::make_unique<AvoidEntityGoal>(*this, [](LivingEntity& e) { return e.type == entity_types::armadillo; }, 6.0f, 1.0, 1.2));
        goal_selector.add(3, std::make_unique<LeapAtTargetGoal>(*this, 0.4f));
        goal_selector.add(4, std::make_unique<SpiderAttackGoal>(*this));
        goal_selector.add(5, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 0.8));
        goal_selector.add(6, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(6, std::make_unique<RandomLookAroundGoal>(*this));
        target_selector.add(1, std::make_unique<HurtByTargetGoal>(*this));
        target_selector.add(2, std::make_unique<SpiderTargetGoal>(*this, nullptr));
        target_selector.add(3, std::make_unique<SpiderTargetGoal>(*this, [](LivingEntity& e) { return e.type == entity_types::iron_golem; }));
    }
    void tick() override {
        Monster::tick();
        set_climbing(horizontal_collision);
    }
    void ai_step() override {
        Monster::ai_step();
        if (climbing()) {  // onClimbable: ladder physics (LivingEntity.handleOnClimbable)
            reset_fall_distance();
            if (horizontal_collision) vel.y = 0.2;
        }
    }
};

// ---------- Creeper ----------
struct Creeper : Monster {
    int old_swell = 0, swell = 0, max_swell = 30, explosion_radius = 3, dropped_skulls = 0;
    int swell_dir() const { return int(data.get(edata::Creeper::SWELL_DIR, -1)); }
    void set_swell_dir(int d) { data.set_int(edata::Creeper::SWELL_DIR, d); }
    bool powered() const { return data.get(edata::Creeper::IS_POWERED, 0); }
    bool ignited() const { return data.get(edata::Creeper::IS_IGNITED, 0); }
    void ignite() { data.set_bool(edata::Creeper::IS_IGNITED, true); }
    void thunder_hit(Entity* lightning) override {
        Entity::thunder_hit(lightning);
        data.set_bool(edata::Creeper::IS_POWERED, true);
    }
    void init() override { Monster::init(); data.set_int(edata::Creeper::SWELL_DIR, -1); }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.creeper.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.creeper.death"; }
    struct SwellGoal : Goal {
        Creeper& c;
        int target = 0;
        explicit SwellGoal(Creeper& cc) : c(cc) { flags = GF_MOVE; }
        bool can_use() override {
            LivingEntity* t = c.target();
            return c.swell_dir() > 0 || (t && !t->is_dead_or_dying() && c.dist_sqr(*t) < 9.0);
        }
        void start() override { c.navigation->stop(); LivingEntity* t = c.target(); target = t ? t->id : 0; }
        void stop() override { target = 0; }
        bool requires_update_every_tick() override { return true; }
        void tick() override {
            LivingEntity* t = living_of(entity_by_id(target));
            if (!t || t->is_dead_or_dying() || c.dist_sqr(*t) > 49.0 || !c.sensing.has_line_of_sight(c, *t)) c.set_swell_dir(-1);
            else c.set_swell_dir(1);
        }
    };
    void register_goals() override {
        goal_selector.add(1, std::make_unique<FloatGoal>(*this));
        goal_selector.add(2, std::make_unique<SwellGoal>(*this));
        goal_selector.add(3, std::make_unique<AvoidEntityGoal>(*this, [](LivingEntity& e) { return e.type == entity_types::ocelot; }, 6.0f, 1.0, 1.2));
        goal_selector.add(3, std::make_unique<AvoidEntityGoal>(*this, [](LivingEntity& e) { return e.type == entity_types::cat; }, 6.0f, 1.0, 1.2));
        goal_selector.add(4, std::make_unique<MeleeAttackGoal>(*this, 1.0, false));
        goal_selector.add(5, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 0.8));
        goal_selector.add(6, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(6, std::make_unique<RandomLookAroundGoal>(*this));
        target_selector.add(1, std::make_unique<NearestAttackableTargetGoal>(*this, true));
        target_selector.add(2, std::make_unique<HurtByTargetGoal>(*this));
    }
    int max_fall_distance() const override {  // Creeper.getMaxFallDistance
        return Monster::max_fall_distance() + (target_id ? swell : 0);
    }
    bool cause_fall_damage(double d, float m, const DamageSource& s) override {
        bool r = Monster::cause_fall_damage(d, m, s);
        swell = std::min(swell + int(d * 1.5), max_swell - 5);
        return r;
    }
    void set_target(LivingEntity* t) override { if (!t || t->type != entity_types::goat) Monster::set_target(t); }
    void tick() override {
        if (is_alive()) {
            old_swell = swell;
            if (ignited()) set_swell_dir(1);
            int dir = swell_dir();
            if (dir > 0 && swell == 0) play_sound("minecraft:entity.creeper.primed", 1.0f, 0.5f);
            swell = std::max(0, swell + dir);
            if (swell >= max_swell) {
                swell = max_swell;
                dead = true;  // explodeCreeper
                explode(this, pos.x, pos.y, pos.z, float(explosion_radius) * (powered() ? 2.0f : 1.0f), false, ExplosionInteraction::MOB);
                discard();
                return;
            }
        }
        Monster::tick();
    }
    int mob_interact(Player& p, int hand) override {
        ItemStack& s = held_stack(p, hand);
        if (item_in_tag(s.item, "creeper_igniters")) {
            ::play_sound(s.item == item_id("fire_charge") ? "minecraft:item.firecharge.use" : "minecraft:item.flintandsteel.use",
                         sound_source(), pos.x, pos.y, pos.z, 1.0f, rand_float() * 0.4f + 0.8f);
            ignite();
            if (p.gamemode != 1) {
                int maxd = item_info[size_t(s.item)].max_damage;
                if (maxd <= 0) { if (--s.count <= 0) s = {}; }
                else if (++s.damage >= maxd) s = {};
            }
            return IR_SUCCESS;
        }
        return Monster::mob_interact(p, hand);
    }
    void save_extra(Nbt& o) const override {
        Monster::save_extra(o);
        o.set("powered", Nbt::num(Nbt::Byte, powered()));
        o.set("Fuse", Nbt::num(Nbt::Short, max_swell));
        o.set("ExplosionRadius", Nbt::num(Nbt::Byte, explosion_radius));
        o.set("ignited", Nbt::num(Nbt::Byte, ignited()));
    }
    void load_extra(const Nbt& in) override {
        Monster::load_extra(in);
        if (nbt_num(in, "powered", 0)) data.set_bool(edata::Creeper::IS_POWERED, true);
        max_swell = int(nbt_num(in, "Fuse", 30));
        explosion_radius = int(nbt_num(in, "ExplosionRadius", 3));
        if (nbt_num(in, "ignited", 0)) ignite();
    }
};
