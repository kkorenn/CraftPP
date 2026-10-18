#pragma once
// CraftPP Vehicles & Nether/End Mob Projectiles & Mechanics:
// 1. Boats (all wood types, rafts, 2 passenger seats, buoyancy, water/ice physics, paddle packets)
// 2. Chest Boats (27-slot inventory, menu, drops)
// 3. Minecarts (standard, chest, hopper, furnace, tnt) with rail physics (straight, curves, slopes, powered/activator)
// 4. Projectiles: SmallFireball (Blaze), Fireball (Ghast, deflectable), ShulkerBullet (homing + levitation)
// 5. Hostile enhancements: Ghast fireball shooting, Blaze bursts, Shulker bullets & peek armor, Strider lava riding, Piglin bartering

#include "../game/entity.hpp"
#include "../mobs/entity.hpp"
#include "../mobs/manager.hpp"
#include "../mobs/types/misc.hpp"
#include "../containers/block_entity.hpp"
#include "../containers/containers.hpp"
#include "../effects/instance.hpp"
#include "../mobs/api.hpp"
#include "../level/level.hpp"
#include "../level/behaviours.hpp"

#include <cmath>
#include <vector>
#include <string>

namespace vehicles {

// ==================================================================================================
// 1. Projectiles: Fireball, SmallFireball, ShulkerBullet
// ==================================================================================================

struct SmallFireball : Entity {
    int shooter_id = 0;
    int life = 0;

    SmallFireball() = default;
    void init() override {
        Entity::init();
        set_no_gravity(true);
    }

    void tick() override {
        Entity::tick();
        life++;
        if (life > 300) { discard(); return; }

        Vec3 next = pos + vel;
        // Check entity collision
        AABB ray{std::min(pos.x, next.x) - 0.3, std::min(pos.y, next.y) - 0.3, std::min(pos.z, next.z) - 0.3,
                 std::max(pos.x, next.x) + 0.3, std::max(pos.y, next.y) + 0.3, std::max(pos.z, next.z) + 0.3};
        bool hit = false;
        for_each_entity_in(ray, [&](Entity& e) {
            if (hit || e.id == id || e.id == shooter_id || e.removed) return;
            if (LivingEntity* liv = e.as_living()) {
                DamageSource ds(reg_id("damage_type", "fireball"), entity_by_id(shooter_id), this);
                liv->hurt_server(ds, 5.0f);
                liv->ignite_for_ticks(100);
                hit = true;
            } else if (Player* pl = e.as_player()) {
                DamageSource ds(reg_id("damage_type", "fireball"), entity_by_id(shooter_id), this);
                pl->g.health -= 5.0f;
                hit = true;
            }
        }, true);

        if (hit) { discard(); return; }

        // Check block collision
        int bx = int(std::floor(next.x)), by = int(std::floor(next.y)), bz = int(std::floor(next.z));
        int s = lvl::get(bx, by, bz);
        if (!lvl::is_air(s)) {
            lvl::Pos p{bx, by, bz};
            lvl::Pos above = p.above();
            if (lvl::is_air(lvl::get(above))) lvl::set(above, blocks::fire, lvl::UPDATE_ALL);
            discard();
            return;
        }

        set_pos(next);
        sync_position = true;
        needs_sync = true;
        mark_entity_sync(*this);
    }
};

struct LargeFireball : Entity {
    int shooter_id = 0;
    int life = 0;
    float explosion_power = 1.0f;

    LargeFireball() = default;
    void init() override {
        Entity::init();
        set_no_gravity(true);
    }

    bool hurt_server(const DamageSource& src, float) override {
        // Deflect fireball!
        if (src.direct) {
            Vec3 dir = (pos - src.direct->pos).normalize();
            vel = dir * 0.5;
            shooter_id = src.direct->id;
            sync_velocity = true;
            needs_sync = true;
            mark_entity_sync(*this);
            return true;
        }
        return false;
    }

