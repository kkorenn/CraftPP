#pragma once
// Item entities (ItemEntity port) on top of the entity framework in src/mobs/ (storage, physics, tracking).
// All state here belongs to the tick thread.
#include "data.hpp"
#include "tick.hpp"
#include "../mobs/entity.hpp"

inline bool give_item(Player& p, int item, int count);  // inventory.hpp
inline int inventory_add(Player& p, ItemStack& s);       // inventory.hpp: returns amount added

struct ItemEntity : Entity {
    ItemStack stack;
    int age = 0, pickup_delay = 0, health = 5;
    int64_t owner_hi = 0, owner_lo = 0;  // Owner: only this player may pick it up (0 = anyone)

    void set_item(const ItemStack& s) {
        stack = s;
        data.set_raw(edata::ItemEntity::ITEM, edser::ITEM_STACK, encode_item_stack(s));
    }
    double default_gravity() const override { return 0.04; }
    bool is_attackable() const override { return false; }
    bool mergable() const { return !removed && pickup_delay != 32767 && age != -32768 && age < 6000 && stack.count < max_stack(stack); }

    void merge_with_neighbours() {  // ItemEntity.mergeWithNeighbours
        if (!mergable()) return;
        for_each_entity_in(bb.inflate(0.5, 0.0, 0.5), [&](Entity& o) {
            if (removed || &o == this || o.type != entity_types::item) return;
            auto& b = static_cast<ItemEntity&>(o);
            if (!b.mergable() || !b.stack.same(stack) || b.owner_hi != owner_hi || b.owner_lo != owner_lo) return;
            if (stack.count + b.stack.count > max_stack(stack)) return;
            ItemEntity& to = b.stack.count < stack.count ? *this : b;
            ItemEntity& from = &to == this ? b : *this;
            to.set_item({to.stack.item, to.stack.count + from.stack.count, to.stack.damage});
            to.pickup_delay = std::max(to.pickup_delay, from.pickup_delay);
            to.age = std::min(to.age, from.age);
            from.stack.count = 0;
            from.discard();
        }, false);
    }
    void move_towards_closest_space(double x, double y, double z) {  // Entity.moveTowardsClosestSpace
        int bx = mth::floor(x), by = mth::floor(y), bz = mth::floor(z);
        double off[3] = {x - bx, y - by, z - bz};
        static const int dirs[5][4] = {{0, 0, -1, 2}, {0, 0, 1, 2}, {-1, 0, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 1}};  // N S W E UP
        int best = 4;
        double best_d = 1e300;
        for (int i = 0; i < 5; i++) {
            auto& d = dirs[i];
            if (sflag(block_at(bx + d[0], by + d[1], bz + d[2]), SF_FULL_BLOCK)) continue;
            int axis = d[3];
            bool positive = d[0] + d[1] + d[2] > 0;
            double dist = positive ? 1.0 - off[axis] : off[axis];
            if (dist < best_d) best_d = dist, best = i;
        }
        float speed = rand_float() * 0.2f + 0.1f;
        auto& d = dirs[best];
        float step = float(d[0] + d[1] + d[2]);
        Vec3 m = vel * 0.75;
        if (d[3] == 0) vel = {step * speed, m.y, m.z};
        else if (d[3] == 1) vel = {m.x, step * speed, m.z};
        else vel = {m.x, m.y, step * speed};
    }
    void tick() override {  // ItemEntity.tick
        if (stack.empty()) { discard(); return; }
        Entity::tick();
        if (pickup_delay > 0 && pickup_delay != 32767) pickup_delay--;
        old = pos;
        Vec3 old_vel = vel;
        auto fluid_move = [&](float f) { vel = {vel.x * f, vel.y + (vel.y < 0.06f ? 5.0E-4f : 0.0f), vel.z * f}; };
        if (is_in_water() && water_height > 0.1f) fluid_move(0.99f);
        else if (is_in_lava() && lava_height > 0.1f) fluid_move(0.95f);
        else apply_gravity();
        no_physics = !no_block_collision(bb.inflate(-1.0E-7));
        if (no_physics) move_towards_closest_space(pos.x, (bb.y0 + bb.y1) / 2.0, pos.z);
        if (!(on_ground && vel.horizontal_sqr() <= 1.0E-5f && (tick_count + id) % 4 != 0)) {
            move(SELF, vel);
            float drag = 0.98f, ground = drag;
            if (on_ground) ground = drag * phys_of(block_below_affecting_movement()).friction;
            vel = vel.multiply(ground, drag, ground);
            if (on_ground && vel.y < 0.0) vel.y *= -0.5;
        }
        bool moved = mth::floor(old.x) != mth::floor(pos.x) || mth::floor(old.y) != mth::floor(pos.y) || mth::floor(old.z) != mth::floor(pos.z);
        if (tick_count % (moved ? 2 : 40) == 0 && mergable()) merge_with_neighbours();
        if (age != -32768) age++;
        needs_sync = needs_sync | update_fluid_interaction();
        if ((vel - old_vel).length_sqr() > 0.01) needs_sync = true;
        if (age >= 6000) discard();
    }
    bool hurt_server(const DamageSource& s, float dmg) override {  // ItemEntity.hurtServer
        if (is_invulnerable_to(s)) return false;
        // ponytail: damage_resistant component (netherite, nether stars vs explosions) not tracked on stacks yet
        needs_sync = true;
        health = int(float(health) - dmg);
        if (health <= 0) discard();
        return true;
    }
    void player_touch(Player& p) override {  // ItemEntity.playerTouch
        if (pickup_delay != 0 || p.g.dead || p.gamemode == 3) return;
        if ((owner_hi || owner_lo) && (owner_hi != p.uuid_hi || owner_lo != p.uuid_lo)) return;
        int before = stack.count;
        int added = inventory_add(p, stack);
        if (added <= 0) return;
        progress::award_stat(p, progress::PICKED_UP, stack.item, added);
        Writer w;
        w.varint(play_cb::take_item_entity).varint(id).varint(p.eid).varint(added);
        broadcast(w);
        p.send(w);
        if (stack.count <= 0) discard();
        else if (stack.count != before) set_item(stack);
    }
    void save_extra(Nbt& o) const override {
        o.set("Health", Nbt::num(Nbt::Short, health));
        o.set("Age", Nbt::num(Nbt::Short, age));
        o.set("PickupDelay", Nbt::num(Nbt::Short, pickup_delay));
        Nbt it = Nbt::compound();
        it.set("id", Nbt::str(item_info[size_t(stack.item)].name));
        it.set("count", Nbt::num(Nbt::Int, stack.count));
        if (stack.damage) {
            Nbt c = Nbt::compound();
            c.set("minecraft:damage", Nbt::num(Nbt::Int, stack.damage));
            it.set("components", c);
        }
        o.set("Item", it);
    }
    void load_extra(const Nbt& in) override {
        health = int(nbt_num(in, "Health", 5));
        age = int(nbt_num(in, "Age", 0));
        pickup_delay = int(nbt_num(in, "PickupDelay", 0));
        ItemStack s;
        if (const Nbt* it = in.get("Item"); it && it->t == Nbt::Compound) {
            if (const Nbt* id = it->get("id"); id && id->t == Nbt::String)
                if (auto f = item_by_name.find(id->s.find(':') == std::string::npos ? "minecraft:" + id->s : id->s); f != item_by_name.end())
                    s = {f->second, int(nbt_num(*it, "count", 1)), 0};
            if (const Nbt* c = it->get("components")) s.damage = int(nbt_num(*c, "minecraft:damage", 0));
        }
        set_item(s);
        if (stack.empty()) discard();
    }
};
inline constexpr double ITEM_HALF_W = 0.125, ITEM_H = 0.25;

