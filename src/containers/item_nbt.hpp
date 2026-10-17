#pragma once
// Vanilla NBT form of the data components this server keeps in ItemStack::comp (DataComponentPatch codec):
// "minecraft:<name>": value, "!minecraft:<name>": {} for removals. Used by player files (Inventory, EnderItems),
// block entity Items and container contents, through server.hpp's item_components_to/from_nbt hooks.
// Components without a codec here are kept as "craftpp:raw/<name>" byte arrays (network encoding).
#include "brewing.hpp"

namespace cont {

inline std::vector<std::string> potion_names, banner_names;
inline const char* DYE_NAMES[16] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                     "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};

inline const std::string& component_name(int id) {
    static std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (auto& [k, i] : component_ids) {
            if (size_t(i) >= v.size()) v.resize(size_t(i) + 1);
            v[size_t(i)] = k;
        }
        return v;
    }();
    static const std::string none;
    return id >= 0 && size_t(id) < names.size() ? names[size_t(id)] : none;
}
inline int index_of(const std::vector<std::string>& v, std::string s) {
    if (s.find(':') == std::string::npos) s = "minecraft:" + s;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == s) return int(i);
    return -1;
}
inline int dye_index(const std::string& s) {
    for (int i = 0; i < 16; i++)
        if (s == DYE_NAMES[i]) return i;
    return -1;
}
inline std::vector<std::string> enchant_names() {
    std::vector<std::string> v;
    for (auto& e : enchants) v.push_back(e.name);
    return v;
}

// Network value -> NBT. False when there is no codec (caller keeps the raw bytes).
inline bool value_to_nbt(int id, const std::vector<uint8_t>& b, Nbt& out) {
    try {
        Reader r(b);
        if (id == comp::custom_name() || id == comp::id("item_name")) return out = comp::nbt_read(r), true;
        if (id == comp::repair_cost() || id == comp::map_id()) return out = Nbt::num(Nbt::Int, r.varint()), true;
        if (id == comp::glint()) return out = Nbt::num(Nbt::Byte, r.u8()), true;
        if (id == comp::base_color()) return out = Nbt::str(DYE_NAMES[r.varint() & 15]), true;
        if (id == comp::enchantments() || id == comp::stored_enchantments()) {
            out = Nbt::compound();
            for (int n = r.varint(); n > 0; n--) {
                int e = r.varint(), lvl = r.varint();
                if (e >= 0 && e < int(enchants.size())) out.set(enchants[size_t(e)].name, Nbt::num(Nbt::Int, lvl));
            }
            return true;
        }
        if (id == comp::container()) {
            out = Nbt::make_list();
            int n = r.varint();
            for (int i = 0; i < n; i++) {
                if (!r.u8()) continue;
                ItemStack s = comp::read_stack(r);
                Nbt e = Nbt::compound();
                e.set("slot", Nbt::num(Nbt::Int, i));
                e.set("item", item_nbt(s));
                out.list.push_back(std::move(e));
            }
            return true;
        }
        if (id == comp::potion_contents()) {
            out = Nbt::compound();
            if (r.u8()) {
                int p = r.varint();
                if (p >= 0 && p < int(potion_names.size())) out.set("potion", Nbt::str(potion_names[size_t(p)]));
            }
            if (r.u8()) out.set("custom_color", Nbt::num(Nbt::Int, r.i32()));
            if (r.varint() != 0) return false;
            if (r.u8()) out.set("custom_name", Nbt::str(r.str()));
            return true;
        }
        if (id == comp::trim()) {
            int m = r.varint() - 1, p = r.varint() - 1;
            if (m < 0 || p < 0 || m >= int(trim_materials.size()) || p >= int(trim_patterns.size())) return false;
            out = Nbt::compound();
            out.set("material", Nbt::str(trim_materials[size_t(m)]));
            out.set("pattern", Nbt::str(trim_patterns[size_t(p)]));
            return true;
        }
        if (id == comp::banner_patterns()) {
            out = Nbt::make_list();
            for (int n = r.varint(); n > 0; n--) {
                int p = r.varint() - 1, c = r.varint();
                if (p < 0 || p >= int(banner_names.size())) return false;
                Nbt l = Nbt::compound();
                l.set("pattern", Nbt::str(banner_names[size_t(p)]));
                l.set("color", Nbt::str(DYE_NAMES[c & 15]));
                out.list.push_back(std::move(l));
            }
            return true;
        }
    } catch (const std::exception&) {
    }
    return false;
}
// NBT -> network value. Empty optional when unknown / malformed.
inline bool value_from_nbt(const std::string& name, const Nbt& v, std::vector<uint8_t>& out) {
    int id = comp::id(name.c_str());
    if (id < 0) return false;
    Writer w;
    auto num = [](const Nbt& n) { return n.is_num() ? int(n.i) : 0; };
    if (id == comp::custom_name() || id == comp::id("item_name")) write_nbt(w, v);
    else if (id == comp::repair_cost() || id == comp::map_id()) w.varint(num(v));
    else if (id == comp::glint()) w.u8(num(v) != 0);
    else if (id == comp::base_color()) {
        int d = dye_index(v.s);
        if (d < 0) return false;
        w.varint(d);
    } else if (id == comp::enchantments() || id == comp::stored_enchantments()) {
        static const std::vector<std::string> names = enchant_names();
        Enchs e;
        for (auto& [k, lvl] : v.comp)
            if (int i = index_of(names, k); i >= 0) e.emplace_back(i, num(lvl));
        std::sort(e.begin(), e.end());
        w.varint(int(e.size()));
        for (auto& [i, l] : e) w.varint(i).varint(l);
    } else if (id == comp::container()) {
        std::vector<ItemStack> items;
        for (auto& e : v.list) {
            int slot = int(comp::nbt_int(e, "slot", -1));
            const Nbt* it = e.get("item");
            if (slot < 0 || slot > 255 || !it) continue;
            if (size_t(slot) >= items.size()) items.resize(size_t(slot) + 1);
            items[size_t(slot)] = item_from(*it);
        }
        w.bytes(comp::encode_contents(items.data(), int(items.size())));
    } else if (id == comp::potion_contents()) {
        std::string pot = v.t == Nbt::String ? v.s : comp::nbt_str(v, "potion");
        int p = pot.empty() ? -1 : index_of(potion_names, pot);
        if (p >= 0) w.u8(1).varint(p);
        else w.u8(0);
        if (const Nbt* c = v.get("custom_color"); c && c->is_num()) w.u8(1).i32(int32_t(c->i));
        else w.u8(0);
        w.varint(0);
        std::string cn = v.t == Nbt::Compound ? comp::nbt_str(v, "custom_name") : "";
        if (!cn.empty()) w.u8(1).str(cn);
        else w.u8(0);
    } else if (id == comp::trim()) {
        int m = index_of(trim_materials, comp::nbt_str(v, "material")), p = index_of(trim_patterns, comp::nbt_str(v, "pattern"));
        if (m < 0 || p < 0) return false;
        w.varint(m + 1).varint(p + 1);
    } else if (id == comp::banner_patterns()) {
        w.varint(int(v.list.size()));
        for (auto& l : v.list) {
            int p = index_of(banner_names, comp::nbt_str(l, "pattern")), c = dye_index(comp::nbt_str(l, "color"));
            if (p < 0 || c < 0) return false;
            w.varint(p + 1).varint(c);
        }
    } else {
        return false;
    }
    out = std::move(w.b);
    return true;
}

