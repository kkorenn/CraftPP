#pragma once
// Environment: Village Raids (Raid state machine, waves 1-5+, raiders tracking, bossbar, victory, Hero of the Village)
#include "boss/level.hpp"
#include "commands/bossbar.hpp"
#include "commands/state.hpp"
#include "mobs/entity.hpp"
#include "mobs/manager.hpp"
#include "progress/progress.hpp"

namespace env {

struct Raid {
    int id = 0;
    int center_x = 0, center_y = 0, center_z = 0;
    int raid_omen_level = 1;
    int num_groups = 5;
    int groups_spawned = 0;
    int cooldown_ticks = 20;
    int post_raid_ticks = 0;
    int ticks_active = 0;
    float total_health = 0.0f;
    enum Status { ONGOING, VICTORY, LOSS, STOPPED } status = ONGOING;
    bool active = true;
    std::unordered_set<int> raiders;
    BossBar bossbar;

    int total_raiders_alive() {
        std::vector<int> dead;
        for (int eid : raiders) {
            auto* e = entity_by_id(eid);
            if (!e || e->removed || !e->is_alive()) dead.push_back(eid);
        }
        for (int eid : dead) raiders.erase(eid);
        return int(raiders.size());
    }

    float health_of_living_raiders() {
        float hp = 0.0f;
        for (int eid : raiders) {
            auto* e = entity_by_id(eid);
            if (e && !e->removed && e->is_alive()) {
                hp += e->health();
            }
        }
        return hp;
    }

    void update_bossbar() {
        if (status == ONGOING) {
            int alive = total_raiders_alive();
            if (groups_spawned == 0) {
                bossbar.name = lit("Raid");
                bossbar.value = 0;
                bossbar.max = 100;
            } else {
                bossbar.name = lit("Raid - Raiders remaining: " + std::to_string(alive));
                bossbar.max = int(std::max(1.0f, total_health));
                bossbar.value = int(health_of_living_raiders());
            }
        } else if (status == VICTORY) {
            bossbar.name = lit("Raid - Victory");
            bossbar.value = 0;
        } else if (status == LOSS) {
            bossbar.name = lit("Raid - Defeat");
            bossbar.value = 0;
        }
        bar_send(bossbar, 2);
        bar_send(bossbar, 3);
    }

    void update_players() {
        std::unordered_set<std::string> nearby;
        for (auto& p : players) {
            if (!p || p->level != &worlds[DIM_OVERWORLD]) continue;
            double dx = p->x - center_x, dz = p->z - center_z;
            if (dx * dx + dz * dz <= 96.0 * 96.0) {
                std::string u = player_uuid(*p);
                nearby.insert(u);
                if (!bossbar.players.count(u)) {
                    bossbar.players.insert(u);
                    p->send(bar_packet(bossbar, 0));
                }
            }
        }
        std::vector<std::string> to_remove;
        for (const auto& u : bossbar.players) {
            if (!nearby.count(u)) to_remove.push_back(u);
        }
        for (const auto& u : to_remove) {
            bossbar.players.erase(u);
            for (auto& p : players) {
                if (p && player_uuid(*p) == u) {
                    p->send(pkt(play_cb::boss_event).i64(bossbar.uh).i64(bossbar.ul).varint(1));
                }
            }
        }
    }

    void spawn_wave() {
        groups_spawned++;
        int w = std::clamp(groups_spawned, 1, 7);
        static const int PIL[] = {0, 4, 3, 3, 4, 4, 4, 2};
        static const int VIN[] = {0, 0, 2, 0, 1, 4, 2, 5};
        static const int RAV[] = {0, 0, 0, 1, 0, 1, 0, 2};
        static const int WIT[] = {0, 0, 0, 0, 3, 0, 0, 1};
        static const int EVO[] = {0, 0, 0, 0, 0, 1, 1, 2};
        int np = PIL[w], nv = VIN[w], nr = RAV[w], nwt = WIT[w], ne = EVO[w];

        ::play_sound("minecraft:event.raid.horn", 4, center_x + 0.5, center_y + 1.0, center_z + 0.5, 64.0f, 1.0f);

        bool captain_spawned = false;
        auto spawn_type = [&](int type, int count) {
            for (int i = 0; i < count; i++) {
                auto e = create_entity(type);
                if (!e) continue;
                double angle = rand_float() * 6.2831853f;
                double dist = 16.0 + rand_float() * 12.0;
                int sx = int(center_x + std::cos(angle) * dist);
                int sz = int(center_z + std::sin(angle) * dist);
                int sy = center_y;
                for (int y = std::min(world.max_y() - 1, center_y + 16); y >= std::max(world.min_y(), center_y - 16); y--) {
                    if (!lvl::is_air(world.get_block(sx, y, sz))) { sy = y + 1; break; }
                }
                e->snap_to(sx + 0.5, sy, sz + 0.5, rand_float() * 360.0f, 0.0f);
                bool make_captain = !captain_spawned && (type == entity_types::pillager);
                if (make_captain) captain_spawned = true;
                mob_finalize_raid(e.get(), make_captain);
                total_health += e->max_health();
                raiders.insert(e->id);
                add_entity(std::move(e));
            }
        };

        spawn_type(entity_types::pillager, np);
        spawn_type(entity_types::vindicator, nv);
        spawn_type(entity_types::witch, nwt);
        spawn_type(entity_types::evoker, ne);
        spawn_type(entity_types::ravager, nr);

        update_bossbar();
    }

