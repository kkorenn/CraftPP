#pragma once
// Entities: packet helpers, tracking (who sees what) and item entities (ItemEntity port).
// All state here belongs to the tick thread.
#include "data.hpp"
#include "tick.hpp"

#include <random>

inline std::mt19937_64 game_rng{std::random_device{}()};
inline float rand_float() { return float(game_rng() >> 40) * 0x1.0p-24f; }  // [0, 1)
inline double rand_double() { return double(game_rng() >> 11) * 0x1.0p-53; }
inline int rand_int(int bound) { return bound <= 0 ? 0 : int(game_rng() % uint64_t(bound)); }

// ---------- packet helpers ----------
inline Writer& write_lpvec3(Writer& w, double x, double y, double z) {  // LpVec3.write
    auto clean = [](double v) { return std::isnan(v) ? 0.0 : std::clamp(v, -1.7179869183E10, 1.7179869183E10); };
    x = clean(x), y = clean(y), z = clean(z);
    double m = std::max({std::abs(x), std::abs(y), std::abs(z)});
    if (m < 3.051944088384301E-5) return w.u8(0);
    int64_t scale = int64_t(std::ceil(m));
    bool partial = (scale & 3) != scale;
    auto pack = [](double v) { return int64_t(std::llround((v * 0.5 + 0.5) * 32766.0)); };
    uint64_t buf = uint64_t(partial ? (scale & 3) | 4 : scale) | uint64_t(pack(x / double(scale))) << 3 |
                   uint64_t(pack(y / double(scale))) << 18 | uint64_t(pack(z / double(scale))) << 33;
    w.u8(uint8_t(buf)).u8(uint8_t(buf >> 8)).i32(int32_t(uint32_t(buf >> 16)));
    if (partial) w.varint(int32_t(scale >> 2));
    return w;
}

// ItemStack.OPTIONAL_STREAM_CODEC (component patch carries only minecraft:damage).
inline Writer& write_slot(Writer& w, const ItemStack& s) {
    if (s.empty()) return w.varint(0);
    w.varint(s.count).varint(s.item);
    if (s.damage > 0) return w.varint(1).varint(0).varint(component_ids["damage"]).varint(s.damage);
    return w.varint(0).varint(0);
}

inline Writer add_entity_packet(int eid, int64_t uh, int64_t ul, int type, double x, double y, double z, double vx,
                                double vy, double vz, float yaw = 0, float pitch = 0, int data = 0) {
    Writer w;
    w.varint(play_cb::add_entity).varint(eid).i64(uh).i64(ul).varint(type).f64(x).f64(y).f64(z);
    write_lpvec3(w, vx, vy, vz);
    w.u8(angle(pitch)).u8(angle(yaw)).u8(angle(yaw)).varint(data);
    return w;
}
inline Writer remove_entities_packet(std::initializer_list<int> ids) {
    Writer w;
    w.varint(play_cb::remove_entities).varint(int32_t(ids.size()));
    for (int id : ids) w.varint(id);
    return w;
}
inline Writer teleport_entity_packet(int eid, double x, double y, double z, double vx, double vy, double vz,
                                     float yaw, float pitch, bool on_ground) {
    return std::move(Writer().varint(play_cb::teleport_entity).varint(eid).f64(x).f64(y).f64(z).f64(vx).f64(vy).f64(vz)
                         .f32(yaw).f32(pitch).i32(0).boolean(on_ground));
}
inline Writer set_entity_motion_packet(int eid, double vx, double vy, double vz) {
    Writer w;
    w.varint(play_cb::set_entity_motion).varint(eid);
    write_lpvec3(w, vx, vy, vz);
    return w;
}
// set_entity_data with one ITEM_STACK entry (serializer 7); end with 0xFF.
inline Writer entity_item_data_packet(int eid, int index, const ItemStack& s) {
    Writer w;
    w.varint(play_cb::set_entity_data).varint(eid).u8(uint8_t(index)).varint(7);
    write_slot(w, s);
    return std::move(w.u8(0xFF));
}
inline Writer entity_event_packet(int eid, int event) {
    return std::move(Writer().varint(play_cb::entity_event).i32(eid).u8(uint8_t(event)));
}

