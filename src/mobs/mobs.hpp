#pragma once
#include "../play.hpp"

inline std::unique_ptr<Entity> make_item_entity() { return std::make_unique<ItemEntity>(); }

void mobs_init() {
    if (!load_mob_data()) _exit(1);
    register_entity_factory(entity_types::item, make_item_entity);
}
Entity* summon_entity(int, double, double, double, const Nbt*, std::string& err) { err = "commands.summon.failed"; return nullptr; }
void spawn_xp_orbs(double, double, double, int) {}
float player_damage_after_armor(Player&, const DamageSource&, float a) { return a; }
void player_knockback_from(Player&, const DamageSource&) {}
void mobs_save_all() { save_all_entities(); }
