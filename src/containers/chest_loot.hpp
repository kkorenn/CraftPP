#pragma once
// Container loot tables (RandomizableContainer: LootTable + LootTableSeed on chests, barrels, dispensers, hoppers,
// shulker boxes, ...): LootTable.fill with RandomSource.create(seed), so a seeded chest gets vanilla's exact
// contents. Tables load lazily from data/vanilla/minecraft/loot_table/<path>.json into loot.hpp's structs, and
// this file adds the item functions those tables use (enchant_randomly, enchant_with_levels, set_damage,
// set_name, set_potion, set_stew_effect, set_instrument, set_ominous_bottle_amplifier, set_enchantments,
// filtered, discard, exploration_map).
// Unpacking happens where vanilla's getItem/createMenu would: opening (chest.hpp unpack_loot), hoppers,
// comparators and breaking (be::unpack_hook).
#include "hopper.hpp"
#include "item_nbt.hpp"

#include <filesystem>
#include <fstream>

namespace cont {

// ---------- registries / tags from the datapack ----------
// Data-driven registry ids are the sorted entry names (tools/gen_data.py json_ids).
inline const std::vector<std::string>& datapack_registry(const std::string& folder) {
    static std::map<std::string, std::vector<std::string>> cache;
    auto it = cache.find(folder);
    if (it != cache.end()) return it->second;
    std::vector<std::string> v;
    std::error_code ec;
    std::string base = "data/vanilla/minecraft/" + folder;
    for (auto& e : std::filesystem::recursive_directory_iterator(base, ec)) {
        if (e.path().extension() != ".json") continue;
        std::string rel = std::filesystem::relative(e.path(), base).generic_string();
        v.push_back("minecraft:" + rel.substr(0, rel.size() - 5));
    }
    std::sort(v.begin(), v.end());
    return cache[folder] = std::move(v);
}
inline int datapack_id(const std::string& folder, std::string name) {
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    auto& v = datapack_registry(folder);
    auto it = std::lower_bound(v.begin(), v.end(), name);
    return it != v.end() && *it == name ? int(it - v.begin()) : -1;
}
// Tag contents in HolderSet order (TagLoader: first occurrence wins, nested tags expanded in place).
inline std::vector<std::string> tag_values(const std::string& folder, std::string tag, int depth = 0) {
    std::vector<std::string> out;
    if (depth > 16) return out;
    if (!tag.empty() && tag[0] == '#') tag = tag.substr(1);
    if (tag.rfind("minecraft:", 0) == 0) tag = tag.substr(10);
    Json j;
    try {
        j = Json::parseFile("data/vanilla/minecraft/tags/" + folder + "/" + tag + ".json");
    } catch (const std::exception&) {
        return out;
    }
    for (auto& v : j["values"].arr) {
        std::string id = v.isString() ? v.asString() : v["id"].asString();
        std::vector<std::string> add = id[0] == '#' ? tag_values(folder, id, depth + 1) : std::vector<std::string>{id};
        for (auto& a : add)
            if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
    }
    return out;
}
inline int enchant_index(std::string name) {
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    for (size_t i = 0; i < enchants.size(); i++)
        if (enchants[i].name == name || "minecraft:" + enchants[i].name == name) return int(i);
    return -1;
}
// A HolderSet field: "#tag", "id" or ["id", ...] -> ids through `id_of`.
template <class F>
inline std::vector<int> holder_set(const Json& j, const std::string& folder, F id_of) {
    std::vector<std::string> names;
    if (j.isArray()) for (auto& v : j.arr) names.push_back(v.asString());
    else if (j.asString()[0] == '#') names = tag_values(folder, j.asString());
    else names.push_back(j.asString());
    std::vector<int> ids;
    for (auto& n : names)
        if (int id = id_of(n); id >= 0) ids.push_back(id);
    return ids;
}

// Text component JSON (set_name) -> NBT (network form of a Component).
inline Nbt json_to_nbt(const Json& j) {
    if (j.isString()) return Nbt::str(j.asString());
    if (j.isBool()) return Nbt::num(Nbt::Byte, j.asBool());
    if (j.isNumber()) return j.str.find_first_of(".eE") == std::string::npos ? Nbt::num(Nbt::Int, j.asInt()) : Nbt::real(Nbt::Double, j.asDouble());
    if (j.isArray()) {
        Nbt l = Nbt::make_list();
        for (auto& v : j.arr) l.list.push_back(json_to_nbt(v));
        return l;
    }
    Nbt c = Nbt::compound();
    for (auto& [k, v] : j.obj) c.set(k, json_to_nbt(v));
    return c;
}

// ---------- extension loot functions ----------
struct LootExt {
    enum K : uint8_t { ENCH_RANDOMLY, ENCH_LEVELS, SET_DAMAGE, SET_NAME, SET_POTION, SET_STEW, SET_INSTRUMENT, SET_OMINOUS,
                       SET_ENCHANTS, FILTERED, DISCARD, EXPLORATION_MAP } k;
    std::vector<int> ids;                 // enchantments / instruments (options)
    bool all_options = false, only_compatible = true, add = false;
    LootNumber n;                         // levels / damage / amplifier
    std::vector<std::pair<int, LootNumber>> pairs;  // set_enchantments, set_stew_effect (effect, duration)
    int value = -1;                       // potion id; set_name: component id
    std::vector<uint8_t> bytes;           // set_name: text NBT
    std::string filter;                   // filtered: component that must be present
    std::vector<LootFunc> pass, fail;     // filtered
};
inline std::vector<LootExt> loot_exts;

inline bool compile_loot_ext(const std::string& t, const Json& j, LootFunc& f) {
    LootExt e{};
    auto ench_id = [](const std::string& n) { return enchant_index(n); };
    if (t == "enchant_randomly") {
        e.k = LootExt::ENCH_RANDOMLY;
        if (auto* o = j.find("options")) e.ids = holder_set(*o, "enchantment", ench_id);
        else e.all_options = true;
        if (auto* c = j.find("only_compatible")) e.only_compatible = c->asBool();
    } else if (t == "enchant_with_levels") {
        e.k = LootExt::ENCH_LEVELS;
        e.n = loot_detail::number(j["levels"]);
        if (auto* o = j.find("options")) e.ids = holder_set(*o, "enchantment", ench_id);
        else e.all_options = true;
    } else if (t == "set_damage") {
        e.k = LootExt::SET_DAMAGE;
        e.n = loot_detail::number(j["damage"]);
        if (auto* a = j.find("add")) e.add = a->asBool();
    } else if (t == "set_name") {
        e.k = LootExt::SET_NAME;
        if (!j.find("name")) return false;
        std::string target = j.find("target") ? j["target"].asString() : "custom_name";
        e.value = target == "item_name" ? comp::id("item_name") : comp::custom_name();
        e.bytes = comp::nbt_bytes(json_to_nbt(j["name"]));
    } else if (t == "set_potion") {
        e.k = LootExt::SET_POTION;
        e.value = index_of(potion_names, j["id"].asString());
    } else if (t == "set_stew_effect") {
        e.k = LootExt::SET_STEW;
        for (auto& x : j["effects"].arr) {
            std::string id = x["type"].asString();
            auto it = effect_ids.find(id.substr(id.find(':') + 1));
            if (it != effect_ids.end()) e.pairs.emplace_back(it->second, loot_detail::number(x["duration"]));
        }
    } else if (t == "set_instrument") {
        e.k = LootExt::SET_INSTRUMENT;
        e.ids = holder_set(j["options"], "instrument", [](const std::string& n) { return datapack_id("instrument", n); });
    } else if (t == "set_ominous_bottle_amplifier") {
        e.k = LootExt::SET_OMINOUS;
        e.n = loot_detail::number(j["amplifier"]);
    } else if (t == "set_enchantments") {
        e.k = LootExt::SET_ENCHANTS;
        for (auto& [k, v] : j["enchantments"].obj)
            if (int id = enchant_index(k); id >= 0) e.pairs.emplace_back(id, loot_detail::number(v));
        if (auto* a = j.find("add")) e.add = a->asBool();
    } else if (t == "filtered") {
        e.k = LootExt::FILTERED;
        if (auto* p = j["item_filter"].find("predicates"))
            for (auto& [k, v] : p->obj) e.filter = k.substr(k.find(':') + 1);
        if (auto* p = j.find("on_pass")) e.pass.push_back(loot_detail::func(*p));
        if (auto* p = j.find("on_fail")) e.fail.push_back(loot_detail::func(*p));
    } else if (t == "discard") {
        e.k = LootExt::DISCARD;
    } else if (t == "exploration_map") {
        e.k = LootExt::EXPLORATION_MAP;  // ponytail: no structure search yet; the map stays blank (filtered discards it)
    } else {
        return false;
    }
    f.t = LootFunc::EXT;
    f.ext = int(loot_exts.size());
    loot_exts.push_back(std::move(e));
    return true;
}

inline void enchant_item(ItemStack& s, int ench, int level) {  // ItemStack.enchant (upgrade)
    static const int book = item_id("book"), ebook = item_id("enchanted_book");
    if (s.item == book) s = ItemStack(ebook, s.count);
    bool stored = s.item == ebook;
    Enchs v = get_enchs(s, stored);
    ench_set_level(v, ench, std::max(ench_level(v, ench), level));
    set_enchs(s, v, stored);
}
inline std::vector<int> all_enchants() {
    std::vector<int> v(enchants.size());
    for (size_t i = 0; i < v.size(); i++) v[i] = int(i);
    return v;
}

inline void apply_loot_ext(const LootFunc& f, ItemStack& s, const LootContext& ctx) {
    const LootExt& e = loot_exts[size_t(f.ext)];
    static const int book = item_id("book"), ebook = item_id("enchanted_book");
    auto varint_comp = [&](int id, int v) {
        Writer w;
        w.varint(v);
        comp::set(s, id, std::move(w.b));
    };
    switch (e.k) {
        case LootExt::ENCH_RANDOMLY: {
            bool is_book = s.item == book, check = !is_book && e.only_compatible;
            std::vector<int> cand;
            for (int id : e.all_options ? all_enchants() : e.ids)
                if (!check || enchants[size_t(id)].supports(s.item)) cand.push_back(id);
            if (cand.empty()) break;
            int id = cand[size_t(loot_int(int(cand.size())))];  // Util.getRandomSafe
            const Ench& en = enchants[size_t(id)];
            int lvl = en.max <= 1 ? 1 : 1 + loot_int(en.max);  // Mth.nextInt(random, minLevel 1, maxLevel)
            if (is_book) s = ItemStack(ebook, 1);
            enchant_item(s, id, lvl);
            break;
        }
        case LootExt::ENCH_LEVELS: {
            int cost = e.n.get_int();
            LegacyRandomSource fallback{int64_t(game_rng())};
            RandomSource& r = loot_rng ? *loot_rng : fallback;
            auto list = select_enchantments(r, s, cost, e.all_options ? all_enchants() : e.ids);
            if (s.item == book) s = ItemStack(ebook, 1);
            for (auto& x : list) enchant_item(s, x.id, x.level);
            break;
        }
        case LootExt::SET_DAMAGE: {
            if (!damageable(s)) break;
            int maxd = max_damage(s);
            float base = e.add ? 1.0f - float(s.damage) / float(maxd) : 0.0f;
            float pct = 1.0f - std::clamp(e.n.get_float() + base, 0.0f, 1.0f);
            s.damage = int(std::floor(pct * float(maxd)));
            break;
        }
        case LootExt::SET_NAME: comp::set(s, e.value, e.bytes); break;
        case LootExt::SET_POTION:
            if (e.value >= 0) {  // PotionContents(potion): Optional potion, no colour, no effects, no name
                Writer w;
                w.u8(1).varint(e.value).u8(0).varint(0).u8(0);
                comp::set(s, comp::potion_contents(), std::move(w.b));
            }
            break;
        case LootExt::SET_STEW: {
            static const int stew = item_id("suspicious_stew");
            if (s.item != stew || e.pairs.empty()) break;
            auto& [effect, dur] = e.pairs[size_t(loot_int(int(e.pairs.size())))];  // Util.getRandom
            int d = dur.get_int();
            static const int instant[3] = {effect_ids.count("instant_health") ? effect_ids["instant_health"] : -1,
                                           effect_ids.count("instant_damage") ? effect_ids["instant_damage"] : -1,
                                           effect_ids.count("saturation") ? effect_ids["saturation"] : -1};
            if (effect != instant[0] && effect != instant[1] && effect != instant[2]) d *= 20;
            std::vector<std::pair<int, int>> list;  // withEffectAdded
            if (auto* b = comp::get(s, comp::id("suspicious_stew_effects"))) {
                Reader r(*b);
                for (int n = r.varint(); n > 0; n--) { int a = r.varint(); list.emplace_back(a, r.varint()); }
            }
            list.emplace_back(effect, d);
            Writer w;
            w.varint(int(list.size()));
            for (auto& [a, b] : list) w.varint(a).varint(b);
            comp::set(s, comp::id("suspicious_stew_effects"), std::move(w.b));
            break;
        }
        case LootExt::SET_INSTRUMENT:
            if (!e.ids.empty()) varint_comp(comp::id("instrument"), e.ids[size_t(loot_int(int(e.ids.size())))] + 1);
            break;
        case LootExt::SET_OMINOUS: varint_comp(comp::id("ominous_bottle_amplifier"), std::clamp(e.n.get_int(), 0, 4)); break;
        case LootExt::SET_ENCHANTS: {
            if (s.item == book) s.item = ebook;
            bool stored = s.item == ebook;
            Enchs v = get_enchs(s, stored);
            for (auto& [id, n] : e.pairs)
                ench_set_level(v, id, std::clamp((e.add ? ench_level(v, id) : 0) + n.get_int(), 0, 255));
            set_enchs(s, v, stored);
            break;
        }
        case LootExt::FILTERED: {
            bool pass = !e.filter.empty() && comp::get(s, comp::id(e.filter.c_str()));
            apply(pass ? e.pass : e.fail, s, ctx);
            break;
        }
        case LootExt::DISCARD: s = {}; break;
        case LootExt::EXPLORATION_MAP: break;
    }
}

// ---------- tables ----------
// Loot table by resource name ("minecraft:chests/simple_dungeon"); -1 when the file is missing. Referenced
// tables (loot_table entries) load with it (roll_loot looks them up in entity_loot_index by path).
inline int container_table(const std::string& name) {
    std::string path = name.rfind("minecraft:", 0) == 0 ? name.substr(10) : name;
    if (auto it = entity_loot_index.find(path); it != entity_loot_index.end()) return it->second;
    if (path.find("..") != std::string::npos || (name.find(':') != std::string::npos && name.rfind("minecraft:", 0) != 0)) return -1;
    Json j;
    try {
        j = Json::parseFile("data/vanilla/minecraft/loot_table/" + path + ".json");
    } catch (const std::exception& e) {
        log_warn("Unknown loot table %s", name.c_str());
        entity_loot_index[path] = -1;
        return -1;
    }
    int idx = int(loot_tables.size());
    entity_loot_index[path] = idx;
    loot_tables.push_back(compile_table(j));
    std::vector<std::string> refs;
    std::function<void(const LootEntry&)> walk = [&](const LootEntry& e) {
        if (e.t == LootEntry::TABLE) refs.push_back(e.table);
        for (auto& k : e.kids) walk(k);
    };
    for (auto& p : loot_tables[size_t(idx)].pools)
        for (auto& e : p.entries) walk(e);
    for (auto& r : refs) container_table(r);
    return idx;
}

// LootTable.fill(container, params(ORIGIN, THIS_ENTITY = player), seed).
inline void fill_container(std::vector<ItemStack>& items, int table, int64_t seed, int x, int y, int z) {
    LegacyRandomSource r(seed != 0 ? seed : int64_t(game_rng()));  // seed 0: the level random
    LootRngScope scope(&r);
    LootContext ctx{};
    ctx.x = x, ctx.y = y, ctx.z = z, ctx.origin = true;
    std::vector<ItemStack> raw, result;
    roll_loot(loot_tables[size_t(table)], ctx, raw);
    for (auto& s : raw) {  // createStackSplitter
        int m = max_stack(s);
        if (s.count < m) result.push_back(s);
        else for (int left = s.count; left > 0; left -= std::min(m, left)) result.push_back(s.copy(std::min(m, left)));
    }
    auto shuffle = [&](auto& v) {  // Util.shuffle
        for (int i = int(v.size()); i > 1; i--) std::swap(v[size_t(i - 1)], v[size_t(r.nextInt(i))]);
    };
    std::vector<int> slots;  // getAvailableSlots
    for (int i = 0; i < int(items.size()); i++)
        if (items[size_t(i)].empty()) slots.push_back(i);
    shuffle(slots);
    // shuffleAndSplitItems
    std::vector<ItemStack> split, keep;
    for (auto& s : result) {
        if (s.empty()) continue;
        (s.count > 1 ? split : keep).push_back(s);
    }
    auto next_int = [&](int lo, int hi) { return lo >= hi ? lo : r.nextInt(hi - lo + 1) + lo; };  // Mth.nextInt
    while (int(slots.size()) - int(keep.size()) - int(split.size()) > 0 && !split.empty()) {
        size_t i = size_t(next_int(0, int(split.size()) - 1));
        ItemStack s = split[i];
        split.erase(split.begin() + long(i));
        int remove = next_int(1, s.count / 2);
        ItemStack copy = s.copy(remove);
        s.count -= remove;
        if (s.count > 1 && r.nextBoolean()) split.push_back(s);
        else keep.push_back(s);
        if (copy.count > 1 && r.nextBoolean()) split.push_back(copy);
        else keep.push_back(copy);
    }
    keep.insert(keep.end(), split.begin(), split.end());
    shuffle(keep);
    for (auto& s : keep) {
        if (slots.empty()) {
            log_warn("Tried to over-fill a container");
            return;
        }
        items[size_t(slots.back())] = s;
        slots.pop_back();
    }
}

inline void fill_block_entity_loot(be::Live& b, Player*) {
    int t = container_table(b.loot_table);
    if (t >= 0 && !b.items.empty()) fill_container(b.items, t, b.loot_seed, b.x, b.y, b.z);
}

inline void register_chest_loot() {
    loot_ext_compile = compile_loot_ext;
    loot_ext_apply = apply_loot_ext;
    loot_biome_at = [](int x, int y, int z) { return lvl::biome_at({x, y, z}); };
    loot_biome_id = [](const std::string& n) {
        std::string full = n.find(':') == std::string::npos ? "minecraft:" + n : n;
        for (size_t i = 0; i < std::size(biomes::NAMES); i++)
            if (full == biomes::NAMES[i]) return int(i);
        return -1;
    };
    fill_loot = fill_block_entity_loot;
    be::unpack_hook = [](be::Live& b) { unpack_loot(b, nullptr); };
}

}  // namespace cont
