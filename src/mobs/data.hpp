#pragma once
// Mob/entity data tables from data/mobs.json (tools/gen_mob_data.py): per-state collision boxes and block flags,
// path types, fluids, block physics, attribute registry, entity types (dimensions, tracking, default attributes,
// spawn placements), mob categories, biome spawners, tags, item attribute modifiers. Loaded once at startup by
// load_mob_data() (mobs_init); read-only afterwards. Shared by every translation unit (inline globals).
#include "../game/data.hpp"
#include "../game/vec.hpp"

#include <unordered_map>

// ---------- block states ----------
enum StateFlag : uint32_t {
    SF_FULL_BLOCK = 1, SF_CONDUCTOR = 2, SF_SIGNAL = 4,
    SF_SPAWN_GENERIC = 8, SF_SPAWN_FIRE_IMMUNE = 16, SF_SPAWN_POLAR_BEAR = 32, SF_SPAWN_OCELOT = 64, SF_SPAWN_PARROT = 128,
    SF_REPLACEABLE = 256, SF_AIR = 1024, SF_SOLID = 2048, SF_SUFFOCATING = 4096, SF_SOLID_RENDER = 8192,
    SF_STURDY_DOWN = 1u << 20, SF_STURDY_UP = 1u << 21, SF_STURDY_NORTH = 1u << 22, SF_STURDY_SOUTH = 1u << 23,
    SF_STURDY_WEST = 1u << 24, SF_STURDY_EAST = 1u << 25,
};
enum Fluid : uint8_t { FLUID_NONE = 0, FLUID_WATER = 1, FLUID_LAVA = 2 };
// PathType ordinals (vanilla enum order, verified against the dump at load).
enum PathType : uint8_t {
    PT_BLOCKED, PT_OPEN, PT_WALKABLE, PT_WALKABLE_DOOR, PT_TRAPDOOR, PT_POWDER_SNOW, PT_ON_TOP_OF_POWDER_SNOW, PT_FENCE,
    PT_LAVA, PT_WATER, PT_WATER_BORDER, PT_RAIL, PT_UNPASSABLE_RAIL, PT_FIRE_IN_NEIGHBOR, PT_FIRE, PT_DAMAGING_IN_NEIGHBOR,
    PT_DAMAGING, PT_DOOR_OPEN, PT_DOOR_WOOD_CLOSED, PT_DOOR_IRON_CLOSED, PT_BREACH, PT_LEAVES, PT_STICKY_HONEY, PT_COCOA,
    PT_DAMAGE_CAUTIOUS, PT_ON_TOP_OF_TRAPDOOR, PT_BIG_MOBS_CLOSE_TO_DANGER, PT_COUNT
};
inline float path_malus_default[PT_COUNT];

inline std::vector<AABB> shape_boxes;           // all boxes, unit-cube coords
inline std::vector<uint32_t> shape_start;       // shape i = boxes [shape_start[i], shape_start[i + 1])
inline std::vector<uint16_t> state_shape_idx;   // block state -> shape
inline std::vector<uint32_t> state_flag_bits;   // block state -> StateFlag bits | light emission << 16
inline std::vector<uint8_t> state_path_type;    // block state -> PathType (WalkNodeEvaluator.getPathTypeFromState)
inline std::vector<uint8_t> state_fluid_bits;   // amount | type << 4 (1 water, 2 lava) | falling 64 | source 128
struct BlockPhys { float friction = 0.6f, speed = 1, jump = 1, blast = 0; };
inline std::vector<BlockPhys> block_phys;       // by block index

