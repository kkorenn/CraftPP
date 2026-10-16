#pragma once
// Portals (tick thread; block code runs in cur_level through `world`):
//  - nether portals: PortalShape (frame search, ignition by fire, collapse when the frame breaks), flint and steel
//    (FlintAndSteelItem / BaseFireBlock), travel (NetherPortalBlock.getPortalDestination, PortalForcer search +
//    creation, BlockUtil.getLargestRectangleAround, PortalShape.getRelativePosition).
//  - end portals: eyes of ender in frames (EnderEyeItem.useOn + the frame BlockPattern), travel to the end platform
//    and back to the world spawn (EndPortalBlock), the credits (dimensions.hpp show_end_credits).
//  - standing in a portal: Entity.setAsInsidePortal / handlePortal + PortalProcessor (80 ticks survival, 0 creative).
#include "dimensions.hpp"

namespace portal {

enum Axis { X, Y, Z };
struct Pos {
    int x, y, z;
    int get(int a) const { return a == X ? x : a == Y ? y : z; }
    Pos rel(int a, int n) const { return {x + (a == X ? n : 0), y + (a == Y ? n : 0), z + (a == Z ? n : 0)}; }
};

inline uint16_t at(Pos p) { return world.get_block(p.x, p.y, p.z); }
inline bool is_fire(uint16_t s) { return block_index(s) == block_index(blocks::fire) || s == blocks::soul_fire; }
inline bool is_frame(uint16_t s) { return s == blocks::obsidian; }  // #nether_portal_frame
inline bool is_empty(uint16_t s) {                                   // PortalShape.isEmpty
    return s == blocks::air || s == blocks::cave_air || s == blocks::void_air || is_fire(s) || is_nether_portal(s);
}
inline int axis_of(uint16_t portal_state) { return portal_state == blocks::nether_portal ? X : Z; }
inline uint16_t portal_state(int axis) { return uint16_t(axis == X ? blocks::nether_portal : blocks::nether_portal + 1); }

// ---------- PortalShape ----------
struct Shape {
    int axis, right;  // rightDir: WEST (x-1) for an X portal, SOUTH (z+1) for a Z portal; `right` = its step
    Pos bottom_left;
    int width = 0, height = 0, portal_blocks = 0;
    int raxis() const { return axis == X ? X : Z; }
    Pos step(Pos p, int n) const { return p.rel(raxis(), right * n); }
    bool valid() const { return width >= 2 && width <= 21 && height >= 3 && height <= 21; }
    bool complete() const { return valid() && portal_blocks == width * height; }
};

inline int distance_until_edge_above_frame(const Shape& s, Pos p, int dir) {
    for (int w = 0; w <= 21; w++) {
        Pos q = p.rel(s.raxis(), s.right * dir * w);
        uint16_t b = at(q);
        if (!is_empty(b)) return is_frame(b) ? w : 0;
        if (!is_frame(at(q.rel(Y, -1)))) break;
    }
    return 0;
}

inline Shape find_any_shape(Pos pos, int axis) {
    Shape s{axis, axis == X ? -1 : 1, pos};
    int min_y = std::max(world.min_y(), pos.y - 21);  // calculateBottomLeft
    while (pos.y > min_y && is_empty(at(pos.rel(Y, -1)))) pos = pos.rel(Y, -1);
    int edge = distance_until_edge_above_frame(s, pos, -1) - 1;
    if (edge < 0) { s.bottom_left = pos; return s; }
    s.bottom_left = s.step(pos, -edge);
    int width = distance_until_edge_above_frame(s, s.bottom_left, 1);  // calculateWidth
    if (width < 2 || width > 21) return s;
    s.width = width;
    int h = 0;  // getDistanceUntilTop
    for (; h < 21; h++) {
        Pos row = s.bottom_left.rel(Y, h);
        if (!is_frame(at(s.step(row, -1))) || !is_frame(at(s.step(row, width)))) break;
        bool stop = false;
        for (int i = 0; i < width && !stop; i++) {
            uint16_t b = at(s.step(row, i));
            if (!is_empty(b)) stop = true;
            else if (is_nether_portal(b)) s.portal_blocks++;
        }
        if (stop) break;
    }
    bool top = h >= 3 && h <= 21;  // hasTopFrame
    for (int i = 0; top && i < width; i++) top = is_frame(at(s.step(s.bottom_left.rel(Y, h), i)));
    s.height = top ? h : 0;
    return s;
}

// findEmptyPortalShape: preferred axis first, then the other one.
inline std::optional<Shape> find_empty_shape(Pos pos, int preferred) {
    for (int axis : {preferred, preferred == X ? int(Z) : int(X)}) {
        Shape s = find_any_shape(pos, axis);
        if (s.valid() && s.portal_blocks == 0) return s;
    }
    return std::nullopt;
}

inline void create_portal_blocks(const Shape& s) {
    for (int h = 0; h < s.height; h++)
        for (int i = 0; i < s.width; i++) {
            Pos p = s.step(s.bottom_left.rel(Y, h), i);
            change_block(p.x, p.y, p.z, portal_state(s.axis));
        }
}

inline bool in_portal_dimension() { return cur_level == &worlds[DIM_OVERWORLD] || cur_level == &worlds[DIM_NETHER]; }

// BaseFireBlock.onPlace (fire lights a frame) and NetherPortalBlock.updateShape (a portal block whose frame or
// neighbouring portal block went away turns to air, which cascades through the portal).
inline void block_changed(int x, int y, int z, uint16_t old_state, uint16_t new_state) {
    if (is_fire(new_state) && !is_fire(old_state) && in_portal_dimension())
        if (auto s = find_empty_shape({x, y, z}, X)) return create_portal_blocks(*s);
    if (is_nether_portal(new_state)) return;
    static const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (auto& d : dirs) {
        Pos n{x + d[0], y + d[1], z + d[2]};
        uint16_t b = cur_level->peek_block(n.x, n.y, n.z);
        if (!is_nether_portal(b)) continue;
        int axis = axis_of(b);
        int update_axis = d[0] ? X : d[1] ? Y : Z;
        if (axis != update_axis && update_axis != Y) continue;  // wrongAxis
        if (!find_any_shape(n, axis).complete()) change_block(n.x, n.y, n.z, blocks::air);
    }
}

// ---------- BlockUtil.getLargestRectangleAround ----------
struct Rect { Pos min; int size1, size2; };
template <class Test>
Rect largest_rectangle(Pos center, int axis1, int limit1, int axis2, int limit2, Test test) {
    auto limit = [&](Pos p, int axis, int dir, int lim) {
        int m = 0;
        while (m < lim && test(p = p.rel(axis, dir))) m++;
        return m;
    };
    int neg1 = limit(center, axis1, -1, limit1), pos1 = limit(center, axis1, 1, limit1);
    struct B { int min, max; };
    std::vector<B> bounds(size_t(neg1 + 1 + pos1));
    bounds[size_t(neg1)] = {limit(center, axis2, -1, limit2), limit(center, axis2, 1, limit2)};
    int c2 = bounds[size_t(neg1)].min;
    for (int i = 1; i <= neg1; i++) {
        B last = bounds[size_t(neg1 - (i - 1))];
        Pos p = center.rel(axis1, -i);
        bounds[size_t(neg1 - i)] = {limit(p, axis2, -1, last.min), limit(p, axis2, 1, last.max)};
    }
    for (int i = 1; i <= pos1; i++) {
        B last = bounds[size_t(neg1 + i - 1)];
        Pos p = center.rel(axis1, i);
        bounds[size_t(neg1 + i)] = {limit(p, axis2, -1, last.min), limit(p, axis2, 1, last.max)};
    }
    int min1 = 0, min2 = 0, s1 = 0, s2 = 0;
    std::vector<int> columns(bounds.size());
    for (int i2 = c2; i2 >= 0; i2--) {
        for (size_t i1 = 0; i1 < bounds.size(); i1++) {
            int lo = c2 - bounds[i1].min, hi = c2 + bounds[i1].max;
            columns[i1] = i2 >= lo && i2 <= hi ? hi + 1 - i2 : 0;
        }
        // getMaxRectangleLocation (largest rectangle in a histogram)
        int max_start = 0, max_end = 0, max_height = 0, n = int(columns.size());
        std::vector<int> stack{0};
        for (int col = 1; col <= n; col++) {
            int h = col == n ? 0 : columns[size_t(col)];
            while (!stack.empty()) {
                int sh = columns[size_t(stack.back())];
                if (h >= sh) { stack.push_back(col); break; }
                stack.pop_back();
                int start = stack.empty() ? 0 : stack.back() + 1;
                if (sh * (col - start) > max_height * (max_end - max_start)) max_end = col, max_start = start, max_height = sh;
            }
            if (stack.empty()) stack.push_back(col);
        }
        int n1 = 1 + (max_end - 1) - max_start;
        if (n1 * max_height > s1 * s2) min1 = max_start, min2 = i2, s1 = n1, s2 = max_height;
    }
    return {center.rel(axis1, min1 - neg1).rel(axis2, min2 - c2), s1, s2};
}

// ---------- PortalForcer ----------
inline bool replaceable_no_fluid(uint16_t s) { return block_of(s).replaceable && !is_fluid(s); }  // canPortalReplaceBlock
inline bool solid_block(uint16_t s) { return block_of(s).collision && !block_of(s).replaceable; }  // ponytail: isSolid
inline int motion_blocking_height(int x, int z) {  // Heightmap MOTION_BLOCKING: top blocking / fluid block + 1
    for (int y = world.max_y() - 1; y >= world.min_y(); y--) {
        uint16_t s = world.get_block(x, y, z);
        if (block_of(s).collision || is_fluid(s)) return y + 1;
    }
    return world.min_y();
}

// findClosestPortalPosition: nearest nether portal block in a square of radius 16 (to the nether) / 128.
// The index (World::portals) stands in for the PoiManager.
// ponytail: ties on (distance, y) go to the lowest x, z; vanilla takes POI section order.
inline std::optional<Pos> find_closest_portal(World& w, Pos approx, bool to_nether) {
    int r = to_nether ? 16 : 128;
    std::optional<Pos> best;
    double best_d = 0;
    std::lock_guard lock(w.m);
    for (auto it = w.portals.lower_bound({approx.x - r, INT_MIN, INT_MIN}); it != w.portals.end(); ++it) {
        auto [x, y, z] = *it;
        if (x > approx.x + r) break;
        if (std::abs(z - approx.z) > r) continue;
        double d = double(x - approx.x) * (x - approx.x) + double(y - approx.y) * (y - approx.y) + double(z - approx.z) * (z - approx.z);
        if (!best || d < best_d || (d == best_d && y < best->y)) best = Pos{x, y, z}, best_d = d;
    }
    return best;
}

inline bool can_host_frame(Pos origin, int dx, int dz, int offset) {
    int cx = -dz, cz = dx;  // direction.getClockWise()
    for (int w = -1; w < 3; w++)
        for (int h = -1; h < 4; h++) {
            uint16_t s = world.get_block(origin.x + dx * w + cx * offset, origin.y + h, origin.z + dz * w + cz * offset);
            if (h < 0 && !solid_block(s)) return false;
            if (h >= 0 && !replaceable_no_fluid(s)) return false;
        }
    return true;
}

// PortalForcer.createPortal in cur_level. ponytail: no world border (vanilla clamps to it).
inline Rect create_portal(Pos origin, int axis) {
    int dx = axis == X ? 1 : 0, dz = axis == Z ? 1 : 0;  // Direction.get(POSITIVE, axis)
    double full_d = -1, part_d = -1;
    Pos full{}, part{};
    int max_y = std::min(world.max_y() - 1, world.min_y() + cur_level->logical_height - 1);
    // BlockPos.spiralAround(origin, 16, EAST, SOUTH)
    const int dirs[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    int cx = origin.x, cz = origin.z + 1, leg = -1, leg_size = 0, leg_index = 0;
    for (;;) {
        cx += dirs[(leg + 4) % 4][0], cz += dirs[(leg + 4) % 4][1];
        if (leg_index >= leg_size) {
            if (leg >= 4 * 16) break;
            leg++, leg_index = 0, leg_size = leg / 2 + 1;
        }
        leg_index++;
        int height = std::min(max_y, motion_blocking_height(cx, cz));
        Pos col{cx, origin.y, cz};
        for (int y = height; y >= world.min_y(); y--) {
            col.y = y;
            if (!replaceable_no_fluid(at(col))) continue;
            int first_empty = y;
            while (y > world.min_y() && replaceable_no_fluid(at({cx, y - 1, cz}))) y--;
            if (y + 4 > max_y) continue;
            int dy = first_empty - y;
            if (dy > 0 && dy < 3) continue;
            col.y = y;
            if (!can_host_frame(col, dx, dz, 0)) continue;
            double d = double(origin.x - cx) * (origin.x - cx) + double(origin.y - y) * (origin.y - y) + double(origin.z - cz) * (origin.z - cz);
            if (can_host_frame(col, dx, dz, -1) && can_host_frame(col, dx, dz, 1) && (full_d == -1 || full_d > d)) full_d = d, full = col;
            if (full_d == -1 && (part_d == -1 || part_d > d)) part_d = d, part = col;
        }
    }
    if (full_d == -1 && part_d != -1) full = part, full_d = part_d;
    if (full_d == -1) {  // no spot: a platform in the air
        int min_start = std::max(world.min_y() + 1, 70), max_start = max_y - 9;
        if (max_start < min_start) return {origin, 0, 0};
        full = {origin.x - dx, std::clamp(origin.y, min_start, max_start), origin.z - dz};
        int cwx = -dz, cwz = dx;
        for (int box = -1; box < 2; box++)
            for (int w = 0; w < 2; w++)
                for (int h = -1; h < 3; h++)
                    change_block(full.x + w * dx + box * cwx, full.y + h, full.z + w * dz + box * cwz,
                                 h < 0 ? blocks::obsidian : blocks::air);
    }
    for (int w = -1; w < 3; w++)
        for (int h = -1; h < 4; h++)
            if (w == -1 || w == 2 || h == -1 || h == 3) change_block(full.x + w * dx, full.y + h, full.z + w * dz, blocks::obsidian);
    for (int w = 0; w < 2; w++)
        for (int h = 0; h < 3; h++) change_block(full.x + w * dx, full.y + h, full.z + w * dz, portal_state(axis));
    return {full, 2, 3};
}

// ---------- travel ----------
constexpr double PLAYER_W = double(0.6f), PLAYER_H = double(1.8f);  // EntityDimensions are floats

inline bool box_free(double x, double y, double z) {  // no full-cube collision for a standing player box at feet (x, y, z)
    for (int bx = int(std::floor(x - PLAYER_W / 2)); bx <= int(std::floor(x + PLAYER_W / 2 - 1e-7)); bx++)
        for (int by = int(std::floor(y)); by <= int(std::floor(y + PLAYER_H - 1e-7)); by++)
            for (int bz = int(std::floor(z - PLAYER_W / 2)); bz <= int(std::floor(z + PLAYER_W / 2 - 1e-7)); bz++)
                if (block_of(world.get_block(bx, by, bz)).collision) return false;
    return true;
}

// NetherPortalBlock.getPortalDestination + getDimensionTransitionFromExit + createDimensionTransition.
inline void nether_portal_travel(Player& p) {
    World* from = p.level;
    World* to = from == &worlds[DIM_NETHER] ? &worlds[DIM_OVERWORLD] : &worlds[DIM_NETHER];
    bool to_nether = to == &worlds[DIM_NETHER];
    double scale = from->coordinate_scale / to->coordinate_scale;
    Pos approx{int(std::floor(p.x * scale)), int(std::floor(p.y)), int(std::floor(p.z * scale))};
    Pos entry{p.portal_x, p.portal_y, p.portal_z};
    uint16_t entry_state = at(entry);
    int entry_axis = is_nether_portal(entry_state) ? axis_of(entry_state) : X;
    // Entity.getRelativePortalPosition (PortalShape.getRelativePosition) on the entry portal
    double off_x = 0.5, off_y = 0, off_z = 0;
    if (is_nether_portal(entry_state)) {
        Rect area = largest_rectangle(entry, entry_axis, 21, Y, 21, [&](Pos q) { return at(q) == entry_state; });
        double pos_axis = entry_axis == X ? p.x : p.z, pos_fwd = entry_axis == X ? p.z : p.x;
        double width = area.size1 - PLAYER_W, height = area.size2 - PLAYER_H;
        off_x = width > 0 ? std::clamp((pos_axis - (area.min.get(entry_axis) + PLAYER_W / 2)) / width, 0.0, 1.0) : 0.5;
        off_y = height > 0 ? std::clamp((p.y - area.min.y) / height, 0.0, 1.0) : 0.0;
        off_z = pos_fwd - (area.min.get(entry_axis == X ? Z : X) + 0.5);
    }
    InLevel in_level(to);
    Rect exit;
    if (auto found = find_closest_portal(*to, approx, to_nether)) {
        uint16_t st = at(*found);
        exit = largest_rectangle(*found, axis_of(st), 21, Y, 21, [&](Pos q) { return at(q) == st; });
    } else {
        if (p.gamemode == 3) return;
        exit = create_portal(approx, entry_axis);
        if (exit.size1 == 0) return;  // vanilla: "Unable to create a portal"
    }
    uint16_t bl = at(exit.min);
    int axis = is_nether_portal(bl) ? axis_of(bl) : X;
    float rotation = entry_axis == axis ? 0.0f : 90.0f;
    double right = PLAYER_W / 2 + (exit.size1 - PLAYER_W) * off_x, up = (exit.size2 - PLAYER_H) * off_y, fwd = 0.5 + off_z;
    bool xa = axis == X;
    double tx = exit.min.x + (xa ? right : fwd), ty = exit.min.y + up, tz = exit.min.z + (xa ? fwd : right);
    // PortalShape.findCollisionFreePosition: allowed centers reach at most 1 block up.
    // ponytail: full-cube collision and a 1/16 step search stand in for Level.findFreePosition's shape search.
    for (double lift = 0; lift <= 1.0; lift += 1.0 / 16)
        if (box_free(tx, ty + lift, tz)) { ty += lift; break; }
    change_dimension(p, to, tx, ty, tz, p.yaw + rotation, p.pitch);
}

// EndPortalBlock.getPortalDestination: into the end onto the platform, or out of it to the world spawn.
inline void end_portal_travel(Player& p) {
    if (p.level == &worlds[DIM_END]) {
        World* from = p.level;
        float yaw = p.yaw, pitch = p.pitch;
        place_new_player(p);  // findRespawnPositionAndUseSpawnBlock: the world spawn
        World* spawn = p.level;
        p.level = from;
        change_dimension(p, spawn, p.x, p.y, p.z, yaw, pitch);  // ponytail: vanilla turns to the spawn angle
        return;
    }
    World& end = worlds[DIM_END];
    create_end_platform(end, 100, 49, 0);                       // ServerLevel.END_SPAWN_POINT.below()
    change_dimension(p, &end, 100.5, 49, 0.5, 90.0f, p.pitch);  // facing west
}

// ---------- standing in portals ----------
// Entity.checkInsideBlocks against the portal shapes (nether: 4/16 thick slab; end: y 6/16..12/16).
inline int touched_portal(Player& p, Pos& where) {
    double x0 = p.x - 0.3 + 1e-7, x1 = p.x + 0.3 - 1e-7, y0 = p.y + 1e-7, y1 = p.y + 1.8 - 1e-7;
    double z0 = p.z - 0.3 + 1e-7, z1 = p.z + 0.3 - 1e-7;
    for (int bx = int(std::floor(x0)); bx <= int(std::floor(x1)); bx++)
        for (int by = int(std::floor(y0)); by <= int(std::floor(y1)); by++)
            for (int bz = int(std::floor(z0)); bz <= int(std::floor(z1)); bz++) {
                uint16_t s = world.get_block(bx, by, bz);
                bool hit = false;
                if (is_nether_portal(s)) {
                    bool ax = axis_of(s) == X;
                    double lo = ax ? bz + 0.375 : bx + 0.375, hi = lo + 0.25;
                    double a0 = ax ? z0 : x0, a1 = ax ? z1 : x1;
                    hit = a1 > lo && a0 < hi;
                } else if (s == blocks::end_portal) {
                    hit = y1 > by + 0.375 && y0 < by + 0.75;
                }
                if (hit) {
                    where = {bx, by, bz};
                    return is_nether_portal(s) ? 1 : 2;
                }
            }
    return 0;
}

inline void player_tick(Player& p) {
    if (p.g.dead || p.won_game || !p.g.loaded) return;
    Pos where{};
    int kind = p.gamemode == 3 ? 0 : touched_portal(p, where);
    if (kind == 2 && p.level == &worlds[DIM_END] && !p.seen_credits) return show_end_credits(p);
    if (kind) {  // setAsInsidePortal
        if (p.portal_cooldown > 0) p.portal_cooldown = 10;  // Player.getDimensionChangingDelay
        else {
            if (p.portal_kind != kind) p.portal_kind = kind, p.portal_time = 0;
            p.portal_x = where.x, p.portal_y = where.y, p.portal_z = where.z;
            p.portal_this_tick = true;
        }
    }
    // handlePortal
    if (p.portal_cooldown > 0) p.portal_cooldown--;
    if (!p.portal_kind) return;
    if (!p.portal_this_tick) {  // decayTick
        p.portal_time = std::max(p.portal_time - 4, 0);
        if (p.portal_time <= 0) p.portal_kind = 0;
        return;
    }
    p.portal_this_tick = false;
    int wait = 0;  // getPortalTransitionTime: end portals are instant
    if (p.portal_kind == 1)
        wait = std::max(0, rule(p.gamemode == 1 || p.gamemode == 3 ? "players_nether_portal_creative_delay"
                                                                     : "players_nether_portal_default_delay"));
    if (p.portal_time++ < wait) return;
    int k = p.portal_kind;
    p.portal_kind = 0;
    p.portal_cooldown = 10;  // setPortalCooldown
    if (k == 1) nether_portal_travel(p);
    else end_portal_travel(p);
}

// ---------- items ----------
inline int item_named(const char* name) {
    auto it = item_by_name.find(name);
    return it == item_by_name.end() ? -1 : it->second;
}

// End portal frame: facing index (north, south, west, east) and eye flag from the state.
inline bool frame_with_eye(uint16_t s, int facing) { return s == blocks::end_portal_frame - 4 + facing; }

// EndPortalFrameBlock.getOrCreatePortalShape().find: a 3x3 interior ringed by 12 eyed frames facing inwards.
inline std::optional<Pos> find_end_portal(Pos clicked) {
    enum { N, S, W, E };
    for (int mx = clicked.x - 3; mx <= clicked.x + 1; mx++)
        for (int mz = clicked.z - 3; mz <= clicked.z + 1; mz++) {
            int y = clicked.y;
            bool ok = true;
            for (int i = 0; i < 3 && ok; i++)
                ok = frame_with_eye(world.get_block(mx + i, y, mz - 1), S) && frame_with_eye(world.get_block(mx + i, y, mz + 3), N) &&
                     frame_with_eye(world.get_block(mx - 1, y, mz + i), E) && frame_with_eye(world.get_block(mx + 3, y, mz + i), W);
            if (ok) return Pos{mx, y, mz};
        }
    return std::nullopt;
}

// Returns true when the item use was handled here (flint and steel, eye of ender on a frame).
inline bool use_item_on(Player& p, int hand, int x, int y, int z, int face) {
    if (p.gamemode == 3 || p.g.dead || !within_reach(p, x, y, z)) return false;
    ItemStack& slot = p.inv[hand == 1 ? 40u : size_t(p.selected)];
    if (slot.empty()) return false;
    static const int flint = item_named("minecraft:flint_and_steel"), eye = item_named("minecraft:ender_eye");
    uint16_t clicked = world.get_block(x, y, z);
    if (slot.item == eye) {  // EnderEyeItem.useOn
        if (block_index(clicked) != block_index(blocks::end_portal_frame) || clicked < blocks::end_portal_frame) return false;
        change_block(x, y, z, clicked - 4);  // eye = true, same facing
        if (p.gamemode != 1 && --slot.count <= 0) slot = {};
        if (auto m = find_end_portal({x, y, z}))
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) change_block(m->x + i, y, m->z + j, blocks::end_portal);
        return true;
    }
    if (slot.item != flint) return false;
    // FlintAndSteelItem.useOn -> BaseFireBlock.canBePlacedAt. ponytail: no campfire/candle lighting, fire never
    // spreads or burns out, survival test = solid block below (FireBlock.canSurvive also allows flammable sides).
    static const int off[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    if (face < 0 || face > 5) return true;
    int fx = x + off[face][0], fy = y + off[face][1], fz = z + off[face][2];
    uint16_t target = world.get_block(fx, fy, fz);
    if (target != blocks::air && target != blocks::cave_air && target != blocks::void_air)  // !isAir
        return p.send(block_update(fx, fy, fz, target)), true;
    uint16_t below = world.get_block(fx, fy - 1, fz);
    bool survives = solid_block(below);
    bool portal = false;
    if (!survives && in_portal_dimension()) {  // isPortal: next to a frame and an empty shape fits
        for (auto& d : off)
            if (is_frame(world.get_block(fx + d[0], fy + d[1], fz + d[2]))) {
                // forwardDirection.getCounterClockWise().getAxis(): facing along z -> X portal first
                float yaw = std::fmod(std::fmod(p.yaw, 360.0f) + 360.0f, 360.0f);
                bool facing_z = (yaw < 45 || yaw >= 315) || (yaw >= 135 && yaw < 225);
                portal = find_empty_shape({fx, fy, fz}, facing_z ? X : Z).has_value();
                break;
            }
    }
    if (!survives && !portal) return true;
    bool soul = block_index(below) == block_index(blocks::soul_sand) || block_index(below) == block_index(blocks::soul_soil);
    change_block(fx, fy, fz, soul ? blocks::soul_fire : blocks::fire);
    if (survival_like(p)) {  // hurtAndBreak(1)
        if (hand == 1) std::swap(p.inv[40], held(p));
        hurt_tool(p, 1);
        if (hand == 1) std::swap(p.inv[40], held(p));
    }
    return true;
}

inline void init() {
    on_block_changed = block_changed;
    on_player_tick(player_tick);
    PacketHandler prev = handlers()[play_sb::use_item_on];
    on(play_sb::use_item_on, [prev](Player& p, Reader& in, int id) {
        Reader peek = in;
        int hand = peek.varint();
        int x, y, z;
        read_block_pos(peek, x, y, z);
        int face = peek.varint();
        peek.f32(); peek.f32(); peek.f32(); peek.u8(); peek.u8();
        int seq = peek.varint();
        if (portal::use_item_on(p, hand, x, y, z, face)) {
            p.send(Writer().varint(play_cb::block_changed_ack).varint(seq));
            return;
        }
        prev(p, in, id);
    });
}

}  // namespace portal
