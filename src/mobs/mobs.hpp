#pragma once
// ============================================================================================================
// CraftPP mob framework (port of vanilla 26.3 net.minecraft.world.entity.*). Tick-thread only.
// Include this header from a mob module .cpp (src/mobs/**/*.cpp are compiled automatically).
//
// FILES
//   data.hpp      generated tables (data/mobs.json, tools/gen_mob_data.py): per-state collision boxes, block flags,
//                 path types, fluids; attributes; entity types (size, tracking, default attributes, spawn placement);
//                 mob categories; biome spawners; tags; item attribute modifiers. Entity data slots: src/gen/entity_data.hpp
//   entity.hpp    Entity base: SynchedData, DamageSource (+ DT_* damage tag bits), block_at() (loaded chunks only,
//                 unloaded = barrier), Entity.move (collision shapes, step-up), fluids, persistence (NBT)
//   manager.hpp   storage, chunk index, O(active) ticking, tracking + movement packets, entity chunk files
//   living.hpp    LivingEntity (attributes, damage, armour, death, loot, xp, travel physics), PlayerEntity proxy,
//                 clip_blocks() raycast, light (sky-light heuristic behind sky_light_at / block_light_at)
//   ai/path.hpp   PathFinder (A*, per-tick node budget), NodeEvaluator, WalkNodeEvaluator, Path
//   mob.hpp       Goal / GoalSelector, Move/Look/Jump/BodyRotation controls, PathNavigation, Sensing,
//                 TargetingConditions, Mob, PathfinderMob, AgeableMob, Animal, Monster, spawn rule registry
//   mob_impl.hpp  method bodies for mob.hpp and path.hpp
//   ai/goals.hpp  goal library (Float, Panic, RandomStroll, LookAtPlayer, Tempt, Breed, FollowParent, EatBlock,
//                 MeleeAttack, NearestAttackableTarget, HurtByTarget, AvoidEntity, Restrict/FleeSun, LeapAtTarget)
//   ai/brain.hpp  Brain: memories, sensors, activities, behaviors (villager-style AI)
//   types/*.hpp   concrete entities (animals.hpp, monsters.hpp, misc.hpp: xp orb, arrow, explode())
//   spawn.hpp     NaturalSpawner (categories, global + per-player caps, spawn costs, placements) + chunk-gen spawns
//   mobs.cpp      registration of the first mobs, hooks, combat packet handlers, /summon (api.hpp)
//
// ADDING A MOB (pattern: types/animals.hpp)
//   struct Fox : Animal { void register_goals() override {...}; sounds; save_extra/load_extra; ... };
//   static MobRegistration reg([] { register_entity_factory(entity_types::fox, make_entity<Fox>); });
//   Attributes, size, tracking range, category and spawn placement come from the generated type data; spawn rule
//   predicates are looked up by their vanilla name (register_spawn_rule("Fox::checkFoxSpawnRules", fn)).
//   create_entity(type) constructs + init() (size, health, controls, registerGoals); add_entity() spawns it.
//   Refer to other entities by id across ticks (entity_by_id); pointers stay valid until the end of the tick
//   they are removed in. finalize_spawn(reason, group) is called by natural spawning and /summon.
//
// HOOKS: entity_pre_tick_hooks / entity_post_tick_hooks (manager.hpp), MobRegistration, register_spawn_rule.
// ============================================================================================================
#include "../play.hpp"
#include "spawn.hpp"
#include "ai/brain.hpp"

// Registration functions of mob modules, run by mobs_init after the data is loaded.
inline std::vector<void (*)()>& mob_registrations() {
    static std::vector<void (*)()> v;
    return v;
}
struct MobRegistration {
    explicit MobRegistration(void (*f)()) { mob_registrations().push_back(f); }
};
