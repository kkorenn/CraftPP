#pragma once
// Static per-entity-type data from data/mobs.json (tools/gen_mob_data.py): category, size, attributes,
// spawn egg, loot table, breeding/tempt/taming items, plus villager trades, trade sets, POI and professions.
// Shared by every mob of a type: a mob stores only its type id and its own mutable state.
#include "../worldgen/json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

enum class MobCategory : uint8_t { misc, monster, creature, ambient, axolotls, underground_water_creature, water_creature, water_ambient };

struct AttributeInfo {
    std::string name;
    double def = 0, min = 0, max = 0;
};
struct MobType {
    std::string name;  // without "minecraft:"
    MobCategory category = MobCategory::misc;
    float width = 0.6f, height = 1.8f, eye = 1.53f;
    uint8_t tracking_range = 5, update_interval = 3;
    bool fire_immune = false, no_peaceful = false, living = false;
    int egg = -1;          // spawn egg item id
    std::string loot;      // "entities/<name>" or empty
    std::vector<std::pair<uint16_t, double>> attrs;  // AttributeSupplier: (attribute id, base value), sorted
    std::vector<uint16_t> food, tempt, tame, poison;  // sorted item ids
    std::vector<std::string> tags;                    // entity_type tags containing this type

    bool has_attr(int id) const {
        auto it = std::lower_bound(attrs.begin(), attrs.end(), std::pair<uint16_t, double>(uint16_t(id), -1e300));
        return it != attrs.end() && it->first == id;
    }
    double attr(int id) const;  // base value; attribute default if the supplier lacks it
    bool is_food(int item) const { return std::binary_search(food.begin(), food.end(), uint16_t(item)); }
    bool is_tempt(int item) const {
        return std::binary_search(tempt.begin(), tempt.end(), uint16_t(item)) || (tempt.empty() && is_food(item));
    }
    bool has_tag(const std::string& t) const { return std::find(tags.begin(), tags.end(), t) != tags.end(); }
};

inline std::vector<AttributeInfo> attribute_info;  // by attribute protocol id
inline std::vector<MobType> mob_types;             // by entity type protocol id

inline double MobType::attr(int id) const {
    auto it = std::lower_bound(attrs.begin(), attrs.end(), std::pair<uint16_t, double>(uint16_t(id), -1e300));
    return it != attrs.end() && it->first == id ? it->second : attribute_info[size_t(id)].def;
}
inline int attribute_id(const std::string& name) {
    for (size_t i = 0; i < attribute_info.size(); i++)
        if (attribute_info[i].name == name) return int(i);
    return -1;
}
inline int mob_type_id(const std::string& name) {
    std::string n = name.rfind("minecraft:", 0) == 0 ? name.substr(10) : name;
    for (size_t i = 0; i < mob_types.size(); i++)
        if (mob_types[i].name == n) return int(i);
    return -1;
}

inline void load_trading(const Json& j);  // trading.hpp

inline bool load_mob_data(const std::string& path = "data/mobs.json") {
    Json j;
    try {
        j = Json::parseFile(path);
    } catch (const std::exception& e) {
        fprintf(stderr, "%s: %s (run: python3 tools/gen_mob_data.py)\n", path.c_str(), e.what());
        return false;
    }
    auto u16list = [](const Json* a) {
        std::vector<uint16_t> v;
        if (a)
            for (auto& x : a->arr) v.push_back(uint16_t(x.asInt()));
        std::sort(v.begin(), v.end());
        return v;
    };
    for (auto& [name, a] : j["attributes"].obj) {
        size_t id = size_t(a[0].asInt());
        if (attribute_info.size() <= id) attribute_info.resize(id + 1);
        attribute_info[id] = {name, a[1].asDouble(), a[2].asDouble(), a[3].asDouble()};
    }
    static const char* cats[] = {"misc", "monster", "creature", "ambient", "axolotls", "underground_water_creature",
                                 "water_creature", "water_ambient"};
    for (auto& [name, t] : j["types"].obj) {
        size_t id = size_t(t["id"].asInt());
        if (mob_types.size() <= id) mob_types.resize(id + 1);
        MobType& m = mob_types[id];
        m.name = name;
        for (int c = 0; c < 8; c++)
            if (t["cat"].asString() == cats[c]) m.category = MobCategory(c);
        m.width = float(t["w"].asDouble()), m.height = float(t["h"].asDouble()), m.eye = float(t["eye"].asDouble());
        m.tracking_range = uint8_t(t["track"].asInt()), m.update_interval = uint8_t(t["interval"].asInt());
        m.fire_immune = t.has("fire_immune"), m.no_peaceful = t.has("no_peaceful");
        m.egg = t.getInt("egg", -1);
        if (auto* l = t.find("loot")) m.loot = l->asString();
        if (auto* a = t.find("attrs")) {
            m.living = true;
            for (auto& [an, v] : a->obj) m.attrs.emplace_back(uint16_t(attribute_id(an)), v.asDouble());
            std::sort(m.attrs.begin(), m.attrs.end());
        }
        m.food = u16list(t.find("food")), m.tempt = u16list(t.find("tempt"));
        m.tame = u16list(t.find("tame")), m.poison = u16list(t.find("poison"));
        if (auto* tg = t.find("tags"))
            for (auto& x : tg->arr) m.tags.push_back(x.asString());
    }
    load_trading(j);
    return true;
}
