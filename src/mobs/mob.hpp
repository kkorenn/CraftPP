#pragma once
// Mob / PathfinderMob / AgeableMob / Animal / Monster, the goal system (Goal, GoalSelector), controls
// (MoveControl, LookControl, JumpControl, BodyRotationControl), Sensing and PathNavigation. Overview: mobs.hpp.
#include "ai/path.hpp"

struct Mob;

// ---------- goals (net.minecraft.world.entity.ai.goal) ----------
enum GoalFlag : uint8_t { GF_MOVE = 1, GF_LOOK = 2, GF_JUMP = 4, GF_TARGET = 8 };
struct Goal {
    uint8_t flags = 0;
    virtual ~Goal() = default;
    virtual bool can_use() = 0;
    virtual bool can_continue_to_use() { return can_use(); }
    virtual bool is_interruptable() { return true; }
    virtual void start() {}
    virtual void stop() {}
    virtual bool requires_update_every_tick() { return false; }
    virtual void tick() {}
    int adjusted_tick_delay(int t) { return requires_update_every_tick() ? t : reduced_tick_delay(t); }
    static int reduced_tick_delay(int t) { return (t + 1) / 2; }  // Mth.positiveCeilDiv(t, 2)
};
struct WrappedGoal {
    int prio;
    std::unique_ptr<Goal> goal;
    bool running = false;
    bool can_be_replaced_by(const WrappedGoal& o) const { return goal->is_interruptable() && o.prio < prio; }
    void start() { if (!running) { running = true; goal->start(); } }
    void stop() { if (running) { running = false; goal->stop(); } }
};
struct GoalSelector {  // GoalSelector: insertion-ordered goals, one goal per control flag
    std::vector<WrappedGoal> goals;
    WrappedGoal* locked[4] = {};
    uint8_t disabled = 0;
    template <class G> G* add(int prio, std::unique_ptr<G> g) {
        G* raw = g.get();
        goals.push_back({prio, std::move(g)});
        std::fill(std::begin(locked), std::end(locked), nullptr);  // vector may have moved: re-derive from running
        for (auto& w : goals)
            if (w.running)
                for (int f = 0; f < 4; f++) if (w.goal->flags >> f & 1) locked[f] = &w;
        return raw;
    }
    void remove(Goal* g) {
        for (auto& w : goals) if (w.goal.get() == g) w.stop();
        std::erase_if(goals, [&](auto& w) { return w.goal.get() == g; });
        std::fill(std::begin(locked), std::end(locked), nullptr);
        for (auto& w : goals)
            if (w.running)
                for (int f = 0; f < 4; f++) if (w.goal->flags >> f & 1) locked[f] = &w;
    }
    void tick() {
        for (auto& w : goals)
            if (w.running && ((w.goal->flags & disabled) || !w.goal->can_continue_to_use())) w.stop();
        for (auto& l : locked) if (l && !l->running) l = nullptr;
        for (auto& w : goals) {
            if (w.running || (w.goal->flags & disabled)) continue;
            bool ok = true;
            for (int f = 0; f < 4 && ok; f++)
                if ((w.goal->flags >> f & 1) && locked[f] && !locked[f]->can_be_replaced_by(w)) ok = false;
            if (!ok || !w.goal->can_use()) continue;
            for (int f = 0; f < 4; f++)
                if (w.goal->flags >> f & 1) {
                    if (locked[f]) locked[f]->stop();
                    locked[f] = &w;
                }
            w.start();
        }
        tick_running(true);
    }
    void tick_running(bool all) {
        for (auto& w : goals)
            if (w.running && (all || w.goal->requires_update_every_tick())) w.goal->tick();
    }
    void set_flag(uint8_t f, bool enabled) { disabled = enabled ? disabled & ~f : disabled | f; }
    template <class G> bool is_running() const {
        for (auto& w : goals) if (w.running && dynamic_cast<G*>(w.goal.get())) return true;
        return false;
    }
};

