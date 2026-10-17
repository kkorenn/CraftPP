#pragma once
// Method bodies for mob.hpp / ai/path.hpp (Mob, controls, navigation, A* pathfinding, WalkNodeEvaluator).
#include "mob.hpp"

// ---------- TargetingConditions ----------
inline bool TargetingConditions::test(LivingEntity* targeter, LivingEntity& target) const {
    if (targeter == &target || !target.can_be_seen_by_anyone()) return false;
    if (selector && !selector(target)) return false;
    if (!targeter) return !combat || (target.can_be_seen_as_enemy() && difficulty() != 0);
    if (combat && (!targeter->can_attack(target) || targeter->is_allied_to(target))) return false;
    if (range > 0.0) {
        double mod = test_invisible ? target.visibility_percent(targeter) : 1.0;
        double vis = std::max(range * mod, 2.0);
        if (targeter->dist_sqr(target.pos.x, target.pos.y, target.pos.z) > vis * vis) return false;
    }
    if (los)
        if (Mob* m = targeter->as_mob(); m && !m->sensing.has_line_of_sight(*m, target)) return false;
    return true;
}

// ---------- controls ----------
inline void MoveControl::tick() {
    if (op == STRAFE) {
        float sp = float(mob.attr(attr::movement_speed)), spm = float(speed_mod) * sp;
        float xa = strafe_fwd, za = strafe_right;
        float d = std::max(1.0f, std::sqrt(xa * xa + za * za));
        d = spm / d;
        xa *= d, za *= d;
        float s = mth::sin(mob.yaw * mth::DEG), c = mth::cos(mob.yaw * mth::DEG);
        float dx = xa * c - za * s, dz = za * c + xa * s;
        if (mob.navigation && mob.navigation->eval &&
            path_type_static(mth::floor(mob.pos.x + dx), mob.block_y(), mth::floor(mob.pos.z + dz)) != PT_WALKABLE)
            strafe_fwd = 1.0f, strafe_right = 0.0f;
        mob.speed = spm;
        mob.zza = strafe_fwd;
        mob.xxa = strafe_right;
        op = WAIT;
    } else if (op == MOVE_TO) {
        op = WAIT;
        double xd = wx - mob.pos.x, zd = wz - mob.pos.z, yd = wy - mob.pos.y;
        if (xd * xd + yd * yd + zd * zd < 2.5000003E-7f) { mob.zza = 0; return; }
        float yr = float(std::atan2(zd, xd) * 180.0f / float(M_PI)) - 90.0f;
        mob.yaw = rotlerp(mob.yaw, yr, 90.0f);
        mob.set_speed(float(speed_mod * mob.attr(attr::movement_speed)));
        int bx = mob.block_x(), by = mob.block_y(), bz = mob.block_z();
        int s = block_at(bx, by, bz);
        double top = 0;
        for (const AABB* b = shape_begin(s); b != shape_end(s); b++) top = std::max(top, b->y1);
        if ((yd > mob.max_up_step() && xd * xd + zd * zd < std::max(1.0f, mob.width)) ||
            (has_collision(s) && mob.pos.y < top + by && !block_in_tag(s, "doors") && !block_in_tag(s, "fences"))) {
            mob.jump_control->jump();
            op = JUMPING;
        }
    } else if (op == JUMPING) {
        mob.set_speed(float(speed_mod * mob.attr(attr::movement_speed)));
        if (mob.on_ground || (mob.is_in_liquid() && mob.is_affected_by_fluids())) op = WAIT;
    } else {
        mob.zza = 0;
    }
}
inline void LookControl::set_look_at(double x, double y, double z) { set_look_at(x, y, z, float(mob.head_rot_speed()), float(mob.max_head_x_rot())); }
inline std::optional<float> LookControl::x_rot_d() const {
    double xd = wx - mob.pos.x, yd = wy - mob.eye_y(), zd = wz - mob.pos.z;
    double sd = std::sqrt(xd * xd + zd * zd);
    if (!(std::abs(yd) > 1.0E-5f) && !(std::abs(sd) > 1.0E-5f)) return std::nullopt;
    return float(-(std::atan2(yd, sd) * 180.0f / float(M_PI)));
}
inline std::optional<float> LookControl::y_rot_d() const {
    double xd = wx - mob.pos.x, zd = wz - mob.pos.z;
    if (!(std::abs(zd) > 1.0E-5f) && !(std::abs(xd) > 1.0E-5f)) return std::nullopt;
    return float(std::atan2(zd, xd) * 180.0f / float(M_PI)) - 90.0f;
}
inline void LookControl::tick() {
    if (reset_x_rot_on_tick()) mob.pitch = 0.0f;
    if (cooldown > 0) {
        cooldown--;
        if (auto y = y_rot_d()) mob.y_head_rot = rotate_towards(mob.y_head_rot, *y, y_max);
        if (auto x = x_rot_d()) mob.pitch = rotate_towards(mob.pitch, *x, x_max);
    } else {
        mob.y_head_rot = rotate_towards(mob.y_head_rot, mob.y_body_rot, 10.0f);
    }
    if (!mob.navigation->is_done()) mob.y_head_rot = rotate_if_necessary(mob.y_head_rot, mob.y_body_rot, float(mob.max_head_y_rot()));
}
inline void JumpControl::tick() { mob.set_jumping(jump_); jump_ = false; }
inline void BodyRotationControl::client_tick() {
    double xd = mob.pos.x - mob.old.x, zd = mob.pos.z - mob.old.z;
    float mh = float(mob.max_head_y_rot());
    if (xd * xd + zd * zd > 2.5000003E-7f) {
        mob.y_body_rot = mob.yaw;
        mob.y_head_rot = rotate_if_necessary(mob.y_head_rot, mob.y_body_rot, mh);
        last_stable_head = mob.y_head_rot;
        head_stable = 0;
    } else if (std::abs(mob.y_head_rot - last_stable_head) > 15.0f) {
        head_stable = 0;
        last_stable_head = mob.y_head_rot;
        mob.y_body_rot = rotate_if_necessary(mob.y_body_rot, mob.y_head_rot, mh);
    } else if (++head_stable > 10) {
        float frac = std::clamp(float(head_stable - 10) / 10.0f, 0.0f, 1.0f);
        mob.y_body_rot = rotate_if_necessary(mob.y_body_rot, mob.y_head_rot, mh * (1.0f - frac));
    }
}

