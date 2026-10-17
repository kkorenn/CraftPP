#pragma once
// Item data components the effects module reads and writes (network encoding, as ItemStack::comp stores them):
// minecraft:enchantments / stored_enchantments (ItemEnchantments) and minecraft:potion_contents (PotionContents).
// Included from effects.hpp before enchant.hpp.

inline int component_id(const char* name) {
    auto it = component_ids.find(name);
    return it == component_ids.end() ? -1 : it->second;
}
inline bool item_has_comp(const ItemStack& s) { return s.comp != nullptr; }
inline const std::vector<uint8_t>* item_component(const ItemStack& s, int id) { return comp::get(s, id); }  // src/containers

// ---------- ItemEnchantments (enchantment registry id -> level) ----------
using EnchList = std::vector<std::pair<int, int>>;
inline EnchList decode_enchants(const std::vector<uint8_t>* b) {
    EnchList out;
    if (!b) return out;
    try {
        Reader r(*b);
        for (int n = r.varint(); n > 0 && out.size() < 256; n--) {
            int id = r.varint(), lvl = r.varint();
            out.emplace_back(id, lvl);
        }
    } catch (const std::exception&) {
    }
    return out;
}
inline EnchList item_enchants(const ItemStack& s) {
    static const int id = component_id("enchantments");
    return item_has_comp(s) ? decode_enchants(item_component(s, id)) : EnchList{};
}

// ---------- PotionContents ----------
struct Potion { std::string name; std::vector<EffectInstance> effects; };
inline std::vector<Potion> potions;  // minecraft:potion registry order (Potions.java)
inline int potion_id(const std::string& name) {
    std::string n = name.rfind("minecraft:", 0) == 0 ? name.substr(10) : name;
    // registry names differ from Potion.name for long_/strong_ variants: data/effects.json keeps the registry key
    for (size_t i = 0; i < potions.size(); i++) if (potions[i].name == n) return int(i);
    return -1;
}
struct PotionContents {
    int potion = -1;                      // minecraft:potion id, -1 none
    std::optional<int> color;             // customColor
    std::vector<EffectInstance> custom;   // customEffects
    std::optional<std::string> name;      // customName
    bool has_effects() const { return !custom.empty() || (potion >= 0 && !potions[size_t(potion)].effects.empty()); }
    std::vector<EffectInstance> all_effects() const {  // getAllEffects
        std::vector<EffectInstance> v;
        if (potion >= 0) v = potions[size_t(potion)].effects;
        v.insert(v.end(), custom.begin(), custom.end());
        return v;
    }
    // forEachEffect with withScaledDuration(scale)
    std::vector<EffectInstance> scaled_effects(float scale) const {
        std::vector<EffectInstance> v = all_effects();
        for (auto& e : v)
            if (!e.infinite() && e.duration != 0) e.duration = std::max(mth::floor(float(e.duration) * scale), 1);
        return v;
    }
    int color_value() const {  // getColor: customColor, else the blend of the visible effects, else -13083194
        if (color) return *color;
        int r = 0, g = 0, b = 0, w = 0;
        for (auto& e : all_effects()) {
            if (!e.visible) continue;
            int c = mob_effects[size_t(e.id)].color, a = e.amp + 1;
            r += a * (c >> 16 & 255), g += a * (c >> 8 & 255), b += a * (c & 255), w += a;
        }
        return w == 0 ? -13083194 : int(0xFF000000u | uint32_t(r / w) << 16 | uint32_t(g / w) << 8 | uint32_t(b / w));
    }
};
inline void read_effect_details(Reader& r, EffectInstance& x, int depth = 0) {  // MobEffectInstance.Details.STREAM_CODEC
    x.amp = r.varint() & 255, x.duration = r.varint();
    x.ambient = r.u8(), x.visible = r.u8(), x.icon = r.u8();
    if (r.u8() && depth < 16) {
        x.hidden = std::make_unique<EffectInstance>();
        x.hidden->id = x.id;
        read_effect_details(r, *x.hidden, depth + 1);
    }
}
inline void write_effect_details(Writer& w, const EffectInstance& x) {
    w.varint(x.amp).varint(x.duration).boolean(x.ambient).boolean(x.visible).boolean(x.icon);
    w.boolean(x.hidden != nullptr);
    if (x.hidden) write_effect_details(w, *x.hidden);
}
inline PotionContents decode_potion(const std::vector<uint8_t>* b) {
    PotionContents p;
    if (!b) return p;
    try {
        Reader r(*b);
        if (r.u8()) p.potion = r.varint();
        if (size_t(p.potion) >= potions.size()) p.potion = -1;
        if (r.u8()) p.color = r.i32();
        for (int n = r.varint(); n > 0 && p.custom.size() < 256; n--) {
            EffectInstance x;
            x.id = r.varint();
            read_effect_details(r, x);
            if (size_t(x.id) < mob_effects.size()) p.custom.push_back(std::move(x));
        }
        if (r.u8()) p.name = r.str();
    } catch (const std::exception&) {
    }
    return p;
}
inline std::vector<uint8_t> encode_potion(const PotionContents& p) {
    Writer w;
    w.boolean(p.potion >= 0);
    if (p.potion >= 0) w.varint(p.potion);
    w.boolean(p.color.has_value());
    if (p.color) w.i32(*p.color);
    w.varint(int(p.custom.size()));
    for (auto& x : p.custom) { w.varint(x.id); write_effect_details(w, x); }
    w.boolean(p.name.has_value());
    if (p.name) w.str(*p.name);
    return std::move(w.b);
}
inline PotionContents potion_contents(const ItemStack& s) {
    static const int id = component_id("potion_contents");
    return item_has_comp(s) ? decode_potion(item_component(s, id)) : PotionContents{};
}
inline float potion_duration_scale(const ItemStack& s) {  // minecraft:potion_duration_scale (default from the item)
    static const int id = component_id("potion_duration_scale"), lingering = item_by_name["minecraft:lingering_potion"],
                     tipped = item_by_name["minecraft:tipped_arrow"];
    if (const auto* b = item_has_comp(s) ? item_component(s, id) : nullptr; b && b->size() == 4) {
        Reader r(*b);
        return r.f32();
    }
    return s.item == lingering ? 0.25f : s.item == tipped ? 0.125f : 1.0f;  // item prototypes (Items.java)
}
