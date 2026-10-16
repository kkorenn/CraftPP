#pragma once
// Shared pieces of the mob roster: sounds by type id, TamableAnimal (+ owner goals), simple swimming and
// flying movement for mobs whose vanilla navigation (WaterBound / Flying) the framework doesn't have yet.
#include "../mobs.hpp"

inline std::string entity_sound(const Entity& e, const char* what) { return "minecraft:entity." + e.info().id + "." + what; }
inline Player* player_by_uuid(int64_t hi, int64_t lo) {
    for (auto& p : players)
        if (p->uuid_hi == hi && p->uuid_lo == lo) return p.get();
    return nullptr;
}
inline Nbt uuid_nbt(int64_t hi, int64_t lo) {
    Nbt u;
    u.t = Nbt::IntArray;
    u.arr = {int64_t(int32_t(hi >> 32)), int64_t(int32_t(hi)), int64_t(int32_t(lo >> 32)), int64_t(int32_t(lo))};
    return u;
}
inline bool uuid_from_nbt(const Nbt* u, int64_t& hi, int64_t& lo) {
    if (!u || u->t != Nbt::IntArray || u->arr.size() != 4) return false;
    hi = int64_t(uint64_t(uint32_t(u->arr[0])) << 32 | uint32_t(u->arr[1]));
    lo = int64_t(uint64_t(uint32_t(u->arr[2])) << 32 | uint32_t(u->arr[3]));
    return true;
}
inline int held_item(Player& p, int hand) { return held_stack(p, hand).item; }
inline bool sneaking(const Player& p) { return p.g.sneaking; }

// ---------- TamableAnimal ----------
struct TamableAnimal : Animal {
    bool ordered_to_sit = false;
    int64_t owner_hi = 0, owner_lo = 0;

    uint8_t tflags() const { return uint8_t(data.get(edata::TamableAnimal::FLAGS, 0)); }
    bool is_tame() const { return tflags() & 4; }
    bool in_sitting_pose() const { return tflags() & 1; }
    void set_flag(int bit, bool v) { data.set_byte(edata::TamableAnimal::FLAGS, v ? tflags() | bit : tflags() & ~bit); }
    void set_in_sitting_pose(bool v) { set_flag(1, v); }
    virtual void apply_taming_side_effects() {}
    void set_tame(bool t, bool side_effects) {
        set_flag(4, t);
        if (side_effects) apply_taming_side_effects();
    }
    void set_owner(int64_t hi, int64_t lo) {
        owner_hi = hi, owner_lo = lo;
        Writer w;
        w.boolean(hi || lo);
        if (hi || lo) w.i64(hi).i64(lo);
        data.set_raw(edata::TamableAnimal::OWNERUUID, edser::OPTIONAL_LIVING_ENTITY_REFERENCE, w.b);
    }
    bool is_owned_by(const Player& p) const { return (owner_hi || owner_lo) && p.uuid_hi == owner_hi && p.uuid_lo == owner_lo; }
    PlayerEntity* owner() const {
        Player* p = owner_hi || owner_lo ? player_by_uuid(owner_hi, owner_lo) : nullptr;
        return p ? player_entity(*p) : nullptr;
    }
    void tame(Player& p) {  // TamableAnimal.tame
        set_tame(true, true);
        set_owner(p.uuid_hi, p.uuid_lo);
        set_persistence_required();
    }
    void set_ordered_to_sit(bool v) { ordered_to_sit = v; }
    bool is_allied_to(Entity& e) override {
        if (is_tame()) {
            if (Player* p = e.as_player()) return is_owned_by(*p);
            if (auto* t = dynamic_cast<TamableAnimal*>(&e)) return t->is_tame() && t->owner_hi == owner_hi && t->owner_lo == owner_lo;
        }
        return Animal::is_allied_to(e);
    }
    bool can_attack(LivingEntity& t) override { return !is_allied_to(t) && Animal::can_attack(t); }
    // TamableAnimal.wantsToAttack: never the owner, other pets of the owner, or (wolves) creepers/ghasts.
    virtual bool wants_to_attack(LivingEntity& t, LivingEntity&) { return !is_allied_to(t); }
    void die(const DamageSource& s) override {
        // ponytail: vanilla tells the owner "<pet> was slain by ..." when showDeathMessages; not ported
        Animal::die(s);
    }
    void save_extra(Nbt& o) const override {
        Animal::save_extra(o);
        if (owner_hi || owner_lo) o.set("Owner", uuid_nbt(owner_hi, owner_lo));
        o.set("Sitting", Nbt::num(Nbt::Byte, ordered_to_sit));
    }
    void load_extra(const Nbt& in) override {
        Animal::load_extra(in);
        int64_t hi, lo;
        if (uuid_from_nbt(in.get("Owner"), hi, lo)) {
            set_owner(hi, lo);
            set_tame(true, false);
        } else {
            set_tame(false, true);
        }
        ordered_to_sit = nbt_num(in, "Sitting", 0) != 0;
        set_in_sitting_pose(ordered_to_sit);
    }
};