// ---------- navigation ----------
inline float PathNavigation::max_path_length() const { return std::max(float(mob.attr(attr::follow_range)), required_path_length); }
inline void PathNavigation::init_finder() {
    finder = std::make_unique<PathFinder>(eval.get(), mth::floor(mob.attr_base(attr::follow_range) * 16.0));
}
inline void PathNavigation::recompute() {
    if (server_tick - last_recompute <= 20 || !can_update_path()) { delayed_recompute = true; return; }
    if (!has_target) return;
    path.reset();
    path = create_path(tx, ty, tz, reach);
    last_recompute = server_tick;
    delayed_recompute = false;
}
inline std::unique_ptr<Path> PathNavigation::create_path(int x, int y, int z, int r) {
    return create_path_impl({{x, y, z}}, 8, false, r, max_path_length());
}
inline std::unique_ptr<Path> PathNavigation::create_path_to(Entity& e, int r) {
    return create_path_impl({{e.block_x(), e.block_y(), e.block_z()}}, 16, true, r, max_path_length());
}
inline std::unique_ptr<Path> PathNavigation::create_path_impl(const std::vector<std::array<int, 3>>& targets, int, bool above,
                                                             int r, float max_len) {
    if (targets.empty() || mob.pos.y < MIN_Y || !can_update_path()) return nullptr;
    if (path && !path->done() && has_target)
        for (auto& t : targets)
            if (t[0] == tx && t[1] == ty && t[2] == tz) return std::make_unique<Path>(*path);
    if (path_budget_left <= 0) { path_budget_misses++; return nullptr; }
    (void)above;  // ponytail: the search region is unbounded (PathNavigationRegion clamps to maxLen + offset)
    auto p = finder->find(mob, targets, max_len, r, max_visited_mult);
    if (p) {
        has_target = true;
        tx = p->tx, ty = p->ty, tz = p->tz;
        reach = r;
        reset_stuck_timeout();
    }
    return p;
}
inline bool PathNavigation::move_to(std::unique_ptr<Path> np, double sp) {
    if (!np) { path.reset(); return false; }
    if (!np->same_as(path.get())) path = std::move(np);
    if (is_done()) return false;
    trim_path();
    if (path->count() <= 0) return false;
    speed_mod = sp;
    last_stuck_check = tick_;
    last_stuck_pos = temp_mob_pos();
    return true;
}
inline double PathNavigation::ground_y(const Vec3& t) {
    int x = mth::floor(t.x), y = mth::floor(t.y), z = mth::floor(t.z);
    return sflag(block_at(x, y - 1, z), SF_AIR) ? t.y : floor_level(x, y, z);
}
inline void PathNavigation::tick() {
    tick_++;
    if (delayed_recompute) recompute();
    if (is_done()) return;
    if (can_update_path()) follow_the_path();
    else if (path && !path->done()) {
        Vec3 mp = temp_mob_pos(), p = path->next_entity_pos(mob);
        if (mp.y > p.y && !mob.on_ground && mth::floor(mp.x) == mth::floor(p.x) && mth::floor(mp.z) == mth::floor(p.z)) path->advance();
    }
    if (!is_done()) {
        Vec3 t = path->next_entity_pos(mob);
        mob.move_control->set_wanted(t.x, ground_y(t), t.z, speed_mod);
    }
}
inline void PathNavigation::follow_the_path() {
    Vec3 mp = temp_mob_pos();
    max_dist_to_waypoint = mob.width > 0.75f ? mob.width / 2.0f : 0.75f - mob.width / 2.0f;
    const PathPoint& n = path->next_node();
    double xd = std::abs(mob.pos.x - (n.x + 0.5)), yd = std::abs(mob.pos.y - n.y), zd = std::abs(mob.pos.z - (n.z + 0.5));
    bool close = xd < max_dist_to_waypoint && zd < max_dist_to_waypoint && yd < 1.0f;
    if (close || (can_cut_corner(n.type) && should_target_next_node(mp))) path->advance();
    do_stuck_detection(mp);
}
inline bool PathNavigation::should_target_next_node(const Vec3& mp) {
    if (path->next + 1 >= path->count()) return false;
    const PathPoint& c = path->next_node();
    Vec3 cur{c.x + 0.5, double(c.y), c.z + 0.5};
    if (!(mp.dist_sqr(cur) < 4.0)) return false;
    if (can_move_directly(mp, path->next_entity_pos(mob))) return true;
    const PathPoint& nn = path->nodes[size_t(path->next + 1)];
    Vec3 nxt{nn.x + 0.5, double(nn.y), nn.z + 0.5};
    Vec3 to_cur = cur - mp, to_next = nxt - mp;
    double a = to_cur.length_sqr(), b = to_next.length_sqr();
    if (!(b < a) && !(a < 0.5)) return false;
    return to_next.normalize().dot(to_cur.normalize()) < 0.0;
}
inline void PathNavigation::do_stuck_detection(const Vec3& mp) {
    if (tick_ - last_stuck_check > 100) {
        float es = mob.speed >= 1.0f ? mob.speed : mob.speed * mob.speed;
        float th = es * 100.0f * 0.25f;
        if (mp.dist_sqr(last_stuck_pos) < th * th) { stuck = true; stop(); }
        else stuck = false;
        last_stuck_check = tick_;
        last_stuck_pos = mp;
    }
    if (path && !path->done()) {
        const PathPoint& n = path->next_node();
        int64_t key = mth::block_key(n.x, n.y, n.z);
        int64_t now = server_tick;
        if (key == timeout_node) timeout_timer += now - last_timeout_check;
        else {
            timeout_node = key;
            double d = (mp - Vec3{n.x + 0.5, double(n.y), n.z + 0.5}).length();
            timeout_limit = mob.speed > 0.0f ? d / mob.speed * 20.0 : 0.0;
        }
        if (timeout_limit > 0.0 && double(timeout_timer) > timeout_limit * 3.0) { reset_stuck_timeout(); stop(); }
        last_timeout_check = now;
    }
}
inline bool PathNavigation::should_recompute_path(int x, int y, int z) const {
    if (delayed_recompute || !path || path->done() || path->count() == 0) return false;
    const PathPoint* t = path->end();
    Vec3 mid{(t->x + mob.pos.x) / 2.0, (t->y + mob.pos.y) / 2.0, (t->z + mob.pos.z) / 2.0};
    double r = path->count() - path->next;
    return Vec3{x + 0.5, y + 0.5, z + 0.5}.dist_sqr(mid) < r * r;
}
inline bool GroundPathNavigation::can_update_path() { return mob.on_ground || mob.is_in_liquid(); }
inline int GroundPathNavigation::surface_y() {
    if (mob.is_in_water() && can_float()) {
        int s = mob.block_y();
        int st = block_at(mob.block_x(), s, mob.block_z());
        int steps = 0;
        while (fluid_type(st) == FLUID_WATER) {
            st = block_at(mob.block_x(), ++s, mob.block_z());
            if (++steps > 16) return mob.block_y();
        }
        return s;
    }
    return mth::floor(mob.pos.y + 0.5);
}
inline Vec3 GroundPathNavigation::temp_mob_pos() { return {mob.pos.x, double(surface_y()), mob.pos.z}; }
inline std::unique_ptr<Path> GroundPathNavigation::create_path(int x, int y, int z, int r) {
    if (!loaded_chunk(x >> 4, z >> 4)) return nullptr;
    if (!can_path_below_surface) {  // findSurfacePosition
        if (sflag(block_at(x, y, z), SF_AIR)) {
            int cy = y - 1;
            while (cy >= MIN_Y && sflag(block_at(x, cy, z), SF_AIR)) cy--;
            if (cy >= MIN_Y) y = cy + 1;
            else {
                cy = y + 1;
                while (cy < MIN_Y + HEIGHT && sflag(block_at(x, cy, z), SF_AIR)) cy++;
                y = cy;
            }
        }
        if (sflag(block_at(x, y, z), SF_SOLID)) {
            int cy = y + 1;
            while (cy < MIN_Y + HEIGHT && sflag(block_at(x, cy, z), SF_SOLID)) cy++;
            y = cy;
        }
    }
    return PathNavigation::create_path(x, y, z, r);
}
inline void GroundPathNavigation::trim_path() {
    if (!avoid_sun || !path) return;
    if (can_see_sky(mob.block_x(), mth::floor(mob.pos.y + 0.5), mob.block_z())) return;
    for (int i = 0; i < path->count(); i++) {
        auto& n = path->nodes[size_t(i)];
        if (can_see_sky(n.x, n.y, n.z)) { path->truncate(i); return; }
    }
}