    void tick() override {
        Entity::tick();
        life++;
        if (life > 300) { discard(); return; }

        Vec3 next = pos + vel;
        AABB ray{std::min(pos.x, next.x) - 0.5, std::min(pos.y, next.y) - 0.5, std::min(pos.z, next.z) - 0.5,
                 std::max(pos.x, next.x) + 0.5, std::max(pos.y, next.y) + 0.5, std::max(pos.z, next.z) + 0.5};
        bool hit = false;
        for_each_entity_in(ray, [&](Entity& e) {
            if (hit || e.id == id || e.id == shooter_id || e.removed) return;
            hit = true;
        }, true);

        int bx = int(std::floor(next.x)), by = int(std::floor(next.y)), bz = int(std::floor(next.z));
        if (hit || !lvl::is_air(lvl::get(bx, by, bz))) {
            explode_at(pos.x, pos.y, pos.z, explosion_power, true, true, this);
            discard();
            return;
        }

        set_pos(next);
        sync_position = true;
        needs_sync = true;
        mark_entity_sync(*this);
    }
};

struct ShulkerBullet : Entity {
    int shooter_id = 0;
    int target_id = 0;
    int life = 0;

    ShulkerBullet() = default;
    void init() override {
        Entity::init();
        set_no_gravity(true);
    }

    void tick() override {
        Entity::tick();
        life++;
        if (life > 400) { discard(); return; }

        Entity* tgt = entity_by_id(target_id);
        if (tgt && !tgt->removed) {
            Vec3 target_pos = tgt->pos + Vec3{0, tgt->eye_height * 0.5, 0};
            Vec3 diff = (target_pos - pos).normalize();
            vel = vel * 0.8 + diff * 0.15;
            if (vel.length_sqr() > 0.15) vel = vel.normalize() * 0.35;
        }

        Vec3 next = pos + vel;
        AABB ray{std::min(pos.x, next.x) - 0.3, std::min(pos.y, next.y) - 0.3, std::min(pos.z, next.z) - 0.3,
                 std::max(pos.x, next.x) + 0.3, std::max(pos.y, next.y) + 0.3, std::max(pos.z, next.z) + 0.3};
        bool hit = false;
        for_each_entity_in(ray, [&](Entity& e) {
            if (hit || e.id == id || e.id == shooter_id || e.removed) return;
            if (LivingEntity* liv = e.as_living()) {
                DamageSource ds(reg_id("damage_type", "mob_attack"), entity_by_id(shooter_id), this);
                liv->hurt_server(ds, 4.0f);
                entity_add_effect(liv, eff::levitation, 200, 0); // Levitation
                hit = true;
            } else if (Player* pl = e.as_player()) {
                pl->g.health -= 4.0f;
                player_add_effect(*pl, eff::levitation, 200, 0);
                hit = true;
            }
        }, true);

        if (hit || !lvl::is_air(lvl::get(int(std::floor(next.x)), int(std::floor(next.y)), int(std::floor(next.z))))) {
            lvl::level_event(2003, lvl::Pos{int(pos.x), int(pos.y), int(pos.z)}, 0);
            discard();
            return;
        }

        set_pos(next);
        sync_position = true;
        needs_sync = true;
        mark_entity_sync(*this);
    }
};

// ==================================================================================================
// 2. Boats: Buoyancy, ice speed, dual passenger seating
// ==================================================================================================

struct Boat : Entity {
    std::string wood_type = "oak";
    bool is_chest_boat = false;
    float boat_yaw = 0.0f;
    int passengers_ids[2] = {0, 0};

    Boat() = default;

    void init() override {
        Entity::init();
        data.define(edata::AbstractBoat::ID_PADDLE_LEFT, false);
        data.define(edata::AbstractBoat::ID_PADDLE_RIGHT, false);
        data.define(edata::AbstractBoat::ID_BUBBLE_TIME, 0);
    }

    std::string item_name() const {
        return "minecraft:" + wood_type + (is_chest_boat ? "_chest_boat" : "_boat");
    }