struct SitWhenOrderedToSitGoal : Goal {  // SitWhenOrderedToSitGoal
    TamableAnimal& mob;
    explicit SitWhenOrderedToSitGoal(TamableAnimal& m) : mob(m) { flags = GF_JUMP | GF_MOVE; }
    bool can_continue_to_use() override { return mob.ordered_to_sit; }
    bool can_use() override {
        if (!mob.is_tame() || mob.is_in_water() || !mob.on_ground) return false;
        PlayerEntity* o = mob.owner();
        if (!o) return true;
        return (mob.dist_sqr(*o) >= 144.0 || !o->last_hurt_by()) && mob.ordered_to_sit;
    }
    void start() override { mob.navigation->stop(); mob.set_in_sitting_pose(true); }
    void stop() override { mob.set_in_sitting_pose(false); }
};

// FollowOwnerGoal (+ TamableAnimal.tryToTeleportToOwner when more than 12 blocks away).
struct FollowOwnerGoal : Goal {
    TamableAnimal& mob;
    double speed;
    float start_dist, stop_dist;
    int recalc = 0;
    FollowOwnerGoal(TamableAnimal& m, double s, float start, float stop) : mob(m), speed(s), start_dist(start), stop_dist(stop) {
        flags = GF_MOVE | GF_LOOK;
    }
    bool unable_to_move() const { return mob.ordered_to_sit; }  // ponytail: pets never ride vehicles here
    bool can_use() override {
        PlayerEntity* o = mob.owner();
        if (!o || o->is_spectator() || unable_to_move()) return false;
        return mob.dist_sqr(*o) >= double(start_dist) * start_dist;
    }
    bool can_continue_to_use() override {
        PlayerEntity* o = mob.owner();
        if (!o || mob.navigation->is_done() || unable_to_move()) return false;
        return mob.dist_sqr(*o) > double(stop_dist) * stop_dist;
    }
    void start() override { recalc = 0; }
    void stop() override { mob.navigation->stop(); }
    bool can_teleport_to(int x, int y, int z) const {
        if (!sflag(block_at(x, y - 1, z), SF_STURDY_UP)) return false;
        AABB b = mob.bb.move(x + 0.5 - mob.pos.x, y - mob.pos.y, z + 0.5 - mob.pos.z);
        return no_block_collision(b) && !contains_any_liquid(b);
    }
    void try_teleport(PlayerEntity& o) {
        int ox = o.block_x(), oy = o.block_y(), oz = o.block_z();
        for (int i = 0; i < 10; i++) {
            int dx = rand_between(-3, 3), dy = rand_between(-1, 1), dz = rand_between(-3, 3);
            if (std::abs(dx) < 2 && std::abs(dz) < 2) continue;
            if (!can_teleport_to(ox + dx, oy + dy, oz + dz)) continue;
            mob.snap_to(ox + dx + 0.5, oy + dy, oz + dz + 0.5, mob.yaw, mob.pitch);
            mob.teleport_to(ox + dx + 0.5, oy + dy, oz + dz + 0.5);
            mob.navigation->stop();
            return;
        }
    }
    void tick() override {
        PlayerEntity* o = mob.owner();
        if (!o) return;
        bool far = mob.dist_sqr(*o) >= 144.0;
        if (!far) mob.look_control->set_look_at(*o, 10.0f, float(mob.max_head_x_rot()));
        if (--recalc > 0) return;
        recalc = adjusted_tick_delay(10);
        if (far) try_teleport(*o);
        else mob.navigation->move_to(*o, speed);
    }
};

