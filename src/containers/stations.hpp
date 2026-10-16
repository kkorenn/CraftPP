#pragma once
// Workstation menus that own their slots (ContainerLevelAccess menus): stonecutter, smithing table,
// grindstone, anvil, loom, cartography table, enchanting table. Inputs go back to the player on close
// (clearContainer). The update_recipes packet (stonecutter list + recipe property sets) is sent on join.
#include "enchant.hpp"

namespace cont {

// Base: `items` are the container slots; `inputs` the ones returned on close; block must stay at (x, y, z).
struct StationMenu : Menu {
    int x = 0, y = 0, z = 0, block = -1;
    std::vector<ItemStack> items;
    int inputs = 0;  // slots [0, inputs) are cleared into the player on close
    int64_t last_sound = -1;
    int size() const override { return int(items.size()); }
    ItemStack& at(int i) override { return items[size_t(i)]; }
    bool still_valid(Player& p) override {  // stillValid(access, player, block): same block, interaction range + 4
        if (bs::R().block_of(world.get_block(x, y, z)) != block) return false;
        return in_range(p, x, y, z);
    }
    void removed(Player& p) override {  // clearContainer
        for (int i = 0; i < inputs; i++) {
            ItemStack& s = items[size_t(i)];
            if (s.empty()) continue;
            if (p.g.dead) player_drop(p, s, false);
            else if (inventory_add(p, s), s.count > 0) player_drop(p, s, false);
            s = {};
        }
    }
    // quickMoveStack skeleton shared by the input/result menus: result -> inventory (backwards), inputs ->
    // inventory, inventory -> the menu slot range `route` returns ({-1, -1}: inventory <-> hotbar instead).
    template <class Route>
    ItemStack quick(Player& p, int i, int result, Route route) {
        SlotRef r = menu_slot(p, i);
        if (r.s->empty()) return {};
        ItemStack stack = *r.s, clicked = stack;
        int inv0 = size(), hot0 = inv0 + 27, end = inv0 + 36;
        bool ok;
        if (i == result) ok = move_to(p, stack, inv0, end, true);
        else if (i < inv0) ok = move_to(p, stack, inv0, end, false);
        else {
            auto [a, b] = route(stack);
            if (a >= 0) ok = move_to(p, stack, a, b, false);
            else ok = i < hot0 ? move_to(p, stack, hot0, end, false) : move_to(p, stack, inv0, hot0, false);
        }
        if (!ok) return {};
        *r.s = stack;
        slot_changed(p, r);
        if (stack.count == clicked.count) return {};
        if (i < inv0) on_take(p, i, stack);
        return clicked;
    }
    void sound_once(const char* name) {  // lastSoundTime: once per tick
        if (last_sound == server_tick) return;
        last_sound = server_tick;
        play_sound(name, x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
    }
};
inline void level_event(int type, int x, int y, int z, int data) {  // ClientboundLevelEventPacket
    Writer w;
    w.varint(play_cb::level_event).i32(type).i64(int64_t(be::key(x, y, z))).i32(data).boolean(false);
    broadcast_near(x, z, w);
}

// ---------- stonecutter ----------
inline std::vector<int> stonecutter_options(int item) {  // SingleInputSet.selectByInput
    std::vector<int> v;
    for (int k = 0; k < int(stonecutting.size()); k++)
        if (in_sorted(stonecutting[size_t(k)].in, item)) v.push_back(k);
    return v;
}
struct StonecutterMenu : StationMenu {
    int sel = -1, input_item = -1;
    std::vector<int> options;
    StonecutterMenu() { items.resize(2), inputs = 1; }
    bool may_place(int i, const ItemStack&) override { return i == 0; }
    bool pick_all_ok(int i) override { return i != 1; }
    void setup_result() {
        bool ok = sel >= 0 && sel < int(options.size()) && !items[0].empty();
        items[1] = ok ? stonecutting[size_t(options[size_t(sel)])].result : ItemStack{};
    }
    void changed(Player&, int) override {  // slotsChanged
        int it = items[0].empty() ? -1 : items[0].item;
        if (it == input_item) return;
        input_item = it, sel = -1, items[1] = {};
        options = it < 0 ? std::vector<int>{} : stonecutter_options(it);
    }
    void on_take(Player& p, int i, const ItemStack&) override {
        if (i != 1) return;
        if (!items[0].empty() && --items[0].count <= 0) items[0] = {};
        if (!items[0].empty()) setup_result();
        else changed(p, 0);
        sound_once("ui.stonecutter.take_result");
    }
    bool button(Player&, int id) override {
        if (sel == id) return false;
        if (id >= 0 && id < int(options.size())) sel = id, setup_result();
        return true;
    }
    ItemStack quick_move(Player& p, int i) override {
        SlotRef r = menu_slot(p, i);
        if (r.s->empty()) return {};
        ItemStack stack = *r.s, clicked = stack;
        bool ok;
        if (i == 1) ok = move_to(p, stack, 2, 38, true);
        else if (i == 0) ok = move_to(p, stack, 2, 38, false);
        else if (!stonecutter_options(stack.item).empty()) ok = move_to(p, stack, 0, 1, false);
        else if (i < 29) ok = move_to(p, stack, 29, 38, false);
        else ok = move_to(p, stack, 2, 29, false);
        if (!ok) return {};
        *r.s = stack;
        slot_changed(p, r);
        if (stack.count == clicked.count) return {};
        if (i == 1) {
            on_take(p, 1, stack);
            if (!stack.empty()) player_drop(p, stack, false);
        }
        return clicked;
    }
    int data_count() const override { return 1; }
    int data(int) override { return sel; }
};

// ---------- smithing table (ItemCombinerMenu) ----------
inline bool smithing_slot_ok(int slot, int item) {  // SMITHING_TEMPLATE / BASE / ADDITION property sets
    for (auto& r : smithing)
        if (in_sorted(slot == 0 ? r.t : slot == 1 ? r.b : r.a, item)) return true;
    return false;
}
inline int trim_material_index(int item) {
    auto& m = extra(item).trim_material;
    for (size_t k = 0; k < trim_materials.size(); k++)
        if (trim_materials[k] == m) return int(k);
    return -1;
}
inline int trim_pattern_index(const std::string& p) {
    for (size_t k = 0; k < trim_patterns.size(); k++)
        if (trim_patterns[k] == p) return int(k);
    return -1;
}
struct SmithingMenu : StationMenu {
    SmithingMenu() { items.resize(4), inputs = 3; }
    bool may_place(int i, const ItemStack& s) override { return i < 3 && smithing_slot_ok(i, s.item); }
    bool pick_all_ok(int i) override { return i != 3; }
    bool may_pickup(Player&, int i) override { return i != 3 || !items[3].empty(); }
    void changed(Player&, int) override {  // createResult
        items[3] = {};
        const ItemStack &t = items[0], &b = items[1], &a = items[2];
        if (t.empty() || b.empty() || a.empty()) return;
        for (auto& r : smithing) {
            if (!in_sorted(r.t, t.item) || !in_sorted(r.b, b.item) || !in_sorted(r.a, a.item)) continue;
            if (!r.result.empty()) {  // SmithingTransformRecipe: base.transmuteCopy(result)
                ItemStack res = transmute(b, r.result.item);
                res.count = r.result.count;
                items[3] = res;
                return;
            }
            int mat = trim_material_index(a.item), pat = trim_pattern_index(r.pattern);  // SmithingTrimRecipe
            if (mat < 0 || pat < 0) return;
            Writer w;
            w.varint(mat + 1).varint(pat + 1);  // ArmorTrim: Holder references
            if (auto* cur = comp::get(b, comp::trim()); cur && *cur == w.b) return;
            ItemStack res = b.copy(1);
            comp::set(res, comp::trim(), std::move(w.b));
            items[3] = res;
            return;
        }
    }
    void on_take(Player& p, int i, const ItemStack&) override {
        if (i != 3) return;
        for (int k = 0; k < 3; k++)
            if (!items[size_t(k)].empty() && --items[size_t(k)].count <= 0) items[size_t(k)] = {};
        changed(p, 0);
        level_event(1044, x, y, z, 0);
    }
    ItemStack quick_move(Player& p, int i) override {
        return quick(p, i, 3, [&](const ItemStack& s) {
            bool can = false;  // canMoveIntoInputSlots
            for (int k = 0; k < 3; k++) can |= smithing_slot_ok(k, s.item) && items[size_t(k)].empty();
            return can ? std::pair{0, 3} : std::pair{-1, -1};
        });
    }
    int data_count() const override { return 1; }
    int data(int) override {  // hasRecipeError
        return !items[0].empty() && !items[1].empty() && !items[2].empty() && items[3].empty();
    }
};

// ---------- grindstone ----------
struct GrindstoneMenu : StationMenu {
    GrindstoneMenu() { items.resize(3), inputs = 2; }
    bool may_place(int i, const ItemStack& s) override { return i < 2 && (damageable(s) || has_any_enchants(s)); }
    static ItemStack remove_non_curses(ItemStack it) {
        bool stored = is_book_store(it);
        Enchs e = get_enchs(it, stored);
        std::erase_if(e, [](auto& x) { return !ench_tagged("curse", x.first); });
        set_enchs(it, e, stored);
        static const int ebook = item_id("enchanted_book"), book = item_id("book");
        if (it.item == ebook && e.empty()) it = transmute(it, book);
        int cost = 0;
        for (size_t k = 0; k < e.size(); k++) cost = increased_repair_cost(cost);
        set_repair_cost(it, cost);
        return it;
    }
    ItemStack compute(const ItemStack& in, const ItemStack& add) {
        if (in.empty() && add.empty()) return {};
        if (in.count > 1 || add.count > 1) return {};
        if (in.empty() || add.empty()) {
            const ItemStack& it = in.empty() ? add : in;
            return has_any_enchants(it) ? remove_non_curses(it) : ItemStack{};
        }
        if (in.item != add.item) return {};  // mergeItems
        int dur = std::max(max_damage(in), max_damage(add));
        int remaining = (max_damage(in) - in.damage) + (max_damage(add) - add.damage) + dur * 5 / 100;
        int count = 1;
        if (!damageable(in)) {
            if (max_stack(in) < 2 || !(in == add)) return {};
            count = 2;
        }
        ItemStack n = in.copy(count);
        if (damageable(n)) n.damage = std::max(dur - remaining, 0);  // ponytail: max_damage stays the prototype's
        bool stored = is_book_store(n);
        Enchs e = get_enchs(n, stored);
        for (auto& [id, lvl] : crafting_enchs(add))
            if (!ench_tagged("curse", id) || ench_level(e, id) == 0) ench_set_level(e, id, std::max(ench_level(e, id), lvl));
        set_enchs(n, e, stored);
        return remove_non_curses(n);
    }
    void changed(Player&, int i) override {
        if (i < 2) items[2] = compute(items[0], items[1]);
    }
    void on_take(Player& p, int i, const ItemStack&) override {
        if (i != 2) return;
        int amount = 0;
        for (int k = 0; k < 2; k++)
            for (auto& [id, lvl] : crafting_enchs(items[size_t(k)]))
                if (!ench_tagged("curse", id)) amount += enchants[size_t(id)].min_cost(lvl);
        if (amount > 0) {
            int half = int(std::ceil(amount / 2.0));
            give_experience(p, half + rand_int(half));  // ponytail: no ExperienceOrb entities: straight to the player
        }
        level_event(1042, x, y, z, 0);
        items[0] = items[1] = {};
    }
    ItemStack quick_move(Player& p, int i) override {
        bool full = !items[0].empty() && !items[1].empty();
        return quick(p, i, 2, [&](const ItemStack&) { return full ? std::pair{-1, -1} : std::pair{0, 2}; });
    }
};

// ---------- anvil ----------
struct AnvilMenu : StationMenu {
    int cost = 0, repair_count = 0;
    bool only_renaming = false;
    std::string name;
    bool has_name = false;
    Player* player = nullptr;
    AnvilMenu() { items.resize(3), inputs = 2; }
    bool may_place(int i, const ItemStack&) override { return i < 2; }
    bool pick_all_ok(int i) override { return i != 2; }
    bool may_pickup(Player& p, int i) override {
        return i != 2 || ((p.gamemode == 1 || p.g.xp_level >= cost) && cost > 0);
    }
    void create_result(Player& p) {
        ItemStack input = items[0];
        only_renaming = false;
        cost = 1;
        int price = 0, naming = 0;
        int64_t tax = 0;
        if (input.empty() || !can_store_enchants(input)) {
            items[2] = {}, cost = 0;
            return;
        }
        ItemStack result = input;
        ItemStack add = items[1];
        Enchs ench = crafting_enchs(result);
        tax += int64_t(repair_cost(input)) + repair_cost(add);
        repair_count = 0;
        bool infinite = p.gamemode == 1;
        if (!add.empty()) {
            bool book = comp::get(add, comp::stored_enchantments()) != nullptr;
            if (damageable(result) && in_sorted(extra(input.item).repair, add.item)) {
                int amount = std::min(result.damage, max_damage(result) / 4);
                if (amount <= 0) { items[2] = {}, cost = 0; return; }
                int count = 0;
                for (; amount > 0 && count < add.count; count++) {
                    result.damage -= amount;
                    price++;
                    amount = std::min(result.damage, max_damage(result) / 4);
                }
                repair_count = count;
            } else {
                if (!book && (result.item != add.item || !damageable(result))) { items[2] = {}, cost = 0; return; }
                if (damageable(result) && !book) {
                    int rem = (max_damage(input) - input.damage) + (max_damage(add) - add.damage) + max_damage(result) * 12 / 100;
                    int dmg = std::max(0, max_damage(result) - rem);
                    if (dmg < result.damage) result.damage = dmg, price += 2;
                }
                bool any_ok = false, any_bad = false;
                for (auto& [id, add_lvl] : crafting_enchs(add)) {
                    int cur = ench_level(ench, id);
                    int lvl = cur == add_lvl ? add_lvl + 1 : std::max(add_lvl, cur);
                    const Ench& e = enchants[size_t(id)];
                    bool ok = e.supports(input.item);
                    static const int ebook = item_id("enchanted_book");
                    if (infinite || input.item == ebook) ok = true;
                    for (auto& [other, _] : ench)
                        if (other != id && !compatible(id, other)) ok = false, price++;
                    if (!ok) {
                        any_bad = true;
                    } else {
                        any_ok = true;
                        lvl = std::min(lvl, e.max);
                        ench_set_level(ench, id, lvl);
                        int fee = e.anvil_cost;
                        if (book) fee = std::max(1, fee / 2);
                        price += fee * lvl;
                        if (input.count > 1) price = 40;
                    }
                }
                if (any_bad && !any_ok) { items[2] = {}, cost = 0; return; }
            }
        }
        std::string blank = name;
        blank.erase(0, blank.find_first_not_of(" \t\n\r"));
        if (has_name && !blank.empty()) {
            if (name != hover_name(input)) {
                naming = 1, price += 1;
                comp::set(result, comp::custom_name(), comp::text_literal(name));
            }
        } else if (comp::get(input, comp::custom_name())) {
            naming = 1, price += 1;
            comp::remove(result, comp::custom_name());
        }
        int final_price = price <= 0 ? 0 : int(std::clamp<int64_t>(tax + price, 0, INT32_MAX));
        cost = final_price;
        if (price <= 0) result = {};
        if (naming == price && naming > 0) {
            if (cost >= 40) cost = 39;
            only_renaming = true;
        }
        if (cost >= 40 && !infinite) result = {};
        if (!result.empty()) {
            int base = std::max(repair_cost(result), repair_cost(add));
            if (naming != price || naming == 0) base = increased_repair_cost(base);
            set_repair_cost(result, base);
            set_enchs(result, ench, is_book_store(result));
        }
        items[2] = result;
    }
    void changed(Player& p, int i) override {
        if (i < 2) create_result(p);
    }
    void on_take(Player& p, int i, const ItemStack&) override {
        if (i != 2) return;
        bool infinite = p.gamemode == 1;
        if (!infinite) give_levels(p, -cost);
        items[0] = {};
        if (repair_count > 0) {
            if (!items[1].empty() && (items[1].count -= repair_count) <= 0) items[1] = {};
        } else {
            items[1] = {};
        }
        cost = 0;
        int state = world.get_block(x, y, z);
        if (!infinite && rand_float() < 0.12f) {  // AnvilBlock.damage
            std::string n = bs::R().block(bs::R().block_of(state)).name;
            int next = n == "anvil" ? bs::R().block_index("chipped_anvil") : n == "chipped_anvil" ? bs::R().block_index("damaged_anvil") : -1;
            if (next < 0) {
                change_block(x, y, z, blocks::air);
                level_event(1029, x, y, z, 0);
                return;
            }
            int ns = with(bs::R().default_state(next), "facing", bs::R().value(state, "facing"));
            change_block(x, y, z, ns);
            block = next;
        }
        level_event(1030, x, y, z, 0);
    }
    void rename(Player& p, const std::string& raw) {  // setItemName
        std::string n;
        for (unsigned char c : raw)
            if (c >= 32 && c != 127 && c != 0xA7) n += char(c);  // StringUtil.filterText (ponytail: byte-wise)
        if (n.size() > 50) return;
        name = n, has_name = true;
        if (!items[2].empty()) {
            if (n.empty() || n.find_first_not_of(" \t") == std::string::npos) comp::remove(items[2], comp::custom_name());
            else comp::set(items[2], comp::custom_name(), comp::text_literal(n));
        }
        create_result(p);
    }
    ItemStack quick_move(Player& p, int i) override {
        return quick(p, i, 2, [&](const ItemStack&) { return std::pair{0, 2}; });
    }
    int data_count() const override { return 1; }
    int data(int) override { return cost; }
};

// ---------- loom ----------
inline int dye_of(int item) { return extra(item).dye; }
struct LoomMenu : StationMenu {
    int sel = -1;
    std::vector<int> patterns;
    LoomMenu() { items.resize(4), inputs = 3; }
    static bool is_banner(const ItemStack& s) { return tagged("banners", s.item); }
    static bool is_dye(const ItemStack& s) { return tagged("loom_dyes", s.item) && dye_of(s.item) >= 0; }
    static bool is_pattern(const ItemStack& s) { return tagged("loom_patterns", s.item) && !extra(s.item).banner_tag.empty(); }
    bool may_place(int i, const ItemStack& s) override {
        return i == 0 ? is_banner(s) : i == 1 ? is_dye(s) : i == 2 ? is_pattern(s) : false;
    }
    std::vector<int> selectable(const ItemStack& pat) {
        if (pat.empty()) return banner_no_item;
        auto it = banner_tags.find(extra(pat.item).banner_tag.substr(1));
        return it == banner_tags.end() ? std::vector<int>{} : it->second;
    }
    static int layer_count(const ItemStack& s) {
        auto* b = comp::get(s, comp::banner_patterns());
        if (!b) return 0;
        Reader r(*b);
        return r.varint();
    }
    void setup(int pattern) {
        const ItemStack &banner = items[0], &dye = items[1];
        if (banner.empty() || dye.empty()) return void(items[3] = {});
        ItemStack res = banner.copy(1);
        Writer w;
        auto* old = comp::get(banner, comp::banner_patterns());
        int n = old ? layer_count(banner) : 0;
        w.varint(n + 1);
        if (old) {
            Reader r(*old);
            r.varint();
            w.bytes(old->data() + r.i, old->size() - r.i);
        }
        w.varint(pattern + 1).varint(dye_of(dye.item));
        comp::set(res, comp::banner_patterns(), std::move(w.b));
        if (!(res == items[3])) items[3] = res;
    }
    void changed(Player&, int i) override {  // slotsChanged
        if (i == 3) return;
        const ItemStack &banner = items[0], &dye = items[1], &pat = items[2];
        if (banner.empty() || dye.empty()) {
            items[3] = {}, patterns.clear(), sel = -1;
            return;
        }
        bool valid = sel >= 0 && sel < int(patterns.size());
        auto prev = patterns;
        patterns = selectable(pat);
        int show = -1;
        if (patterns.size() == 1) sel = 0, show = patterns[0];
        else if (!valid) sel = -1;
        else {
            int v = prev[size_t(sel)];
            auto it = std::find(patterns.begin(), patterns.end(), v);
            sel = it == patterns.end() ? -1 : int(it - patterns.begin());
            show = sel >= 0 ? v : -1;
        }
        if (show >= 0) {
            if (layer_count(banner) >= 6) sel = -1, items[3] = {};
            else setup(show);
        } else {
            items[3] = {};
        }
    }
    bool button(Player&, int id) override {
        if (id < 0 || id >= int(patterns.size())) return false;
        sel = id;
        setup(patterns[size_t(id)]);
        return true;
    }
    void on_take(Player& p, int i, const ItemStack&) override {
        if (i != 3) return;
        for (int k = 0; k < 2; k++)
            if (!items[size_t(k)].empty() && --items[size_t(k)].count <= 0) items[size_t(k)] = {};
        if (items[0].empty() || items[1].empty()) sel = -1;
        changed(p, 0);
        sound_once("ui.loom.take_result");
    }
    ItemStack quick_move(Player& p, int i) override {
        return quick(p, i, 3, [&](const ItemStack& s) {
            int k = is_banner(s) ? 0 : is_dye(s) ? 1 : is_pattern(s) ? 2 : -1;
            return k < 0 ? std::pair{-1, -1} : std::pair{k, k + 1};
        });
    }
    int data_count() const override { return 1; }
    int data(int) override { return sel; }
};

// ---------- cartography table ----------
// ponytail: no map data exists yet (MapItemSavedData), so like vanilla with an unknown map the result stays
// empty; the slots and shift-click routing work.
struct CartographyMenu : StationMenu {
    CartographyMenu() { items.resize(3), inputs = 2; }
    bool may_place(int i, const ItemStack& s) override {
        static const int paper = item_id("paper"), map = item_id("map"), pane = item_id("glass_pane");
        return i == 0 ? comp::get(s, comp::map_id()) != nullptr : i == 1 ? (s.item == paper || s.item == map || s.item == pane) : false;
    }
    ItemStack quick_move(Player& p, int i) override {
        static const int paper = item_id("paper"), map = item_id("map"), pane = item_id("glass_pane");
        return quick(p, i, 2, [&](const ItemStack& s) {
            if (comp::get(s, comp::map_id())) return std::pair{0, 1};
            return s.item == paper || s.item == map || s.item == pane ? std::pair{1, 2} : std::pair{-1, -1};
        });
    }
};

// ---------- enchanting table ----------
struct EnchantingMenu : StationMenu {
    int costs[3] = {0, 0, 0}, clue[3] = {-1, -1, -1}, level_clue[3] = {-1, -1, -1};
    Player* owner = nullptr;
    LegacyRandomSource random{0};
    EnchantingMenu() { items.resize(2), inputs = 2; }
    bool may_place(int i, const ItemStack& s) override {
        static const int lapis = item_id("lapis_lazuli");
        return i == 1 ? s.item == lapis : true;
    }
    int slot_limit(int i, const ItemStack& s) override { return i == 0 ? 1 : StationMenu::slot_limit(i, s); }
    int bookshelves() const {
        int n = 0;
        static const auto& provider = bs::R().tag("minecraft:enchantment_power_provider");
        static const auto& transmitter = bs::R().tag("minecraft:enchantment_power_transmitter");
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = 0; dy <= 1; dy++)
                for (int dz = -2; dz <= 2; dz++)
                    if ((std::abs(dx) == 2 || std::abs(dz) == 2) && bs::R().in(world.get_block(x + dx, y + dy, z + dz), provider) &&
                        bs::R().in(world.get_block(x + dx / 2, y + dy, z + dz / 2), transmitter))
                        n++;
        return n;
    }
    std::vector<EnchInst> list_for(const ItemStack& s, int slot, int cost) {  // getEnchantmentList
        random.setSeed(int64_t(owner->g.enchant_seed) + slot);
        auto out = select_enchantments(random, s, cost, ench_tags["in_enchanting_table"]);
        static const int book = item_id("book");
        if (s.item == book && out.size() > 1) out.erase(out.begin() + random.nextInt(int(out.size())));
        return out;
    }
    void changed(Player&, int i) override {  // slotsChanged
        if (i > 1) return;
        const ItemStack& s = items[0];
        if (s.empty() || !enchantable(s)) {
            for (int k = 0; k < 3; k++) costs[k] = 0, clue[k] = level_clue[k] = -1;
            return;
        }
        int shelves = bookshelves();
        random.setSeed(owner->g.enchant_seed);
        for (int k = 0; k < 3; k++) {
            costs[k] = enchantment_cost(random, k, shelves, s);
            clue[k] = level_clue[k] = -1;
            if (costs[k] < k + 1) costs[k] = 0;
        }
        for (int k = 0; k < 3; k++)
            if (costs[k] > 0) {
                auto l = list_for(s, k, costs[k]);
                if (!l.empty()) {
                    auto& e = l[size_t(random.nextInt(int(l.size())))];
                    clue[k] = e.id, level_clue[k] = e.level;
                }
            }
    }
    bool button(Player& p, int id) override {  // clickMenuButton
        if (id < 0 || id > 2) return false;
        ItemStack& s = items[0];
        ItemStack& lapis = items[1];
        int need = id + 1;
        bool infinite = p.gamemode == 1;
        if ((lapis.empty() || lapis.count < need) && !infinite) return false;
        if (costs[id] <= 0 || s.empty() || ((p.g.xp_level < need || p.g.xp_level < costs[id]) && !infinite)) return false;
        auto list = list_for(s, id, costs[id]);
        if (list.empty()) return true;
        // Player.onEnchantmentPerformed
        p.g.xp_level -= need;
        if (p.g.xp_level < 0) p.g.xp_level = 0, p.g.xp_progress = 0, p.g.xp_total = 0;
        send_experience(p);
        p.g.enchant_seed = int32_t(game_rng());
        static const int book = item_id("book"), ebook = item_id("enchanted_book");
        bool to_book = s.item == book;
        if (to_book) s = transmute(s, ebook);
        Enchs e = get_enchs(s, to_book);
        for (auto& x : list) ench_set_level(e, x.id, std::max(ench_level(e, x.id), x.level));  // ItemStack.enchant (upgrade)
        set_enchs(s, e, to_book);
        if (!infinite && (lapis.count -= need) <= 0) lapis = {};
        changed(p, 0);
        play_sound("block.enchantment_table.use", x + 0.5, y + 0.5, z + 0.5, 1.0f, rand_float() * 0.1f + 0.9f);
        return true;
    }
    ItemStack quick_move(Player& p, int i) override {
        static const int lapis = item_id("lapis_lazuli");
        SlotRef r = menu_slot(p, i);
        if (r.s->empty()) return {};
        ItemStack stack = *r.s, clicked = stack;
        bool ok;
        if (i == 0 || i == 1) ok = move_to(p, stack, 2, 38, true);
        else if (stack.item == lapis) ok = move_to(p, stack, 1, 2, true);
        else {
            if (!items[0].empty() || !may_place(0, stack)) return {};
            items[0] = stack.copy(1);
            stack.count--;
            if (stack.count <= 0) stack = {};
            changed(p, 0);
            ok = true;
        }
        if (!ok) return {};
        *r.s = stack;
        slot_changed(p, r);
        if (stack.count == clicked.count) return {};
        return clicked;
    }
    int data_count() const override { return 10; }
    int data(int i) override {
        if (i < 3) return costs[i];
        if (i == 3) return owner->g.enchant_seed & -16;
        return i < 7 ? clue[i - 4] : level_clue[i - 7];
    }
};

// ---------- opening ----------
template <class M>
inline bool open_station(Player& p, int x, int y, int z, const char* menu, const char* title_key) {
    auto m = std::make_shared<M>();
    m->x = x, m->y = y, m->z = z, m->block = bs::R().block_of(world.get_block(x, y, z));
    m->type = menu_ids[menu];
    if constexpr (std::is_same_v<M, EnchantingMenu>) m->owner = &p;
    if constexpr (std::is_same_v<M, AnvilMenu>) m->player = &p;
    std::vector<uint8_t> title = comp::text_translate(title_key);
    if constexpr (std::is_same_v<M, EnchantingMenu>)
        if (auto b = be::get(x, y, z); b && !b->custom_name.empty()) title = b->custom_name;
    open_menu(p, m, title);
    return true;
}
inline bool use_station(Player& p, int state, int x, int y, int z) {
    const bs::Block& b = bs::R().block(bs::R().block_of(state));
    if (b.name == "stonecutter") return open_station<StonecutterMenu>(p, x, y, z, "stonecutter", "container.stonecutter");
    if (b.name == "smithing_table") return open_station<SmithingMenu>(p, x, y, z, "smithing", "container.upgrade");
    if (b.name == "grindstone") return open_station<GrindstoneMenu>(p, x, y, z, "grindstone", "container.grindstone_title");
    if (b.is_a("AnvilBlock")) return open_station<AnvilMenu>(p, x, y, z, "anvil", "container.repair");
    if (b.name == "loom") return open_station<LoomMenu>(p, x, y, z, "loom", "container.loom");
    if (b.name == "cartography_table") return open_station<CartographyMenu>(p, x, y, z, "cartography_table", "container.cartography_table");
    if (b.name == "enchanting_table") return open_station<EnchantingMenu>(p, x, y, z, "enchantment", "container.enchant");
    return false;
}

// ClientboundUpdateRecipesPacket: recipe property sets + the stonecutter list (client shows the options).
inline Writer update_recipes_packet() {
    Writer w;
    w.varint(play_cb::update_recipes);
    auto set = [&](const char* key, const std::vector<int>& v) {
        w.str(key).varint(int(v.size()));
        for (int i : v) w.varint(i);
    };
    auto collect = [](auto f) {
        std::vector<int> v;
        for (int i = 0; i < int(item_info.size()); i++)
            if (f(i)) v.push_back(i);
        return v;
    };
    auto cook = [&](int k) {
        return collect([&](int i) {
            for (auto& r : cooking[size_t(k)])
                if (in_sorted(r.in, i)) return true;
            return false;
        });
    };
    w.varint(7);
    set("minecraft:furnace_input", cook(0));
    set("minecraft:blast_furnace_input", cook(1));
    set("minecraft:smoker_input", cook(2));
    set("minecraft:campfire_input", cook(3));
    set("minecraft:smithing_template", collect([](int i) { return smithing_slot_ok(0, i); }));
    set("minecraft:smithing_base", collect([](int i) { return smithing_slot_ok(1, i); }));
    set("minecraft:smithing_addition", collect([](int i) { return smithing_slot_ok(2, i); }));
    w.varint(int(stonecutting.size()));
    int item_stack_display = slot_display_ids["item_stack"];
    for (auto& r : stonecutting) {
        w.varint(int(r.in.size()) + 1);
        for (int i : r.in) w.varint(i);
        w.varint(item_stack_display).varint(r.result.item).varint(r.result.count).varint(0).varint(0);
    }
    return w;
}

}  // namespace cont
