#pragma once
// Goal library (net.minecraft.world.entity.ai.goal + .target + ai.util random positions). Each goal takes its owner
// by reference; "class" filters of vanilla (Player.class, Wolf.class) become entity type predicates.
#include "../mob_impl.hpp"

// ---------- random positions (RandomPos / DefaultRandomPos / LandRandomPos / GoalUtils) ----------
namespace randpos {
struct BP { int x, y, z; };
inline bool restricted_mob(PathfinderMob& m, double h) {  // GoalUtils.mobRestricted
    if (m.home_radius < 0) return false;
    double r = m.home_radius + h + 1.0;
    return Vec3{m.home_x + 0.5, m.home_y + 0.5, m.home_z + 0.5}.dist_sqr(m.pos) < r * r;
}
inline BP random_direction(int h, int v) { int x = rand_int(2 * h + 1) - h, y = rand_int(2 * v + 1) - v, z = rand_int(2 * h + 1) - h; return {x, y, z}; }
inline std::optional<BP> random_direction_within(double minh, double maxh, int v, int fly, double xd, double zd, double maxrad) {
    double center = std::atan2(zd, xd) - float(M_PI / 2);
    double yr = center + (2.0f * rand_float() - 1.0f) * maxrad;
    double dist = mth::lerp(std::sqrt(rand_double()), minh, maxh) * 1.4142135f;
    double xt = -dist * std::sin(yr), zt = dist * std::cos(yr);
    if (std::abs(xt) > maxh || std::abs(zt) > maxh) return std::nullopt;
    int yt = rand_int(2 * v + 1) - v + fly;
    return BP{mth::floor(xt), yt, mth::floor(zt)};
}
inline BP toward(PathfinderMob& m, double h, BP d) {  // RandomPos.generateRandomPosTowardDirection
    double xt = d.x, zt = d.z;
    if (m.home_radius >= 0 && h > 1.0) {
        xt += m.pos.x > m.home_x ? -rand_double() * h / 2.0 : rand_double() * h / 2.0;
        zt += m.pos.z > m.home_z ? -rand_double() * h / 2.0 : rand_double() * h / 2.0;
    }
    return {mth::floor(xt + m.pos.x), mth::floor(d.y + m.pos.y), mth::floor(zt + m.pos.z)};
}
inline bool has_malus(PathfinderMob& m, BP p) { return m.pathfinding_malus(path_type_static(p.x, p.y, p.z)) != 0.0f; }
inline bool ok_default(PathfinderMob& m, bool restrict, BP p) {
    if (p.y < MIN_Y || p.y >= MIN_Y + HEIGHT) return false;
    if (restrict && !m.is_within_home(p.x, p.y, p.z)) return false;
    if (!m.navigation->is_stable_destination(p.x, p.y, p.z)) return false;
    return true;
}
template <class Gen, class W> inline std::optional<Vec3> generate(Gen gen, W weight) {  // RandomPos.generateRandomPos
    double best = -INFINITY;
    std::optional<BP> bp;
    for (int i = 0; i < 10; i++) {
        auto p = gen();
        if (!p) continue;
        double v = weight(*p);
        if (v > best) best = v, bp = p;
    }
    if (!bp) return std::nullopt;
    return Vec3{bp->x + 0.5, double(bp->y), bp->z + 0.5};
}
inline std::optional<Vec3> default_pos(PathfinderMob& m, int h, int v) {  // DefaultRandomPos.getPos
    bool r = restricted_mob(m, h);
    return generate([&]() -> std::optional<BP> {
        BP p = toward(m, h, random_direction(h, v));
        if (!ok_default(m, r, p) || has_malus(m, p)) return std::nullopt;
        return p;
    }, [&](BP p) { return m.walk_target_value(p.x, p.y, p.z); });
}
inline std::optional<Vec3> default_pos_away(PathfinderMob& m, int h, int v, const Vec3& avoid) {  // DefaultRandomPos.getPosAway
    Vec3 dir = m.pos - avoid;
    bool r = restricted_mob(m, h);
    return generate([&]() -> std::optional<BP> {
        auto d = random_direction_within(0.0, h, v, 0, dir.x, dir.z, float(M_PI / 2));
        if (!d) return std::nullopt;
        BP p = toward(m, h, *d);
        if (!ok_default(m, r, p) || has_malus(m, p)) return std::nullopt;
        return p;
    }, [&](BP p) { return m.walk_target_value(p.x, p.y, p.z); });
}
inline std::optional<BP> move_up_out_of_solid(PathfinderMob& m, BP p) {  // LandRandomPos.movePosUpOutOfSolid
    while (p.y < MIN_Y + HEIGHT && sflag(block_at(p.x, p.y, p.z), SF_SOLID)) p.y++;
    if (fluid_type(block_at(p.x, p.y, p.z)) == FLUID_WATER || has_malus(m, p)) return std::nullopt;
    return p;
}
inline std::optional<Vec3> land_pos(PathfinderMob& m, int h, int v) {  // LandRandomPos.getPos
    bool r = restricted_mob(m, h);
    return generate([&]() -> std::optional<BP> {
        BP p = toward(m, h, random_direction(h, v));
        if (!ok_default(m, r, p)) return std::nullopt;
        return move_up_out_of_solid(m, p);
    }, [&](BP p) { return m.walk_target_value(p.x, p.y, p.z); });
}
}  // namespace randpos

