#pragma once
// Survival data tables from data/game.json (tools/gen_game_data.py): blocks, items, tools, food, registries.
// Loaded once at startup; read-only afterwards.
#include "../worldgen/json.hpp"
#include "player_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

struct BlockProp {
    std::string name;
    std::vector<std::string> values;
};
struct BlockInfo {
    std::string name;
    int first = 0, count = 1;
    float hardness = 0;  // destroyTime; -1 = unbreakable
    bool requires_tool = false, collision = true, replaceable = false;
    std::vector<BlockProp> props;  // in state order: last property varies fastest
    int loot = -1;                 // index into loot_tables (loot.hpp)
};
struct ToolRule {
    std::vector<uint16_t> blocks;  // sorted block indices
    float speed = -1;              // < 0: rule has no speed
    int8_t correct = -1;           // -1: rule doesn't decide correct_for_drops
};
struct ToolInfo {
    float default_speed = 1;
    int damage_per_block = 1;
    bool creative_destroy = true;  // can_destroy_blocks_in_creative
    std::vector<ToolRule> rules;
};
struct ItemInfo {
    std::string name;
    int stack = 64, max_damage = 0;
    int tool = -1;  // index into tools
    int nutrition = -1;
    float saturation = 0;
    bool always_eat = false;
    float consume_seconds = -1;  // < 0: not consumable
    int use_remainder = -1, craft_remainder = -1;
    int equip = -1;  // Inventory armor index (36 feet .. 39 head), 40 offhand, -1 none
    float armor = 0, toughness = 0;  // attribute modifiers in the equip slot
    bool damage_on_hurt = true;      // equippable.damage_on_hurt
};
struct DamageType {
    int id = 0;
    std::string msg;
    float exhaustion = 0;
    bool fall_variants = false;
    bool bypasses_armor = false;  // #minecraft:bypasses_armor
};

inline std::vector<BlockInfo> block_info;
inline std::vector<ItemInfo> item_info;
inline std::vector<ToolInfo> tools;
inline std::vector<uint16_t> state_block;  // block state -> block index
inline std::map<std::string, int> item_by_name, block_by_name;
inline std::map<std::string, DamageType> damage_types;
inline std::map<std::string, int> menu_ids, component_ids;
inline std::vector<uint16_t> tag_climbable, tag_fall_reset;

inline const BlockInfo& block_of(int state) { return block_info[state_block[size_t(state)]]; }
inline int block_index(int state) { return state_block[size_t(state)]; }
inline int max_stack(int item) { return item >= 0 && item < int(item_info.size()) ? item_info[size_t(item)].stack : 64; }
inline int max_stack(const ItemStack& s) { return max_stack(s.item); }
inline bool in_sorted(const std::vector<uint16_t>& v, int x) { return std::binary_search(v.begin(), v.end(), uint16_t(x)); }

// Value index of property `prop` in `state`, or -1.
inline int state_prop(int state, const std::string& prop) {
    const BlockInfo& b = block_of(state);
    int off = state - b.first, stride = 1;
    for (int k = int(b.props.size()) - 1; k >= 0; k--) {
        int n = int(b.props[size_t(k)].values.size());
        if (b.props[size_t(k)].name == prop) return off / stride % n;
        stride *= n;
    }
    return -1;
}
inline std::string state_prop_str(int state, const std::string& prop) {
    int v = state_prop(state, prop);
    if (v < 0) return "";
    for (auto& p : block_of(state).props)
        if (p.name == prop) return p.values[size_t(v)];
    return "";
}

// Tool.getMiningSpeed / isCorrectForDrops.
inline float tool_speed(int item, int state) {
    if (item < 0 || item_info[size_t(item)].tool < 0) return 1.0f;
    const ToolInfo& t = tools[size_t(item_info[size_t(item)].tool)];
    int b = block_index(state);
    for (auto& r : t.rules)
        if (r.speed >= 0 && in_sorted(r.blocks, b)) return r.speed;
    return t.default_speed;
}
inline bool tool_correct(int item, int state) {
    if (item < 0 || item_info[size_t(item)].tool < 0) return false;
    int b = block_index(state);
    for (auto& r : tools[size_t(item_info[size_t(item)].tool)].rules)
        if (r.correct >= 0 && in_sorted(r.blocks, b)) return r.correct;
    return false;
}

