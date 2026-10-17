#pragma once
// Entry points of the mob module (src/mobs/*.cpp) for the rest of the server (survival, commands).
// Everything is tick-thread only. Framework overview: src/mobs/mobs.hpp.
#include <string>

struct Entity;
struct Nbt;
struct Player;
struct DamageSource;
struct ItemStack;

void mobs_init();  // game_init: data, entity types, hooks, packet handlers
// /summon: creates `type` (entity_types::*) at x y z, merges `tag` (may be null), runs finalizeSpawn for mobs
// without NBT. Returns null and sets `err` to a translation key on failure.
Entity* summon_entity(int type, double x, double y, double z, const Nbt* tag, std::string& err);
void spawn_xp_orbs(double x, double y, double z, int amount);  // ExperienceOrb.award
// Player damage with armour, toughness and difficulty scaling (survival hurt() routes through this).
float player_damage_after_armor(Player& p, const DamageSource& s, float amount);
void player_knockback_from(Player& p, const DamageSource& s);  // LivingEntity.dealDefaultKnockback for players
void mobs_save_all();
void player_entity_respawned(Player& p);  // respawn: watchers drop and re-add the player entity  // save every loaded entity chunk (server stop)

// Mob effects / enchantments on players (src/effects/effects.cpp).
float player_max_health(const Player& p);
bool player_has_effect(const Player& p, const char* effect);  // "slow_falling"
double player_attribute(const Player& p, const char* attribute);  // "safe_fall_distance"
bool player_effect_blocks_damage(Player& p, const DamageSource& s);  // fire resistance vs is_fire
// Resistance, protection enchantments and absorption (getDamageAfterMagicAbsorb + actuallyHurt); returns health loss.
float player_damage_after_effects(Player& p, const DamageSource& s, float amount);
void player_hurt_effects(Player& p, const DamageSource& s, float amount);  // MobEffect.onMobHurt (infested)
float player_destroy_speed(Player& p, float tool_speed, bool eye_in_water);  // Player.getDestroySpeed after the tool
void effects_player_respawned(Player& p);  // drop the old effects' modifiers and absorption
void player_consume_effects(Player& p, int item, const ItemStack& stack);  // Consumable.onConsume effects (food, potions, milk)