// Level.getNearestPlayer(conditions, mob, x, y, z) / getNearestEntity over a candidate list.
inline LivingEntity* nearest_player(const TargetingConditions& tc, LivingEntity* src, double x, double y, double z) {
    LivingEntity* best = nullptr;
    double bd = -1;
    for (Entity* e : player_entities()) {
        LivingEntity* l = e->as_living();
        if (!l || !tc.test(src, *l)) continue;
        double d = l->dist_sqr(x, y, z);
        if (bd == -1 || d < bd) bd = d, best = l;
    }
    return best;
}
template <class Pred>
inline LivingEntity* nearest_entity(const AABB& box, Pred&& type_ok, const TargetingConditions& tc, LivingEntity* src, double x, double y, double z) {
    LivingEntity* best = nullptr;
    double bd = -1;
    for_each_entity_in(box, [&](Entity& e) {
        LivingEntity* l = e.as_living();
        if (!l || !type_ok(*l) || !tc.test(src, *l)) return;
        double d = l->dist_sqr(x, y, z);
        if (bd == -1 || d < bd) bd = d, best = l;
    });
    return best;
}

// ---------- goals ----------
struct FloatGoal : Goal {
    Mob& mob;
    explicit FloatGoal(Mob& m) : mob(m) { flags = GF_JUMP; m.navigation->set_can_float(true); }
    bool can_use() override { return (mob.is_in_water() && mob.water_height > mob.fluid_jump_threshold()) || mob.is_in_lava(); }
    bool requires_update_every_tick() override { return true; }
    void tick() override { if (rand_float() < 0.8f) mob.jump_control->jump(); }
};

struct PanicGoal : Goal {
    PathfinderMob& mob;
    double speed;
    double px = 0, py = 0, pz = 0;
    bool running = false;
    PanicGoal(PathfinderMob& m, double s) : mob(m), speed(s) { flags = GF_MOVE; }
    virtual bool should_panic() {
        if (mob.last_damage_type < 0 || server_tick - mob.last_damage_stamp > 40) return false;  // getLastDamageSource (40 tick timeout)
        DamageSource d;
        d.type = mob.last_damage_type;
        return d.is(DT_PANIC_CAUSES);
    }
    bool can_use() override {
        if (!should_panic()) return false;
        if (mob.is_on_fire() && !has_collision(block_at(mob.block_x(), mob.block_y(), mob.block_z()))) {  // lookForWater (5, 1)
            int bx = mob.block_x(), by = mob.block_y(), bz = mob.block_z();
            for (int d = 0; d <= 6; d++)  // findBlocksInBoxByManhattanDistance order: by distance
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -5; dx <= 5; dx++)
                        for (int dz = -5; dz <= 5; dz++) {
                            if (std::abs(dx) + std::abs(dy) + std::abs(dz) != d) continue;
                            if (fluid_type(block_at(bx + dx, by + dy, bz + dz)) == FLUID_WATER) {
                                px = bx + dx, py = by + dy, pz = bz + dz;
                                return true;
                            }
                        }
        }
        auto p = randpos::default_pos(mob, 5, 4);
        if (!p) return false;
        px = p->x, py = p->y, pz = p->z;
        return true;
    }
    void start() override { mob.navigation->move_to(px, py, pz, speed); running = true; }
    void stop() override { running = false; }
    bool can_continue_to_use() override { return !mob.navigation->is_done(); }
};
inline bool Mob::is_panicking() const { return goal_selector.is_running<PanicGoal>(); }