inline int view_dist_cached() {
    static int v = view_distance();
    return v;
}
inline bool sees_chunk(const Player& p, int cx, int cz) {
    int v = view_dist_cached();
    return std::abs(cx - p.cx) <= v && std::abs(cz - p.cz) <= v;
}
// Sends to every player whose view contains (x, z).
inline void broadcast_near(double x, double z, const Writer& w, const Player* except = nullptr) {
    int cx = int(std::floor(x)) >> 4, cz = int(std::floor(z)) >> 4;
    for (auto& p : players)
        if (p.get() != except && sees_chunk(*p, cx, cz)) p->send(w);
}

// ---------- collision ----------
inline bool solid_at(int x, int y, int z) {
    if (y < MIN_Y) return false;
    return block_of(world.get_block(x, y, z)).collision;
}

// Entity.move for an axis-aligned box against full-cube solid blocks (y first, then x, z).
// ponytail: every colliding block is treated as a full cube (slabs, stairs, fences...); use real shapes later.
struct Box { double x0, y0, z0, x1, y1, z1; };
inline void move_box(double& x, double& y, double& z, double half_w, double h, double& vx, double& vy, double& vz,
                     bool& on_ground, bool& hit_h) {
    auto clip = [&](int axis, double d) {
        if (d == 0) return 0.0;
        Box b{x - half_w, y, z - half_w, x + half_w, y + h, z + half_w};
        double lo[3] = {b.x0, b.y0, b.z0}, hi[3] = {b.x1, b.y1, b.z1};
        double from = d > 0 ? hi[axis] : lo[axis];
        int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        int s = int(std::floor(from + (d > 0 ? -1e-7 : 1e-7))), e = int(std::floor(from + d));
        int step = d > 0 ? 1 : -1;
        for (int c = s + step; d > 0 ? c <= e : c >= e; c += step)
            for (int i = int(std::floor(lo[a1] + 1e-7)); i <= int(std::floor(hi[a1] - 1e-7)); i++)
                for (int k = int(std::floor(lo[a2] + 1e-7)); k <= int(std::floor(hi[a2] - 1e-7)); k++) {
                    int p[3];
                    p[axis] = c, p[a1] = i, p[a2] = k;
                    if (solid_at(p[0], p[1], p[2])) return d > 0 ? std::min(d, c - from) : std::max(d, c + 1 - from);
                }
        return d;
    };
    double dy = clip(1, vy);
    y += dy;
    double dx = clip(0, vx);
    x += dx;
    double dz = clip(2, vz);
    z += dz;
    on_ground = vy < 0 && dy != vy;
    hit_h = dx != vx || dz != vz;
    if (dy != vy) vy = 0;
    if (dx != vx) vx = 0;
    if (dz != vz) vz = 0;
}

// ---------- item entities ----------
struct ItemEntity {
    int eid;
    int64_t uh, ul;
    double x, y, z, vx, vy, vz;
    double sx, sy, sz;  // position last sent
    ItemStack stack;
    int age = 0, pickup_delay = 10, tick_count = 0;
    bool on_ground = false, removed = false, dirty_item = false;
    float yaw;
    std::vector<int> watchers;  // player eids that have this entity spawned
};
inline std::vector<ItemEntity> item_entities;
inline constexpr double ITEM_HALF_W = 0.125, ITEM_H = 0.25;