inline int damage_type_id(const std::string& name) {
    auto it = damage_types.find(name);
    return it == damage_types.end() ? 0 : it->second.id;
}

inline void load_loot_tables(const Json& loot);  // loot.hpp
inline void load_recipes(const Json& recipes);   // crafting.hpp

inline bool load_game_data(const std::string& path = "data/game.json") {
    Json j;
    try {
        j = Json::parseFile(path);
    } catch (const std::exception& e) {
        log_error("%s: %s (run: python3 tools/gen_game_data.py)", path.c_str(), e.what());
        return false;
    }
    auto u16list = [](const Json& a) {
        std::vector<uint16_t> v;
        v.reserve(a.size());
        for (auto& x : a.arr) v.push_back(uint16_t(x.asInt()));
        return v;
    };
    for (auto& b : j["blocks"].arr) {
        BlockInfo bi;
        bi.name = "minecraft:" + b["n"].asString();
        bi.first = b["f"].asInt();
        bi.count = b["c"].asInt();
        bi.hardness = b["h"].asFloat();
        bi.requires_tool = b["t"].asBool();
        bi.collision = b["col"].asBool();
        bi.replaceable = b["r"].asBool();
        for (auto& p : b["p"].arr) {
            BlockProp bp{p[0].asString(), {}};
            for (auto& v : p[1].arr) bp.values.push_back(v.asString());
            bi.props.push_back(std::move(bp));
        }
        block_by_name[bi.name] = int(block_info.size());
        block_info.push_back(std::move(bi));
    }
    state_block.assign(size_t(block_info.back().first + block_info.back().count), 0);
    for (size_t i = 0; i < block_info.size(); i++)
        std::fill_n(state_block.begin() + block_info[i].first, block_info[i].count, uint16_t(i));
    static const std::map<std::string, int> equip_slots = {{"feet", 36}, {"legs", 37}, {"chest", 38}, {"head", 39}, {"offhand", 40}};
    for (auto& it : j["items"].arr) {
        ItemInfo ii;
        ii.name = "minecraft:" + it["n"].asString();
        ii.stack = it["s"].asInt();
        ii.max_damage = it.getInt("d", 0);
        if (auto* t = it.find("tool")) {
            ToolInfo ti;
            ti.default_speed = (*t)["ds"].asFloat();
            ti.damage_per_block = (*t)["dpb"].asInt();
            ti.creative_destroy = !t->find("nc");
            for (auto& r : (*t)["r"].arr) {
                ToolRule tr;
                tr.blocks = u16list(r[0]);
                if (!r[1].isNull()) tr.speed = r[1].asFloat();
                if (!r[2].isNull()) tr.correct = r[2].asBool() ? 1 : 0;
                ti.rules.push_back(std::move(tr));
            }
            ii.tool = int(tools.size());
            tools.push_back(std::move(ti));
        }
        if (auto* f = it.find("food")) {
            ii.nutrition = (*f)[0].asInt();
            ii.saturation = (*f)[1].asFloat();
            ii.always_eat = (*f)[2].asBool();
        }
        if (auto* c = it.find("cs")) ii.consume_seconds = c->asFloat();
        ii.use_remainder = it.getInt("ur", -1);
        ii.craft_remainder = it.getInt("cr", -1);
        if (auto* e = it.find("eq"))
            if (auto s = equip_slots.find(e->asString()); s != equip_slots.end()) ii.equip = s->second;
        if (auto* a = it.find("ar")) ii.armor = (*a)[0].asFloat(), ii.toughness = (*a)[1].asFloat();
        ii.damage_on_hurt = !it.find("nd");
        item_by_name[ii.name] = int(item_info.size());
        item_info.push_back(std::move(ii));
    }
    auto& reg = j["registries"];
    for (auto& [k, v] : reg["damage_type"].obj)
        damage_types[k] = {v[0].asInt(), v[1].asString(), v[2].asFloat(), v[3].asString() == "fall_variants",
                           v.arr.size() > 4 && v[4].asBool()};
    for (auto& [k, v] : reg["menu"].obj) menu_ids[k] = v.asInt();
    for (auto& [k, v] : reg["data_component_type"].obj) component_ids[k] = v.asInt();
    tag_climbable = u16list(j["block_tags"]["climbable"]);
    tag_fall_reset = u16list(j["block_tags"]["fall_damage_resetting"]);
    load_loot_tables(j["loot"]);
    load_recipes(j["recipes"]);
    return true;
}