// ---------- controls ----------
inline float rotate_towards(float from, float to, float max) {  // Control.rotateTowards
    return from + std::clamp(mth::wrap_degrees(to - from), -max, max);
}
inline float rotate_if_necessary(float base, float target, float max) {  // Mth.rotateIfNecessary
    return target - std::clamp(mth::wrap_degrees(target - base), -max, max);
}
struct MoveControl {
    enum Op { WAIT, MOVE_TO, STRAFE, JUMPING };
    Mob& mob;
    double wx = 0, wy = 0, wz = 0, speed_mod = 0;
    float strafe_fwd = 0, strafe_right = 0;
    Op op = WAIT;
    explicit MoveControl(Mob& m) : mob(m) {}
    virtual ~MoveControl() = default;
    bool has_wanted() const { return op == MOVE_TO; }
    void set_wanted(double x, double y, double z, double sp) {
        wx = x, wy = y, wz = z, speed_mod = sp;
        if (op != JUMPING) op = MOVE_TO;
    }
    void strafe(float f, float r) { op = STRAFE, strafe_fwd = f, strafe_right = r, speed_mod = 0.25; }
    void set_wait() { op = WAIT; }
    static float rotlerp(float a, float b, float max) {
        float r = a + std::clamp(mth::wrap_degrees(b - a), -max, max);
        if (r < 0.0f) r += 360.0f;
        else if (r > 360.0f) r -= 360.0f;
        return r;
    }
    virtual void tick();
};
struct LookControl {
    Mob& mob;
    float y_max = 0, x_max = 0;
    int cooldown = 0;
    double wx = 0, wy = 0, wz = 0;
    explicit LookControl(Mob& m) : mob(m) {}
    virtual ~LookControl() = default;
    void set_look_at(double x, double y, double z, float ym, float xm) { wx = x, wy = y, wz = z, y_max = ym, x_max = xm, cooldown = 2; }
    void set_look_at(double x, double y, double z);
    void set_look_at(Entity& e) { set_look_at(e.pos.x, e.eye_y(), e.pos.z); }
    void set_look_at(Entity& e, float ym, float xm) { set_look_at(e.pos.x, e.eye_y(), e.pos.z, ym, xm); }
    bool is_looking_at_target() const { return cooldown > 0; }
    virtual bool reset_x_rot_on_tick() const { return true; }
    std::optional<float> x_rot_d() const;
    std::optional<float> y_rot_d() const;
    virtual void tick();
};
struct JumpControl {
    Mob& mob;
    bool jump_ = false;
    explicit JumpControl(Mob& m) : mob(m) {}
    virtual ~JumpControl() = default;
    void jump() { jump_ = true; }
    virtual void tick();
};
struct BodyRotationControl {
    Mob& mob;
    int head_stable = 0;
    float last_stable_head = 0;
    explicit BodyRotationControl(Mob& m) : mob(m) {}
    virtual ~BodyRotationControl() = default;
    virtual void client_tick();
};

