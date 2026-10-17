#pragma once
// Environment & Mechanics subsystem entry point.
#include "weather.hpp"
#include "sleep.hpp"
#include "raid.hpp"

namespace env {

inline void tick() {
    tick_weather_lightning();
    tick_sleeping();
    tick_raids();
}

inline void init() {
    register_entity_factory(entity_types::lightning_bolt, make_entity<LightningBolt>);

    lvl::use_bed_hook = use_bed;
    lvl::use_anchor_hook = use_respawn_anchor;

    cmd_raid_start = [](Player& p, int omen_lvl) -> bool {
        int px = int(std::floor(p.x)), py = int(std::floor(p.y)), pz = int(std::floor(p.z));
        if (get_raid_at(px, py, pz)) return false;
        return create_or_extend_raid(p, px, py, pz, omen_lvl) != nullptr;
    };
    cmd_raid_stop = [](Player& p) -> bool {
        int px = int(std::floor(p.x)), py = int(std::floor(p.y)), pz = int(std::floor(p.z));
        auto* r = get_raid_at(px, py, pz);
        if (!r) return false;
        r->stop();
        return true;
    };
    cmd_raid_check = [](Player& p) -> std::optional<RaidInfo> {
        int px = int(std::floor(p.x)), py = int(std::floor(p.y)), pz = int(std::floor(p.z));
        auto* r = get_raid_at(px, py, pz);
        if (!r) return std::nullopt;
        return RaidInfo{r->groups_spawned, r->raid_omen_level, r->total_raiders_alive(), r->health_of_living_raiders(), r->total_health};
    };

    bad_omen_village_check = [](Player& p, int amp) -> bool {
        if (p.level != &worlds[DIM_OVERWORLD] || difficulty() == 0) return false;
        int px = int(std::floor(p.x)), py = int(std::floor(p.y)), pz = int(std::floor(p.z));
        if (is_village(px, py, pz)) {
            player_add_effect(p, eff::raid_omen, 600, amp);
            return true;
        }
        return false;
    };
    raid_omen_expire = [](Player& p, int amp) {
        int px = int(std::floor(p.x)), py = int(std::floor(p.y)), pz = int(std::floor(p.z));
        create_or_extend_raid(p, px, py, pz, amp + 1);
    };

    on_tick(tick);
}

}  // namespace env