struct RandomStrollGoal : Goal {
    PathfinderMob& mob;
    double wx = 0, wy = 0, wz = 0, speed;
    int interval;
    bool force = false, check_no_action;
    RandomStrollGoal(PathfinderMob& m, double s, int iv = 120, bool cna = true) : mob(m), speed(s), interval(iv), check_no_action(cna) { flags = GF_MOVE; }
    virtual std::optional<Vec3> position() { return randpos::default_pos(mob, 10, 7); }
    bool can_use() override {
        if (!force) {
            if (check_no_action && mob.no_action_time >= 100) return false;
            if (rand_int(reduced_tick_delay(interval)) != 0) return false;
        }
        auto p = position();
        if (!p) return false;
        wx = p->x, wy = p->y, wz = p->z;
        force = false;
        return true;
    }
    bool can_continue_to_use() override { return !mob.navigation->is_done(); }
    void start() override { mob.navigation->move_to(wx, wy, wz, speed); }
    void stop() override { mob.navigation->stop(); }
    void trigger() { force = true; }
};
struct WaterAvoidingRandomStrollGoal : RandomStrollGoal {
    float probability;
    WaterAvoidingRandomStrollGoal(PathfinderMob& m, double s, float p = 0.001f) : RandomStrollGoal(m, s), probability(p) {}
    std::optional<Vec3> position() override {
        if (mob.is_in_water()) {
            auto p = randpos::land_pos(mob, 15, 7);
            return p ? p : RandomStrollGoal::position();
        }
        return rand_float() >= probability ? randpos::land_pos(mob, 10, 7) : RandomStrollGoal::position();
    }
};

struct LookAtPlayerGoal : Goal {
    Mob& mob;
    int look_at = 0;
    float dist, probability;
    int look_time = 0;
    bool only_horizontal;
    std::function<bool(LivingEntity&)> type_ok;  // null = players
    LookAtPlayerGoal(Mob& m, float d, float p = 0.02f, bool oh = false, std::function<bool(LivingEntity&)> t = nullptr)
        : mob(m), dist(d), probability(p), only_horizontal(oh), type_ok(std::move(t)) { flags = GF_LOOK; }
    TargetingConditions cond() const { auto c = TargetingConditions::for_non_combat(); c.set_range(dist); return c; }
    bool can_use() override {
        if (rand_float() >= probability) return false;
        if (LivingEntity* t = mob.target()) look_at = t->id;
        LivingEntity* l = type_ok ? nearest_entity(mob.bb.inflate(dist, 3.0, dist), type_ok, cond(), &mob, mob.pos.x, mob.eye_y(), mob.pos.z)
                                  : nearest_player(cond(), &mob, mob.pos.x, mob.eye_y(), mob.pos.z);
        look_at = l ? l->id : 0;
        return l != nullptr;
    }
    bool can_continue_to_use() override {
        Entity* e = entity_by_id(look_at);
        if (!e || !e->is_alive()) return false;
        return mob.dist_sqr(*e) > dist * dist ? false : look_time > 0;
    }
    void start() override { look_time = adjusted_tick_delay(40 + rand_int(40)); }
    void stop() override { look_at = 0; }
    void tick() override {
        Entity* e = entity_by_id(look_at);
        if (e && e->is_alive()) {
            mob.look_control->set_look_at(e->pos.x, only_horizontal ? mob.eye_y() : e->eye_y(), e->pos.z);
            look_time--;
        }
    }
};
struct RandomLookAroundGoal : Goal {
    Mob& mob;
    double rx = 0, rz = 0;
    int look_time = 0;
    explicit RandomLookAroundGoal(Mob& m) : mob(m) { flags = GF_MOVE | GF_LOOK; }
    bool can_use() override { return rand_float() < 0.02f; }
    bool can_continue_to_use() override { return look_time >= 0; }
    void start() override {
        double r = M_PI * 2 * rand_double();
        rx = std::cos(r), rz = std::sin(r);
        look_time = 20 + rand_int(20);
    }
    bool requires_update_every_tick() override { return true; }
    void tick() override { look_time--; mob.look_control->set_look_at(mob.pos.x + rx, mob.eye_y(), mob.pos.z + rz); }
};