// ---------- navigation ----------
struct PathNavigation {
    Mob& mob;
    std::unique_ptr<NodeEvaluator> eval;
    std::unique_ptr<PathFinder> finder;
    std::unique_ptr<Path> path;
    double speed_mod = 0;
    int tick_ = 0, last_stuck_check = 0;
    Vec3 last_stuck_pos{0, 0, 0};
    int64_t timeout_node = INT64_MIN;
    int64_t timeout_timer = 0, last_timeout_check = 0;
    double timeout_limit = 0;
    float max_dist_to_waypoint = 0.5f;
    bool delayed_recompute = false, stuck = false;
    int64_t last_recompute = 0;
    bool has_target = false;
    int tx = 0, ty = 0, tz = 0, reach = 0;
    float max_visited_mult = 1.0f, required_path_length = 16.0f;
    explicit PathNavigation(Mob& m) : mob(m) {}
    virtual ~PathNavigation() = default;
    void init_finder();
    float max_path_length() const;
    virtual bool can_update_path() = 0;
    virtual Vec3 temp_mob_pos() = 0;
    virtual bool can_navigate_ground() const = 0;
    virtual double ground_y(const Vec3& t);
    bool is_done() const { return !path || path->done(); }
    bool is_in_progress() const { return !is_done(); }
    void stop() { path.reset(); }
    void recompute();
    virtual std::unique_ptr<Path> create_path(int x, int y, int z, int reach_range);
    std::unique_ptr<Path> create_path_impl(const std::vector<std::array<int, 3>>& targets, int radius_off, bool above, int reach_range, float max_len);
    virtual std::unique_ptr<Path> create_path_to(Entity& e, int reach_range);
    bool move_to(double x, double y, double z, double sp) { return move_to(create_path(mth::floor(x), mth::floor(y), mth::floor(z), 1), sp); }
    bool move_to(double x, double y, double z, int reach_range, double sp) {
        return move_to(create_path(mth::floor(x), mth::floor(y), mth::floor(z), reach_range), sp);
    }
    bool move_to(Entity& e, double sp) { auto p = create_path_to(e, 1); return p && move_to(std::move(p), sp); }
    bool move_to(std::unique_ptr<Path> p, double sp);
    void set_speed_modifier(double s) { speed_mod = s; }
    virtual void tick();
    void follow_the_path();
    bool should_target_next_node(const Vec3& mp);
    void do_stuck_detection(const Vec3& mp);
    void reset_stuck_timeout() { timeout_node = INT64_MIN, timeout_timer = 0, timeout_limit = 0, stuck = false; }
    virtual void trim_path() {}
    virtual bool can_move_directly(const Vec3&, const Vec3&) { return false; }
    bool can_cut_corner(uint8_t t) const { return t != PT_FIRE_IN_NEIGHBOR && t != PT_DAMAGING_IN_NEIGHBOR && t != PT_WALKABLE_DOOR; }
    bool is_stable_destination(int x, int y, int z) const { return sflag(block_at(x, y - 1, z), SF_SOLID_RENDER); }
    void set_can_float(bool v) { eval->can_float = v; }
    bool can_float() const { return eval->can_float; }
    void set_can_open_doors(bool v) { eval->can_open_doors = v; }
    bool should_recompute_path(int x, int y, int z) const;
};
struct GroundPathNavigation : PathNavigation {
    bool avoid_sun = false, can_path_below_surface = false;
    explicit GroundPathNavigation(Mob& m) : PathNavigation(m) { eval = std::make_unique<WalkNodeEvaluator>(); init_finder(); }
    bool can_update_path() override;
    Vec3 temp_mob_pos() override;
    bool can_navigate_ground() const override { return true; }
    std::unique_ptr<Path> create_path(int x, int y, int z, int reach_range) override;
    std::unique_ptr<Path> create_path_to(Entity& e, int reach_range) override {
        return create_path(e.block_x(), e.block_y(), e.block_z(), reach_range);
    }
    int surface_y();
    void trim_path() override;
    void set_can_walk_over_fences(bool v) { eval->can_walk_over_fences = v; }
};

// Sensing: line-of-sight results cached for one tick.
struct Sensing {
    std::vector<int> seen, unseen;
    void tick() { seen.clear(), unseen.clear(); }
    bool has_line_of_sight(LivingEntity& self, Entity& t) {
        if (std::find(seen.begin(), seen.end(), t.id) != seen.end()) return true;
        if (std::find(unseen.begin(), unseen.end(), t.id) != unseen.end()) return false;
        bool r = self.has_line_of_sight(t);
        (r ? seen : unseen).push_back(t.id);
        return r;
    }
};