inline uint32_t sflags(int state) { return state_flag_bits[size_t(state)]; }
inline bool sflag(int state, uint32_t f) { return (state_flag_bits[size_t(state)] & f) != 0; }
inline int light_emission(int state) { return int(state_flag_bits[size_t(state)] >> 16 & 15); }
inline int fluid_type(int state) { return state_fluid_bits[size_t(state)] >> 4 & 3; }
inline int fluid_amount(int state) { return state_fluid_bits[size_t(state)] & 15; }
inline bool fluid_source(int state) { return state_fluid_bits[size_t(state)] & 128; }
inline bool fluid_falling(int state) { return state_fluid_bits[size_t(state)] & 64; }
inline float fluid_own_height(int state) { return float(fluid_amount(state)) / 9.0f; }  // FlowingFluid.getOwnHeight
inline const AABB* shape_begin(int state) { return shape_boxes.data() + shape_start[state_shape_idx[size_t(state)]]; }
inline const AABB* shape_end(int state) { return shape_boxes.data() + shape_start[state_shape_idx[size_t(state)] + 1u]; }
inline bool has_collision(int state) { return shape_begin(state) != shape_end(state); }
inline const BlockPhys& phys_of(int state) { return block_phys[size_t(block_index(state))]; }

// ---------- attributes (registry protocol ids) ----------
struct AttributeInfo { std::string id; double def, min, max; bool sync; };
inline std::vector<AttributeInfo> attribute_info;
namespace attr {  // ids filled at load (by name)
inline int armor, armor_toughness, attack_damage, attack_knockback, attack_speed, follow_range, gravity, jump_strength,
    knockback_resistance, max_health, movement_speed, movement_efficiency, safe_fall_distance, fall_damage_multiplier,
    step_height, water_movement_efficiency, scale, explosion_knockback_resistance, burning_time, air_drag_modifier,
    friction_modifier, spawn_reinforcements, tempt_range, sweeping_damage_ratio, max_absorption, luck, oxygen_bonus,
    flying_speed, entity_interaction_range;
}

// ---------- mob categories ----------
enum MobCategoryId { CAT_MONSTER, CAT_CREATURE, CAT_AMBIENT, CAT_AXOLOTLS, CAT_UNDERGROUND_WATER_CREATURE, CAT_WATER_CREATURE,
                     CAT_WATER_AMBIENT, CAT_MISC, CAT_COUNT };
struct MobCategoryInfo { std::string name; int max; bool friendly, persistent; int despawn, no_despawn; };
inline std::vector<MobCategoryInfo> mob_categories;

// ---------- entity types ----------
enum class SpawnPlacement : uint8_t { NONE, ON_GROUND, IN_WATER, IN_LAVA, NO_RESTRICTIONS };
enum class Heightmap : uint8_t { WORLD_SURFACE, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES, OCEAN_FLOOR };
struct EntityTypeInfo {
    std::string id;  // without namespace
    float w = 0, h = 0, eye = 0;
    bool fixed = false;
    int cat = CAT_MISC, range = 5, interval = 3;
    bool deltas = true, fire_immune = false, summon = true, serialize = true, far = true, peaceful = true;
    std::vector<uint16_t> immune;                   // block indices (immuneTo tag)
    std::vector<std::pair<uint16_t, double>> attr;  // default attributes (DefaultAttributes), sorted by attribute id
    SpawnPlacement place = SpawnPlacement::NONE;
    Heightmap heightmap = Heightmap::MOTION_BLOCKING_NO_LEAVES;
    std::string spawn_rule;  // e.g. "Monster::checkMonsterSpawnRules"
    int loot = -1;           // default loot table index
    bool has_attr(int a) const {
        auto it = std::lower_bound(attr.begin(), attr.end(), std::pair<uint16_t, double>{uint16_t(a), -1e300});
        return it != attr.end() && it->first == a;
    }
    double attr_base(int a) const {
        auto it = std::lower_bound(attr.begin(), attr.end(), std::pair<uint16_t, double>{uint16_t(a), -1e300});
        return it != attr.end() && it->first == a ? it->second : attribute_info[size_t(a)].def;
    }
};
inline std::vector<EntityTypeInfo> entity_type_info;  // by entity_types::* id
inline std::map<std::string, int> entity_type_by_name;  // "cow" -> id
inline int entity_type_id(const std::string& name) {
    std::string n = name.rfind("minecraft:", 0) == 0 ? name.substr(10) : name;
    auto it = entity_type_by_name.find(n);
    return it == entity_type_by_name.end() ? -1 : it->second;
}