struct TemptGoal : Goal {
    PathfinderMob& mob;
    double speed;
    double px = 0, py = 0, pz = 0, prx = 0, pry = 0;
    int player = 0, calm_down = 0;
    std::function<bool(const ItemStack&)> items;
    bool can_scare;
    double stop_dist;
    TemptGoal(PathfinderMob& m, double s, std::function<bool(const ItemStack&)> it, bool scare, double sd = 2.5)
        : mob(m), speed(s), items(std::move(it)), can_scare(scare), stop_dist(sd) { flags = GF_MOVE | GF_LOOK; }
    bool can_use() override {
        if (calm_down > 0) { calm_down--; return false; }
        auto c = TargetingConditions::for_non_combat();
        c.ignore_los().set_range(mob.attr(attr::tempt_range));
        c.selector = [this](LivingEntity& t) { return items(t.item_in_slot(ES_MAINHAND)) || items(t.item_in_slot(ES_OFFHAND)); };
        LivingEntity* p = nearest_player(c, &mob, mob.pos.x, mob.pos.y, mob.pos.z);
        player = p ? p->id : 0;
        return p != nullptr;
    }
    bool can_continue_to_use() override {
        Entity* p = entity_by_id(player);
        if (can_scare && p) {
            if (mob.dist_sqr(*p) < 36.0) {
                if (p->dist_sqr(px, py, pz) > 0.010000000000000002) return false;
                if (std::abs(p->pitch - prx) > 5.0 || std::abs(p->yaw - pry) > 5.0) return false;
            } else px = p->pos.x, py = p->pos.y, pz = p->pos.z;
            prx = p->pitch, pry = p->yaw;
        }
        return can_use();
    }
    void start() override { if (Entity* p = entity_by_id(player)) px = p->pos.x, py = p->pos.y, pz = p->pos.z; }
    void stop() override { player = 0; mob.navigation->stop(); calm_down = reduced_tick_delay(100); }
    void tick() override {
        Entity* p = entity_by_id(player);
        if (!p) return;
        mob.look_control->set_look_at(*p, float(mob.max_head_y_rot() + 20), float(mob.max_head_x_rot()));
        if (mob.dist_sqr(*p) < stop_dist * stop_dist) mob.navigation->stop();
        else mob.navigation->move_to(*p, speed);
    }
};