// TargetingConditions
struct TargetingConditions {
    bool combat = true, los = true, test_invisible = true;
    double range = -1;
    std::function<bool(LivingEntity&)> selector;
    static TargetingConditions for_combat() { return {}; }
    static TargetingConditions for_non_combat() { TargetingConditions t; t.combat = false; return t; }
    TargetingConditions& set_range(double r) { range = r; return *this; }
    TargetingConditions& ignore_los() { los = false; return *this; }
    TargetingConditions& ignore_invisibility() { test_invisible = false; return *this; }
    bool test(LivingEntity* targeter, LivingEntity& target) const;
};

enum class SpawnReason { NATURAL, CHUNK_GENERATION, SPAWNER, STRUCTURE, BREEDING, MOB_SUMMONED, JOCKEY, EVENT, CONVERSION,
                         REINFORCEMENT, TRIGGERED, BUCKET, SPAWN_ITEM_USE, COMMAND, DISPENSER, PATROL, TRIAL_SPAWNER, LOAD,
                         DIMENSION_TRAVEL };
struct SpawnGroupData { virtual ~SpawnGroupData() = default; };
// DifficultyInstance (local difficulty), computed from world difficulty, day time and moon phase.
struct DifficultyInstance {
    int base = 1;
    float effective = 0;
    float special_multiplier() const { return effective < 2.0f ? 0.0f : effective > 4.0f ? 1.0f : (effective - 2.0f) / 2.0f; }
    bool harder_than(float v) const { return effective > v; }
};
inline DifficultyInstance difficulty_at(int, int, int) {  // ServerLevel.getCurrentDifficultyAt (no inhabited time yet)
    DifficultyInstance d;
    d.base = difficulty();
    if (d.base == 0) return d;
    bool hard = d.base == 3;
    float scale = 0.75f;
    float g = std::clamp((float(clocks[0].total) + -72000.0f) / 1440000.0f, 0.0f, 1.0f) * 0.25f;
    scale += g;
    float c = 0.0f;  // ponytail: chunk inhabited time not tracked, moon phase brightness assumed 1.0
    c += std::clamp(1.0f * 0.25f, 0.0f, g);
    if (d.base == 1) c *= 0.5f;
    scale += c;
    d.effective = float(d.base) * scale;
    (void)hard;
    return d;
}

// ---------- Mob ----------
struct Mob : LivingEntity {
    GoalSelector goal_selector, target_selector;
    std::unique_ptr<MoveControl> move_control;
    std::unique_ptr<LookControl> look_control;
    std::unique_ptr<JumpControl> jump_control;
    std::unique_ptr<BodyRotationControl> body_control;
    std::unique_ptr<PathNavigation> navigation;
    Sensing sensing;
    int target_id = 0;
    int ambient_sound_time = 0, xp_reward = 0;
    bool persistence_required = false, can_pick_up_loot_ = false, initialized = false;
    std::array<float, 8> drop_chances{0.085f, 0.085f, 0.085f, 0.085f, 0.085f, 0.085f, 0.085f, 0.085f};
    std::vector<std::pair<uint8_t, float>> malus;  // setPathfindingMalus overrides
    int home_x = 0, home_y = 0, home_z = 0, home_radius = -1;

    Mob* as_mob() override { return this; }
    void init() override;  // controls, navigation, registerGoals (vanilla Mob constructor)
    virtual void register_goals() {}
    virtual std::unique_ptr<PathNavigation> create_navigation() { return std::make_unique<GroundPathNavigation>(*this); }

