// Item stacks between the bridge's JSON ({item, count, meta}) and CraftPP's ItemStack (data components in their
// network encoding, see game/player_state.hpp). Meta keys follow the bridge's ItemMetaImpl.toJson.
#include "internal.hpp"

namespace plugins::detail {
namespace {

int cid(const char* name) { return comp::id(name); }

std::vector<uint8_t> text_bytes(const std::string& legacy) {
    Writer w;
    legacy_component(w, legacy);
    return std::move(w.b);
}

// Text component NBT -> legacy "§" string (colours and decorations of each part; the bridge speaks legacy text).
void legacy_of(const Nbt& n, std::string& out) {
    static const char* names[16] = {"black", "dark_blue", "dark_green", "dark_aqua", "dark_red", "dark_purple", "gold", "gray",
                                    "dark_gray", "blue", "green", "aqua", "red", "light_purple", "yellow", "white"};
    if (n.t == Nbt::String) { out += n.s; return; }
    if (n.t == Nbt::List) { for (auto& e : n.list) legacy_of(e, out); return; }
    if (n.t != Nbt::Compound) return;
    if (const Nbt* c = n.get("color"); c && c->t == Nbt::String) {
        const char* code = "0123456789abcdef";
        for (int i = 0; i < 16; i++)
            if (c->s == names[i]) out += std::string("\xC2\xA7") + code[i];
        if (c->s.size() == 7 && c->s[0] == '#') {
            out += "\xC2\xA7x";
            for (int i = 1; i < 7; i++) out += std::string("\xC2\xA7") + c->s[size_t(i)];
        }
    }
    static const std::pair<const char*, char> deco[] = {{"bold", 'l'}, {"italic", 'o'}, {"underlined", 'n'}, {"strikethrough", 'm'}, {"obfuscated", 'k'}};
    for (auto& [k, code] : deco)
        if (const Nbt* b = n.get(k); b && b->is_num() && b->i) out += std::string("\xC2\xA7") + code;
    if (const Nbt* t = n.get("text"); t && t->t == Nbt::String) out += t->s;
    else if (const Nbt* t = n.get("translate"); t && t->t == Nbt::String) out += t->s;
    if (const Nbt* e = n.get("extra")) legacy_of(*e, out);
}
std::string legacy_of_bytes(const std::vector<uint8_t>& b) {
    Reader r(b);
    std::string s;
    legacy_of(comp::nbt_read(r), s);
    return s;
}

// Bukkit ItemFlag -> the components its tooltip hides (Paper's CraftItemFlag mapping).
const std::vector<const char*>& flag_components(const std::string& flag) {
    static const std::map<std::string, std::vector<const char*>> map = {
        {"HIDE_ENCHANTS", {"enchantments"}},
        {"HIDE_ATTRIBUTES", {"attribute_modifiers"}},
        {"HIDE_UNBREAKABLE", {"unbreakable"}},
        {"HIDE_DESTROYS", {"can_break"}},
        {"HIDE_PLACED_ON", {"can_place_on"}},
        {"HIDE_DYE", {"dyed_color"}},
        {"HIDE_ARMOR_TRIM", {"trim"}},
        {"HIDE_STORED_ENCHANTS", {"stored_enchantments"}},
        {"HIDE_ADDITIONAL_TOOLTIP", {"banner_patterns", "bees", "block_entity_data", "block_state", "bundle_contents",
                                     "charged_projectiles", "container", "container_loot", "firework_explosion", "fireworks",
                                     "instrument", "map_id", "painting/variant", "pot_decorations", "potion_contents",
                                     "tropical_fish/pattern", "written_book_content", "jukebox_playable", "ominous_bottle_amplifier",
                                     "suspicious_stew_effects", "provides_trim_material"}},
    };
    static const std::vector<const char*> none;
    auto it = map.find(flag);
    return it == map.end() ? none : it->second;
}

}  // namespace

ItemStack item_from_json(const Json& j) {
    if (!j.isObject()) return {};
    auto found = item_by_name.find(norm_id(text_arg(j, "item")));
    if (found == item_by_name.end() || found->second <= 0) return {};
    ItemStack s(found->second, std::clamp(int(num_arg(j, "count", 1)), 1, 99));
    const Json* meta = j.find("meta");
    if (!meta || !meta->isObject()) return s;
    const Json& m = *meta;
    s.damage = std::max(0, int(num_arg(m, "damage")));
    if (m.has("name")) comp::set(s, cid("custom_name"), text_bytes(text_arg(m, "name")));
    if (m.has("itemName")) comp::set(s, cid("item_name"), text_bytes(text_arg(m, "itemName")));
    if (auto* lore = m.find("lore"); lore && lore->isArray()) {
        Writer w;
        w.varint(int(std::min<size_t>(lore->size(), 256)));
        for (size_t i = 0; i < lore->size() && i < 256; i++) legacy_component(w, (*lore)[i].isString() ? (*lore)[i].asString() : "");
        comp::set(s, cid("lore"), std::move(w.b));
    }
    if (auto* ench = m.find("enchants"); ench && ench->isObject()) {
        std::vector<std::pair<int, int>> list;
        for (auto& [name, lvl] : ench->obj)
            for (size_t i = 0; i < cont::enchants.size(); i++)
                if (cont::enchants[i].name == norm_id(name) && lvl.isNumber()) list.emplace_back(int(i), lvl.asInt());
        std::sort(list.begin(), list.end());
        Writer w;
        w.varint(int(list.size()));
        for (auto& [e, lvl] : list) w.varint(e).varint(lvl);
        bool stored = s.item == item_by_name["minecraft:enchanted_book"];
        comp::set(s, cid(stored ? "stored_enchantments" : "enchantments"), std::move(w.b));
    }
    if (m.has("maxDamage")) comp::set(s, cid("max_damage"), Writer().varint(std::max(1, int(num_arg(m, "maxDamage")))).b);
    if (m.has("maxStackSize")) comp::set(s, cid("max_stack_size"), Writer().varint(std::clamp(int(num_arg(m, "maxStackSize")), 1, 99)).b);
    if (bool_arg(m, "unbreakable")) comp::set(s, cid("unbreakable"), {});
    if (auto* g = m.find("glint"); g && g->isBool()) comp::set(s, cid("enchantment_glint_override"), Writer().boolean(g->asBool()).b);
    if (m.has("color")) comp::set(s, cid("dyed_color"), Writer().i32(int32_t(int64_t(num_arg(m, "color")))).b);
    if (auto* cmd = m.find("customModelData"); cmd && cmd->isObject()) {
        Writer w;
        auto* floats = cmd->find("floats");
        w.varint(floats && floats->isArray() ? int(floats->size()) : 0);
        if (floats && floats->isArray())
            for (auto& f : floats->arr) w.f32(float(f.isNumber() ? f.asDouble() : 0));
        w.varint(0).varint(0).varint(0);  // flags, strings, colors
        comp::set(s, cid("custom_model_data"), std::move(w.b));
    }
    auto* flags = m.find("flags");
    bool hide = bool_arg(m, "hideTooltip");
    if (hide || (flags && flags->isArray() && flags->size())) {
        std::vector<int> hidden;
        if (flags && flags->isArray())
            for (auto& f : flags->arr)
                if (f.isString())
                    for (const char* c : flag_components(f.asString()))
                        if (int id = cid(c); id >= 0 && std::find(hidden.begin(), hidden.end(), id) == hidden.end()) hidden.push_back(id);
        Writer w;
        w.boolean(hide).varint(int(hidden.size()));
        for (int id : hidden) w.varint(id);
        comp::set(s, cid("tooltip_display"), std::move(w.b));
    }
    return s;
}

std::string item_to_json(const ItemStack& s) {
    if (s.empty() || s.item >= int(item_names.size())) return "null";
    std::string meta;
    auto add = [&](const std::string& kv) { meta += (meta.empty() ? "" : ",") + kv; };
    if (s.damage > 0) add("\"damage\":" + std::to_string(s.damage));
    try {
        if (auto* b = comp::get(s, cid("custom_name"))) add("\"name\":" + q(legacy_of_bytes(*b)));
        if (auto* b = comp::get(s, cid("item_name"))) add("\"itemName\":" + q(legacy_of_bytes(*b)));
        if (auto* b = comp::get(s, cid("lore"))) {
            Reader r(*b);
            std::string lines;
            for (int n = r.varint(); n > 0; n--) {
                std::string line;
                legacy_of(comp::nbt_read(r), line);
                lines += (lines.empty() ? "" : ",") + q(line);
            }
            add("\"lore\":[" + lines + "]");
        }
        for (const char* k : {"enchantments", "stored_enchantments"})
            if (auto* b = comp::get(s, cid(k))) {
                Reader r(*b);
                std::string map;
                for (int n = r.varint(); n > 0; n--) {
                    int e = r.varint(), lvl = r.varint();
                    if (e >= 0 && size_t(e) < cont::enchants.size())
                        map += (map.empty() ? "" : ",") + q(cont::enchants[size_t(e)].name) + ":" + std::to_string(lvl);
                }
                add("\"enchants\":{" + map + "}");
            }
        if (auto* b = comp::get(s, cid("max_damage"))) add("\"maxDamage\":" + std::to_string(Reader(*b).varint()));
        if (auto* b = comp::get(s, cid("max_stack_size"))) add("\"maxStackSize\":" + std::to_string(Reader(*b).varint()));
        if (comp::get(s, cid("unbreakable"))) add("\"unbreakable\":true");
        if (auto* b = comp::get(s, cid("enchantment_glint_override"))) add(std::string("\"glint\":") + (Reader(*b).u8() ? "true" : "false"));
        if (auto* b = comp::get(s, cid("dyed_color"))) add("\"color\":" + std::to_string(Reader(*b).i32()));
        if (auto* b = comp::get(s, cid("custom_model_data"))) {
            Reader r(*b);
            int n = r.varint();
            std::string floats;
            for (int i = 0; i < n; i++) floats += (i ? "," : "") + num(r.f32());
            add("\"customModelData\":{\"floats\":[" + floats + "]}");
        }
        if (auto* b = comp::get(s, cid("tooltip_display"))) {
            Reader r(*b);
            if (r.u8()) add("\"hideTooltip\":true");
            std::vector<int> hidden;
            for (int n = r.varint(); n > 0; n--) hidden.push_back(r.varint());
            std::string flags;
            for (const char* flag : {"HIDE_ENCHANTS", "HIDE_ATTRIBUTES", "HIDE_UNBREAKABLE", "HIDE_DESTROYS", "HIDE_PLACED_ON",
                                     "HIDE_DYE", "HIDE_ARMOR_TRIM", "HIDE_STORED_ENCHANTS", "HIDE_ADDITIONAL_TOOLTIP"}) {
                int first = cid(flag_components(flag).front());
                if (std::find(hidden.begin(), hidden.end(), first) != hidden.end()) flags += (flags.empty() ? "" : ",") + q(flag);
            }
            if (!flags.empty()) add("\"flags\":[" + flags + "]");
        }
    } catch (const std::exception&) {
        // a malformed component (from a client or an old file) only drops that part of the meta
    }
    return "{\"item\":" + q(item_names[size_t(s.item)]) + ",\"count\":" + std::to_string(s.count) +
           (meta.empty() ? "" : ",\"meta\":{" + meta + "}") + "}";
}

}  // namespace plugins::detail