inline ItemEntity& spawn_item(const ItemStack& s, double x, double y, double z, double vx, double vy, double vz,
                              int pickup_delay = 10) {
    ItemEntity e;
    e.eid = next_entity_id++;
    e.uh = int64_t(game_rng() & ~0xF000ull | 0x4000), e.ul = int64_t(game_rng() & 0x3FFFFFFFFFFFFFFFull | 0x8000000000000000ull);
    e.x = x, e.y = y, e.z = z, e.vx = vx, e.vy = vy, e.vz = vz, e.sx = x, e.sy = y, e.sz = z;
    e.stack = s;
    e.pickup_delay = pickup_delay;
    e.yaw = rand_float() * 360.0f;
    item_entities.push_back(std::move(e));
    return item_entities.back();
}
// new ItemEntity(level, x, y, z, stack): random small horizontal push, 0.2 up.
inline void spawn_item(const ItemStack& s, double x, double y, double z) {
    spawn_item(s, x, y, z, rand_double() * 0.2 - 0.1, 0.2, rand_double() * 0.2 - 0.1);
}
// Block.popResource: drops `s` at a block, split into max-size stacks.
inline void pop_resource(int bx, int by, int bz, ItemStack s) {
    while (!s.empty()) {
        ItemStack part = s;
        part.count = std::min(s.count, max_stack(s));
        s.count -= part.count;
        double hh = ITEM_H / 2;
        spawn_item(part, bx + 0.5 + (rand_double() * 0.5 - 0.25), by + 0.5 + (rand_double() * 0.5 - 0.25) - hh,
                   bz + 0.5 + (rand_double() * 0.5 - 0.25));
    }
}
// LivingEntity.createItemStackToDrop for players (Q / death / inventory throw). Pickup delay 40.
inline void player_drop(const Player& p, const ItemStack& s, bool randomly) {
    if (s.empty()) return;
    double ey = p.y + 1.62 - 0.3;
    if (randomly) {
        float pow = rand_float() * 0.5f, dir = rand_float() * float(M_PI * 2);
        spawn_item(s, p.x, ey, p.z, -std::sin(dir) * pow, 0.2f, std::cos(dir) * pow, 40);
    } else {
        float sx = std::sin(p.pitch * float(M_PI / 180)), cx = std::cos(p.pitch * float(M_PI / 180));
        float sy = std::sin(p.yaw * float(M_PI / 180)), cy = std::cos(p.yaw * float(M_PI / 180));
        float dir = rand_float() * float(M_PI * 2), pow2 = 0.02f * rand_float();
        spawn_item(s, p.x, ey, p.z, -sy * cx * 0.3f + std::cos(dir) * pow2,
                   -sx * 0.3f + 0.1f + (rand_float() - rand_float()) * 0.1f, cy * cx * 0.3f + std::sin(dir) * pow2, 40);
    }
}

inline bool give_item(Player& p, int item, int count);  // inventory.hpp
inline int inventory_add(Player& p, ItemStack& s);       // inventory.hpp: returns amount added

inline void item_merge(ItemEntity& a) {  // ItemEntity.mergeWithNeighbours
    auto mergable = [](const ItemEntity& e) {
        return !e.removed && e.pickup_delay != 32767 && e.age < 6000 && e.stack.count < max_stack(e.stack);
    };
    if (!mergable(a)) return;
    for (auto& b : item_entities) {
        if (&b == &a || !mergable(b) || !b.stack.same(a.stack)) continue;
        if (std::abs(b.x - a.x) >= 0.5 + 2 * ITEM_HALF_W || std::abs(b.z - a.z) >= 0.5 + 2 * ITEM_HALF_W ||
            std::abs(b.y - a.y) >= ITEM_H)
            continue;
        if (a.stack.count + b.stack.count > max_stack(a.stack)) continue;
        ItemEntity& to = b.stack.count < a.stack.count ? a : b;
        ItemEntity& from = &to == &a ? b : a;
        to.stack.count += from.stack.count;
        to.pickup_delay = std::max(to.pickup_delay, from.pickup_delay);
        to.age = std::min(to.age, from.age);
        to.dirty_item = true;
        from.stack.count = 0;
        from.removed = true;
        if (a.removed) return;
    }
}

inline void tick_item(ItemEntity& e) {
    if (e.stack.empty()) { e.removed = true; return; }
    e.tick_count++;
    if (e.pickup_delay > 0 && e.pickup_delay != 32767) e.pickup_delay--;
    double ox = e.x, oy = e.y, oz = e.z;
    int fluid = world.get_block(int(std::floor(e.x)), int(std::floor(e.y + 0.1)), int(std::floor(e.z)));
    bool in_water = block_index(fluid) == block_index(blocks::water);
    if (in_water) {  // setUnderwaterMovement: float up
        e.vx *= 0.99f;
        e.vy += e.vy < 0.06f ? 5.0E-4f : 0;
        e.vz *= 0.99f;
    } else {
        e.vy -= 0.04;
    }
    if (solid_at(int(std::floor(e.x)), int(std::floor(e.y + ITEM_H / 2)), int(std::floor(e.z)))) {
        e.y = std::floor(e.y + ITEM_H / 2) + 1;  // ponytail: vanilla moveTowardsClosestSpace picks any free side
        e.vy = 0.1;
    }
    if (!(e.on_ground && e.vx * e.vx + e.vz * e.vz <= 1e-5 && (e.tick_count + e.eid) % 4 != 0)) {
        bool hit_h;
        move_box(e.x, e.y, e.z, ITEM_HALF_W, ITEM_H, e.vx, e.vy, e.vz, e.on_ground, hit_h);
        double ground = e.on_ground ? 0.98 * 0.6 : 0.98;  // ponytail: all blocks have friction 0.6 (no ice/slime yet)
        e.vx *= ground, e.vy *= 0.98, e.vz *= ground;
        if (e.on_ground && e.vy < 0) e.vy *= -0.5;
    }
    bool moved = std::floor(ox) != std::floor(e.x) || std::floor(oy) != std::floor(e.y) || std::floor(oz) != std::floor(e.z);
    if (e.tick_count % (moved ? 2 : 40) == 0) item_merge(e);
    if (++e.age >= 6000) e.removed = true;
    if (e.y < MIN_Y - 64) e.removed = true;
}