struct BreedGoal : Goal {
    Animal& animal;
    int partner = 0, love_time = 0;
    double speed;
    BreedGoal(Animal& a, double s) : animal(a), speed(s) { flags = GF_MOVE | GF_LOOK; }
    Animal* get_partner() const { return dynamic_cast<Animal*>(entity_by_id(partner)); }
    bool can_use() override {
        if (!animal.is_in_love()) return false;
        auto tc = TargetingConditions::for_non_combat();
        tc.set_range(8.0).ignore_los();
        double best = 1e300;
        partner = 0;
        for_each_entity_in(animal.bb.inflate(8.0), [&](Entity& e) {
            auto* a = dynamic_cast<Animal*>(&e);
            if (!a || a->type != animal.type || !tc.test(&animal, *a)) return;
            if (animal.can_mate(*a) && !a->is_panicking() && animal.dist_sqr(*a) < best) best = animal.dist_sqr(*a), partner = a->id;
        }, false);
        return partner != 0;
    }
    bool can_continue_to_use() override {
        Animal* p = get_partner();
        return p && p->is_alive() && p->is_in_love() && love_time < 60 && !p->is_panicking();
    }
    void stop() override { partner = 0; love_time = 0; }
    void tick() override {
        Animal* p = get_partner();
        if (!p) return;
        animal.look_control->set_look_at(*p, 10.0f, float(animal.max_head_x_rot()));
        animal.navigation->move_to(*p, speed);
        love_time++;
        if (love_time >= adjusted_tick_delay(60) && animal.dist_sqr(*p) < 9.0) animal.spawn_child_from_breeding(*p);
    }
};
struct FollowParentGoal : Goal {
    Animal& animal;
    int parent = 0, recalc = 0;
    double speed;
    FollowParentGoal(Animal& a, double s) : animal(a), speed(s) {}
    bool can_use() override {
        if (animal.age >= 0) return false;
        Animal* closest = nullptr;
        double cd = 1e300;
        for_each_entity_in(animal.bb.inflate(8.0, 4.0, 8.0), [&](Entity& e) {
            auto* a = dynamic_cast<Animal*>(&e);
            if (!a || a->type != animal.type || a->age < 0) return;
            double d = animal.dist_sqr(*a);
            if (!(d > cd)) cd = d, closest = a;
        }, false);
        if (!closest || cd < 9.0) return false;
        parent = closest->id;
        return true;
    }
    bool can_continue_to_use() override {
        Entity* p = entity_by_id(parent);
        if (animal.age >= 0 || !p || !p->is_alive()) return false;
        double d = animal.dist_sqr(*p);
        return !(d < 9.0) && !(d > 256.0);
    }
    void start() override { recalc = 0; }
    void stop() override { parent = 0; }
    void tick() override {
        if (--recalc > 0) return;
        recalc = adjusted_tick_delay(10);
        if (Entity* p = entity_by_id(parent)) animal.navigation->move_to(*p, speed);
    }
};

struct EatBlockGoal : Goal {
    Mob& mob;
    int eat_tick = 0;
    std::function<void()> ate;
    explicit EatBlockGoal(Mob& m) : mob(m) { flags = GF_MOVE | GF_LOOK | GF_JUMP; }
    static bool edible(int s) { return block_in_tag(s, "edible_for_sheep"); }
    bool can_use() override {
        if (rand_int(adjusted_tick_delay(mob.is_baby() ? 50 : 1000)) != 0) return false;
        int x = mob.block_x(), y = mob.block_y(), z = mob.block_z();
        return edible(block_at(x, y, z)) || block_index(block_at(x, y - 1, z)) == block_index(blocks::grass_block);
    }
    void start() override { eat_tick = adjusted_tick_delay(40); mob.broadcast_event(10); mob.navigation->stop(); }
    void stop() override { eat_tick = 0; }
    bool can_continue_to_use() override { return eat_tick > 0; }
    void tick() override {
        eat_tick = std::max(0, eat_tick - 1);
        if (eat_tick != adjusted_tick_delay(4)) return;
        int x = mob.block_x(), y = mob.block_y(), z = mob.block_z();
        if (edible(block_at(x, y, z))) {
            if (rule("mob_griefing")) change_block(x, y, z, blocks::air);  // destroyBlock(pos, false): no drops
            if (ate) ate();
        } else if (block_index(block_at(x, y - 1, z)) == block_index(blocks::grass_block)) {
            if (rule("mob_griefing")) {
                Writer w;  // levelEvent 2001 (block break particles)
                w.varint(play_cb::level_event).i32(2001).i64(mth::block_key(x, y - 1, z)).i32(blocks::grass_block).boolean(false);
                broadcast_near(x, z, w);
                change_block(x, y - 1, z, blocks::dirt);
            }
            if (ate) ate();
        }
    }
};