// ---------- node evaluation ----------
inline void NodeEvaluator::prepare(Mob& m) {
    mob = &m;
    pool.clear();
    by_hash.clear();
    type_cache.clear();
    entity_width = mth::floor(m.width + 1.0f);
    entity_height = mth::floor(m.height + 1.0f);
    entity_depth = mth::floor(m.width + 1.0f);
}
inline bool WalkNodeEvaluator::can_start_at(int x, int y, int z) {
    uint8_t t = cached_type(x, y, z);
    return t != PT_OPEN && mob->pathfinding_malus(t) >= 0.0f;
}
inline int WalkNodeEvaluator::start() {
    Mob& m = *mob;
    int sy = m.block_y();
    int st = block_at(m.block_x(), sy, m.block_z());
    if (can_float && m.is_in_water()) {
        while (fluid_type(st) == FLUID_WATER) st = block_at(m.block_x(), ++sy, m.block_z());
        sy--;
    } else if (m.on_ground) {
        sy = mth::floor(m.pos.y + 0.5);
    } else {
        int y = mth::floor(m.pos.y + 1.0);
        while (y > MIN_Y) {
            sy = y;
            y--;
            int b = block_at(m.block_x(), y, m.block_z());
            if (!sflag(b, SF_AIR) && state_path_type[size_t(b)] == PT_BLOCKED) break;
        }
    }
    int bx = m.block_x(), bz = m.block_z();
    if (!can_start_at(bx, sy, bz)) {
        int cs[4][2] = {{mth::floor(m.bb.x0), mth::floor(m.bb.z0)}, {mth::floor(m.bb.x0), mth::floor(m.bb.z1)},
                        {mth::floor(m.bb.x1), mth::floor(m.bb.z0)}, {mth::floor(m.bb.x1), mth::floor(m.bb.z1)}};
        for (auto& c : cs)
            if (can_start_at(c[0], sy, c[1])) { bx = c[0], bz = c[1]; break; }
    }
    int n = node(bx, sy, bz);
    at(n).type = cached_type(bx, sy, bz);
    at(n).cost_malus = m.pathfinding_malus(at(n).type);
    return n;
}
inline double WalkNodeEvaluator::node_floor(int x, int y, int z) {
    if ((can_float || is_amphibious()) && fluid_type(block_at(x, y, z)) == FLUID_WATER) return y + 0.5;
    return floor_level(x, y, z);
}
inline bool WalkNodeEvaluator::can_reach_without_collision(const PNode& to) {
    AABB b = mob->bb;
    Vec3 d{to.x - mob->pos.x + b.xsize() / 2.0, to.y - mob->pos.y + b.ysize() / 2.0, to.z - mob->pos.z + b.zsize() / 2.0};
    double size = (b.xsize() + b.ysize() + b.zsize()) / 3.0;
    int steps = int(std::ceil(d.length() / size));
    d = d * (1.0 / steps);
    for (int i = 1; i <= steps; i++) {
        b = b.move(d);
        if (has_collisions(b)) return false;
    }
    return true;
}
inline int WalkNodeEvaluator::try_jump_on(int x, int y, int z, int jump, double nh, int dx, int dz, uint8_t cur) {
    int above = find_accepted(x, y + 1, z, jump - 1, nh, dx, dz, cur);
    if (above < 0) return -1;
    if (mob->width >= 1.0f) return above;
    uint8_t at_t = at(above).type;
    if (at_t != PT_OPEN && at_t != PT_WALKABLE) return above;
    double cx = x - dx + 0.5, cz = z - dz + 0.5, hw = mob->width / 2.0;
    AABB grow{cx - hw, floor_level(mth::floor(cx), y + 1, mth::floor(cz)) + 0.001, cz - hw, cx + hw,
              mob->height + floor_level(at(above).x, at(above).y, at(above).z) - 0.002, cz + hw};
    return has_collisions(grow) ? -1 : above;
}
inline int WalkNodeEvaluator::find_accepted(int x, int y, int z, int jump, double nh, int dx, int dz, uint8_t cur) {
    int best = -1;
    double maxy = node_floor(x, y, z);
    if (maxy - nh > std::max(1.125, double(mob->max_up_step()))) return -1;
    uint8_t t = cached_type(x, y, z);
    float cost = mob->pathfinding_malus(t);
    auto get_update = [&](int xx, int yy, int zz, uint8_t tt, float c) {
        int n = node(xx, yy, zz);
        at(n).type = tt;
        at(n).cost_malus = std::max(at(n).cost_malus, c);
        return n;
    };
    auto partial = [](uint8_t pt) { return pt == PT_FENCE || pt == PT_DOOR_WOOD_CLOSED || pt == PT_DOOR_IRON_CLOSED; };
    if (cost >= 0.0f) best = get_update(x, y, z, t, cost);
    if (partial(cur) && best >= 0 && at(best).cost_malus >= 0.0f && !can_reach_without_collision(at(best))) best = -1;
    if (t == PT_WALKABLE || (is_amphibious() && t == PT_WATER)) return best;
    if ((best < 0 || at(best).cost_malus < 0.0f) && jump > 0 && (t != PT_FENCE || can_walk_over_fences) &&
        t != PT_UNPASSABLE_RAIL && t != PT_TRAPDOOR && t != PT_POWDER_SNOW) {
        best = try_jump_on(x, y, z, jump, nh, dx, dz, cur);
    } else if (!is_amphibious() && t == PT_WATER && !can_float) {
        for (int yy = y - 1; yy > MIN_Y; yy--) {  // tryFindFirstNonWaterBelow
            uint8_t lt = cached_type(x, yy, z);
            if (lt != PT_WATER) return best;
            best = get_update(x, yy, z, lt, mob->pathfinding_malus(lt));
        }
    } else if (t == PT_OPEN) {  // tryFindFirstGroundNodeBelow
        for (int cy = y - 1; cy >= MIN_Y; cy--) {
            if (y - cy > mob->max_fall_distance()) {
                int n = node(x, cy, z);
                at(n).type = PT_BLOCKED, at(n).cost_malus = -1.0f;
                return n;
            }
            uint8_t lt = cached_type(x, cy, z);
            float lc = mob->pathfinding_malus(lt);
            if (lt != PT_OPEN) {
                if (lc >= 0.0f) return get_update(x, cy, z, lt, lc);
                int n = node(x, cy, z);
                at(n).type = PT_BLOCKED, at(n).cost_malus = -1.0f;
                return n;
            }
        }
        int n = node(x, y, z);
        at(n).type = PT_BLOCKED, at(n).cost_malus = -1.0f;
        return n;
    } else if (partial(t) && best < 0) {
        int n = node(x, y, z);
        at(n).closed = true, at(n).type = t, at(n).cost_malus = path_malus_default[t];
        best = n;
    }
    return best;
}
inline int WalkNodeEvaluator::neighbors(int* out, int cur) {
    int p = 0, jump = 0;
    PNode c = at(cur);
    uint8_t above = cached_type(c.x, c.y + 1, c.z), here = cached_type(c.x, c.y, c.z);
    if (mob->pathfinding_malus(above) >= 0.0f && here != PT_STICKY_HONEY) jump = mth::floor(std::max(1.0f, mob->max_up_step()));
    double ph = node_floor(c.x, c.y, c.z);
    static const int dirs[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};  // N E S W (Plane.HORIZONTAL)
    static const int d2[4] = {2, 3, 0, 1};                             // get2DDataValue
    for (int i = 0; i < 4; i++) {
        int n = find_accepted(c.x + dirs[i][0], c.y, c.z + dirs[i][1], jump, ph, dirs[i][0], dirs[i][1], here);
        reuse[d2[i]] = n;
        if (n >= 0 && !at(n).closed && (at(n).cost_malus >= 0.0f || at(cur).cost_malus < 0.0f)) out[p++] = n;
    }
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;  // getClockWise
        int ew = reuse[d2[i]], ns = reuse[d2[j]];
        if (ns < 0 || ew < 0 || at(ns).y > c.y || at(ew).y > c.y) continue;
        if (at(ew).type == PT_WALKABLE_DOOR || at(ns).type == PT_WALKABLE_DOOR) continue;
        if (mob->width > 1.0f && (at(ew).cost_malus > 0.0f || at(ns).cost_malus > 0.0f)) continue;
        bool posts = at(ns).type == PT_FENCE && at(ew).type == PT_FENCE && mob->width < 0.5;
        if (!((at(ns).y < c.y || at(ns).cost_malus >= 0.0f || posts) && (at(ew).y < c.y || at(ew).cost_malus >= 0.0f || posts))) continue;
        int dn = find_accepted(c.x + dirs[i][0] + dirs[j][0], c.y, c.z + dirs[i][1] + dirs[j][1], jump, ph, dirs[i][0], dirs[i][1], here);
        if (dn >= 0 && !at(dn).closed && at(dn).type != PT_WALKABLE_DOOR && at(dn).cost_malus >= 0.0f) out[p++] = dn;
    }
    return p;
}
inline uint8_t WalkNodeEvaluator::path_type_of_mob(int x, int y, int z) {  // getPathTypeOfMob
    uint8_t types[PT_COUNT];
    int nt = 0;
    int mbx = mob->block_x(), mby = mob->block_y(), mbz = mob->block_z();
    for (int dx = 0; dx < entity_width; dx++)
        for (int dy = 0; dy < entity_height; dy++)
            for (int dz = 0; dz < entity_depth; dz++) {
                uint8_t t = path_type(x + dx, y + dy, z + dz);
                if (t == PT_DOOR_WOOD_CLOSED && can_open_doors && can_pass_doors) t = PT_WALKABLE_DOOR;
                if (t == PT_DOOR_OPEN && !can_pass_doors) t = PT_BLOCKED;
                if (t == PT_RAIL && path_type(mbx, mby, mbz) != PT_RAIL && path_type(mbx, mby - 1, mbz) != PT_RAIL) t = PT_UNPASSABLE_RAIL;
                bool dup = false;
                for (int k = 0; k < nt; k++) dup |= types[k] == t;
                if (!dup) types[nt++] = t;
            }
    if (nt == 1) return types[0];
    for (int k = 0; k < nt; k++) if (types[k] == PT_FENCE) return PT_FENCE;
    for (int k = 0; k < nt; k++) if (types[k] == PT_UNPASSABLE_RAIL) return PT_UNPASSABLE_RAIL;
    std::sort(types, types + nt);  // EnumSet iteration order
    uint8_t best = PT_BLOCKED;
    float best_m = mob->pathfinding_malus(best);
    for (int k = 0; k < nt; k++) {
        float m = mob->pathfinding_malus(types[k]);
        if (m < 0.0f) return types[k];
        if (m >= best_m) best_m = m, best = types[k];
    }
    uint8_t curt = path_type(x, y, z);
    if (entity_width > 1) {
        bool cheaper = mob->pathfinding_malus(curt) < best_m;
        return cheaper && mob->pathfinding_malus(PT_BIG_MOBS_CLOSE_TO_DANGER) < best_m ? uint8_t(PT_BIG_MOBS_CLOSE_TO_DANGER) : best;
    }
    return curt == PT_OPEN && best != PT_OPEN && best_m == 0.0f ? uint8_t(PT_OPEN) : best;
}