    float pathfinding_malus(uint8_t t) const {
        for (auto& [k, v] : malus) if (k == t) return v;
        return path_malus_default[t];
    }
    void set_pathfinding_malus(uint8_t t, float v) {
        for (auto& [k, m] : malus) if (k == t) { m = v; return; }
        malus.emplace_back(t, v);
    }
    // ---- target ----
    LivingEntity* target_unchecked() const { return living_of(entity_by_id(target_id)); }
    LivingEntity* as_valid_target(LivingEntity* t) {
        if (!t) return nullptr;
        if (t->as_player() && (t->is_creative() || t->is_spectator())) return nullptr;
        return can_attack(*t) ? t : nullptr;
    }
    LivingEntity* target() { return as_valid_target(target_unchecked()); }
    virtual void set_target(LivingEntity* t) { t = as_valid_target(t); target_id = t ? t->id : 0; }
    // ---- mob flags ----
    bool no_ai() const { return data.get(edata::Mob::MOB_FLAGS, 0) & 1; }
    void set_no_ai(bool v) { int f = int(data.get(edata::Mob::MOB_FLAGS, 0)); data.set_byte(edata::Mob::MOB_FLAGS, v ? f | 1 : f & ~1); }
    bool left_handed() const { return data.get(edata::Mob::MOB_FLAGS, 0) & 2; }
    void set_left_handed(bool v) { int f = int(data.get(edata::Mob::MOB_FLAGS, 0)); data.set_byte(edata::Mob::MOB_FLAGS, v ? f | 2 : f & ~2); }
    bool aggressive() const { return data.get(edata::Mob::MOB_FLAGS, 0) & 4; }
    void set_aggressive(bool v) { int f = int(data.get(edata::Mob::MOB_FLAGS, 0)); data.set_byte(edata::Mob::MOB_FLAGS, v ? f | 4 : f & ~4); }
    // ---- movement helpers ----
    void set_zza(float v) { zza = v; }
    void set_xxa(float v) { xxa = v; }
    void set_yya(float v) { yya = v; }
    void set_speed(float s) { speed = s; zza = s; }
    void set_jumping(bool j) { jumping = j; }
    void stop_in_place() { navigation->stop(); xxa = yya = 0; set_speed(0); vel = {0, 0, 0}; }
    virtual int max_head_x_rot() const { return 40; }
    virtual int max_head_y_rot() const { return 75; }
    virtual int head_rot_speed() const { return 10; }
    void look_at(Entity& e, float ymax, float xmax) {  // Mob.lookAt
        double xd = e.pos.x - pos.x, zd = e.pos.z - pos.z;
        double yd = e.as_living() ? e.eye_y() - eye_y() : (e.bb.y0 + e.bb.y1) / 2.0 - eye_y();
        double sd = std::sqrt(xd * xd + zd * zd);
        float yr = float(std::atan2(zd, xd) * 180.0 / M_PI) - 90.0f, xr = float(-(std::atan2(yd, sd) * 180.0 / M_PI));
        pitch = mth::rotlerp(pitch, xr, xmax);
        yaw = mth::rotlerp(yaw, yr, ymax);
    }
    bool is_effective_ai() const { return !no_ai(); }
    bool is_path_finding() const { return !navigation->is_done(); }
    int max_fall_distance() const override {  // Mob.getMaxFallDistance
        if (!target_id) return 3;
        int saferegion = int(health() - max_health() * 0.33f);
        saferegion -= (3 - difficulty()) * 4;
        if (saferegion < 0) saferegion = 0;
        return saferegion + 3;
    }
    // ---- ticking ----
    void tick() override;
    void base_tick() override;
    void ai_step() override;
    void server_ai_step() override;  // final in vanilla: sensing, selectors, navigation, controls
    virtual void custom_server_ai_step() {}
    void tick_head_turn(float) override { body_control->client_tick(); }
    void check_despawn() override;
    virtual bool remove_when_far_away(double) { return true; }
    virtual bool requires_custom_persistence() const { return false; }
    bool is_persistence_required() const { return persistence_required; }
    void set_persistence_required() { persistence_required = true; }
    virtual int ambient_sound_interval() const { return 80; }
    virtual std::string ambient_sound() const { return ""; }
    void play_ambient_sound() { make_sound(ambient_sound()); }
    // ---- spawning ----
    virtual SpawnGroupData* finalize_spawn(SpawnReason reason, SpawnGroupData* group);
    virtual bool check_spawn_rules(SpawnReason) { return true; }
    virtual bool check_spawn_obstruction() {  // Mob.checkSpawnObstruction: free of fluids and collisions
        return !contains_any_liquid(bb) && no_block_collision(bb);
    }
    virtual int max_spawn_cluster_size() const { return 4; }
    virtual bool is_max_group_size_reached(int) const { return false; }
    virtual void populate_default_equipment(const DifficultyInstance&) {}
    // ---- combat ----
    bool do_hurt_target(Entity& target) override;
    bool is_within_melee_attack_range(LivingEntity& t) {
        static const double reach = std::sqrt(2.04f) - 0.6f;
        return attack_bounding_box(reach).intersects(t.bb);
    }
    AABB attack_bounding_box(double h) const { return bb.inflate(h, 0.0, h); }
    bool can_attack(LivingEntity& t) override { return !t.is_type(entity_types::ghast) && LivingEntity::can_attack(t); }
    void drop_custom_death_loot(const DamageSource& s, bool player_killed) override;
    int base_experience_reward() override;
    void set_last_hurt_by_mob(LivingEntity* e) override { LivingEntity::set_last_hurt_by_mob(e); }
    // ---- interaction ----
    int interact(Player& p, int hand) override;
    virtual int mob_interact(Player&, int) { return IR_PASS; }
    // ---- light helpers (Entity.getLightLevelDependentMagicValue) ----
    float light_magic_value() const {
        int l = max_local_raw_brightness(block_x(), mth::floor(eye_y()), block_z());
        float f = float(l) / 15.0f;
        return f / (4.0f - 3.0f * f);
    }
    virtual float walk_target_value(int, int, int) { return 0.0f; }  // PathfinderMob.getWalkTargetValue
    bool is_sun_burn_tick() const;
    virtual int sun_protection_slot() const { return ES_HEAD; }
    // ---- persistence ----
    void save_extra(Nbt& o) const override;
    void load_extra(const Nbt& in) override;
    bool is_panicking() const;
};