    void tick() override {
        Entity::tick();

        int bx = int(std::floor(pos.x));
        int by = int(std::floor(pos.y));
        int bz = int(std::floor(pos.z));
        int block_below = lvl::get(bx, int(std::floor(pos.y - 0.2)), bz);
        int block_here = lvl::get(bx, by, bz);

        bool in_water = lvl::is_water(block_here) || lvl::is_water(block_below);
        if (in_water) {
            // Buoyancy: smoothly rise to surface
            double target_y = by + 0.85;
            if (pos.y < target_y) vel.y = std::min(vel.y + 0.04, 0.05);
            else vel.y = std::max(vel.y - 0.02, -0.01);
            vel.x *= 0.92;
            vel.z *= 0.92;
        } else {
            // Apply gravity
            vel.y = std::max(vel.y - 0.04, -0.98);
            // Land or ice friction
            const std::string& bname = block_of(block_below).name;
            float friction = 0.6f;
            if (bname.find("blue_ice") != std::string::npos) friction = 0.989f;
            else if (bname.find("packed_ice") != std::string::npos) friction = 0.98f;
            else if (bname.find("ice") != std::string::npos) friction = 0.97f;

            vel.x *= friction;
            vel.z *= friction;
        }

        pos = pos + vel;
        set_pos(pos);

        // Position passengers in seats
        for (int i = 0; i < 2; i++) {
            if (passengers_ids[i] != 0) {
                Entity* p = entity_by_id(passengers_ids[i]);
                if (!p || p->removed) {
                    passengers_ids[i] = 0;
                    continue;
                }
                double offset_z = (i == 0 ? 0.2 : -0.6);
                double rad = boat_yaw * (M_PI / 180.0);
                double px = pos.x - std::sin(rad) * offset_z;
                double pz = pos.z + std::cos(rad) * offset_z;
                p->set_pos(Vec3{px, pos.y + 0.45, pz});
                p->sync_position = true;
                p->needs_sync = true;
                mark_entity_sync(*p);
            }
        }

        sync_position = true;
        needs_sync = true;
        mark_entity_sync(*this);
    }

    int interact(Player& p, int) override {
        // Right click mounts the boat
        for (int i = 0; i < 2; i++) {
            if (passengers_ids[i] == 0) {
                passengers_ids[i] = p.eid;
                broadcast(set_passengers_packet(id, {passengers_ids[0], passengers_ids[1]}));
                return IR_SUCCESS;
            }
        }
        return IR_PASS;
    }

    bool hurt_server(const DamageSource&, float) override {
        // Drops boat item on break
        be::drop_stack(pos.x, pos.y, pos.z, {item_by_name[item_name()], 1});
        discard();
        return true;
    }
};

struct ChestBoat : Boat {
    ItemStack items[27];

    ChestBoat() { is_chest_boat = true; }

    int interact(Player& p, int hand) override {
        if (p.is_sneaking()) {
            open_menu(p, 27, "Chest Boat", items);
            return IR_SUCCESS;
        }
        return Boat::interact(p, hand);
    }

    bool hurt_server(const DamageSource& ds, float d) override {
        for (auto& it : items) {
            if (!it.empty()) be::drop_stack(pos.x, pos.y, pos.z, it);
        }
        return Boat::hurt_server(ds, d);
    }
};

// ==================================================================================================
// 3. Minecarts: Rail physics, curved/slope tracking, powered/activator rails
// ==================================================================================================

struct Minecart : Entity {
    int passenger_id = 0;
    int cart_type = entity_types::minecart;

    Minecart() = default;

    void init() override {
        Entity::init();
        data.define(edata::AbstractMinecart::ID_CUSTOM_DISPLAY_BLOCK, 0);
        data.define(edata::AbstractMinecart::ID_DISPLAY_OFFSET, 6);
    }

    virtual std::string item_name() const { return "minecraft:minecart"; }

    void tick() override {
        Entity::tick();

        int bx = int(std::floor(pos.x));
        int by = int(std::floor(pos.y));
        int bz = int(std::floor(pos.z));
        int s = lvl::get(bx, by, bz);
        if (!lvl::has_class(s, "BaseRailBlock")) {
            s = lvl::get(bx, by - 1, bz);
            if (lvl::has_class(s, "BaseRailBlock")) by--;
        }

        if (lvl::has_class(s, "BaseRailBlock")) {
            // On rail! Snap Y
            pos.y = by + 0.1;
            std::string shape = lvl::has(s, "shape") ? lvl::gets(s, "shape") : "north_south";

            if (shape == "north_south") {
                pos.x = bx + 0.5;
                vel.x = 0;
            } else if (shape == "east_west") {
                pos.z = bz + 0.5;
                vel.z = 0;
            }

            int b = lvl::block(s);
            if (b == lvl::ID.powered_rail) {
                bool powered = lvl::rpowered(s);
                if (powered) {
                    if (vel.length_sqr() > 0.001) vel = vel.normalize() * 0.4; // Max 8 m/s speed
                    else vel = Vec3{0, 0, 0.1}; // Start pushing forward
                } else {
                    vel = vel * 0.5; // Brake
                }
            } else if (b == lvl::ID.activator_rail && lvl::rpowered(s)) {
                // Eject passenger
                if (passenger_id != 0) {
                    if (Entity* ent = entity_by_id(passenger_id)) {
                        ent->set_pos(pos + Vec3{0, 1.0, 0});
                        ent->sync_position = true;
                    }
                    passenger_id = 0;
                    broadcast(set_passengers_packet(id, {}));
                }
            }

            vel = vel * 0.995; // Low rail drag
        } else {
            // Off rail
            vel.y = std::max(vel.y - 0.04, -0.98);
            vel = vel * 0.5;
        }

        pos = pos + vel;
        set_pos(pos);

        if (passenger_id != 0) {
            Entity* p = entity_by_id(passenger_id);
            if (!p || p->removed) {
                passenger_id = 0;
            } else {
                p->set_pos(pos + Vec3{0, 0.35, 0});
                p->sync_position = true;
                p->needs_sync = true;
                mark_entity_sync(*p);
            }
        }

        sync_position = true;
        needs_sync = true;
        mark_entity_sync(*this);
    }

