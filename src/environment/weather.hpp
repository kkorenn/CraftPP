#pragma once
// Environment: weather simulation, lightning bolt entities, thunder strikes, copper de-oxidation.
#include "boss/level.hpp"
#include "commands/state.hpp"
#include "mobs/entity.hpp"
#include "mobs/manager.hpp"
#include "progress/progress.hpp"

namespace env {

// De-oxidize copper blocks hit by lightning (WeatheringCopper.getPrevious)
inline void deoxidize_copper(int x, int y, int z) {
    int s = world.get_block(x, y, z);
    if (s <= 0) return;
    std::string n = block_of(s).name;
    if (n.rfind("minecraft:waxed_", 0) == 0 || n.rfind("waxed_", 0) == 0 || n.find("waxed_") != std::string::npos) return;
    std::string target;
    if (n.find("oxidized_") != std::string::npos) {
        size_t pos = n.find("oxidized_");
        target = n.substr(0, pos) + "weathered_" + n.substr(pos + 9);
    } else if (n.find("weathered_") != std::string::npos) {
        size_t pos = n.find("weathered_");
        target = n.substr(0, pos) + "exposed_" + n.substr(pos + 10);
    } else if (n.find("exposed_") != std::string::npos) {
        size_t pos = n.find("exposed_");
        std::string prefix = n.substr(0, pos);
        std::string suffix = n.substr(pos + 8);
        if (suffix == "copper") suffix = "copper_block";
        target = prefix + suffix;
    }
    if (!target.empty()) {
        int b = block_id(target.c_str());
        if (b >= 0) {
            int new_state = lvl::def(b);
            world.set_block(x, y, z, new_state);
            broadcast(block_update(x, y, z, new_state));
            ::play_sound("minecraft:item.axe.scrape", 4, x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
        }
    }
}

// LightningBolt entity (EntityType.LIGHTNING_BOLT, id 78)
struct LightningBolt : Entity {
    int life = 2;
    int flashes = 1;
    bool visual_only = false;
    Player* cause = nullptr;
    std::unordered_set<int> hit_entities;

    void init() override {
        Entity::init();
        life = 2;
        flashes = 1 + rand_int(3);
    }

    void tick() override {
        Entity::tick();
        if (life == 2) {
            ::play_sound("minecraft:entity.lightning_bolt.thunder", 3, pos.x, pos.y, pos.z, 10000.0f, 0.8f + rand_float() * 0.2f);
            ::play_sound("minecraft:entity.lightning_bolt.impact", 3, pos.x, pos.y, pos.z, 2.0f, 0.5f + rand_float() * 0.2f);

            int bx = block_x(), by = block_y(), bz = block_z();
            // Check lightning rod
            for (int dy = 0; dy >= -1; dy--) {
                int s = world.get_block(bx, by + dy, bz);
                if (block_of(s).name.find("lightning_rod") != std::string::npos) {
                    world.set_block(bx, by + dy, bz, lvl::with(s, "powered", true));
                    broadcast(block_update(bx, by + dy, bz, lvl::with(s, "powered", true)));
                    level_event(3002, bx, by + dy, bz, 0, nullptr);
                    break;
                }
            }

            // Copper oxidation stripping within 1 block
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dz = -1; dz <= 1; dz++)
                        deoxidize_copper(bx + dx, by + dy, bz + dz);

            // Spawn fire on solid surfaces around strike
            if (difficulty() >= 2 && !visual_only) {
                for (int i = 0; i < 4; i++) {
                    int fx = bx + rand_int(3) - 1, fz = bz + rand_int(3) - 1;
                    int fy = by + rand_int(3) - 1;
                    if (fy >= world.min_y() && fy < world.max_y() && lvl::is_air(world.get_block(fx, fy, fz)) && !lvl::is_air(world.get_block(fx, fy - 1, fz))) {
                        world.set_block(fx, fy, fz, blocks::fire);
                        broadcast(block_update(fx, fy, fz, blocks::fire));
                    }
                }
            }
        }

        if (life >= 0 && !visual_only) {
            AABB box{pos.x - 3.0, pos.y - 3.0, pos.z - 3.0, pos.x + 3.0, pos.y + 9.0, pos.z + 3.0};
            for_each_entity_in(box, [&](Entity& e) {
                if (&e == this || hit_entities.count(e.id)) return;
                hit_entities.insert(e.id);
                e.thunder_hit(this);
            }, false);

            for (auto& p : players) {
                if (!boss::here(*p)) continue;
                double dx = p->x - pos.x, dy = p->y - pos.y, dz = p->z - pos.z;
                if (dx * dx + dy * dy + dz * dz <= 9.0) {
                    boss::hurt_player(*p, 5.0f, "lightning_bolt", true);
                    p->send(Writer().varint(play_cb::set_entity_motion).varint(p->eid).i16(0).i16(0).i16(0));
                }
            }
        }

        life--;
        if (life < 0) {
            if (flashes == 0) {
                remove(Removal::DISCARDED);
            } else if (life < -rand_int(10)) {
                flashes--;
                life = 1;
            }
        }
    }
};

inline LightningBolt* spawn_lightning(double x, double y, double z, Player* cause = nullptr, bool visual = false) {
    auto e = std::make_unique<LightningBolt>();
    e->type = entity_types::lightning_bolt;
    e->visual_only = visual;
    e->cause = cause;
    e->snap_to(x, y, z, 0, 0);
    return add_entity_t(std::move(e));
}

// Tick hook for ambient lightning during thunderstorms
inline void tick_weather_lightning() {
    if (!weather.thundering || !rule("advance_weather") || players.empty()) return;
    if (rand_int(100) != 0) return;

    auto& p = *players[size_t(rand_int(int(players.size())))];
    if (p.level != &worlds[DIM_OVERWORLD]) return;

    int cx = int(std::floor(p.x)) + rand_int(48) - 24;
    int cz = int(std::floor(p.z)) + rand_int(48) - 24;

    // Search for lightning rod within 32 blocks
    int rod_x = 0, rod_y = 0, rod_z = 0;
    bool found_rod = false;
    for (int dx = -16; dx <= 16 && !found_rod; dx++) {
        for (int dz = -16; dz <= 16 && !found_rod; dz++) {
            int tx = int(std::floor(p.x)) + dx;
            int tz = int(std::floor(p.z)) + dz;
            for (int ty = int(std::floor(p.y)) + 16; ty >= int(std::floor(p.y)) - 16; ty--) {
                if (ty < world.min_y() || ty >= world.max_y()) continue;
                int s = world.get_block(tx, ty, tz);
                if (block_of(s).name.find("lightning_rod") != std::string::npos) {
                    rod_x = tx; rod_y = ty; rod_z = tz;
                    found_rod = true;
                    break;
                }
            }
        }
    }

    if (found_rod) {
        spawn_lightning(rod_x + 0.5, rod_y + 0.5, rod_z + 0.5);
    } else {
        // Strike surface
        int top_y = world.min_y() + 1;
        for (int y = world.max_y() - 1; y >= world.min_y(); y--) {
            int s = world.get_block(cx, y, cz);
            if (!lvl::is_air(s)) { top_y = y + 1; break; }
        }
        if (top_y > world.min_y()) {
            spawn_lightning(cx + 0.5, top_y, cz + 0.5);
        }
    }
}

}  // namespace env
