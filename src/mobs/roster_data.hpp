#pragma once
// Roster data from data/roster.json (tools/gen_roster_data.py): breeding / tempt / taming items and spawn egg
// per entity type, plus villager trades, trade sets, POI types and professions (trading.hpp).
// Static, shared by every mob of a type; a mob stores only its own mutable state.
#include "../game/data.hpp"
#include "../worldgen/json.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

struct RosterType {
    std::string name;  // entity type without "minecraft:"
    int egg = -1;      // spawn egg item id
    std::vector<uint16_t> food, tempt, tame, poison;  // sorted item ids

    static bool has(const std::vector<uint16_t>& v, int item) { return std::binary_search(v.begin(), v.end(), uint16_t(item)); }
    bool is_food(int item) const { return has(food, item); }
    bool is_tempt(int item) const { return tempt.empty() ? is_food(item) : has(tempt, item); }
    bool is_tame_item(int item) const { return has(tame, item); }
};
inline std::vector<RosterType> roster_types;  // by entity type protocol id (every type)

inline const RosterType& roster(int type) {
    static const RosterType none;
    return type >= 0 && size_t(type) < roster_types.size() ? roster_types[size_t(type)] : none;
}
inline void load_trading(const Json& j);  // trading.hpp

inline bool load_roster_data(const std::string& path = "data/roster.json") {
    Json j;
    try {
        j = Json::parseFile(path);
    } catch (const std::exception& e) {
        fprintf(stderr, "%s: %s (run: python3 tools/gen_roster_data.py)\n", path.c_str(), e.what());
        return false;
    }
    auto u16list = [](const Json* a) {
        std::vector<uint16_t> v;
        if (a)
            for (auto& x : a->arr) v.push_back(uint16_t(x.asInt()));
        return v;  // the generator emits sorted lists
    };
    for (auto& [name, t] : j["types"].obj) {
        size_t id = size_t(t["id"].asInt());
        if (roster_types.size() <= id) roster_types.resize(id + 1);
        RosterType& r = roster_types[id];
        r.name = name;
        r.egg = t.getInt("egg", -1);
        r.food = u16list(t.find("food")), r.tempt = u16list(t.find("tempt"));
        r.tame = u16list(t.find("tame")), r.poison = u16list(t.find("poison"));
    }
    load_trading(j);
    return true;
}
