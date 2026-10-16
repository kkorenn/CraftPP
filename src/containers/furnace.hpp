#pragma once
// Furnace, smoker, blast furnace (AbstractFurnaceBlockEntity + AbstractFurnaceMenu). 26.3 fuel is data driven
// (cooking_fuel component -> context providers): smoker/blast furnace ("fast_cooking") burn fuel twice as fast
// and cook at speed multiplier 2.
#include "chest.hpp"

namespace cont {

// Player.giveExperiencePoints.
// ponytail: furnace XP goes straight to the player; ExperienceOrb entities don't exist yet.
inline int xp_needed(int level) { return level >= 30 ? 112 + (level - 30) * 9 : level >= 15 ? 37 + (level - 15) * 5 : 7 + level * 2; }
inline void give_experience(Player& p, int amount) {
    auto& g = p.g;
    g.xp_progress += float(amount) / float(xp_needed(g.xp_level));
    g.xp_total = std::clamp(g.xp_total + amount, 0, INT32_MAX);
    while (g.xp_progress < 0) {
        float rem = g.xp_progress * float(xp_needed(g.xp_level));
        if (g.xp_level > 0) {
            g.xp_level--;
            g.xp_progress = 1.0f + rem / float(xp_needed(g.xp_level));
        } else {
            g.xp_level = 0, g.xp_progress = 0;
        }
    }
    while (g.xp_progress >= 1.0f) {
        g.xp_progress = (g.xp_progress - 1.0f) * float(xp_needed(g.xp_level));
        g.xp_level++;
        g.xp_progress /= float(xp_needed(g.xp_level));
    }
    send_experience(p);
}
inline void give_levels(Player& p, int levels) {  // Player.giveExperienceLevels
    p.g.xp_level = std::max(0, p.g.xp_level + levels);
    if (levels < 0) p.g.xp_total = std::max(0, p.g.xp_total);
    send_experience(p);
}

inline int furnace_kind(int type) {  // index into cooking[]
    return type == block_entity_types::blast_furnace ? 1 : type == block_entity_types::smoker ? 2 : 0;
}
inline const Cooking* find_cooking(int kind, const ItemStack& in) {
    if (in.empty()) return nullptr;
    for (auto& r : cooking[size_t(kind)])
        if (in_sorted(r.in, in.item)) return &r;
    return nullptr;
}
inline bool is_fuel(const ItemStack& s) { return !s.empty() && fuels.count(s.item); }

struct FurnaceBE : be::Live {
    int lit = 0, lit_total = 0, cook = 0, cook_total = 0;
    float speed = 1.0f;
    std::map<std::string, int> used;  // RecipesUsed
    int prev_item = -2;               // input slot item last seen (setItem change detection)
    ComponentsPtr prev_comp;