// PathfinderMob
struct PathfinderMob : Mob {
    float walk_target_value(int x, int y, int z) override { (void)x; (void)y; (void)z; return 0.0f; }
    bool check_spawn_rules(SpawnReason) override { return walk_target_value(block_x(), block_y(), block_z()) >= 0.0f; }
    bool is_path_finding() const { return !navigation->is_done(); }
    bool is_within_home() const {  // isWithinHome
        if (home_radius == -1) return true;
        double dx = home_x + 0.5 - pos.x, dy = home_y + 0.5 - pos.y, dz = home_z + 0.5 - pos.z;
        return dx * dx + dy * dy + dz * dz < double(home_radius) * home_radius;
    }
    bool is_within_home(int x, int y, int z) const {
        if (home_radius == -1) return true;
        double dx = home_x - x, dy = home_y - y, dz = home_z - z;
        return dx * dx + dy * dy + dz * dz < double(home_radius) * home_radius;
    }
};

// AgeableMob
struct AgeableMobGroupData : SpawnGroupData {
    int group_size = 0;
    bool spawn_baby;
    float baby_chance;
    explicit AgeableMobGroupData(bool baby, float chance = 0.05f) : spawn_baby(baby), baby_chance(chance) {}
};
struct AgeableMob : PathfinderMob {
    int age = 0, forced_age = 0, forced_age_timer = 0;
    bool is_baby() const override { return age < 0; }
    void set_age(int a) {
        int o = age;
        age = a;
        if ((o < 0 && a >= 0) || (o >= 0 && a < 0)) {
            data.set_bool(edata::AgeableMob::BABY, a < 0);
            refresh_dimensions();
        }
    }
    void set_baby(bool b) { set_age(b ? -24000 : 0); }
    bool age_locked() const { return data.get(edata::AgeableMob::AGE_LOCKED, 0); }
    bool can_age_up() const { return is_baby() && !age_locked(); }
    void age_up(int seconds, bool forced = false) {
        int a = age + seconds * 20;
        if (a > 0) a = 0;
        int delta = a - age;
        set_age(a);
        if (forced) { forced_age += delta; if (forced_age_timer == 0) forced_age_timer = 40; }
        if (age == 0) set_age(forced_age);
    }
    virtual void refresh_dimensions() {
        float s = age_scale();
        set_size(info().w * s, info().h * s, info().eye * s);
    }
    virtual std::unique_ptr<Entity> breed_offspring(AgeableMob&) { return create_entity(type); }  // getBreedOffspring
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override;
    void ai_step() override;
    void save_extra(Nbt& o) const override;
    void load_extra(const Nbt& in) override;
};