inline ItemEntity& spawn_item(const ItemStack& s, double x, double y, double z, double vx, double vy, double vz,
                              int pickup_delay = 10) {
    auto e = std::make_unique<ItemEntity>();
    e->type = entity_types::item;
    e->set_size(e->info().w, e->info().h, e->info().eye);
    e->snap_to(x, y, z, rand_float() * 360.0f, 0);
    e->vel = {vx, vy, vz};
    e->set_item(s);
    e->pickup_delay = pickup_delay;
    return *add_entity_t(std::move(e));
}
// new ItemEntity(level, x, y, z, stack): random small horizontal push, 0.2 up.
inline ItemEntity& spawn_item(const ItemStack& s, double x, double y, double z) {
    return spawn_item(s, x, y, z, rand_double() * 0.2 - 0.1, 0.2, rand_double() * 0.2 - 0.1);
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
    progress::award_stat(const_cast<Player&>(p), progress::DROPPED, s.item, s.count);
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

// Player.aiStep touch: entities in the player box inflated (1, 0.5, 1).
inline void entity_player_touches() {
    for (auto& pp : players) {
        Player& p = *pp;
        if (p.g.dead || p.gamemode == 3 || p.level != cur_level) continue;
        AABB box = AABB::at_feet(p.x, p.y, p.z, 0.6, 1.8).inflate(1.0, 0.5, 1.0);
        for_each_entity_in(box, [&](Entity& e) { e.player_touch(p); }, false);
    }
}
inline void tick_entities() { tick_world_entities(); }
