#pragma once
// Item data components (ItemStack::comp): get/set by name, network patch parsing for the components the server
// itself produces, binary NBT reading, and the stack <-> NBT/text forms used by block entities and player files.
#include "../game/survival.hpp"
#include "../commands/nbt.hpp"

#include <sstream>

namespace comp {

inline int id(const char* name) {
    auto it = component_ids.find(name);
    return it == component_ids.end() ? -1 : it->second;
}
// Cached ids of the components used here.
#define COMP_ID(fn, name) inline int fn() { static int v = id(name); return v; }
COMP_ID(custom_name, "custom_name")
COMP_ID(container, "container")
COMP_ID(repair_cost, "repair_cost")
COMP_ID(enchantments, "enchantments")
COMP_ID(stored_enchantments, "stored_enchantments")
COMP_ID(potion_contents, "potion_contents")
COMP_ID(written_book_content, "written_book_content")
COMP_ID(writable_book_content, "writable_book_content")
COMP_ID(base_color, "base_color")
COMP_ID(banner_patterns, "banner_patterns")
COMP_ID(map_id, "map_id")
COMP_ID(glint, "enchantment_glint_override")
COMP_ID(lock, "lock")
COMP_ID(trim, "trim")
#undef COMP_ID

inline const std::vector<uint8_t>* get(const ItemStack& s, int id) {
    if (!s.comp || id < 0) return nullptr;
    for (auto& [k, v] : s.comp->set)
        if (k == id) return &v;
    return nullptr;
}
inline void set(ItemStack& s, int id, std::vector<uint8_t> v) {
    if (id < 0) return;
    auto c = s.comp ? std::make_shared<ItemComponents>(*s.comp) : std::make_shared<ItemComponents>();
    auto it = std::lower_bound(c->set.begin(), c->set.end(), id, [](auto& e, int k) { return e.first < k; });
    if (it != c->set.end() && it->first == id) it->second = std::move(v);
    else c->set.insert(it, {id, std::move(v)});
    std::erase(c->removed, id);
    s.comp = std::move(c);
}
inline void remove(ItemStack& s, int id) {
    if (!get(s, id)) return;
    auto c = std::make_shared<ItemComponents>(*s.comp);
    std::erase_if(c->set, [&](auto& e) { return e.first == id; });
    s.comp = c->set.empty() && c->removed.empty() ? nullptr : ComponentsPtr(std::move(c));
}

// ---------- binary NBT ----------
inline std::string nbt_read_string(Reader& r) {
    size_t n = r.u16();
    if (r.i + n > r.n) throw std::runtime_error("nbt string");
    std::string s(reinterpret_cast<const char*>(r.p + r.i), n);
    r.i += n;
    return s;
}
inline Nbt nbt_read_payload(Reader& r, uint8_t t, int depth = 0) {
    if (depth > 512) throw std::runtime_error("nbt too deep");
    Nbt n;
    n.t = Nbt::Type(t);
    switch (t) {
        case Nbt::Byte: n.i = int8_t(r.u8()); break;
        case Nbt::Short: n.i = int16_t(r.u16()); break;
        case Nbt::Int: n.i = r.i32(); break;
        case Nbt::Long: n.i = r.i64(); break;
        case Nbt::Float: n.d = r.f32(); break;
        case Nbt::Double: n.d = r.f64(); break;
        case Nbt::String: n.s = nbt_read_string(r); break;
        case Nbt::List: {
            uint8_t et = r.u8();
            int len = r.i32();
            if (len < 0 || size_t(len) > r.n - r.i) throw std::runtime_error("nbt list");
            for (int k = 0; k < len; k++) n.list.push_back(nbt_read_payload(r, et, depth + 1));
            break;
        }
        case Nbt::Compound:
            for (uint8_t et; (et = r.u8()) != 0;) {
                std::string k = nbt_read_string(r);
                n.comp.emplace_back(std::move(k), nbt_read_payload(r, et, depth + 1));
            }
            break;
        case Nbt::ByteArray: case Nbt::IntArray: case Nbt::LongArray: {
            int len = r.i32();
            if (len < 0 || size_t(len) > r.n - r.i) throw std::runtime_error("nbt array");
            for (int k = 0; k < len; k++) n.arr.push_back(t == Nbt::ByteArray ? int8_t(r.u8()) : t == Nbt::IntArray ? r.i32() : r.i64());
            break;
        }
        default: throw std::runtime_error("nbt tag type");
    }
    return n;
}
inline Nbt nbt_read(Reader& r) { return nbt_read_payload(r, r.u8()); }  // network form (nameless root)
inline Nbt nbt_parse(const std::vector<uint8_t>& b) {
    if (b.empty()) return Nbt::compound();
    Reader r(b);
    return nbt_read(r);
}
inline std::vector<uint8_t> nbt_bytes(const Nbt& n) {
    Writer w;
    write_nbt(w, n);
    return std::move(w.b);
}
inline int64_t nbt_int(const Nbt& c, const char* k, int64_t def = 0) {
    const Nbt* v = c.get(k);
    return v && v->is_num() ? (v->t == Nbt::Float || v->t == Nbt::Double ? int64_t(v->d) : v->i) : def;
}
inline std::string nbt_str(const Nbt& c, const char* k) {
    const Nbt* v = c.get(k);
    return v && v->t == Nbt::String ? v->s : "";
}

// ---------- network patch ----------
inline ItemStack read_stack(Reader& r);
// Skips one component value. Only components this server creates (or simple ones) are understood.
inline void skip_value(Reader& r, int id) {
    auto list_of_pairs = [&] { for (int n = r.varint(); n > 0; n--) r.varint(), r.varint(); };
    if (id == comp::custom_name() || id == comp::id("item_name") || id == comp::lock()) nbt_read(r);
    else if (id == comp::repair_cost() || id == comp::id("damage") || id == comp::map_id() || id == comp::base_color() ||
             id == comp::id("max_damage") || id == comp::id("max_stack_size") || id == comp::id("rarity"))
        r.varint();
    else if (id == comp::enchantments() || id == comp::stored_enchantments()) list_of_pairs();
    else if (id == comp::glint()) r.u8();
    else if (id == comp::container()) {
        for (int n = r.varint(); n > 0; n--)
            if (r.u8()) read_stack(r);
    } else if (id == comp::potion_contents()) {
        if (r.u8()) r.varint();
        if (r.u8()) r.i32();
        for (int n = r.varint(); n > 0; n--) {  // custom effects: MobEffectInstance.STREAM_CODEC (details may nest)
            r.varint();
            for (bool more = true; more;) { r.varint(), r.varint(), r.u8(), r.u8(), r.u8(); more = r.u8(); }
        }
        if (r.u8()) r.str();
    } else if (id == comp::writable_book_content()) {
        for (int n = r.varint(); n > 0; n--) { r.str(); if (r.u8()) r.str(); }
    } else if (id == comp::written_book_content()) {
        r.str(); if (r.u8()) r.str();  // title (filterable)
        r.str(); r.varint();           // author, generation
        for (int n = r.varint(); n > 0; n--) { nbt_read(r); if (r.u8()) nbt_read(r); }
        r.u8();  // resolved
    } else if (id == comp::banner_patterns()) {
        for (int n = r.varint(); n > 0; n--) {
            if (r.varint() == 0) { r.str(); r.str(); if (r.u8()) r.str(); if (r.u8()) r.str(); }  // inline pattern
            r.varint();
        }
    } else {
        throw std::runtime_error("unsupported component " + std::to_string(id));
    }
}
inline void read_patch(Reader& r, ItemStack& s) {
    int added = r.varint(), removed = r.varint();
    if (added < 0 || removed < 0 || added > 256 || removed > 256) throw std::runtime_error("bad patch");
    ItemComponents c;
    for (int k = 0; k < added; k++) {
        int cid = r.varint();
        size_t at = r.i;
        skip_value(r, cid);
        if (cid == comp::id("damage")) {
            Reader d = r;
            d.i = at;
            s.damage = d.varint();
            continue;
        }
        c.set.emplace_back(cid, std::vector<uint8_t>(r.p + at, r.p + r.i));
    }
    for (int k = 0; k < removed; k++) c.removed.push_back(r.varint());
    std::sort(c.set.begin(), c.set.end(), [](auto& a, auto& b) { return a.first < b.first; });
    if (!c.set.empty() || !c.removed.empty()) s.comp = std::make_shared<const ItemComponents>(std::move(c));
}
inline ItemStack read_stack(Reader& r) {  // ItemStackTemplate: item, count, patch
    ItemStack s;
    s.item = r.varint();
    s.count = r.varint();
    if (s.item < 0 || s.item >= ITEM_COUNT) throw std::runtime_error("bad item");
    read_patch(r, s);
    return s;
}
inline ItemStack read_optional_stack(Reader& r) {  // ItemStack.OPTIONAL_STREAM_CODEC
    int n = r.varint();
    if (n <= 0) return {};
    ItemStack s;
    s.item = r.varint();
    if (s.item < 0 || s.item >= ITEM_COUNT) throw std::runtime_error("bad item");
    s.count = n;
    read_patch(r, s);
    return s;
}
inline void write_patch(Writer& w, const ItemStack& s) {
    int added = (s.damage > 0) + (s.comp ? int(s.comp->set.size()) : 0);
    w.varint(added).varint(s.comp ? int(s.comp->removed.size()) : 0);
    if (s.damage > 0) w.varint(component_ids["damage"]).varint(s.damage);
    if (!s.comp) return;
    for (auto& [k, v] : s.comp->set) w.varint(k).bytes(v);
    for (int k : s.comp->removed) w.varint(k);
}

// ---------- container contents (ItemContainerContents) ----------
inline std::vector<uint8_t> encode_contents(const ItemStack* items, int n) {
    int last = n - 1;
    while (last >= 0 && items[last].empty()) last--;
    Writer w;
    w.varint(last + 1);
    for (int i = 0; i <= last; i++) {
        if (items[i].empty()) { w.u8(0); continue; }
        w.u8(1).varint(items[i].item).varint(items[i].count);
        write_patch(w, items[i]);
    }
    return std::move(w.b);
}
inline std::vector<ItemStack> decode_contents(const std::vector<uint8_t>& v) {
    std::vector<ItemStack> out;
    try {
        Reader r(v);
        int n = r.varint();
        for (int i = 0; i < n && i < 256; i++) out.push_back(r.u8() ? read_stack(r) : ItemStack{});
    } catch (const std::exception&) {
    }
    return out;
}

// ---------- names ----------
// Text component as network NBT: plain string tag (Component.literal) or {translate:key}.
inline std::vector<uint8_t> text_literal(const std::string& s) {
    Writer w;
    w.u8(Nbt::String);
    nbt_string(w, s);
    return std::move(w.b);
}
inline std::vector<uint8_t> text_translate(const std::string& key) {
    Nbt c = Nbt::compound();
    c.set("translate", Nbt::str(key));
    return nbt_bytes(c);
}

// ---------- persistence ----------
// Stack in vanilla ItemStack NBT ({id, count, components}): server.hpp item_nbt / item_from, whose component
// part is src/containers/item_nbt.hpp.
inline Nbt stack_nbt(const ItemStack& s) { return item_nbt(s); }
inline ItemStack stack_from_nbt(const Nbt& n) { return item_from(n); }
// Items list in BlockEntity / Player form: [{Slot:b, id, count, components}].
inline Nbt items_nbt(const ItemStack* items, int n) {
    Nbt l = Nbt::make_list();
    for (int i = 0; i < n; i++)
        if (!items[i].empty()) {
            Nbt e = stack_nbt(items[i]);
            e.comp.insert(e.comp.begin(), {"Slot", Nbt::num(Nbt::Byte, i)});
            l.list.push_back(std::move(e));
        }
    return l;
}
inline void items_from_nbt(const Nbt& tag, const char* key, ItemStack* items, int n) {
    const Nbt* l = tag.get(key);
    if (!l || l->t != Nbt::List) return;
    for (auto& e : l->list) {
        int slot = int(nbt_int(e, "Slot", -1)) & 255;
        if (slot < n) items[slot] = stack_from_nbt(e);
    }
}

}  // namespace comp