// Animal
struct Animal : AgeableMob {
    int in_love = 0;
    int love_cause = 0;  // player entity id
    void init() override {
        AgeableMob::init();
        set_pathfinding_malus(PT_FIRE_IN_NEIGHBOR, 16.0f);
        set_pathfinding_malus(PT_FIRE, -1.0f);
    }
    virtual bool is_food(const ItemStack&) const { return false; }
    void custom_server_ai_step() override { if (age != 0) in_love = 0; }
    void ai_step() override {
        AgeableMob::ai_step();
        if (age != 0) in_love = 0;
        if (in_love > 0) in_love--;
    }
    void actually_hurt(const DamageSource& s, float dmg) override { in_love = 0; AgeableMob::actually_hurt(s, dmg); }
    float walk_target_value(int x, int y, int z) override {
        if (block_index(block_at(x, y - 1, z)) == block_index(blocks::grass_block)) return 10.0f;
        int l = max_local_raw_brightness(x, y, z);
        float f = float(l) / 15.0f;
        return f / (4.0f - 3.0f * f) - 0.5f;
    }
    int ambient_sound_interval() const override { return 120; }
    bool remove_when_far_away(double) override { return false; }
    int base_experience_reward() override { return 1 + rand_int(3); }
    int mob_interact(Player& p, int hand) override;
    virtual void play_eating_sound() {}
    bool can_fall_in_love() const { return in_love <= 0; }
    void set_in_love(Player* p) { in_love = 600; if (p) if (auto* pe = player_entity(*p)) love_cause = pe->id; broadcast_event(18); }
    bool is_in_love() const { return in_love > 0; }
    virtual bool can_mate(Animal& o) { return &o != this && o.type == type && is_in_love() && o.is_in_love(); }
    void spawn_child_from_breeding(Animal& partner);
    void save_extra(Nbt& o) const override { AgeableMob::save_extra(o); o.set("InLove", Nbt::num(Nbt::Int, in_love)); }
    void load_extra(const Nbt& in) override { AgeableMob::load_extra(in); in_love = int(nbt_num(in, "InLove", 0)); }
};

// Monster
struct Monster : PathfinderMob {
    void init() override { PathfinderMob::init(); xp_reward = 5; }
    int sound_source() const override { return SRC_HOSTILE; }
    void ai_step() override {
        if (light_magic_value() > 0.5f) no_action_time += 2;  // updateNoActionTime
        PathfinderMob::ai_step();
    }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.hostile.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.hostile.death"; }
    float walk_target_value(int x, int y, int z) override {
        int l = max_local_raw_brightness(x, y, z);
        float f = float(l) / 15.0f;
        return -(f / (4.0f - 3.0f * f) - 0.5f);
    }
    bool should_drop_experience() const override { return true; }
    bool should_drop_loot() const override { return rule("mob_drops"); }
};