// ---------- biome spawners (environment attribute gameplay/natural_mob_spawns) ----------
struct SpawnerData { int type, weight, min, max; };
struct BiomeSpawnInfo {
    std::array<std::vector<SpawnerData>, CAT_COUNT> spawners;
    std::vector<std::array<double, 3>> costs;  // {type, energy_budget, charge}
    float creature_probability = 0.1f;
    float temperature = 0.5f;
    std::string id;
    const double* cost(int type) const {
        for (auto& c : costs) if (int(c[0]) == type) return &c[1];
        return nullptr;
    }
};
inline std::vector<BiomeSpawnInfo> biome_spawns;  // by biome id

// ---------- tags ----------
inline std::map<std::string, std::vector<uint16_t>> etype_tags, damage_tags, block_tags_ext, item_tags_ext;
inline bool entity_in_tag(int type, const std::string& tag) {
    auto it = etype_tags.find(tag);
    return it != etype_tags.end() && in_sorted(it->second, type);
}
inline bool block_in_tag(int state, const std::string& tag) {
    auto it = block_tags_ext.find(tag);
    return it != block_tags_ext.end() && in_sorted(it->second, block_index(state));
}
inline bool item_in_tag(int item, const std::string& tag) {
    auto it = item_tags_ext.find(tag);
    return item >= 0 && it != item_tags_ext.end() && in_sorted(it->second, item);
}

// ---------- items: attribute modifiers (attack damage/speed, armor) ----------
enum EquipSlot : uint8_t { ES_MAINHAND, ES_OFFHAND, ES_FEET, ES_LEGS, ES_CHEST, ES_HEAD, ES_BODY, ES_SADDLE, ES_ANY,
                           ES_HAND, ES_ARMOR };
struct ItemMod { uint16_t attr; double amount; uint8_t op, slot; };
struct ItemCombat { std::vector<ItemMod> mods; int weapon_damage = -1; };  // weapon_damage: item_damage_per_attack, -1 = not a weapon
inline std::unordered_map<int, ItemCombat> item_combat;
inline std::unordered_map<int, int> smelt_result;  // furnace_smelt
inline bool slot_matches(uint8_t group, int slot) {  // EquipmentSlotGroup.test
    if (group == ES_ANY) return true;
    if (group == ES_HAND) return slot == ES_MAINHAND || slot == ES_OFFHAND;
    if (group == ES_ARMOR) return slot >= ES_FEET && slot <= ES_BODY;
    return group == slot;
}

inline std::map<std::string, int> entity_loot_index;  // "entities/cow", "shearing/sheep/white" -> loot_tables index
inline void load_entity_loot(const Json& loot);         // loot.hpp
inline void init_damage_tags(const Json& scaling);     // mobs/entity.hpp