inline void components_to_nbt(const ItemStack& s, Nbt& c) {
    if (!s.comp) return;
    for (auto& [id, bytes] : s.comp->set) {
        const std::string& name = component_name(id);
        Nbt v;
        if (value_to_nbt(id, bytes, v)) {
            c.set("minecraft:" + name, std::move(v));
        } else {
            Nbt a;
            a.t = Nbt::ByteArray;
            for (uint8_t b : bytes) a.arr.push_back(int8_t(b));
            c.set("craftpp:raw/" + name, std::move(a));
        }
    }
    for (int id : s.comp->removed) c.set("!minecraft:" + component_name(id), Nbt::compound());
}
inline void components_from_nbt(ItemStack& s, const Nbt& c) {
    ItemComponents out;
    for (auto& [k, v] : c.comp) {
        bool removed = !k.empty() && k[0] == '!';
        std::string key = removed ? k.substr(1) : k;
        if (key.rfind("craftpp:raw/", 0) == 0 && v.t == Nbt::ByteArray) {
            int id = comp::id(key.substr(12).c_str());
            if (id < 0) continue;
            std::vector<uint8_t> b;
            for (auto x : v.arr) b.push_back(uint8_t(x));
            out.set.emplace_back(id, std::move(b));
            continue;
        }
        std::string name = key.rfind("minecraft:", 0) == 0 ? key.substr(10) : key;
        if (name == "damage") continue;  // ItemStack::damage (server.hpp item_from)
        int id = comp::id(name.c_str());
        if (id < 0) continue;
        if (removed) { out.removed.push_back(id); continue; }
        std::vector<uint8_t> b;
        if (value_from_nbt(name, v, b)) out.set.emplace_back(id, std::move(b));
    }
    std::sort(out.set.begin(), out.set.end(), [](auto& a, auto& b) { return a.first < b.first; });
    if (!out.set.empty() || !out.removed.empty()) s.comp = std::make_shared<const ItemComponents>(std::move(out));
}

}  // namespace cont
