#pragma once
// Brewing stand (BrewingStandBlockEntity + BrewingStandMenu, 26.3 data-driven brewing recipes) and beacon
// (BeaconBlockEntity levels/beam + BeaconMenu + set_beacon). Potion and beacon *effects* are applied through
// hooks for a later effects port (beacon_effect_hook).
#include "stations.hpp"
#include "../gen/light_data.hpp"
#include "../progress/api.hpp"

namespace cont {

// ---------- brewing data ----------
struct PotionIng {
    std::vector<uint16_t> items;
    std::vector<int> potions;  // empty + !any: no potion_contents predicate
    bool any = true;
};
struct Brew {
    PotionIng in, reagent;
    ItemStack result;
};
inline std::vector<Brew> brews;
inline std::map<std::string, int> effect_ids;

inline int potion_of(const ItemStack& s) {  // PotionContents.potion(), -1 = none
    auto* b = comp::get(s, comp::potion_contents());
    if (!b || b->empty() || (*b)[0] == 0) return -1;
    Reader r(*b);
    r.u8();
    return r.varint();
}
inline bool ing_test(const PotionIng& g, const ItemStack& s) {  // PotionIngredient.test
    if (s.empty() || !in_sorted(g.items, s.item)) return false;
    if (g.any) return true;
    int p = potion_of(s);
    return p >= 0 && std::find(g.potions.begin(), g.potions.end(), p) != g.potions.end();
}
inline void load_brewing_data(const Json& j) {
    auto ing = [](const Json& o) {
        PotionIng g;
        for (auto& x : o["i"].arr) g.items.push_back(uint16_t(x.asInt()));
        if (!o["p"].isNull()) {
            g.any = false;
            for (auto& x : o["p"].arr) g.potions.push_back(x.asInt());
        }
        return g;
    };
    for (auto& r : j["brewing"].arr) {
        Brew b{ing(r["in"]), ing(r["re"]), {r["r"][0].asInt(), r["r"][1].asInt()}};
        if (!r["p"].isNull()) {
            Writer w;  // PotionContents(potion), no colour, no custom effects, no name
            w.u8(1).varint(r["p"].asInt()).u8(0).varint(0).u8(0);
            comp::set(b.result, comp::potion_contents(), std::move(w.b));
        }
        brews.push_back(std::move(b));
    }
    for (auto& [k, v] : j["effects"].obj) effect_ids[k] = v.asInt();
}
inline bool is_reagent(const ItemStack& s) {  // BREWING_REAGENTS property set
    for (auto& b : brews)
        if (in_sorted(b.reagent.items, s.item)) return true;
    return false;
}
inline bool is_potion_input(const ItemStack& s) {  // PotionIngredient.isPotionInput
    if (tagged("brewing_potion_inputs", s.item)) return true;
    for (auto& b : brews)
        if (in_sorted(b.in.items, s.item)) return true;
    return false;
}
inline const Brew* find_brew(const ItemStack& in, const ItemStack& reagent) {
    for (auto& b : brews)
        if (ing_test(b.in, in) && ing_test(b.reagent, reagent)) return &b;
    return nullptr;
}
inline bool is_brewing_fuel(const ItemStack& s) { return !s.empty() && extra(s.item).brewing_fuel; }

struct BrewingBE : be::Live {
    int brew_time = 0, total_brew = 400, fuel = 0, total_fuel = 20;
    float speed = 1.0f;
    int ingredient = -1;  // item being brewed
    int bits = -1;        // lastPotionCount
    BrewingBE() { items.resize(5); }
    bool ticks() const override { return true; }
    void removed_side_effects() override { be::drop_contents(*this); }
    bool worldly() const override { return true; }
    std::vector<int> face_slots(int d) const override {
        return d == bs::UP ? std::vector<int>{3} : d == bs::DOWN ? std::vector<int>{0, 1, 2, 3} : std::vector<int>{0, 1, 2, 4};
    }
    bool can_place(int slot, const ItemStack& s) const override {
        if (slot == 4) return is_brewing_fuel(s);
        return slot == 3 ? is_reagent(s) : is_potion_input(s) && items[size_t(slot)].empty();
    }
    bool can_take_face(int slot, const ItemStack& s, int) const override {
        static const int bottle = item_id("glass_bottle");
        return slot != 3 || s.item == bottle;
    }
    void load(const Nbt& n) override {
        brew_time = int(comp::nbt_int(n, "BrewTime"));
        total_brew = int(comp::nbt_int(n, "total_brew_time", 400));
        if (brew_time > 0) ingredient = items[3].empty() ? -1 : items[3].item;
        fuel = int(comp::nbt_int(n, "Fuel"));
        total_fuel = int(comp::nbt_int(n, "total_fuel", 20));
        const Nbt* sp = n.get("speed_multiplier");
        speed = sp && sp->is_num() ? float(sp->as_double()) : 1.0f;
    }
    void save(Nbt& n) const override {
        n.set("BrewTime", Nbt::num(Nbt::Int, brew_time));
        n.set("total_brew_time", Nbt::num(Nbt::Int, total_brew));
        n.set("Fuel", Nbt::num(Nbt::Int, fuel));
        n.set("total_fuel", Nbt::num(Nbt::Int, total_fuel));
        n.set("speed_multiplier", Nbt::real(Nbt::Float, speed));
    }
    bool brewable() const {
        const ItemStack& r = items[3];
        if (r.empty() || !is_reagent(r)) return false;
        for (int k = 0; k < 3; k++)
            if (!items[size_t(k)].empty() && find_brew(items[size_t(k)], r)) return true;
        return false;
    }
    void tick() override {  // serverTick
        ItemStack& f = items[4];
        if (fuel <= 0 && is_brewing_fuel(f)) {
            // brewing_fuel: uses = context_int_provider brewing/uses_default (20), speed brewing/speed_default (1.0)
            fuel = total_fuel = 20;
            speed = 1.0f;
            int rem = item_info[size_t(f.item)].craft_remainder;
            if (--f.count <= 0) f = rem >= 0 ? ItemStack{rem, 1} : ItemStack{};
            else if (rem >= 0) be::drop_stack(x, y, z, {rem, 1});
            set_changed();
        }
        bool can = brewable();
        if (brew_time > 0) {
            brew_time--;
            if (brew_time == 0 && can) {
                ItemStack& r = items[3];
                for (int k = 0; k < 3; k++)
                    if (auto* b = find_brew(items[size_t(k)], r)) items[size_t(k)] = b->result;
                int rem = item_info[size_t(r.item)].craft_remainder;
                if (--r.count <= 0) r = rem >= 0 ? ItemStack{rem, 1} : ItemStack{};
                else if (rem >= 0) be::drop_stack(x, y, z, {rem, 1});
                level_event(1035, x, y, z, 0);
            } else if (!can || items[3].empty() || items[3].item != ingredient) {
                brew_time = 0;
            }
            set_changed();
        } else if (can && fuel > 0) {
            float sp = speed > 0 ? speed : 1.0f;
            fuel--;
            brew_time = total_brew = int(std::ceil(400.0f / sp));
            ingredient = items[3].item;
            set_changed();
        }
        int nb = (!items[0].empty()) | (!items[1].empty()) << 1 | (!items[2].empty()) << 2;
        if (nb != bits) {
            bits = nb;
            int s = state;
            for (int k = 0; k < 3; k++) s = with(s, ("has_bottle_" + std::to_string(k)).c_str(), nb >> k & 1 ? "true" : "false");
            set_state(*this, s);
        }
    }
};

struct BrewingMenu : SlotsMenu {
    std::shared_ptr<BrewingBE> b;
    bool may_place(int i, const ItemStack& s) override {
        if (i < 3) return is_potion_input(s);  // PotionSlot
        if (i == 3) return is_reagent(s);
        return is_brewing_fuel(s);
    }
    int slot_limit(int i, const ItemStack& s) override { return i < 3 ? 1 : SlotsMenu::slot_limit(i, s); }
    void on_take(Player& p, int i, const ItemStack& s) override {
        if (i < 3) {
            int pot = potion_of(s);
            if (pot >= 0) progress::brewed_potion(p, pot);
        }
    }
    ItemStack quick_move(Player& p, int i) override {
        SlotRef r = menu_slot(p, i);
        if (r.s->empty()) return {};
        ItemStack stack = *r.s, clicked = stack;
        bool ok;
        if (i >= 5) {
            if (is_brewing_fuel(clicked)) {
                if (move_to(p, stack, 4, 5, false) || (is_reagent(stack) && !move_to(p, stack, 3, 4, false))) return {};
                ok = true;
            } else if (is_reagent(stack)) ok = move_to(p, stack, 3, 4, false);
            else if (is_potion_input(clicked)) ok = move_to(p, stack, 0, 3, false);
            else if (i < 32) ok = move_to(p, stack, 32, 41, false);
            else ok = move_to(p, stack, 5, 32, false);
        } else {
            ok = move_to(p, stack, 5, 41, true);
            if (ok && i < 3) on_take(p, i, clicked);
        }
        if (!ok) return {};
        *r.s = stack;
        slot_changed(p, r);
        if (stack.count == clicked.count) return {};
        return clicked;
    }
    int data_count() const override { return 2; }
    int data(int i) override { return i == 0 ? b->brew_time : b->fuel; }
};

// ---------- beacon ----------
inline void (*beacon_effect_hook)(Player& p, int effect, int amplifier, int duration) = nullptr;  // effects port

inline int beacon_tier(int effect) {  // getRequiredLevelsFor
    static const char* tiers[4][2] = {{"speed", "haste"}, {"resistance", "jump_boost"}, {"strength", nullptr}, {"regeneration", nullptr}};
    if (effect < 0) return 0;
    for (int t = 0; t < 4; t++)
        for (auto* n : tiers[t])
            if (n && effect_ids[n] == effect) return t + 1;
    return 0;
}
struct BeaconBE : be::Live {
    int levels = 0, primary = -1, secondary = -1;
    bool beam = false;
    bool ticks() const override { return true; }
    bool full_update_tag() const override { return true; }
    void load(const Nbt& n) override {
        auto eff = [&](const char* k) {
            std::string s = comp::nbt_str(n, k);
            auto it = effect_ids.find(s.rfind("minecraft:", 0) == 0 ? s.substr(10) : s);
            return it == effect_ids.end() || beacon_tier(it->second) == 0 ? -1 : it->second;
        };
        primary = eff("primary_effect"), secondary = eff("secondary_effect");
    }
    void save(Nbt& n) const override {
        for (auto& [name, id] : effect_ids) {
            if (id == primary) n.set("primary_effect", Nbt::str("minecraft:" + name));
            if (id == secondary) n.set("secondary_effect", Nbt::str("minecraft:" + name));
        }
    }
    void removed_side_effects() override {
        play_sound("block.beacon.deactivate", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
    }
    bool beam_clear() const {  // the beam reaches the sky (no fully opaque block above, except bedrock)
        for (int yy = y + 1; yy < world.max_y(); yy++) {
            int s = world.get_block(x, yy, z);
            if (s == blocks::bedrock) continue;
            if ((light_data::PROPS[s] >> 4 & 15) >= 15) return false;
        }
        return true;
    }
    int base() const {  // updateBase
        static const auto& tag = bs::R().tag("minecraft:beacon_base_blocks");
        int lv = 0;
        for (int step = 1; step <= 4; lv = step++) {
            int ly = y - step;
            if (ly < world.min_y()) break;
            for (int lx = x - step; lx <= x + step; lx++)
                for (int lz = z - step; lz <= z + step; lz++)
                    if (!bs::R().in(world.get_block(lx, ly, lz), tag)) return lv;
        }
        return lv;
    }
    // ponytail: the beam is re-checked in one pass every 80 ticks (vanilla scans 10 blocks a tick), and beam
    // colour sections aren't tracked (the client renders the beam itself).
    void tick() override {
        if (server_tick % 80 != 0) return;
        int prev = levels;
        beam = beam_clear();
        levels = beam ? base() : 0;
        if (levels > 0 && primary >= 0) {
            double range = levels * 10 + 10;
            int amp = levels >= 4 && primary == secondary ? 1 : 0, dur = (9 + levels * 2) * 20;
            if (beacon_effect_hook)
                for (auto& pp : players) {
                    Player& p = *pp;
                    if (p.x < x - range || p.x > x + 1 + range || p.z < z - range || p.z > z + 1 + range || p.y < y - range) continue;
                    beacon_effect_hook(p, primary, amp, dur);
                    if (levels >= 4 && primary != secondary && secondary >= 0) beacon_effect_hook(p, secondary, 0, dur);
                }
        }
        if (levels > 0) play_sound("block.beacon.ambient", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
        if (prev < levels) {
            for (auto& pp : players) {
                Player& p = *pp;
                if (p.x >= x - 10 && p.x <= x + 10 && p.y >= y - 5 && p.y <= y + 5 && p.z >= z - 10 && p.z <= z + 10)
                    progress::construct_beacon(p, levels);
            }
        }
        if (prev == 0 && levels > 0) play_sound("block.beacon.activate", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
        else if (prev > 0 && levels == 0) play_sound("block.beacon.deactivate", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
        if (prev != levels) set_changed();
    }
};
struct BeaconMenu : StationMenu {
    std::shared_ptr<BeaconBE> b;
    BeaconMenu() { items.resize(1), inputs = 1; }
    bool may_place(int, const ItemStack& s) override { return tagged("beacon_payment_items", s.item); }
    int slot_limit(int, const ItemStack&) override { return 1; }
    bool still_valid(Player& p) override { return !b->removed && in_range(p, *b); }
    ItemStack quick_move(Player& p, int i) override {
        SlotRef r = menu_slot(p, i);
        if (r.s->empty()) return {};
        ItemStack stack = *r.s, clicked = stack;
        bool ok;
        if (i == 0) ok = move_to(p, stack, 1, 37, true);
        else if (items[0].empty() && may_place(0, stack) && stack.count == 1) ok = move_to(p, stack, 0, 1, false);
        else if (i < 28) ok = move_to(p, stack, 28, 37, false);
        else ok = move_to(p, stack, 1, 28, false);
        if (!ok) return {};
        *r.s = stack;
        slot_changed(p, r);
        if (stack.count == clicked.count) return {};
        return clicked;
    }
    bool update_effects(int prim, int sec) {  // updateEffects
        if (items[0].empty()) return false;
        int pl = beacon_tier(prim), sl = beacon_tier(sec);
        if (sec >= 0 && b->levels < 4) return false;
        if (pl > b->levels || sl > b->levels) return false;
        if (pl >= 4 || !(sl == 0 || sl >= 4 || prim == sec)) return false;
        b->primary = beacon_tier(prim) ? prim : -1;  // filterEffect
        b->secondary = beacon_tier(sec) ? sec : -1;
        if (--items[0].count <= 0) items[0] = {};
        b->set_changed();
        be::flush(*b);  // blockEntityChanged: clients see the new effects
        return true;
    }
    int data_count() const override { return 3; }
    int data(int i) override { return i == 0 ? b->levels : (i == 1 ? b->primary : b->secondary) + 1; }
};

inline bool use_brewing_or_beacon(Player& p, int state, int x, int y, int z) {
    int type = be::block_entity_type(state);
    if (type == block_entity_types::brewing_stand) {
        auto b = std::dynamic_pointer_cast<BrewingBE>(be::get(x, y, z));
        if (!b) return true;
        auto m = std::make_shared<BrewingMenu>();
        m->b = b;
        menu_over(m, {b});
        open_slots_menu(p, m, "brewing_stand", title_of(*b, "container.brewing"));
        return true;
    }
    if (type == block_entity_types::beacon) {
        auto b = std::dynamic_pointer_cast<BeaconBE>(be::get(x, y, z));
        if (!b) return true;
        auto m = std::make_shared<BeaconMenu>();
        m->b = b, m->x = x, m->y = y, m->z = z, m->block = bs::R().block_of(state);
        m->type = menu_ids["beacon"];
        open_menu(p, m, title_of(*b, "container.beacon"));
        return true;
    }
    return false;
}

inline void register_brewing_beacon() {
    be::factories[block_entity_types::brewing_stand] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<BrewingBE>(); };
    be::factories[block_entity_types::beacon] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<BeaconBE>(); };
    for (int t : {block_entity_types::brewing_stand, block_entity_types::beacon}) be::ticking_types.push_back(t);
    be::empty_update_tag.push_back(block_entity_types::brewing_stand);
    on(play_sb::set_beacon, [](Player& p, Reader& in, int) {
        int prim = in.u8() ? in.varint() : -1;
        int sec = in.u8() ? in.varint() : -1;
        auto* m = dynamic_cast<BeaconMenu*>(p.g.menu.get());
        if (!m || !m->still_valid(p)) return;
        if (!m->update_effects(prim, sec)) {
            log_warn("Player %s tried to set invalid beacon effects", p.name.c_str());
            p.quit_reason = "multiplayer.disconnect.generic";
            kick(p);
        }
    });
}

}  // namespace cont