// ---------- A* ----------
inline std::unique_ptr<Path> PathFinder::find(Mob& m, const std::vector<std::array<int, 3>>& tgts, float max_len, int reach, float mult) {
    path_searches_total++;
    heap.clear();
    eval->prepare(m);
    int from = eval->start();
    if (from < 0) { eval->done(); return nullptr; }
    struct T { int node; float best_h = 3.4028235E38f; int best = -1; bool reached = false; std::array<int, 3> pos; };
    std::vector<T> ts;
    for (auto& t : tgts) ts.push_back({eval->target(t[0], t[1], t[2]), 3.4028235E38f, -1, false, t});
    auto best_h = [&](int n) {
        float b = 3.4028235E38f;
        for (auto& t : ts) {
            float h = eval->at(n).dist(eval->at(t.node));
            if (h < t.best_h) t.best_h = h, t.best = n;
            b = std::min(h, b);
        }
        return b;
    };
    eval->at(from).g = 0;
    eval->at(from).h = best_h(from);
    eval->at(from).f = eval->at(from).h;
    insert(from);
    int count = 0, limit = int(float(max_visited) * mult);
    int nb[32];
    bool any_reached = false;
    while (!heap.empty()) {
        if (++count >= limit) break;
        if (--path_budget_left < 0) break;
        int cur = pop();
        eval->at(cur).closed = true;
        for (auto& t : ts)
            if (eval->at(cur).manhattan(eval->at(t.node)) <= float(reach)) t.reached = true, any_reached = true;
        if (any_reached) break;
        if (eval->at(cur).dist(eval->at(from)) >= max_len) continue;
        int nc = eval->neighbors(nb, cur);
        for (int i = 0; i < nc; i++) {
            int n = nb[i];
            float d = eval->at(cur).dist(eval->at(n));
            eval->at(n).walked = eval->at(cur).walked + d;
            float tg = eval->at(cur).g + d + eval->at(n).cost_malus;
            if (eval->at(n).walked < max_len && (eval->at(n).heap_idx < 0 || tg < eval->at(n).g)) {
                eval->at(n).came_from = cur;
                eval->at(n).g = tg;
                eval->at(n).h = best_h(n) * 1.5f;
                if (eval->at(n).heap_idx >= 0) change_cost(n, eval->at(n).g + eval->at(n).h);
                else {
                    eval->at(n).f = eval->at(n).g + eval->at(n).h;
                    insert(n);
                }
            }
        }
    }
    path_nodes_total += count;
    std::unique_ptr<Path> best;
    for (auto& t : ts) {
        if (any_reached && !t.reached) continue;
        if (t.best < 0) continue;
        auto p = std::make_unique<Path>();
        for (int n = t.best; n >= 0; n = eval->at(n).came_from) {
            auto& nd = eval->at(n);
            p->nodes.push_back({nd.x, nd.y, nd.z, nd.type});
        }
        std::reverse(p->nodes.begin(), p->nodes.end());
        p->tx = t.pos[0], p->ty = t.pos[1], p->tz = t.pos[2];
        p->reached = t.reached;
        auto& e = p->nodes.back();
        p->dist_to_target = float(std::abs(e.x - p->tx) + std::abs(e.y - p->ty) + std::abs(e.z - p->tz));
        if (!best || (any_reached ? p->count() < best->count()
                                  : p->dist_to_target < best->dist_to_target ||
                                        (p->dist_to_target == best->dist_to_target && p->count() < best->count())))
            best = std::move(p);
    }
    eval->done();
    return best;
}