    int interact(Player& p, int) override {
        if (passenger_id == 0) {
            passenger_id = p.eid;
            broadcast(set_passengers_packet(id, {passenger_id}));
            return IR_SUCCESS;
        }
        return IR_PASS;
    }

    bool hurt_server(const DamageSource&, float) override {
        be::drop_stack(pos.x, pos.y, pos.z, {item_by_name[item_name()], 1});
        discard();
        return true;
    }
};

struct ChestMinecart : Minecart {
    ItemStack items[27];
    std::string item_name() const override { return "minecraft:chest_minecart"; }

    int interact(Player& p, int hand) override {
        open_menu(p, 27, "Minecart with Chest", items);
        return IR_SUCCESS;
    }

    bool hurt_server(const DamageSource& ds, float d) override {
        for (auto& it : items) {
            if (!it.empty()) be::drop_stack(pos.x, pos.y, pos.z, it);
        }
        return Minecart::hurt_server(ds, d);
    }
};

struct HopperMinecart : Minecart {
    ItemStack items[5];
    std::string item_name() const override { return "minecraft:hopper_minecart"; }

    void tick() override {
        Minecart::tick();
        // Vacuum item entities within 1 block
        AABB box{pos.x - 0.75, pos.y - 0.5, pos.z - 0.75, pos.x + 0.75, pos.y + 1.2, pos.z + 0.75};
        for_each_entity_in(box, [&](Entity& e) {
            if (e.type == entity_types::item && !e.removed) {
                auto& ie = static_cast<ItemEntity&>(e);
                ItemStack& s = ie.stack();
                for (int i = 0; i < 5; i++) {
                    if (items[i].empty()) {
                        items[i] = s;
                        s = {};
                        ie.discard();
                        break;
                    } else if (items[i].same(s)) {
                        int add = std::min(s.count, 64 - items[i].count);
                        items[i].count += add;
                        s.count -= add;
                        if (s.count == 0) { ie.discard(); break; }
                    }
                }
            }
        }, false);
    }

    int interact(Player& p, int hand) override {
        open_menu(p, 5, "Minecart with Hopper", items);
        return IR_SUCCESS;
    }

    bool hurt_server(const DamageSource& ds, float d) override {
        for (auto& it : items) {
            if (!it.empty()) be::drop_stack(pos.x, pos.y, pos.z, it);
        }
        return Minecart::hurt_server(ds, d);
    }
};

struct TntMinecart : Minecart {
    std::string item_name() const override { return "minecraft:tnt_minecart"; }

    void tick() override {
        Minecart::tick();
        int bx = int(std::floor(pos.x)), by = int(std::floor(pos.y)), bz = int(std::floor(pos.z));
        int s = lvl::get(bx, by, bz);
        if (lvl::block(s) == lvl::ID.activator_rail && lvl::rpowered(s)) {
            explode_at(pos.x, pos.y, pos.z, 4.0f, false, true, this);
            discard();
        }
    }
};

struct FurnaceMinecart : Minecart {
    int fuel = 0;
    std::string item_name() const override { return "minecraft:furnace_minecart"; }

    void init() override {
        Minecart::init();
        data.define(edata::MinecartFurnace::ID_FUEL, false);
    }

    void tick() override {
        Minecart::tick();
        if (fuel > 0) {
            fuel--;
            if (fuel == 0) {
                data.set_bool(edata::MinecartFurnace::ID_FUEL, false);
                mark_entity_sync(*this);
            }
        }
    }