inline bool load_mob_data(const std::string& path = "data/mobs.json") {
    Json j;
    try {
        j = Json::parseFile(path);
    } catch (const std::exception& e) {
        fprintf(stderr, "%s: %s (run: python3 tools/gen_mob_data.py)\n", path.c_str(), e.what());
        return false;
    }
    for (auto& s : j["shapes"].arr) {
        shape_start.push_back(uint32_t(shape_boxes.size()));
        for (size_t i = 0; i + 5 < s.arr.size(); i += 6)
            shape_boxes.push_back({s[i].asDouble(), s[i + 1].asDouble(), s[i + 2].asDouble(), s[i + 3].asDouble(),
                                   s[i + 4].asDouble(), s[i + 5].asDouble()});
    }
    shape_start.push_back(uint32_t(shape_boxes.size()));
    for (auto& v : j["state_shape"].arr) state_shape_idx.push_back(uint16_t(v.asInt()));
    for (auto& v : j["state_flags"].arr) state_flag_bits.push_back(uint32_t(v.asInt()));
    for (auto& v : j["state_path"].arr) state_path_type.push_back(uint8_t(v.asInt()));
    for (auto& v : j["state_fluid"].arr) state_fluid_bits.push_back(uint8_t(v.asInt()));
    static const char* pt_names[] = {"BLOCKED", "OPEN", "WALKABLE", "WALKABLE_DOOR", "TRAPDOOR", "POWDER_SNOW",
        "ON_TOP_OF_POWDER_SNOW", "FENCE", "LAVA", "WATER", "WATER_BORDER", "RAIL", "UNPASSABLE_RAIL", "FIRE_IN_NEIGHBOR",
        "FIRE", "DAMAGING_IN_NEIGHBOR", "DAMAGING", "DOOR_OPEN", "DOOR_WOOD_CLOSED", "DOOR_IRON_CLOSED", "BREACH", "LEAVES",
        "STICKY_HONEY", "COCOA", "DAMAGE_CAUTIOUS", "ON_TOP_OF_TRAPDOOR", "BIG_MOBS_CLOSE_TO_DANGER"};
    auto& pts = j["path_types"].arr;
    if (pts.size() != PT_COUNT) { fprintf(stderr, "mobs.json: PathType enum changed\n"); return false; }
    for (size_t i = 0; i < pts.size(); i++) {
        if (pts[i][0].asString() != pt_names[i]) { fprintf(stderr, "mobs.json: PathType %zu mismatch\n", i); return false; }
        path_malus_default[i] = pts[i][1].asFloat();
    }
    for (auto& b : j["block_phys"].arr) block_phys.push_back({b[0].asFloat(), b[1].asFloat(), b[2].asFloat(), b[3].asFloat()});
    for (auto& a : j["attributes"].arr)
        attribute_info.push_back({a["id"].asString(), a["def"].asDouble(), a["min"].asDouble(), a["max"].asDouble(), a["sync"].asBool()});
    auto aid = [](const char* n) {
        for (size_t i = 0; i < attribute_info.size(); i++) if (attribute_info[i].id == n) return int(i);
        fprintf(stderr, "mobs.json: no attribute %s\n", n);
        return 0;
    };
    using namespace attr;
    armor = aid("armor"), armor_toughness = aid("armor_toughness"), attack_damage = aid("attack_damage");
    attack_knockback = aid("attack_knockback"), attack_speed = aid("attack_speed"), follow_range = aid("follow_range");
    gravity = aid("gravity"), jump_strength = aid("jump_strength"), knockback_resistance = aid("knockback_resistance");
    max_health = aid("max_health"), movement_speed = aid("movement_speed"), movement_efficiency = aid("movement_efficiency");
    safe_fall_distance = aid("safe_fall_distance"), fall_damage_multiplier = aid("fall_damage_multiplier");
    step_height = aid("step_height"), water_movement_efficiency = aid("water_movement_efficiency"), scale = aid("scale");
    explosion_knockback_resistance = aid("explosion_knockback_resistance"), burning_time = aid("burning_time");
    air_drag_modifier = aid("air_drag_modifier"), friction_modifier = aid("friction_modifier");
    spawn_reinforcements = aid("spawn_reinforcements"), tempt_range = aid("tempt_range");
    sweeping_damage_ratio = aid("sweeping_damage_ratio"), max_absorption = aid("max_absorption"), luck = aid("luck");
    oxygen_bonus = aid("oxygen_bonus"), flying_speed = aid("flying_speed");
    entity_interaction_range = aid("entity_interaction_range");
    for (auto& c : j["categories"].arr)
        mob_categories.push_back({c["name"].asString(), c["max"].asInt(), c["friendly"].asBool(), c["persistent"].asBool(),
                                  c["despawn"].asInt(), c["no_despawn"].asInt()});
    static const std::map<std::string, SpawnPlacement> places = {{"ON_GROUND", SpawnPlacement::ON_GROUND},
        {"IN_WATER", SpawnPlacement::IN_WATER}, {"IN_LAVA", SpawnPlacement::IN_LAVA}, {"NO_RESTRICTIONS", SpawnPlacement::NO_RESTRICTIONS}};
    static const std::map<std::string, Heightmap> hms = {{"WORLD_SURFACE", Heightmap::WORLD_SURFACE},
        {"MOTION_BLOCKING", Heightmap::MOTION_BLOCKING}, {"MOTION_BLOCKING_NO_LEAVES", Heightmap::MOTION_BLOCKING_NO_LEAVES},
        {"OCEAN_FLOOR", Heightmap::OCEAN_FLOOR}};
    for (auto& e : j["entity_types"].arr) {
        EntityTypeInfo t;
        t.id = e["id"].asString();
        t.w = e["w"].asFloat(), t.h = e["h"].asFloat(), t.eye = e["eye"].asFloat(), t.fixed = e["fixed"].asBool();
        t.cat = e["cat"].asInt(), t.range = e["range"].asInt(), t.interval = e["interval"].asInt();
        t.deltas = e["deltas"].asBool(), t.fire_immune = e["fire_immune"].asBool(), t.summon = e["summon"].asBool();
        t.serialize = e["serialize"].asBool(), t.far = e["far"].asBool(), t.peaceful = e["peaceful"].asBool();
        for (auto& b : e["immune"].arr) t.immune.push_back(uint16_t(b.asInt()));
        if (auto* a = e.find("attr"))
            for (auto& kv : a->arr) t.attr.emplace_back(uint16_t(kv[0].asInt()), kv[1].asDouble());
        if (auto* p = e.find("place")) {
            t.place = places.at((*p)[0].asString());
            t.heightmap = hms.at((*p)[1].asString());
            t.spawn_rule = (*p)[2].asString();
        }
        entity_type_by_name[t.id] = int(entity_type_info.size());
        entity_type_info.push_back(std::move(t));
    }
    for (auto& b : j["biome_spawns"].arr) {
        BiomeSpawnInfo bi;
        bi.id = b["id"].asString();
        for (auto& c : b["cats"].arr)
            for (auto& s : c[1].arr)
                bi.spawners[size_t(c[0].asInt())].push_back({s[0].asInt(), s[1].asInt(), s[2].asInt(), s[3].asInt()});
        for (auto& c : b["costs"].arr) bi.costs.push_back({c[0].asDouble(), c[1].asDouble(), c[2].asDouble()});
        bi.creature_probability = b["creature_p"].asFloat();
        bi.temperature = b["temp"].asFloat();
        biome_spawns.push_back(std::move(bi));
    }
    auto tags = [](const Json& o, std::map<std::string, std::vector<uint16_t>>& out) {
        for (auto& [k, v] : o.obj) {
            auto& vec = out[k];
            for (auto& x : v.arr) vec.push_back(uint16_t(x.asInt()));
            std::sort(vec.begin(), vec.end());
        }
    };
    tags(j["entity_tags"], etype_tags);
    tags(j["damage_tags"], damage_tags);
    tags(j["block_tags"], block_tags_ext);
    tags(j["item_tags"], item_tags_ext);
    init_damage_tags(j["damage_scaling"]);
    for (auto& m : j["item_mods"].arr) {
        ItemCombat ic;
        for (auto& x : m[1].arr) ic.mods.push_back({uint16_t(x[0].asInt()), x[1].asDouble(), uint8_t(x[2].asInt()), uint8_t(x[3].asInt())});
        ic.weapon_damage = m[2].asInt();
        item_combat[m[0].asInt()] = std::move(ic);
    }
    for (auto& s : j["smelt"].arr) smelt_result[s[0].asInt()] = s[1].asInt();
    load_entity_loot(j["loot"]);
    for (auto& t : entity_type_info) t.loot = -1;
    for (auto& [name, idx] : entity_loot_index)
        if (name.rfind("entities/", 0) == 0) {
            int id = entity_type_id(name.substr(9));
            if (id >= 0) entity_type_info[size_t(id)].loot = idx;
        }
    return true;
}
