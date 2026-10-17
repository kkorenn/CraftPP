#pragma once
// Progression (src/progress/): advancements (net.minecraft.advancements, server.PlayerAdvancements), the recipe
// book (stats.ServerRecipeBook, recipebook.ServerPlaceRecipe) and statistics (stats.ServerStatsCounter).
// Other subsystems call these at the vanilla call sites, one line each. Tick thread only.
#include <cstdint>
#include <memory>

struct Player;
struct Entity;
struct ItemStack;
struct DamageSource;
struct Nbt;

namespace progress {
struct State;  // per-player advancement progress, recipe book, stats (progress.hpp)

void init();  // game_init: data (loaded on a background thread), hooks, packet handlers, commands
void load_player(Player& p, const Nbt& t);        // players/data tags (recipeBook, entered_nether_pos) + json files
void save_player(const Player& p, Nbt& t);        // same; also writes players/advancements + players/stats

// ---- criteria triggers (CriteriaTriggers.X.trigger) ----
void player_killed_entity(Player& p, Entity& victim, const DamageSource& s);  // also killed / mob_kills stats
void entity_killed_player(Player& p, Entity* killer, const DamageSource& s);  // also deaths / killed_by stats
void changed_dimension(Player& p, int from_dim, int to_dim);                  // DIM_* indices
void placed_block(Player& p, int x, int y, int z, const ItemStack& used);     // BlockItem.place (+ used stat)
void item_used_on_block(Player& p, int x, int y, int z, const ItemStack& used);
void consume_item(Player& p, const ItemStack& s);                             // Consumable.onConsume (+ used stat)
void bred_animals(Player& p, Entity& parent, Entity& partner, Entity* child);  // + animals_bred stat
void tame_animal(Player& p, Entity& animal);
void villager_trade(Player& p, Entity& villager, const ItemStack& result);    // + traded_with_villager stat
void brewed_potion(Player& p, int potion);                                    // minecraft:potion registry id
void enchanted_item(Player& p, const ItemStack& s, int levels);               // + enchant_item stat
void effects_changed(Player& p, Entity* source);
void summoned_entity(Player& p, Entity& e);
void slept_in_bed(Player& p);
void used_ender_eye(Player& p, int x, int y, int z);                          // nearest stronghold position
void construct_beacon(Player& p, int levels);
void levitation(Player& p, double sx, double sy, double sz, int ticks);       // start position, ticks levitating
void hero_of_the_village(Player& p);
void enter_block(Player& p, int state);
void moved(Player& p, double ox, double oy, double oz, bool was_on_ground);    // movement stats, jump, fall

// MobEffectsPredicate reads effects through this (set by the effects module): the entity's instance of
// `effect` (mob_effect registry id), or false when it has none.
inline bool (*player_effect)(Entity& e, int effect, int& amplifier, int& duration, bool& ambient, bool& visible) = nullptr;

// ---- recipe book ----
// Crafting result taken (ResultSlot.checkTakeAchievements): crafted stat, recipe_crafted, unlocks the recipe.
// `recipe`: index into ::recipes (inventory.hpp); `grid`: the ingredients before they were consumed.
void crafted(Player& p, int recipe, const ItemStack& result, const ItemStack* grid, int n);
bool may_craft(Player& p, int recipe);  // limited_crafting game rule: only known recipes produce a result

// ---- statistics (Stats / StatType registry order) ----
enum StatType { MINED, CRAFTED, USED, BROKEN, PICKED_UP, DROPPED, KILLED, KILLED_BY, CUSTOM };
void award_stat(Player& p, int type, int id, int amount = 1);  // id: item, block, entity type or custom stat
int custom_stat(const char* name);                              // custom_stat registry id ("jump"), -1 unknown
inline void custom(Player& p, const char* name, int amount = 1) { award_stat(p, CUSTOM, custom_stat(name), amount); }
}  // namespace progress
