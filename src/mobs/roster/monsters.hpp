#pragma once
// Hostile / neutral roster: zombie and skeleton variants, cave spider, slimes (AbstractCubeMob), enderman,
// zombified piglin, plus a table-driven melee monster for the rest.
#include "villager.hpp"

inline const char* type_sound_prefix(const Entity& e) { return e.info().id.c_str(); }

// ---------- zombie family ----------
struct Husk : Zombie {  // never burns in daylight
    std::string ambient_sound() const override { return "minecraft:entity.husk.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.husk.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.husk.death"; }
};
struct Drowned : Zombie {  // ponytail: no trident throwing / swimming navigation yet; fights like a zombie
    bool can_breathe_underwater() const override { return true; }
    std::string ambient_sound() const override { return is_in_water() ? "minecraft:entity.drowned.ambient_water" : "minecraft:entity.drowned.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return is_in_water() ? "minecraft:entity.drowned.hurt_water" : "minecraft:entity.drowned.hurt"; }
    std::string death_sound() const override { return is_in_water() ? "minecraft:entity.drowned.death_water" : "minecraft:entity.drowned.death"; }
};
struct ZombieVillager : Zombie {
    int villager_type = 0, profession = -1, level = 1, xp = 0;
    int conversion_time = 0;
    int64_t converter_hi = 0, converter_lo = 0;
    Nbt offers, gossips;
    bool has_offers = false;
    void sync_data() {
        Writer w;
        write_villager_data(w, villager_type, profession < 0 ? profession_id("none") : profession, level);
        data.set_raw(edata::ZombieVillager::VILLAGER_DATA, edser::VILLAGER_DATA, w.b);
    }
    void init() override { Zombie::init(); sync_data(); }
    std::string ambient_sound() const override { return "minecraft:entity.zombie_villager.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.zombie_villager.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.zombie_villager.death"; }
    bool converting() const { return data.get(edata::ZombieVillager::CONVERTING, 0); }
    // ZombieVillager.mobInteract: a golden apple starts the cure.
    // ponytail: vanilla also requires the Weakness effect; there are no mob effects yet, so the apple alone cures.
    int mob_interact(Player& p, int hand) override {
        ItemStack& s = held_stack(p, hand);
        if (s.item == item_by_name["minecraft:golden_apple"] && !converting()) {
            use_player_item(p, hand);
            converter_hi = p.uuid_hi, converter_lo = p.uuid_lo;
            conversion_time = rand_int(2401) + 3600;
            data.set_bool(edata::ZombieVillager::CONVERTING, true);
            broadcast_event(16);
            return IR_SUCCESS;
        }
        return Zombie::mob_interact(p, hand);
    }
    bool remove_when_far_away(double d) override { return !converting() && xp == 0 && Zombie::remove_when_far_away(d); }
    void tick() override {
        Zombie::tick();
        if (removed || !converting() || !is_alive()) return;
        conversion_time -= 1;  // ponytail: getConversionProgress iron bar / bed speed-up not ported
        if (conversion_time <= 0) finish_conversion();
    }
    void finish_conversion() {  // ZombieVillager.finishConversion
        auto e = create_entity(entity_types::villager);
        auto* v = static_cast<Villager*>(e.get());
        v->snap_to(pos.x, pos.y, pos.z, yaw, pitch);
        v->m.villager_type = villager_type, v->m.profession = profession < 0 ? profession_id("none") : profession;
        v->m.level = level, v->m.xp = xp;
        if (has_offers) load_offers(&offers, v->m.offers), v->offers_ready = true;
        load_gossips(&gossips, v->m.gossips);
        if (converter_hi || converter_lo) {  // ReputationEventType.ZOMBIE_VILLAGER_CURED
            v->m.gossips.add(converter_hi, converter_lo, GossipType::major_positive, 20);
            v->m.gossips.add(converter_hi, converter_lo, GossipType::minor_positive, 25);
        }
        v->sync_villager_data();
        v->set_persistence_required();
        if (is_baby()) v->set_age(-24000);
        play_sound("minecraft:entity.zombie_villager.converted", 1.0f, 1.0f);
        remove(Removal::DISCARDED);
        add_entity(std::move(e));
    }
    void save_extra(Nbt& o) const override {
        Zombie::save_extra(o);
        Nbt vd = Nbt::compound();
        vd.set("type", Nbt::str("minecraft:" + villager_types[size_t(villager_type)]));
        vd.set("profession", Nbt::str("minecraft:" + (profession < 0 ? std::string("none") : professions[size_t(profession)].name)));
        vd.set("level", Nbt::num(Nbt::Int, level));
        o.set("VillagerData", std::move(vd));
        o.set("Xp", Nbt::num(Nbt::Int, xp));
        if (has_offers) o.set("Offers", offers);
        o.set("Gossips", gossips);
        o.set("ConversionTime", Nbt::num(Nbt::Int, converting() ? conversion_time : -1));
        if (converter_hi || converter_lo) o.set("ConversionPlayer", uuid_nbt(converter_hi, converter_lo));
    }
    void load_extra(const Nbt& in) override {
        Zombie::load_extra(in);
        if (const Nbt* vd = in.get("VillagerData")) {
            auto strip = [](const Nbt* s) { return s && s->t == Nbt::String ? (s->s.rfind("minecraft:", 0) == 0 ? s->s.substr(10) : s->s) : ""; };
            std::string t = strip(vd->get("type")), p = strip(vd->get("profession"));
            for (size_t i = 0; i < villager_types.size(); i++)
                if (villager_types[i] == t) villager_type = int(i);
            profession = profession_id(p);
            level = std::clamp(int(nbt_num(*vd, "level", 1)), 1, 5);
        }
        xp = int(nbt_num(in, "Xp", 0));
        if (const Nbt* o = in.get("Offers")) offers = *o, has_offers = true;
        if (const Nbt* g = in.get("Gossips")) gossips = *g;
        int ct = int(nbt_num(in, "ConversionTime", -1));
        if (ct > -1) {
            conversion_time = ct;
            data.set_bool(edata::ZombieVillager::CONVERTING, true);
            uuid_from_nbt(in.get("ConversionPlayer"), converter_hi, converter_lo);
        }
        sync_data();
    }
};

// Zombie.killedEntity: villagers killed by zombies on normal (50%) / hard (100%) become zombie villagers.
inline void Villager::die(const DamageSource& s) {
    int d = difficulty();
    if (s.causing && zombie_type(s.causing->type) && s.causing->type != entity_types::zombified_piglin &&
        s.causing->type != entity_types::zoglin && (d == 3 || (d == 2 && rand_bool()))) {
        auto e = create_entity(entity_types::zombie_villager);
        auto* z = static_cast<ZombieVillager*>(e.get());
        z->snap_to(pos.x, pos.y, pos.z, yaw, pitch);
        z->villager_type = m.villager_type, z->profession = m.profession, z->level = m.level, z->xp = m.xp;
        if (offers_ready) z->offers = save_offers(m.offers), z->has_offers = true;
        z->gossips = save_gossips(m.gossips);
        z->sync_data();
        z->finalize_spawn(SpawnReason::CONVERSION, nullptr);
        if (is_baby()) z->set_baby(true);
        z->set_persistence_required();
        ::play_sound("minecraft:entity.zombie.infect", SRC_HOSTILE, pos.x, pos.y, pos.z, 2.0f, 1.0f);
        stop_trading();
        remove(Removal::DISCARDED);
        add_entity(std::move(e));
        return;
    }
    AbstractVillager::die(s);
}

// ---------- skeleton family ----------
struct SkeletonVariant : Skeleton {
    std::string ambient_sound() const override { return entity_sound(*this, "ambient"); }
    std::string hurt_sound(const DamageSource&) const override { return entity_sound(*this, "hurt"); }
    std::string death_sound() const override { return entity_sound(*this, "death"); }
};
struct WitherSkeleton : SkeletonVariant {  // stone sword melee, no bow
    void populate_default_equipment(const DifficultyInstance&) override { set_item_slot(ES_MAINHAND, {item_by_name["minecraft:stone_sword"], 1, 0}); }
    bool do_hurt_target(Entity& t) override {
        // ponytail: vanilla also applies Wither II for 10 s (no mob effects yet)
        return SkeletonVariant::do_hurt_target(t);
    }
};
struct CaveSpider : Spider {  // ponytail: poison on hit needs mob effects
    std::string ambient_sound() const override { return "minecraft:entity.spider.ambient"; }
};

// ---------- AbstractCubeMob: slime, magma cube ----------
struct CubeMob : AgeableMob {
    struct CubeMoveControl : MoveControl {
        float yrot = 0;
        int jump_delay = 0;
        bool aggressive = false;
        explicit CubeMoveControl(Mob& m) : MoveControl(m) { yrot = 180.0f * m.yaw / float(M_PI); }
        void set_direction(float y, bool aggr) { yrot = y, aggressive = aggr; }
        void set_wanted_movement(double s) { speed_mod = s, op = MOVE_TO; }
        void tick() override;
    };
    int size() const { return int(data.get(edata::AbstractCubeMob::ID_SIZE, 1)); }
    bool tiny() const { return size() <= 1; }
    bool is_baby() const override { return false; }
    virtual bool can_deal_damage() const { return !tiny() && !no_ai(); }
    virtual int jump_delay() { return rand_int(20) + 10; }
    virtual const char* sound(const char* what) const = 0;
    void set_size_(int s, bool heal) {  // setSize
        s = std::clamp(s, 1, 127);
        data.set_int(edata::AbstractCubeMob::ID_SIZE, s);
        double x = pos.x, y = pos.y, z = pos.z;
        set_size(info().w * float(s), info().h * float(s), info().eye * float(s));
        set_pos(x, y, z);
        set_attr_base(attr::max_health, double(s * s));
        set_attr_base(attr::movement_speed, double(0.2f + 0.1f * float(s)));
        set_attr_base(attr::attack_damage, double(s));
        xp_reward = s;
        if (heal) set_health(max_health());
    }
    void refresh_dimensions() override { float s = float(size()); set_size(info().w * s, info().h * s, info().eye * s); }
    void init() override {
        AgeableMob::init();
        move_control = std::make_unique<CubeMoveControl>(*this);
        set_size_(1, true);
    }
    CubeMoveControl& cube_move() { return static_cast<CubeMoveControl&>(*move_control); }
    struct AttackGoal : Goal {
        CubeMob& c;
        int tired = 0;
        explicit AttackGoal(CubeMob& cc) : c(cc) { flags = GF_LOOK; }
        bool can_use() override { LivingEntity* t = c.target(); return t && c.can_attack(*t); }
        void start() override { tired = reduced_tick_delay(300); }
        bool can_continue_to_use() override { LivingEntity* t = c.target(); return t && c.can_attack(*t) && --tired > 0; }
        bool requires_update_every_tick() override { return true; }
        void tick() override {
            if (LivingEntity* t = c.target()) c.look_at(*t, 10.0f, 10.0f);
            c.cube_move().set_direction(c.yaw, c.can_deal_damage());
        }
    };
    struct FloatGoal_ : Goal {
        CubeMob& c;
        explicit FloatGoal_(CubeMob& cc) : c(cc) { flags = GF_JUMP | GF_MOVE; cc.navigation->set_can_float(true); }
        bool can_use() override { return c.is_in_water() || c.is_in_lava(); }
        bool requires_update_every_tick() override { return true; }
        void tick() override { if (rand_float() < 0.8f) c.jump_control->jump(); c.cube_move().set_wanted_movement(1.2); }
    };
    struct KeepJumpingGoal : Goal {
        CubeMob& c;
        explicit KeepJumpingGoal(CubeMob& cc) : c(cc) { flags = GF_JUMP | GF_MOVE; }
        bool can_use() override { return true; }
        void tick() override { c.cube_move().set_wanted_movement(1.0); }
    };
    struct RandomDirectionGoal : Goal {
        CubeMob& c;
        float chosen = 0;
        int next = 0;
        explicit RandomDirectionGoal(CubeMob& cc) : c(cc) { flags = GF_LOOK; }
        bool can_use() override { return !c.target() && (c.on_ground || c.is_in_water() || c.is_in_lava()); }
        void tick() override {
            if (--next <= 0) next = adjusted_tick_delay(40 + rand_int(60)), chosen = float(rand_int(360));
            c.cube_move().set_direction(chosen, false);
        }
    };
    void register_goals() override {
        goal_selector.add(1, std::make_unique<FloatGoal_>(*this));
        goal_selector.add(2, std::make_unique<AttackGoal>(*this));
        goal_selector.add(3, std::make_unique<RandomDirectionGoal>(*this));
        goal_selector.add(5, std::make_unique<KeepJumpingGoal>(*this));
        target_selector.add(1, std::make_unique<NearestAttackableTargetGoal>(*this, true, nullptr, 10, false, [this](LivingEntity& t) {
            return std::abs(t.pos.y - pos.y) <= 4.0; }));
        target_selector.add(3, std::make_unique<NearestAttackableTargetGoal>(*this, true, [](LivingEntity& e) { return e.type == entity_types::iron_golem; }));
    }
    int max_head_x_rot() const override { return 0; }
    float sound_volume() const override { return 0.4f * float(size()); }
    float cube_pitch() const { return ((rand_float() - rand_float()) * 0.2f + 1.0f) * (tiny() ? 1.4f : 0.8f); }
    int sound_source() const override { return SRC_HOSTILE; }
    std::string hurt_sound(const DamageSource&) const override { return sound("hurt"); }
    std::string death_sound() const override { return sound("death"); }
    void jump_from_ground() override { vel.y = jump_power(); needs_sync = true; }
    void deal_damage(LivingEntity& t) {  // AbstractCubeMob.dealDamage
        if (!is_alive() || !is_within_melee_attack_range(t) || !sensing.has_line_of_sight(*this, t)) return;
        if (t.hurt_server(DamageSource::of("mob_attack", this), float(attr(attr::attack_damage))))
            play_sound("minecraft:entity.slime.attack", 1.0f, (rand_float() - rand_float()) * 0.2f + 1.0f);
    }
    void player_touch(Player& p) override {
        if (!can_deal_damage() || p.gamemode == 1 || p.gamemode == 3) return;
        if (PlayerEntity* pe = player_entity(p)) deal_damage(*pe);
    }
    void remove(Removal r) override {  // split on death
        int s = size();
        if (s > 1 && is_dead_or_dying() && r == Removal::KILLED) {
            float off = width / 2.0f;
            int half = s / 2, n = 2 + rand_int(3);
            for (int i = 0; i < n; i++) {
                float xd = (float(i % 2) - 0.5f) * off, zd = (float(i / 2) - 0.5f) * off;
                auto e = create_entity(type);
                auto* c = static_cast<CubeMob*>(e.get());
                c->snap_to(pos.x + xd, pos.y + 0.5, pos.z + zd, rand_float() * 360.0f, 0.0f);
                c->set_size_(half, true);
                if (persistence_required) c->set_persistence_required();
                add_entity(std::move(e));
            }
        }
        AgeableMob::remove(r);
    }
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override {
        g = AgeableMob::finalize_spawn(r, g ? g : new AgeableMobGroupData(false));
        int scale = rand_int(3);
        if (scale < 2 && rand_float() < 0.5f * difficulty_at(block_x(), block_y(), block_z()).special_multiplier()) scale++;
        set_size_(1 << scale, true);
        return g;
    }
    std::unique_ptr<Entity> breed_offspring(AgeableMob&) override { return nullptr; }
    void save_extra(Nbt& o) const override { AgeableMob::save_extra(o); o.set("Size", Nbt::num(Nbt::Int, size() - 1)); }
    void load_extra(const Nbt& in) override { AgeableMob::load_extra(in); set_size_(int(nbt_num(in, "Size", 0)) + 1, false); }
};
inline void CubeMob::CubeMoveControl::tick() {
    auto& c = static_cast<CubeMob&>(mob);
    c.yaw = rotlerp(c.yaw, yrot, 90.0f);
    c.y_head_rot = c.y_body_rot = c.yaw;
    if (op != MOVE_TO) { c.set_zza(0.0f); return; }
    op = WAIT;
    float sp = float(speed_mod * c.attr(attr::movement_speed));
    if (c.on_ground) {
        c.set_speed(sp);
        if (jump_delay-- <= 0) {
            jump_delay = c.jump_delay();
            if (aggressive) jump_delay /= 3;
            c.jump_control->jump();
            c.play_sound(c.sound("jump"), c.sound_volume(), c.cube_pitch());
        } else {
            c.xxa = c.zza = 0;
            c.set_speed(0);
        }
    } else {
        c.set_speed(sp);
    }
}
struct Slime : CubeMob {
    const char* sound(const char* what) const override {
        static thread_local std::string s;
        s = std::string("minecraft:entity.slime.") + what + (tiny() ? "_small" : "");
        return s.c_str();
    }
};
struct MagmaCube : CubeMob {
    bool can_deal_damage() const override { return !no_ai(); }
    int jump_delay() override { return CubeMob::jump_delay() * 4; }
    void jump_from_ground() override { vel.y = jump_power() + 0.1f * float(size()); needs_sync = true; }  // MagmaCube.getJumpPower
    const char* sound(const char* what) const override {
        static thread_local std::string s;
        s = std::string("minecraft:entity.magma_cube.") + what + (tiny() ? "_small" : "");
        return s.c_str();
    }
};

// ---------- Enderman (NeutralMob + teleporting) ----------
struct Enderman : Monster {
    std::string ambient_sound() const override { return target_id ? "minecraft:entity.enderman.scream" : "minecraft:entity.enderman.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.enderman.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.enderman.death"; }
    bool is_sensitive_to_water() const override { return true; }
    void register_goals() override {  // ponytail: stare detection (LookForStaringPlayerGoal) and block carrying not ported
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        goal_selector.add(2, std::make_unique<MeleeAttackGoal>(*this, 1.0, false));
        goal_selector.add(7, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0, 0.0f));
        goal_selector.add(8, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(8, std::make_unique<RandomLookAroundGoal>(*this));
        target_selector.add(2, std::make_unique<HurtByTargetGoal>(*this));
        target_selector.add(4, std::make_unique<NearestAttackableTargetGoal>(*this, false, [](LivingEntity& e) { return e.type == entity_types::endermite; }));
    }
    void set_target(LivingEntity* t) override {
        Monster::set_target(t);
        data.set_bool(edata::Enderman::CREEPY, target_id != 0);
    }
    bool teleport_random() {  // Enderman.teleport: random point within 32 blocks
        double x = pos.x + (rand_double() - 0.5) * 64.0, y = pos.y + double(rand_int(64) - 32), z = pos.z + (rand_double() - 0.5) * 64.0;
        int bx = mth::floor(x), by = mth::floor(y), bz = mth::floor(z);
        if (!chunk_loaded_at(bx, bz)) return false;
        while (by > MIN_Y && !sflag(block_at(bx, by - 1, bz), SF_SOLID)) by--;
        if (by <= MIN_Y || fluid_type(block_at(bx, by, bz)) != FLUID_NONE) return false;
        AABB b = bb.move(x - pos.x, by - pos.y, z - pos.z);
        if (!no_block_collision(b) || contains_any_liquid(b)) return false;
        ::play_sound("minecraft:entity.enderman.teleport", SRC_HOSTILE, pos.x, pos.y, pos.z, 1.0f, 1.0f);
        teleport_to(x, by, z);
        navigation->stop();
        play_sound("minecraft:entity.enderman.teleport", 1.0f, 1.0f);
        return true;
    }
    bool hurt_server(const DamageSource& s, float dmg) override {
        if (s.is(DT_IS_PROJECTILE)) {  // dodges projectiles
            for (int i = 0; i < 64; i++)
                if (teleport_random()) return false;
            return false;
        }
        bool r = Monster::hurt_server(s, dmg);
        if (r && !s.is(DT_BYPASSES_INVULNERABILITY) && rand_int(10) != 0) teleport_random();
        return r;
    }
    void custom_server_ai_step() override {
        Monster::custom_server_ai_step();
        if (is_in_water() || (weather.raining && can_see_sky(block_x(), block_y(), block_z()))) {
            if (tick_count % 20 == 0) hurt_server(DamageSource::of("drown"), 1.0f);
            teleport_random();
        }
        if (is_day() &&  !target() && light_magic_value() > 0.5f &&
            can_see_sky(block_x(), block_y(), block_z()) && rand_float() * 30.0f < (light_magic_value() - 0.4f) * 2.0f)
            teleport_random();
    }
};

// ---------- table-driven melee monsters (and neutral ones that only fight back) ----------
struct MonsterSpec {
    int type;
    double speed;     // MeleeAttackGoal speed modifier
    bool hostile;     // targets players on sight
    bool burns;       // burns in daylight (undead)
    const char* weapon = nullptr;  // main-hand item at spawn
};
inline const MonsterSpec& monster_spec(int type);  // roster.cpp table
struct SimpleMonster : Monster {
    const MonsterSpec* spec = nullptr;
    void init() override { spec = &monster_spec(type); Monster::init(); }
    std::string ambient_sound() const override { return entity_sound(*this, "ambient"); }
    std::string hurt_sound(const DamageSource&) const override { return entity_sound(*this, "hurt"); }
    std::string death_sound() const override { return entity_sound(*this, "death"); }
    void register_goals() override {
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        if (spec->burns) {
            goal_selector.add(2, std::make_unique<RestrictSunGoal>(*this));
            goal_selector.add(3, std::make_unique<FleeSunGoal>(*this, 1.0));
        }
        goal_selector.add(4, std::make_unique<MeleeAttackGoal>(*this, spec->speed, false));
        goal_selector.add(7, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
        goal_selector.add(8, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(8, std::make_unique<RandomLookAroundGoal>(*this));
        target_selector.add(1, std::make_unique<HurtByTargetGoal>(*this));
        if (spec->hostile) target_selector.add(2, std::make_unique<NearestAttackableTargetGoal>(*this, true));
    }
    void populate_default_equipment(const DifficultyInstance&) override {
        if (spec->weapon) set_item_slot(ES_MAINHAND, {item_id(spec->weapon), 1, 0});
    }
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override {
        g = Monster::finalize_spawn(r, g);
        populate_default_equipment(difficulty_at(block_x(), block_y(), block_z()));
        return g;
    }
    void ai_step() override {
        if (spec->burns && is_sun_burn_tick()) ignite_for_ticks(160);
        Monster::ai_step();
    }
};
struct ZombifiedPiglin : SimpleMonster {  // neutral: angers the whole group when hit (HurtByTarget alert others)
    void register_goals() override {
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        goal_selector.add(2, std::make_unique<MeleeAttackGoal>(*this, 1.0, false));
        goal_selector.add(7, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
        auto hurt = std::make_unique<HurtByTargetGoal>(*this);
        hurt->set_alert_others();
        target_selector.add(1, std::move(hurt));
    }
    std::string ambient_sound() const override { return target_id ? "minecraft:entity.zombified_piglin.angry" : "minecraft:entity.zombified_piglin.ambient"; }
};