    void tick() {
        if (!active) return;
        ticks_active++;
        if (ticks_active >= 48000) { stop(); return; }

        update_players();

        if (status == ONGOING) {
            int alive = total_raiders_alive();
            if (alive == 0) {
                if (groups_spawned >= num_groups) {
                    status = VICTORY;
                    update_bossbar();
                    for (auto& p : players) {
                        if (!p || p->level != &worlds[DIM_OVERWORLD]) continue;
                        double dx = p->x - center_x, dz = p->z - center_z;
                        if (dx * dx + dz * dz <= 96.0 * 96.0) {
                            int hero_amp = std::clamp(raid_omen_level - 1, 0, 4);
                            player_add_effect(*p, eff::hero_of_the_village, 48000, hero_amp);
                            progress::award_stat(*p, progress::CUSTOM, progress::custom_stat("raid_win"), 1);
                            progress::hero_of_the_village(*p);
                        }
                    }
                    ::play_sound("minecraft:ui.toast.challenge_complete", 3, center_x + 0.5, center_y + 1.0, center_z + 0.5, 1.0f, 1.0f);
                } else {
                    cooldown_ticks--;
                    if (cooldown_ticks <= 0) {
                        spawn_wave();
                        cooldown_ticks = 20;
                    }
                }
            } else {
                update_bossbar();
            }
        } else {
            post_raid_ticks++;
            if (post_raid_ticks >= 100) {
                stop();
            }
        }
    }

    void stop() {
        status = STOPPED;
        active = false;
        for (const auto& u : bossbar.players) {
            for (auto& p : players) {
                if (p && player_uuid(*p) == u) {
                    p->send(pkt(play_cb::boss_event).i64(bossbar.uh).i64(bossbar.ul).varint(1));
                }
            }
        }
        bossbar.players.clear();
    }
};

inline std::vector<std::unique_ptr<Raid>> active_raids;
inline int next_raid_id = 1;

inline Raid* get_raid_at(int x, int /*y*/, int z) {
    for (auto& r : active_raids) {
        if (r && r->active) {
            double dx = r->center_x - x, dz = r->center_z - z;
            if (dx * dx + dz * dz <= 96.0 * 96.0) return r.get();
        }
    }
    return nullptr;
}

inline Raid* create_or_extend_raid(Player& p, int x, int y, int z, int omen_level = 1) {
    if (p.level != &worlds[DIM_OVERWORLD]) return nullptr;
    if (auto* existing = get_raid_at(x, y, z)) {
        existing->raid_omen_level = std::min(5, existing->raid_omen_level + omen_level);
        return existing;
    }
    auto r = std::make_unique<Raid>();
    r->id = next_raid_id++;
    r->center_x = x; r->center_y = y; r->center_z = z;
    r->raid_omen_level = std::clamp(omen_level, 1, 5);
    int diff = difficulty();
    r->num_groups = diff == 1 ? 3 : (diff == 3 ? 7 : 5);
    r->cooldown_ticks = 10;
    r->bossbar.id = "minecraft:raid_" + std::to_string(r->id);
    r->bossbar.uh = int64_t(game_rng());
    r->bossbar.ul = int64_t(game_rng());
    r->bossbar.color = 2; // red
    r->bossbar.style = 2; // notched_10
    r->bossbar.name = lit("Raid");
    r->bossbar.max = 100;
    r->bossbar.value = 0;
    r->bossbar.visible = true;
    auto* ptr = r.get();
    active_raids.push_back(std::move(r));
    return ptr;
}

inline bool is_village(int x, int y, int z) {
    bool found = false;
    AABB box{x - 64.0, y - 32.0, z - 64.0, x + 64.0, y + 32.0, z + 64.0};
    for_each_entity_in(box, [&](Entity& e) {
        if (e.type == entity_types::villager || e.type == entity_types::iron_golem) found = true;
    }, false);
    if (found) return true;
    for (int dy = -8; dy <= 8 && !found; dy++) {
        for (int dx = -24; dx <= 24 && !found; dx += 2) {
            for (int dz = -24; dz <= 24 && !found; dz += 2) {
                int s = world.get_block(x + dx, y + dy, z + dz);
                if (s > 0 && lvl::is(s, lvl::tag("beds"))) found = true;
            }
        }
    }
    return found;
}

inline void tick_raids() {
    for (auto& r : active_raids) {
        if (r && r->active) r->tick();
    }
    std::erase_if(active_raids, [](const std::unique_ptr<Raid>& r) { return !r || !r->active; });
}

}  // namespace env