// ---------- Mob ----------
inline void Mob::init() {
    LivingEntity::init();
    move_control = std::make_unique<MoveControl>(*this);
    look_control = std::make_unique<LookControl>(*this);
    jump_control = std::make_unique<JumpControl>(*this);
    body_control = std::make_unique<BodyRotationControl>(*this);
    navigation = create_navigation();
    register_goals();
    initialized = true;
}
inline void Mob::tick() {
    LivingEntity::tick();
    if (tick_count % 5 == 0) {  // updateControlFlags (no riding yet: all enabled)
        goal_selector.set_flag(GF_MOVE, true);
        goal_selector.set_flag(GF_JUMP, true);
        goal_selector.set_flag(GF_LOOK, true);
    }
}
inline void Mob::base_tick() {
    LivingEntity::base_tick();
    if (is_alive() && rand_int(1000) < ambient_sound_time++) {
        ambient_sound_time = -ambient_sound_interval();
        play_ambient_sound();
    }
}
inline bool Mob::is_sun_burn_tick() const {  // Mob.isSunBurnTick (monsters_burn: day timeline keyframes 23460..12542)
    int64_t t = ((clocks[0].total % 24000) + 24000) % 24000;
    if (!(t >= 23460 || t < 12542)) return false;
    float br = light_magic_value();
    if (br > 0.5f && rand_float() * 30.0f < (br - 0.4f) * 2.0f && !is_in_water_or_rain())
        return can_see_sky(block_x(), mth::floor(eye_y()), block_z());
    return false;
}
inline void Mob::ai_step() {
    LivingEntity::ai_step();
    if (in_tag("burn_in_daylight") && is_alive() && is_sun_burn_tick()) {  // burnUndead
        int slot = sun_protection_slot();
        ItemStack& helm = equipment[size_t(slot)];
        if (!helm.empty()) {
            int maxd = item_info[size_t(helm.item)].max_damage;
            if (maxd > 0) {
                helm.damage += rand_int(2);
                if (helm.damage >= maxd) { set_item_slot(slot, {}); broadcast_event(47 + slot - ES_MAINHAND); }
            }
        } else {
            ignite_for_ticks(8 * 20);
        }
    }
}
inline void Mob::server_ai_step() {
    no_action_time++;
    if (no_ai()) return;
    sensing.tick();
    if ((tick_count + id) % 2 != 0 && tick_count > 1) {
        target_selector.tick_running(false);
        goal_selector.tick_running(false);
    } else {
        target_selector.tick();
        goal_selector.tick();
    }
    navigation->tick();
    custom_server_ai_step();
    move_control->tick();
    look_control->tick();
    jump_control->tick();
}
inline void Mob::check_despawn() {  // Mob.checkDespawn
    if (difficulty() == 0 && !info().peaceful) { discard(); return; }
    bool persistent = is_persistence_required() || requires_custom_persistence();
    Entity* nearest = nullptr;
    double best = 1e300;
    for (Entity* p : player_entities()) {
        if (p->is_spectator()) continue;
        double d = p->dist_sqr(*this);
        if (d < best) best = d, nearest = p;
    }
    if (!nearest) return;
    const auto& cat = mob_categories[size_t(info().cat)];
    if (!persistent && best > double(cat.despawn) * cat.despawn && remove_when_far_away(best)) { discard(); return; }
    double nd = double(cat.no_despawn) * cat.no_despawn;
    if (!persistent && no_action_time > 600 && rand_int(800) == 0 && best > nd && remove_when_far_away(best)) discard();
    else if (best < nd) no_action_time = 0;
}
inline SpawnGroupData* Mob::finalize_spawn(SpawnReason, SpawnGroupData* g) {  // Mob.finalizeSpawn
    if (!has_attr_modifier(attr::follow_range, "minecraft:random_spawn_bonus"))
        add_attr_modifier(attr::follow_range, "minecraft:random_spawn_bonus", rand_triangle(0.0, 0.11485000000000001), 1);
    set_left_handed(rand_float() < 0.05f);
    return g;
}
inline bool Mob::do_hurt_target(Entity& target) {  // Mob.doHurtTarget
    float dmg = float(attr(attr::attack_damage));
    const ItemStack weapon = main_hand();
    DamageSource s = DamageSource::of("mob_attack", this);
    dmg = enchant_modify_damage(weapon, &target, s, dmg);  // ponytail: item attack damage bonus (mace) not applied
    bool hurt = target.hurt_server(s, dmg);
    if (hurt) {
        cause_extra_knockback(target, attack_knockback(target));
        enchant_post_attack(target, s, &weapon);  // doPostAttackEffects (fire aspect, the victim's thorns)
        set_last_hurt_mob(target);
    }
    return hurt;
}
inline float enchant_equipment_drop_chance(LivingEntity& victim, const DamageSource& s, float chance);
inline bool enchant_prevents_drop(const ItemStack& it);
inline void Mob::drop_custom_death_loot(const DamageSource& src, bool player_killed) {  // Mob.dropCustomDeathLoot
    for (int s = 0; s < 8; s++) {
        ItemStack& it = equipment[size_t(s)];
        float ch = drop_chances[size_t(s)];
        if (ch == 0.0f) continue;
        bool preserve = ch > 1.0f;
        if (src.causing && src.causing->as_living()) ch = enchant_equipment_drop_chance(*this, src, ch);  // looting
        if (it.empty() || enchant_prevents_drop(it)) continue;
        if ((player_killed || preserve) && rand_float() < ch) {
            ItemStack d = it;
            int maxd = item_info[size_t(d.item)].max_damage;
            if (!preserve && maxd > 0)
                d.damage = maxd - rand_int(1 + rand_int(std::max(maxd - 3, 1)));
            spawn_at_location(d);
            it = {};
        }
    }
}
inline int Mob::base_experience_reward() {
    if (xp_reward <= 0) return xp_reward;
    int r = xp_reward;
    for (int s = 0; s < 8; s++)
        if (s != ES_BODY && s != ES_SADDLE && !equipment[size_t(s)].empty() && drop_chances[size_t(s)] <= 1.0f) r += 1 + rand_int(3);
    return r;
}
inline int Mob::interact(Player& p, int hand) {  // Mob.interact (name tags / spawn eggs: ponytail)
    if (!is_alive()) return IR_PASS;
    return mob_interact(p, hand);
}
inline void Mob::save_extra(Nbt& o) const {
    LivingEntity::save_extra(o);
    o.set("CanPickUpLoot", Nbt::num(Nbt::Byte, can_pick_up_loot_));
    o.set("PersistenceRequired", Nbt::num(Nbt::Byte, persistence_required));
    o.set("LeftHanded", Nbt::num(Nbt::Byte, left_handed()));
    if (no_ai()) o.set("NoAI", Nbt::num(Nbt::Byte, 1));
    if (home_radius >= 0) {
        o.set("home_radius", Nbt::num(Nbt::Int, home_radius));
        Nbt hp;
        hp.t = Nbt::IntArray;
        hp.arr = {home_x, home_y, home_z};
        o.set("home_pos", hp);
    }
}
inline void Mob::load_extra(const Nbt& in) {
    LivingEntity::load_extra(in);
    can_pick_up_loot_ = nbt_num(in, "CanPickUpLoot", 0) != 0;
    persistence_required = nbt_num(in, "PersistenceRequired", 0) != 0;
    set_left_handed(nbt_num(in, "LeftHanded", 0) != 0);
    set_no_ai(nbt_num(in, "NoAI", 0) != 0);
    home_radius = int(nbt_num(in, "home_radius", -1));
    if (const Nbt* h = in.get("home_pos"); h && h->t == Nbt::IntArray && h->arr.size() == 3)
        home_x = int(h->arr[0]), home_y = int(h->arr[1]), home_z = int(h->arr[2]);
}