struct OwnerHurtByTargetGoal : TargetGoal {  // the owner was hurt by X: attack X
    TamableAnimal& tam;
    int timestamp = 0;
    explicit OwnerHurtByTargetGoal(TamableAnimal& m) : TargetGoal(m, false), tam(m) { flags = GF_TARGET; }
    bool can_use() override {
        if (!tam.is_tame() || tam.ordered_to_sit) return false;
        PlayerEntity* o = tam.owner();
        if (!o) return false;
        LivingEntity* a = o->last_hurt_by();
        return a && o->last_hurt_by_mob_time != timestamp && can_attack_target(a, TargetingConditions::for_combat()) &&
               tam.wants_to_attack(*a, *o);
    }
    void start() override {
        PlayerEntity* o = tam.owner();
        mob.set_target(o->last_hurt_by());
        timestamp = o->last_hurt_by_mob_time;
        TargetGoal::start();
    }
};
struct OwnerHurtTargetGoal : TargetGoal {  // the owner attacked X: attack X
    TamableAnimal& tam;
    int timestamp = 0;
    explicit OwnerHurtTargetGoal(TamableAnimal& m) : TargetGoal(m, false), tam(m) { flags = GF_TARGET; }
    bool can_use() override {
        if (!tam.is_tame() || tam.ordered_to_sit) return false;
        PlayerEntity* o = tam.owner();
        if (!o) return false;
        LivingEntity* t = living_of(entity_by_id(o->last_hurt_mob));
        return t && o->last_hurt_mob_time != timestamp && can_attack_target(t, TargetingConditions::for_combat()) &&
               tam.wants_to_attack(*t, *o);
    }
    void start() override {
        PlayerEntity* o = tam.owner();
        mob.set_target(living_of(entity_by_id(o->last_hurt_mob)));
        timestamp = o->last_hurt_mob_time;
        TargetGoal::start();
    }
};