struct MeleeAttackGoal : Goal {
    PathfinderMob& mob;
    double speed;
    bool follow_unseen;
    std::unique_ptr<Path> path;
    double ptx = 0, pty = 0, ptz = 0;
    int recalc = 0, until_attack = 0;
    int64_t last_can_use = -100;
    MeleeAttackGoal(PathfinderMob& m, double s, bool fu) : mob(m), speed(s), follow_unseen(fu) { flags = GF_MOVE | GF_LOOK; }
    bool can_use() override {
        if (server_tick - last_can_use < 20) return false;
        last_can_use = server_tick;
        LivingEntity* t = mob.target();
        if (!t || !t->is_alive()) return false;
        path = mob.navigation->create_path_to(*t, 0);
        return path ? true : mob.is_within_melee_attack_range(*t);
    }
    bool can_continue_to_use() override {
        LivingEntity* t = mob.target();
        if (!t || !t->is_alive()) return false;
        if (!follow_unseen) return !mob.navigation->is_done();
        if (!mob.is_within_home(t->block_x(), t->block_y(), t->block_z())) return false;
        return !(t->as_player() && (t->is_spectator() || t->is_creative()));
    }
    void start() override {
        mob.navigation->move_to(std::move(path), speed);
        mob.set_aggressive(true);
        recalc = 0, until_attack = 0;
    }
    void stop() override {
        LivingEntity* t = mob.target_unchecked();
        if (t && t->as_player() && (t->is_creative() || t->is_spectator())) mob.set_target(nullptr);
        mob.set_aggressive(false);
        mob.navigation->stop();
    }
    bool requires_update_every_tick() override { return true; }
    void tick() override {
        LivingEntity* t = mob.target();
        if (!t) return;
        mob.look_control->set_look_at(*t, 30.0f, 30.0f);
        recalc = std::max(recalc - 1, 0);
        if ((follow_unseen || mob.sensing.has_line_of_sight(mob, *t)) && recalc <= 0 &&
            ((ptx == 0 && pty == 0 && ptz == 0) || t->dist_sqr(ptx, pty, ptz) >= 1.0 || rand_float() < 0.05f)) {
            ptx = t->pos.x, pty = t->pos.y, ptz = t->pos.z;
            recalc = 4 + rand_int(7);
            double d = mob.dist_sqr(*t);
            if (d > 1024.0) recalc += 10;
            else if (d > 256.0) recalc += 5;
            auto p = mob.navigation->create_path_to(*t, 0);
            if (!(p && mob.navigation->move_to(std::move(p), speed))) recalc += 15;
            recalc = adjusted_tick_delay(recalc);
        }
        until_attack = std::max(until_attack - 1, 0);
        check_and_perform_attack(*t);
    }
    virtual void check_and_perform_attack(LivingEntity& t) {
        if (until_attack <= 0 && mob.is_within_melee_attack_range(t) && mob.sensing.has_line_of_sight(mob, t)) {
            until_attack = adjusted_tick_delay(20);
            mob.swing(0);
            mob.do_hurt_target(t);
        }
    }
};