// ---------- AgeableMob / Animal ----------
inline SpawnGroupData* AgeableMob::finalize_spawn(SpawnReason r, SpawnGroupData* g) {
    static AgeableMobGroupData fallback(true);
    auto* a = dynamic_cast<AgeableMobGroupData*>(g);
    if (!a) {
        fallback = AgeableMobGroupData(true);
        a = &fallback;
    }
    if (a->spawn_baby && a->group_size > 0 && rand_float() <= a->baby_chance) set_age(-24000);
    a->group_size++;
    return PathfinderMob::finalize_spawn(r, a);
}
inline void AgeableMob::ai_step() {
    PathfinderMob::ai_step();
    if (forced_age_timer > 0) forced_age_timer--;
    if (is_alive()) {
        if (can_age_up()) set_age(age + 1);
        else if (age > 0) set_age(age - 1);
    }
}
inline void AgeableMob::save_extra(Nbt& o) const {
    PathfinderMob::save_extra(o);
    o.set("Age", Nbt::num(Nbt::Int, age));
    o.set("ForcedAge", Nbt::num(Nbt::Int, forced_age));
    o.set("AgeLocked", Nbt::num(Nbt::Byte, age_locked()));
}
inline void AgeableMob::load_extra(const Nbt& in) {
    PathfinderMob::load_extra(in);
    set_age(int(nbt_num(in, "Age", 0)));
    forced_age = int(nbt_num(in, "ForcedAge", 0));
    if (nbt_num(in, "AgeLocked", 0)) data.set_bool(edata::AgeableMob::AGE_LOCKED, true);
}
inline void use_player_item(Player& p, int hand) {  // Animal.usePlayerItem: consume one unless creative
    if (p.gamemode == 1) return;
    ItemStack& s = p.inv[hand ? 40u : size_t(p.selected)];
    if (--s.count <= 0) s = {};
}
inline int Animal::mob_interact(Player& p, int hand) {
    const ItemStack& s = p.inv[hand ? 40u : size_t(p.selected)];
    if (is_food(s)) {
        if (age == 0 && can_fall_in_love()) {
            use_player_item(p, hand);
            set_in_love(&p);
            play_eating_sound();
            return IR_SUCCESS;
        }
        if (can_age_up()) {
            use_player_item(p, hand);
            age_up(int(float(-age / 20) * 0.1f), true);  // getSpeedUpSecondsWhenFeeding
            play_eating_sound();
            return IR_SUCCESS;
        }
    }
    return AgeableMob::mob_interact(p, hand);
}
inline void Animal::spawn_child_from_breeding(Animal& partner) {
    auto up = breed_offspring(partner);
    if (!up) return;
    if (auto* ac = dynamic_cast<AgeableMob*>(up.get())) ac->set_baby(true);
    up->snap_to(pos.x, pos.y, pos.z, 0, 0);
    add_entity(std::move(up));
    set_age(6000);
    partner.set_age(6000);
    in_love = 0, partner.in_love = 0;
    broadcast_event(18);
    if (rule("mob_drops")) spawn_xp_orbs(pos.x, pos.y, pos.z, rand_int(7) + 1);
}