// Simple animal with the AbstractCow-style goal set; spec is static per type (one table for all instances).
struct AnimalSpec {
    int type;
    const char* food;  // item tag ("" = doesn't breed)
    double panic, tempt, follow;
    float volume = 1.0f;
    int fights = 0;  // 1 = fights back (polar bear, panda), 2 = also hunts players (hoglin)
};
inline const AnimalSpec& animal_spec(int type);  // roster.cpp table
struct SimpleAnimal : Animal {
    const AnimalSpec* spec = nullptr;
    void init() override { spec = &animal_spec(type); Animal::init(); }
    bool is_food(const ItemStack& s) const override { return spec && *spec->food && item_in_tag(s.item, spec->food); }
    std::string ambient_sound() const override { return entity_sound(*this, "ambient"); }
    std::string hurt_sound(const DamageSource&) const override { return entity_sound(*this, "hurt"); }
    std::string death_sound() const override { return entity_sound(*this, "death"); }
    float sound_volume() const override { return spec ? spec->volume : 1.0f; }
    void register_goals() override {
        const char* food = spec->food;
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        goal_selector.add(1, std::make_unique<PanicGoal>(*this, spec->panic));
        goal_selector.add(2, std::make_unique<BreedGoal>(*this, 1.0));
        goal_selector.add(3, std::make_unique<TemptGoal>(*this, spec->tempt, [food](const ItemStack& s) { return item_in_tag(s.item, food); }, false));
        goal_selector.add(4, std::make_unique<FollowParentGoal>(*this, spec->follow));
        goal_selector.add(5, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
        goal_selector.add(6, std::make_unique<LookAtPlayerGoal>(*this, 6.0f));
        goal_selector.add(7, std::make_unique<RandomLookAroundGoal>(*this));
        if (spec->fights) {
            goal_selector.add(1, std::make_unique<MeleeAttackGoal>(*this, 1.25, true));
            target_selector.add(1, std::make_unique<HurtByTargetGoal>(*this));
        }
        if (spec->fights == 2) target_selector.add(2, std::make_unique<NearestAttackableTargetGoal>(*this, true));
    }
    std::unique_ptr<Entity> breed_offspring(AgeableMob&) override { return *spec->food ? create_entity(type) : nullptr; }
};

// ---------- swimming (WaterAnimal / AbstractFish approximation) ----------
// ponytail: no WaterBoundPathNavigation / SmoothSwimmingMoveControl yet: swimmers steer straight at a random
// water block nearby (RandomSwimmingGoal targets) with the vanilla swim speeds, and flop on land.
struct SwimMob : PathfinderMob {
    Vec3 swim_target{0, 0, 0};
    bool has_swim_target = false;
    int air = 300;
    virtual double swim_speed() const { return attr(attr::movement_speed); }
    virtual bool breathes_air() const { return false; }  // dolphins / turtles (not here) surface for air
    bool can_breathe_underwater() const override { return true; }
    double default_gravity() const override { return is_in_water() ? 0.0 : LivingEntity::default_gravity(); }
    std::string ambient_sound() const override { return entity_sound(*this, "ambient"); }
    std::string hurt_sound(const DamageSource&) const override { return entity_sound(*this, "hurt"); }
    std::string death_sound() const override { return entity_sound(*this, "death"); }
    bool remove_when_far_away(double) override { return !is_persistence_required(); }
    void pick_target() {
        for (int i = 0; i < 10; i++) {
            int x = block_x() + rand_between(-10, 10), y = block_y() + rand_between(-7, 7), z = block_z() + rand_between(-10, 10);
            if (fluid_type(block_at(x, y, z)) == FLUID_WATER) {
                swim_target = {x + 0.5, y + 0.5, z + 0.5};
                has_swim_target = true;
                return;
            }
        }
    }
    void ai_step() override {
        PathfinderMob::ai_step();
        if (no_ai() || !is_alive()) return;
        if (is_in_water()) {
            air = 300;
            if (!has_swim_target || rand_int(80) == 0 || pos.dist_sqr(swim_target) < 1.0) pick_target();
            if (has_swim_target) {
                Vec3 d = swim_target - pos;
                double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                if (len > 1e-4) {
                    double s = swim_speed() * 0.1;
                    vel = vel + Vec3{d.x / len * s, d.y / len * s, d.z / len * s};
                    yaw = float(std::atan2(d.z, d.x) * 180.0 / M_PI) - 90.0f;
                    y_body_rot = y_head_rot = yaw;
                }
            }
            vel = vel * 0.9;
        } else {
            has_swim_target = false;
            if (on_ground && !breathes_air()) {  // AbstractFish.aiStep flop
                vel = vel + Vec3{(rand_float() * 2.0f - 1.0f) * 0.05f, 0.4f, (rand_float() * 2.0f - 1.0f) * 0.05f};
                on_ground = false;
                play_sound(entity_sound(*this, "flop"), sound_volume(), voice_pitch());
            }
            if (!breathes_air() && --air <= -20) {  // WaterAnimal.handleAirSupply
                air = 0;
                hurt_server(DamageSource::of("dry_out"), 2.0f);
            }
        }
    }
};

// ---------- flying (FlyingMob / FlyingMoveControl approximation) ----------
// ponytail: no FlyingPathNavigation: flyers drift to random points around home (RandomFloatAroundGoal style).
struct FlyMob : PathfinderMob {
    Vec3 fly_target{0, 0, 0};
    bool has_fly_target = false;
    double default_gravity() const override { return 0.0; }
    bool cause_fall_damage(double, float, const DamageSource&) override { return false; }
    std::string ambient_sound() const override { return entity_sound(*this, "ambient"); }
    std::string hurt_sound(const DamageSource&) const override { return entity_sound(*this, "hurt"); }
    std::string death_sound() const override { return entity_sound(*this, "death"); }
    virtual double fly_speed() const { return attr(attr::flying_speed) > 0 ? attr(attr::flying_speed) : 0.1; }
    virtual int wander_range() const { return 8; }
    void ai_step() override {
        PathfinderMob::ai_step();
        if (no_ai() || !is_alive()) return;
        if (LivingEntity* t = target()) fly_target = t->pos + Vec3{0, t->height * 0.5, 0}, has_fly_target = true;
        else if (!has_fly_target || rand_int(60) == 0 || pos.dist_sqr(fly_target) < 1.0) {
            int r = wander_range();
            Vec3 c{pos.x + rand_between(-r, r), pos.y + rand_between(-r / 2, r / 2), pos.z + rand_between(-r, r)};
            if (!has_collision(block_at(mth::floor(c.x), mth::floor(c.y), mth::floor(c.z)))) fly_target = c, has_fly_target = true;
        }
        if (!has_fly_target) return;
        Vec3 d = fly_target - pos;
        double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < 1e-4) return;
        double s = fly_speed() * 0.5;
        vel = vel * 0.9 + Vec3{d.x / len * s * 0.1, d.y / len * s * 0.1, d.z / len * s * 0.1};
        yaw = float(std::atan2(d.z, d.x) * 180.0 / M_PI) - 90.0f;
        y_body_rot = y_head_rot = yaw;
    }
};
