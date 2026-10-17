#pragma once
// Enchantment data (data/minecraft/enchantment via containers.json) and item component helpers used by the
// anvil, grindstone, enchanting table and smithing table: enchantments / stored_enchantments, repair_cost,
// per-item enchantable / repairable data. Enchantment *effects* belong to a later enchantments port.
#include "common.hpp"
#include "../worldgen/random.hpp"

namespace cont {

struct Ench {
    std::string name;
    int max = 1, weight = 1, anvil_cost = 1;
    int min_base = 0, min_per = 0, max_base = 0, max_per = 0;
    std::vector<uint16_t> supported, primary;
    bool has_primary = false;
    std::vector<int> exclusive;
    int min_cost(int lvl) const { return min_base + min_per * (lvl - 1); }
    int max_cost(int lvl) const { return max_base + max_per * (lvl - 1); }
    bool supports(int item) const { return in_sorted(supported, item); }  // canEnchant / isSupportedItem
    bool primary_item(int item) const { return has_primary ? in_sorted(primary, item) : supports(item); }
};
struct ItemExtra {
    int enchantable = 0;  // 0: no ENCHANTABLE component
    bool has_ench = false, brewing_fuel = false, unbreakable = false;
    std::vector<uint16_t> repair;
    std::string trim_material, banner_tag;
    int dye = -1;  // DyeColor id of the DYE component
};
inline std::vector<Ench> enchants;
inline std::map<std::string, std::vector<int>> ench_tags;
inline std::vector<ItemExtra> item_extra;
inline std::vector<int> banner_no_item;                     // banner_pattern tag no_item_required
inline std::map<std::string, std::vector<int>> banner_tags;  // pattern_item/* -> banner_pattern ids

inline bool ench_tagged(const char* tag, int e) {
    auto& v = ench_tags[tag];
    return std::find(v.begin(), v.end(), e) != v.end();
}
inline const ItemExtra& extra(int item) {
    static const ItemExtra none;
    return item >= 0 && item < int(item_extra.size()) ? item_extra[size_t(item)] : none;
}

inline void load_enchant_data(const Json& j) {
    auto ids = [](const Json& a) {
        std::vector<uint16_t> v;
        for (auto& x : a.arr) v.push_back(uint16_t(x.asInt()));
        return v;
    };
    for (auto& e : j["enchantments"].arr) {
        Ench x;
        x.name = e["n"].asString();
        x.max = e["max"].asInt(), x.weight = e["w"].asInt(), x.anvil_cost = e["ac"].asInt();
        x.min_base = e["min"][0].asInt(), x.min_per = e["min"][1].asInt();
        x.max_base = e["maxc"][0].asInt(), x.max_per = e["maxc"][1].asInt();
        x.supported = ids(e["sup"]);
        if (!e["pri"].isNull()) x.primary = ids(e["pri"]), x.has_primary = true;
        for (auto& v : e["ex"].arr) x.exclusive.push_back(v.asInt());
        enchants.push_back(std::move(x));
    }
    for (auto& [k, v] : j["enchantment_tags"].obj)
        for (auto& x : v.arr) ench_tags[k].push_back(x.asInt());
    item_extra.resize(item_info.size());
    for (auto& [k, v] : j["items"].obj) {
        size_t i = size_t(std::stoi(k));
        if (i >= item_extra.size()) continue;
        ItemExtra& x = item_extra[i];
        x.enchantable = v.getInt("ench", 0);
        x.has_ench = v.find("has_ench") != nullptr;
        x.brewing_fuel = v.find("bfuel") != nullptr;
        x.unbreakable = v.find("unbreakable") != nullptr;
        if (auto* r = v.find("rep")) x.repair = ids(*r);
        if (auto* t = v.find("trim")) x.trim_material = t->asString();
        if (auto* b = v.find("banner")) x.banner_tag = b->asString();
        x.dye = v.getInt("dye", -1);
    }
    for (auto& v : j["banner"]["no_item"].arr) banner_no_item.push_back(v.asInt());
    for (auto& [k, v] : j["banner"]["tags"].obj)
        for (auto& x : v.arr) banner_tags[k].push_back(x.asInt());
}

// Enchantment.areCompatible
inline bool compatible(int a, int b) {
    if (a == b) return false;
    auto& ea = enchants[size_t(a)].exclusive;
    auto& eb = enchants[size_t(b)].exclusive;
    return std::find(ea.begin(), ea.end(), b) == ea.end() && std::find(eb.begin(), eb.end(), a) == eb.end();
}

// ---------- components ----------
using Enchs = std::vector<std::pair<int, int>>;  // (enchantment id, level), sorted by id
inline bool is_book_store(const ItemStack& s) {  // has STORED_ENCHANTMENTS (enchanted book)
    static const int book = item_id("enchanted_book");
    return s.item == book || comp::get(s, comp::stored_enchantments());
}
inline Enchs get_enchs(const ItemStack& s, bool stored) {
    Enchs v;
    auto* b = comp::get(s, stored ? comp::stored_enchantments() : comp::enchantments());
    if (!b) return v;
    Reader r(*b);
    for (int n = r.varint(); n > 0; n--) {
        int id = r.varint(), lvl = r.varint();
        v.emplace_back(id, lvl);
    }
    return v;
}
inline Enchs crafting_enchs(const ItemStack& s) { return get_enchs(s, is_book_store(s)); }  // getEnchantmentsForCrafting
inline void set_enchs(ItemStack& s, Enchs v, bool stored) {
    int id = stored ? comp::stored_enchantments() : comp::enchantments();
    std::erase_if(v, [](auto& e) { return e.second <= 0; });
    if (v.empty()) return comp::remove(s, id);  // prototype default: {}
    std::sort(v.begin(), v.end());
    Writer w;
    w.varint(int(v.size()));
    for (auto& [e, l] : v) w.varint(e).varint(std::min(l, 255));
    comp::set(s, id, std::move(w.b));
}
inline int ench_level(const Enchs& v, int id) {
    for (auto& [e, l] : v)
        if (e == id) return l;
    return 0;
}
inline void ench_set_level(Enchs& v, int id, int lvl) {
    for (auto& e : v)
        if (e.first == id) { e.second = lvl; return; }
    v.emplace_back(id, lvl);
}
inline int repair_cost(const ItemStack& s) {
    auto* b = comp::get(s, comp::repair_cost());
    if (!b) return 0;
    Reader r(*b);
    return r.varint();
}
inline void set_repair_cost(ItemStack& s, int c) {
    if (c == 0) return comp::remove(s, comp::repair_cost());
    Writer w;
    w.varint(c);
    comp::set(s, comp::repair_cost(), std::move(w.b));
}
inline int increased_repair_cost(int c) { return int(std::min<int64_t>(int64_t(c) * 2 + 1, INT32_MAX)); }
inline int max_damage(const ItemStack& s) { return s.empty() ? 0 : item_info[size_t(s.item)].max_damage; }
inline bool damageable(const ItemStack& s) { return max_damage(s) > 0 && !extra(s.item).unbreakable; }  // isDamageableItem
inline bool has_any_enchants(const ItemStack& s) {
    return !get_enchs(s, false).empty() || !get_enchs(s, true).empty();
}
inline bool can_store_enchants(const ItemStack& s) { return extra(s.item).has_ench || is_book_store(s); }
inline bool enchantable(const ItemStack& s) {  // ItemStack.isEnchantable: ENCHANTABLE and no enchantments yet
    return extra(s.item).enchantable > 0 && get_enchs(s, false).empty();
}
inline ItemStack transmute(const ItemStack& s, int item) {  // transmuteCopy (same count, components kept)
    ItemStack r = s;
    r.item = item;
    return r;
}
// Hover name as plain text (custom name or the English item name).
inline std::string hover_name(const ItemStack& s) {
    if (auto* n = comp::get(s, comp::custom_name())) {
        try {
            Reader r(*n);
            Nbt t = comp::nbt_read(r);
            return plain(t);
        } catch (const std::exception&) {
        }
    }
    std::string n = item_info[size_t(s.item)].name.substr(10);
    const char* f = lang(("item.minecraft." + n).c_str());
    if (!f) f = lang(("block.minecraft." + n).c_str());
    return f ? f : n;
}

// ---------- enchanting table (EnchantmentHelper) ----------
struct EnchInst {
    int id, level;
};
inline int enchantment_cost(RandomSource& r, int slot, int bookcases, const ItemStack& s) {  // getEnchantmentCost
    if (extra(s.item).enchantable <= 0) return 0;
    bookcases = std::min(bookcases, 15);
    int sel = r.nextInt(8) + 1 + (bookcases >> 1) + r.nextInt(bookcases + 1);
    if (slot == 0) return std::max(sel / 3, 1);
    return slot == 1 ? sel * 2 / 3 + 1 : std::max(sel, bookcases * 2);
}
inline const EnchInst* weighted(RandomSource& r, const std::vector<EnchInst>& v) {  // WeightedRandom.getRandomItem
    int total = 0;
    for (auto& e : v) total += enchants[size_t(e.id)].weight;
    if (total <= 0) return nullptr;
    int sel = r.nextInt(total);
    for (auto& e : v)
        if ((sel -= enchants[size_t(e.id)].weight) < 0) return &e;
    return nullptr;
}
inline std::vector<EnchInst> select_enchantments(RandomSource& r, const ItemStack& s, int cost, const std::vector<int>& source) {
    std::vector<EnchInst> out;
    int ev = extra(s.item).enchantable;
    if (ev <= 0) return out;
    cost += 1 + r.nextInt(ev / 4 + 1) + r.nextInt(ev / 4 + 1);
    float span = (r.nextFloat() + r.nextFloat() - 1.0f) * 0.15f;
    cost = std::max(1, int(std::floor(float(cost) + float(cost) * span + 0.5f)));  // Math.round(float)
    static const int book = item_id("book");
    std::vector<EnchInst> avail;  // getAvailableEnchantmentResults
    for (int id : source) {
        const Ench& e = enchants[size_t(id)];
        if (!(e.primary_item(s.item) || s.item == book)) continue;
        for (int lvl = e.max; lvl >= 1; lvl--)
            if (cost >= e.min_cost(lvl) && cost <= e.max_cost(lvl)) {
                avail.push_back({id, lvl});
                break;
            }
    }
    if (avail.empty()) return out;
    if (auto* e = weighted(r, avail)) out.push_back(*e);
    while (r.nextInt(50) <= cost) {
        if (!out.empty()) {
            int last = out.back().id;
            std::erase_if(avail, [&](const EnchInst& e) { return !compatible(last, e.id); });
        }
        if (avail.empty()) break;
        if (auto* e = weighted(r, avail)) out.push_back(*e);
        cost /= 2;
    }
    return out;
}

}  // namespace cont