// ---------- target goals ----------
struct TargetGoal : Goal {
    Mob& mob;
    bool must_see, must_reach;
    int reach_cache = 0, reach_cache_time = 0, unseen_ticks = 0, target_mob = 0, unseen_memory = 60;
    TargetGoal(Mob& m, bool see, bool reach = false) : mob(m), must_see(see), must_reach(reach) {}
    double follow_distance() const { return mob.attr(attr::follow_range); }
    bool can_continue_to_use() override {
        LivingEntity* t = mob.target();
        if (!t) t = living_of(entity_by_id(target_mob));
        if (!t || !mob.can_attack(*t)) return false;
        // ponytail: scoreboard teams of mobs aren't tracked (Team check)
        double w = follow_distance();
        if (mob.dist_sqr(*t) > w * w) return false;
        if (must_see) {
            if (mob.sensing.has_line_of_sight(mob, *t)) unseen_ticks = 0;
            else if (++unseen_ticks > reduced_tick_delay(unseen_memory)) return false;
        }
        mob.set_target(t);
        return true;
    }
    void start() override { reach_cache = 0, reach_cache_time = 0, unseen_ticks = 0; }
    void stop() override { mob.set_target(nullptr); target_mob = 0; }
    bool can_attack_target(LivingEntity* t, const TargetingConditions& tc) {
        if (!t || !tc.test(&mob, *t)) return false;
        if (auto* pm = dynamic_cast<PathfinderMob*>(&mob); pm && !pm->is_within_home(t->block_x(), t->block_y(), t->block_z())) return false;
        if (must_reach) {
            if (--reach_cache_time <= 0) reach_cache = 0;
            if (reach_cache == 0) {
                reach_cache_time = reduced_tick_delay(10 + rand_int(5));
                auto p = mob.navigation->create_path_to(*t, 0);
                bool ok = false;
                if (p && p->end()) {
                    int xx = p->end()->x - t->block_x(), zz = p->end()->z - t->block_z();
                    ok = xx * xx + zz * zz <= 2.25;
                }
                reach_cache = ok ? 1 : 2;
            }
            if (reach_cache == 2) return false;
        }
        return true;
    }
};
struct NearestAttackableTargetGoal : TargetGoal {
    int random_interval;
    int found = 0;
    std::function<bool(LivingEntity&)> type_ok;  // null = players
    std::function<bool(LivingEntity&)> selector;
    NearestAttackableTargetGoal(Mob& m, bool see, std::function<bool(LivingEntity&)> type = nullptr, int interval = 10,
                                bool reach = false, std::function<bool(LivingEntity&)> sel = nullptr)
        : TargetGoal(m, see, reach), random_interval(reduced_tick_delay(interval)), type_ok(std::move(type)), selector(std::move(sel)) {
        flags = GF_TARGET;
    }
    bool can_use() override {
        if (random_interval > 0 && rand_int(random_interval) != 0) return false;
        auto tc = TargetingConditions::for_combat();
        tc.set_range(follow_distance());
        tc.selector = selector;
        double f = follow_distance();
        LivingEntity* t = type_ok ? nearest_entity(mob.bb.inflate(f, f, f), type_ok, tc, &mob, mob.pos.x, mob.eye_y(), mob.pos.z)
                                  : nearest_player(tc, &mob, mob.pos.x, mob.eye_y(), mob.pos.z);
        found = t ? t->id : 0;
        return t != nullptr;
    }
    void start() override { mob.set_target(living_of(entity_by_id(found))); TargetGoal::start(); }
};
struct HurtByTargetGoal : TargetGoal {
    int timestamp = 0;
    bool alert_same_type = false;
    std::vector<int> ignore_alert_types;
    explicit HurtByTargetGoal(Mob& m) : TargetGoal(m, true) { flags = GF_TARGET; }
    HurtByTargetGoal& set_alert_others(std::vector<int> except = {}) { alert_same_type = true; ignore_alert_types = std::move(except); return *this; }
    bool can_use() override {
        LivingEntity* h = mob.last_hurt_by();
        if (mob.last_hurt_by_mob_time == timestamp || !h) return false;
        if (h->as_player() && rule("universal_anger")) return false;
        auto tc = TargetingConditions::for_combat();
        tc.ignore_los().ignore_invisibility();
        return can_attack_target(h, tc);
    }
    void start() override {
        mob.set_target(mob.last_hurt_by());
        LivingEntity* t = mob.target();
        target_mob = t ? t->id : 0;
        timestamp = mob.last_hurt_by_mob_time;
        unseen_memory = 300;
        if (alert_same_type) {
            double w = follow_distance();
            AABB box = AABB{mob.pos.x, mob.pos.y, mob.pos.z, mob.pos.x + 1, mob.pos.y + 1, mob.pos.z + 1}.inflate(w, 10.0, w);
            LivingEntity* h = mob.last_hurt_by();
            for_each_entity_in(box, [&](Entity& e) {
                Mob* o = e.as_mob();
                if (!o || o == &mob || o->type != mob.type || o->target() || !h || o->is_allied_to(*h)) return;
                if (std::find(ignore_alert_types.begin(), ignore_alert_types.end(), o->type) != ignore_alert_types.end()) return;
                o->set_target(h);
            }, false);
        }
        TargetGoal::start();
    }
};