// ---------- spawn rules (SpawnPlacements predicates) ----------
inline bool valid_spawn_on(int state, int type) {  // BlockState.isValidSpawn(level, pos, type)
    uint32_t bit = SF_SPAWN_GENERIC;
    if (type == entity_types::polar_bear) bit = SF_SPAWN_POLAR_BEAR;
    else if (type == entity_types::ocelot) bit = SF_SPAWN_OCELOT;
    else if (type == entity_types::parrot) bit = SF_SPAWN_PARROT;
    else if (entity_type_info[size_t(type)].fire_immune) bit = SF_SPAWN_FIRE_IMMUNE;
    return sflag(state, bit);
}
inline bool is_spawner_reason(SpawnReason r) { return r == SpawnReason::SPAWNER || r == SpawnReason::TRIAL_SPAWNER; }
inline bool ignores_light(SpawnReason r) { return r == SpawnReason::TRIAL_SPAWNER; }
inline bool check_mob_spawn_rules(int type, SpawnReason r, int x, int y, int z) {  // Mob.checkMobSpawnRules
    return is_spawner_reason(r) || valid_spawn_on(block_at(x, y - 1, z), type);
}
inline int monster_spawn_light_test() { return rand_between(0, 7); }  // dimension monster_spawn_light_level uniform(0, 7)
inline bool is_dark_enough_to_spawn(int x, int y, int z) {  // Monster.isDarkEnoughToSpawn
    if (sky_light_at(x, y, z) > rand_int(32)) return false;
    if (block_light_at(x, y, z) > 0) return false;  // monster_spawn_block_light_limit 0
    int b = weather.thundering ? raw_brightness(x, y, z, 10) : max_local_raw_brightness(x, y, z);
    return b <= monster_spawn_light_test();
}
inline bool check_monster_spawn_rules(int type, SpawnReason r, int x, int y, int z) {
    return (ignores_light(r) || is_dark_enough_to_spawn(x, y, z)) && difficulty() != 0 && check_mob_spawn_rules(type, r, x, y, z);
}
inline bool check_animal_spawn_rules(int, SpawnReason r, int x, int y, int z) {  // Animal.checkAnimalSpawnRules
    bool bright = ignores_light(r) || raw_brightness(x, y, z, 0) > 8;
    return block_in_tag(block_at(x, y - 1, z), "animals_spawnable_on") && bright;
}
// Registry of spawn rule predicates by the name SpawnPlacements uses ("Monster::checkMonsterSpawnRules").
// Mob types register theirs with register_spawn_rule; unknown names fall back to checkMobSpawnRules.
using SpawnRuleFn = bool (*)(int type, SpawnReason r, int x, int y, int z);
inline std::map<std::string, SpawnRuleFn>& spawn_rules() {
    static std::map<std::string, SpawnRuleFn> m = {
        {"Mob::checkMobSpawnRules", check_mob_spawn_rules},
        {"Monster::checkMonsterSpawnRules", check_monster_spawn_rules},
        {"Monster::checkAnyLightMonsterSpawnRules", [](int t, SpawnReason r, int x, int y, int z) { return difficulty() != 0 && check_mob_spawn_rules(t, r, x, y, z); }},
        {"Monster::checkSurfaceMonstersSpawnRules", [](int t, SpawnReason r, int x, int y, int z) {
             return check_monster_spawn_rules(t, r, x, y, z) && (is_spawner_reason(r) || can_see_sky(x, y, z)); }},
        {"Animal::checkAnimalSpawnRules", check_animal_spawn_rules},
    };
    return m;
}
inline void register_spawn_rule(const std::string& name, SpawnRuleFn f) { spawn_rules()[name] = f; }
inline bool check_spawn_rules_for(int type, SpawnReason r, int x, int y, int z) {  // SpawnPlacements.checkSpawnRules
    const auto& t = entity_type_info[size_t(type)];
    if (t.spawn_rule.empty()) return true;
    auto it = spawn_rules().find(t.spawn_rule);
    return it != spawn_rules().end() ? it->second(type, r, x, y, z) : check_mob_spawn_rules(type, r, x, y, z);
}

// ---------- registration ----------
template <class T> inline std::unique_ptr<Entity> make_entity() { return std::make_unique<T>(); }
// create_entity(type) (manager.hpp) constructs a registered type and runs init(); add_entity() puts it in the world.