    FurnaceBE() { items.resize(3); }
    bool worldly() const override { return true; }
    std::vector<int> face_slots(int d) const override { return d == bs::DOWN ? std::vector<int>{2, 1} : d == bs::UP ? std::vector<int>{0} : std::vector<int>{1}; }
    bool can_place(int slot, const ItemStack& s) const override {
        static const int bucket = item_id("bucket");
        if (slot == 2) return false;
        if (slot != 1) return true;
        return is_fuel(s) || (s.item == bucket && items[1].item != bucket);
    }
    bool can_take_face(int slot, const ItemStack& s, int d) const override {
        return d == bs::DOWN && slot == 1 ? tagged("furnace_fuel_bottom_takeable", s.item) : true;
    }
    void item_set(int slot) override { if (slot == 0) input_maybe_changed(); }
    bool fast() const { return type != block_entity_types::furnace; }
    bool ticks() const override { return true; }
    void removed_side_effects() override { be::drop_contents(*this); }
    void load(const Nbt& n) override {
        cook = int(comp::nbt_int(n, "cooking_time_spent"));
        cook_total = int(comp::nbt_int(n, "cooking_total_time"));
        lit = int(comp::nbt_int(n, "lit_time_remaining"));
        lit_total = int(comp::nbt_int(n, "lit_total_time"));
        const Nbt* sp = n.get("speed_multiplier");
        speed = sp && sp->is_num() ? float(sp->as_double()) : 1.0f;
        if (const Nbt* r = n.get("RecipesUsed"); r && r->t == Nbt::Compound)
            for (auto& [k, v] : r->comp) used[k] = int(v.i);
        prev_item = items[0].empty() ? -1 : items[0].item, prev_comp = items[0].comp;
    }
    void save(Nbt& n) const override {
        n.set("cooking_time_spent", Nbt::num(Nbt::Int, cook));
        n.set("cooking_total_time", Nbt::num(Nbt::Int, cook_total));
        n.set("lit_time_remaining", Nbt::num(Nbt::Int, lit));
        n.set("lit_total_time", Nbt::num(Nbt::Int, lit_total));
        n.set("speed_multiplier", Nbt::real(Nbt::Float, speed));
        Nbt r = Nbt::compound();
        for (auto& [k, v] : used) r.set(k, Nbt::num(Nbt::Int, v));
        n.set("RecipesUsed", r);
    }
    int total_cook_time(const Cooking* r) const {  // getTotalCookTime
        if (!r) return 200;
        return speed > 0 ? int(std::ceil(float(r->time) / speed)) : r->time;
    }
    // AbstractFurnaceBlockEntity.setItem(0, stack) with a different item: restart cooking.
    void input_maybe_changed() {
        const ItemStack& in = items[0];
        bool same = !in.empty() && in.item == prev_item && ItemStack::same_comp(in.comp, prev_comp);
        prev_item = in.empty() ? -1 : in.item, prev_comp = in.comp;
        if (same) return;
        cook_total = total_cook_time(find_cooking(furnace_kind(type), in));
        cook = 0;
        set_changed();
    }
    bool can_burn(const ItemStack& r) const {
        const ItemStack& out = items[2];
        if (out.empty()) return true;
        if (!out.same(r)) return false;
        return out.count + r.count <= std::min(max_stack(), ::max_stack(r));
    }
    void tick() override {  // serverTick
        bool changed = false, was_lit = lit > 0, is_lit = false;
        if (lit > 0) is_lit = --lit > 0;
        ItemStack& fuel = items[1];
        ItemStack& in = items[0];
        bool has_in = !in.empty(), has_fuel = !fuel.empty();
        if (is_lit || (has_fuel && has_in)) {
            if (has_in) {
                const Cooking* r = find_cooking(furnace_kind(type), in);
                if (r) {
                    if (can_burn(r->result)) {
                        if (!is_lit) {
                            auto f = fuels.find(fuel.item);
                            int burn = f == fuels.end() ? 0 : f->second.burn[fast()];
                            float sp = f == fuels.end() ? 1.0f : f->second.speed[fast()];
                            lit = lit_total = burn;
                            speed = sp;
                            if (cook_total > 0 && cook < cook_total) {
                                float ratio = float(cook) / float(cook_total);
                                cook_total = total_cook_time(r);
                                cook = int(std::ceil(ratio * float(cook_total)));
                            }
                            if (burn > 0) {  // consumeFuel
                                int rem = item_info[size_t(fuel.item)].craft_remainder;
                                if (--fuel.count <= 0) fuel = rem >= 0 ? ItemStack{rem, 1} : ItemStack{};
                                else if (rem >= 0) be::drop_stack(x, y, z, {rem, 1});
                                is_lit = changed = true;
                            }
                        }
                        if (is_lit) {
                            if (++cook >= cook_total) {
                                cook = 0;
                                cook_total = total_cook_time(r);
                                if (items[2].empty()) items[2] = r->result;  // burn
                                else items[2].count += r->result.count;
                                static const int wet_sponge = item_id("wet_sponge"), bucket = item_id("bucket"),
                                                 water_bucket = item_id("water_bucket");
                                if (in.item == wet_sponge && !fuel.empty() && fuel.item == bucket) fuel = {water_bucket, 1};
                                if (--in.count <= 0) in = {};
                                used[r->id]++;
                                changed = true;
                            }
                        } else {
                            cook = 0;
                        }
                    } else {
                        cook = 0;
                    }
                }
            } else {
                cook = 0;
            }
        } else if (cook > 0) {
            cook = std::clamp(cook - 2, 0, cook_total);
        }
        if (was_lit != is_lit) {
            changed = true;
            set_state(*this, with(state, "lit", is_lit ? "true" : "false"));
        }
        if (changed || lit > 0 || cook > 0) set_changed();
    }
    // awardUsedRecipesAndPopExperience
    void award(Player& p) {
        int total = 0;
        for (auto& [id, n] : used)
            for (auto& r : cooking[size_t(furnace_kind(type))])
                if (r.id == id) {
                    float v = float(n) * r.xp;
                    int xp = int(std::floor(v));
                    float frac = v - std::floor(v);
                    if (frac != 0 && rand_float() < frac) xp++;
                    total += xp;
                }
        used.clear();
        set_changed();
        if (total > 0) give_experience(p, total);
    }
};

struct FurnaceMenu : SlotsMenu {
    std::shared_ptr<FurnaceBE> f;
    int kind = 0;
    bool may_place(int i, const ItemStack& s) override {
        static const int bucket = item_id("bucket");
        if (i == 2) return false;
        if (i == 1) return is_fuel(s) || s.item == bucket;  // FurnaceFuelSlot
        return true;
    }
    int slot_limit(int i, const ItemStack& s) override {
        static const int bucket = item_id("bucket");
        return i == 1 && s.item == bucket ? 1 : SlotsMenu::slot_limit(i, s);
    }
    void on_take(Player& p, int i, const ItemStack&) override {
        if (i == 2) f->award(p);  // FurnaceResultSlot.checkTakeAchievements
    }
    void changed(Player& p, int i) override {
        SlotsMenu::changed(p, i);
        if (i == 0) f->input_maybe_changed();
    }
    ItemStack quick_move(Player& p, int i) override {
        SlotRef r = menu_slot(p, i);
        if (r.s->empty()) return {};
        ItemStack stack = *r.s, clicked = stack;
        bool ok;
        if (i == 2) ok = move_to(p, stack, 3, 39, true);
        else if (i != 1 && i != 0) {
            if (find_cooking(kind, stack)) ok = move_to(p, stack, 0, 1, false);
            else if (is_fuel(stack)) ok = move_to(p, stack, 1, 2, false);
            else if (i < 30) ok = move_to(p, stack, 30, 39, false);
            else ok = move_to(p, stack, 3, 30, false);
        } else {
            ok = move_to(p, stack, 3, 39, false);
        }
        if (!ok) return {};
        *r.s = stack;
        slot_changed(p, r);
        if (stack.count == clicked.count) return {};
        if (i < 3) on_take(p, i, stack);
        return clicked;
    }
    int data_count() const override { return 4; }
    int data(int i) override { return i == 0 ? f->lit : i == 1 ? f->lit_total : i == 2 ? f->cook : f->cook_total; }
};

inline bool use_furnace(Player& p, int state, int x, int y, int z) {
    int type = be::block_entity_type(state);
    if (type != block_entity_types::furnace && type != block_entity_types::smoker && type != block_entity_types::blast_furnace)
        return false;
    auto b = std::dynamic_pointer_cast<FurnaceBE>(be::get(x, y, z));
    if (!b) return true;
    auto m = std::make_shared<FurnaceMenu>();
    m->f = b, m->kind = furnace_kind(type);
    menu_over(m, {b});
    const char* menu = type == block_entity_types::furnace ? "furnace" : type == block_entity_types::smoker ? "smoker" : "blast_furnace";
    const char* key = type == block_entity_types::furnace ? "container.furnace" : type == block_entity_types::smoker ? "container.smoker" : "container.blast_furnace";
    open_slots_menu(p, m, menu, title_of(*b, key));
    return true;
}

inline void register_furnaces() {
    for (int t : {block_entity_types::furnace, block_entity_types::smoker, block_entity_types::blast_furnace}) {
        be::factories[t] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<FurnaceBE>(); };
        be::ticking_types.push_back(t);
        be::empty_update_tag.push_back(t);
    }
}

}  // namespace cont