// ---------- misc goals used by the first mobs ----------
struct RestrictSunGoal : Goal {
    PathfinderMob& mob;
    explicit RestrictSunGoal(PathfinderMob& m) : mob(m) {}
    bool can_use() override { return is_day() && mob.item_in_slot(ES_HEAD).empty() && mob.navigation->can_navigate_ground(); }
    void start() override { if (auto* g = dynamic_cast<GroundPathNavigation*>(mob.navigation.get())) g->avoid_sun = true; }
    void stop() override { if (auto* g = dynamic_cast<GroundPathNavigation*>(mob.navigation.get())) g->avoid_sun = false; }
};
struct FleeSunGoal : Goal {
    PathfinderMob& mob;
    double speed, wx = 0, wy = 0, wz = 0;
    FleeSunGoal(PathfinderMob& m, double s) : mob(m), speed(s) { flags = GF_MOVE; }
    bool can_use() override {
        if (mob.target() || !is_day() || !mob.is_on_fire()) return false;
        if (!can_see_sky(mob.block_x(), mob.block_y(), mob.block_z()) || !mob.item_in_slot(ES_HEAD).empty()) return false;
        int bx = mob.block_x(), by = mob.block_y(), bz = mob.block_z();
        for (int i = 0; i < 10; i++) {
            int x = bx + rand_int(20) - 10, y = by + rand_int(6) - 3, z = bz + rand_int(20) - 10;
            if (!can_see_sky(x, y, z) && mob.walk_target_value(x, y, z) < 0.0f) { wx = x + 0.5, wy = y, wz = z + 0.5; return true; }
        }
        return false;
    }
    bool can_continue_to_use() override { return !mob.navigation->is_done(); }
    void start() override { mob.navigation->move_to(wx, wy, wz, speed); }
};
struct AvoidEntityGoal : Goal {
    PathfinderMob& mob;
    double walk, sprint;
    float max_dist;
    int avoid = 0;
    std::unique_ptr<Path> path;
    std::function<bool(LivingEntity&)> type_ok;
    AvoidEntityGoal(PathfinderMob& m, std::function<bool(LivingEntity&)> t, float d, double w, double s)
        : mob(m), walk(w), sprint(s), max_dist(d), type_ok(std::move(t)) { flags = GF_MOVE; }
    bool can_use() override {
        auto tc = TargetingConditions::for_combat();
        tc.set_range(max_dist);
        tc.selector = [](LivingEntity& e) { return !(e.as_player() && (e.is_creative() || e.is_spectator())); };
        LivingEntity* a = nearest_entity(mob.bb.inflate(max_dist, 3.0, max_dist), type_ok, tc, &mob, mob.pos.x, mob.pos.y, mob.pos.z);
        if (!a) return false;
        auto p = randpos::default_pos_away(mob, 16, 7, a->pos);
        if (!p || a->dist_sqr(p->x, p->y, p->z) < a->dist_sqr(mob)) return false;
        avoid = a->id;
        path = mob.navigation->create_path(mth::floor(p->x), mth::floor(p->y), mth::floor(p->z), 0);
        return path != nullptr;
    }
    bool can_continue_to_use() override { return !mob.navigation->is_done(); }
    void start() override { mob.navigation->move_to(std::move(path), walk); }
    void stop() override { avoid = 0; }
    void tick() override {
        Entity* a = entity_by_id(avoid);
        mob.navigation->set_speed_modifier(a && mob.dist_sqr(*a) < 49.0 ? sprint : walk);
    }
};
struct LeapAtTargetGoal : Goal {
    Mob& mob;
    float yd;
    int target = 0;
    LeapAtTargetGoal(Mob& m, float y) : mob(m), yd(y) { flags = GF_JUMP | GF_MOVE; }
    bool can_use() override {
        LivingEntity* t = mob.target();
        if (!t) return false;
        target = t->id;
        double d = mob.dist_sqr(*t);
        if (d < 4.0 || d > 16.0) return false;
        return mob.on_ground && rand_int(reduced_tick_delay(5)) == 0;
    }
    bool can_continue_to_use() override { return !mob.on_ground; }
    void start() override {
        Entity* t = entity_by_id(target);
        if (!t) return;
        Vec3 d{t->pos.x - mob.pos.x, 0.0, t->pos.z - mob.pos.z};
        if (d.length_sqr() > 1.0E-7) d = d.normalize() * 0.4 + mob.vel * 0.2;
        mob.vel = {d.x, yd, d.z};
    }
};
inline bool is_type_fn(LivingEntity& e, int t) { return e.type == t; }