// Player touching items (Player.aiStep touch box: player bbox inflated 1, 0.5, 1).
inline void item_pickups(Player& p) {
    if (p.g.dead || p.gamemode == 3) return;
    for (auto& e : item_entities) {
        if (e.removed || e.pickup_delay > 0) continue;
        if (std::abs(e.x - p.x) > 1.3 + ITEM_HALF_W || std::abs(e.z - p.z) > 1.3 + ITEM_HALF_W) continue;
        if (e.y + ITEM_H < p.y - 0.5 || e.y > p.y + 1.8 + 0.5) continue;
        int before = e.stack.count;
        int added = inventory_add(p, e.stack);
        if (added <= 0) continue;
        Writer w;
        w.varint(play_cb::take_item_entity).varint(e.eid).varint(p.eid).varint(added);
        broadcast_near(e.x, e.z, w);
        if (e.stack.count <= 0) e.removed = true;
        else if (e.stack.count != before) e.dirty_item = true;
    }
}

// Spawn/despawn per watcher, position sync, removal. Runs after all entity ticks.
inline void track_items() {
    static std::vector<int> gone;
    for (auto& e : item_entities) {
        int cx = int(std::floor(e.x)) >> 4, cz = int(std::floor(e.z)) >> 4;
        bool moved = e.x != e.sx || e.y != e.sy || e.z != e.sz;
        bool sync = moved && (e.tick_count % 20 == 0 || (e.on_ground && std::abs(e.vy) < 1e-3));
        for (auto& pp : players) {
            Player& p = *pp;
            auto w = std::find(e.watchers.begin(), e.watchers.end(), p.eid);
            bool in = !e.removed && sees_chunk(p, cx, cz);
            if (in && w == e.watchers.end()) {
                p.send(add_entity_packet(e.eid, e.uh, e.ul, entity_types::item, e.x, e.y, e.z, e.vx, e.vy, e.vz, e.yaw));
                p.send(entity_item_data_packet(e.eid, 8, e.stack));
                e.watchers.push_back(p.eid);
            } else if (!in && w != e.watchers.end()) {
                p.send(remove_entities_packet({e.eid}));
                e.watchers.erase(w);
            } else if (in) {
                if (e.dirty_item) p.send(entity_item_data_packet(e.eid, 8, e.stack));
                if (sync) p.send(teleport_entity_packet(e.eid, e.x, e.y, e.z, e.vx, e.vy, e.vz, e.yaw, 0, e.on_ground));
            }
        }
        if (sync) e.sx = e.x, e.sy = e.y, e.sz = e.z;
        e.dirty_item = false;
        if (e.removed) gone.push_back(e.eid);
    }
    if (!gone.empty()) {
        std::erase_if(item_entities, [](const ItemEntity& e) { return e.removed; });
        gone.clear();
    }
}

inline void tick_entities() {
    for (size_t i = 0; i < item_entities.size(); i++) tick_item(item_entities[i]);  // index: merging never adds
    for (auto& p : players) item_pickups(*p);
    track_items();
}

// A player left: forget it as a watcher (the client is gone, nothing to send).
inline void entities_forget_player(Player& p) {
    for (auto& e : item_entities) std::erase(e.watchers, p.eid);
}