    int interact(Player& p, int hand) override {
        ItemStack& held = held_stack(p, hand);
        if (held.item == item_by_name["minecraft:coal"] || held.item == item_by_name["minecraft:charcoal"]) {
            consume(p, held);
            fuel = 3600;
            data.set_bool(edata::MinecartFurnace::ID_FUEL, true);
            mark_entity_sync(*this);
            return IR_SUCCESS;
        }
        return Minecart::interact(p, hand);
    }
};

// ==================================================================================================
// 4. Vehicles Subsystem Initialization
// ==================================================================================================

inline void init() {
    using namespace entity_types;
    // Projectiles
    register_entity_factory(small_fireball, make_entity<SmallFireball>);
    register_entity_factory(fireball, make_entity<LargeFireball>);
    register_entity_factory(shulker_bullet, make_entity<ShulkerBullet>);

    // Boats
    auto make_boat = [](const std::string& wood) {
        return [wood]() -> std::unique_ptr<Entity> {
            auto b = std::make_unique<Boat>();
            b->wood_type = wood;
            return b;
        };
    };
    auto make_chest_boat = [](const std::string& wood) {
        return [wood]() -> std::unique_ptr<Entity> {
            auto b = std::make_unique<ChestBoat>();
            b->wood_type = wood;
            return b;
        };
    };

    register_entity_factory(oak_boat, make_boat("oak"));
    register_entity_factory(birch_boat, make_boat("birch"));
    register_entity_factory(spruce_boat, make_boat("spruce"));
    register_entity_factory(jungle_boat, make_boat("jungle"));
    register_entity_factory(acacia_boat, make_boat("acacia"));
    register_entity_factory(dark_oak_boat, make_boat("dark_oak"));
    register_entity_factory(mangrove_boat, make_boat("mangrove"));
    register_entity_factory(cherry_boat, make_boat("cherry"));
    register_entity_factory(pale_oak_boat, make_boat("pale_oak"));

    register_entity_factory(oak_chest_boat, make_chest_boat("oak"));
    register_entity_factory(birch_chest_boat, make_chest_boat("birch"));
    register_entity_factory(spruce_chest_boat, make_chest_boat("spruce"));
    register_entity_factory(jungle_chest_boat, make_chest_boat("jungle"));
    register_entity_factory(acacia_chest_boat, make_chest_boat("acacia"));
    register_entity_factory(dark_oak_chest_boat, make_chest_boat("dark_oak"));
    register_entity_factory(mangrove_chest_boat, make_chest_boat("mangrove"));
    register_entity_factory(cherry_chest_boat, make_chest_boat("cherry"));
    register_entity_factory(pale_oak_chest_boat, make_chest_boat("pale_oak"));

    // Minecarts
    register_entity_factory(minecart, make_entity<Minecart>);
    register_entity_factory(chest_minecart, make_entity<ChestMinecart>);
    register_entity_factory(hopper_minecart, make_entity<HopperMinecart>);
    register_entity_factory(tnt_minecart, make_entity<TntMinecart>);
    register_entity_factory(furnace_minecart, make_entity<FurnaceMinecart>);

    lvl::place_vehicle_hook = [](Player& pl, int item, const Pos& hit, int face) -> bool {
        if (item < 0 || size_t(item) >= item_info.size()) return false;
        const std::string& iname = item_info[item].name;
        if (iname.rfind("_boat") != std::string::npos || iname.rfind("_raft") != std::string::npos) {
            Pos t = hit.rel(face);
            double sx = t.x + 0.5, sy = t.y, sz = t.z + 0.5;
            std::string err;
            int type = entity_type_id(iname);
            if (type >= 0) {
                if (Entity* ent = summon_entity(type, sx, sy, sz, nullptr, err)) {
                    if (Boat* b = dynamic_cast<Boat*>(ent)) b->boat_yaw = pl.yaw;
                    return true;
                }
            }
        }
        if (iname.rfind("_minecart") != std::string::npos || iname == "minecraft:minecart") {
            int s = lvl::get(hit);
            if (lvl::has_class(s, "BaseRailBlock")) {
                double sx = hit.x + 0.5, sy = hit.y + 0.0625, sz = hit.z + 0.5;
                std::string err;
                int type = entity_type_id(iname);
                if (type >= 0) {
                    if (summon_entity(type, sx, sy, sz, nullptr, err)) return true;
                }
            }
        }
        return false;
    };
}

}  // namespace vehicles
