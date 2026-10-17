// Recipe book: recipe registry with displays (RecipeManager.unpackRecipeInfo), ServerRecipeBook (known +
// highlighted recipes, recipe_book_add/remove/settings), ServerPlaceRecipe for the 2x2 and crafting table grids
// (StackedContents matching), ResultSlot awards and /recipe. Overview: progress.hpp.
#include "progress.hpp"

#include <filesystem>

namespace progress {

int find_recipe(const std::string& id) {
    auto it = recipe_by_id.find(ns(id));
    return it == recipe_by_id.end() ? -1 : it->second;
}

// ---------- network encodings ----------
enum SlotDisplayType { SD_EMPTY, SD_ANY_FUEL, SD_WITH_ANY_POTION, SD_ONLY_WITH_COMPONENT, SD_ITEM, SD_ITEM_STACK, SD_TAG,
                       SD_DYED, SD_SMITHING_TRIM, SD_WITH_REMAINDER, SD_COMPOSITE };
enum RecipeDisplayType { RD_SHAPELESS, RD_SHAPED, RD_FURNACE, RD_STONECUTTER, RD_SMITHING };
enum Category { C_BUILDING, C_REDSTONE, C_EQUIPMENT, C_MISC, C_FURNACE_FOOD, C_FURNACE_BLOCKS, C_FURNACE_MISC,
                C_BLAST_BLOCKS, C_BLAST_MISC, C_SMOKER_FOOD, C_STONECUTTER, C_SMITHING, C_CAMPFIRE };

static void holder_set_bytes(Writer& w, const Ingredient& i) {  // ByteBufCodecs.holderSet
    if (!i.tag.empty()) { w.varint(0).str(i.tag); return; }
    w.varint(int(i.items->size()) + 1);
    for (uint16_t id : *i.items) w.varint(id);
}
static Ingredient ingredient(const Json& j) {
    Ingredient i;
    i.items = holder_set("item", j, &i.tag);
    return i;
}
static void sd_ingredient(Writer& w, const Ingredient& i) { w.varint(SD_TAG); holder_set_bytes(w, i); }  // Ingredient.display
static void sd_optional(Writer& w, const Ingredient* i) {
    if (i) sd_ingredient(w, *i);
    else w.varint(SD_EMPTY);
}
static void sd_item(Writer& w, const char* id) { w.varint(SD_ITEM).varint(std::max(0, reg_id("item", id))); }
static void sd_stack(Writer& w, int item, int count) { w.varint(SD_ITEM_STACK).varint(std::max(0, item)).varint(count).varint(0).varint(0); }
static void sd_result(Writer& w, const Json& r) {  // ItemStackSlotDisplay(ItemStackTemplate); component patches not encoded
    int item = r.find("id") ? reg_id("item", r["id"].asString()) : 0;
    sd_stack(w, item, r.getInt("count", 1));
}

static int crafting_category(const Json& j) {
    std::string c = j.find("category") ? j["category"].asString() : "misc";
    return c == "building" ? C_BUILDING : c == "redstone" ? C_REDSTONE : c == "equipment" ? C_EQUIPMENT : C_MISC;
}

void load_recipes() {
    std::vector<std::string> ids;
    std::string base = VANILLA + "recipe";
    for (auto& e : std::filesystem::recursive_directory_iterator(base))
        if (e.path().extension() == ".json") {
            std::string rel = std::filesystem::relative(e.path(), base).generic_string();
            ids.push_back("minecraft:" + rel.substr(0, rel.size() - 5));
        }
    std::sort(ids.begin(), ids.end());
    recipe_defs.resize(ids.size());
    for (size_t i = 0; i < ids.size(); i++) recipe_by_id[ids[i]] = int(i);
    std::map<std::string, int> groups;  // RecipeManager.unpackRecipeInfo group ids
    for (size_t ri = 0; ri < ids.size(); ri++) {
        RecipeDef& r = recipe_defs[ri];
        r.id = ids[ri];
        Json j = Json::parseFile(base + "/" + ids[ri].substr(10) + ".json");
        std::string type = j["type"].asString();
        type = type.substr(type.find(':') + 1);
        r.special = type.rfind("crafting_special_", 0) == 0 || type == "crafting_decorated_pot" || type == "brewing";
        if (r.special) continue;
        if (auto* n = j.find("show_notification")) r.notify = n->b;
        std::string group = j.find("group") ? j["group"].asString() : "";
        if (!group.empty()) r.group = groups.emplace(group, int(groups.size())).first->second;
        std::vector<Writer> displays;
        auto shapeless = [&](const std::vector<const Ingredient*>& ins, const std::function<void(Writer&)>& result) {
            Writer w;
            w.varint(RD_SHAPELESS).varint(int(ins.size()));
            for (auto* i : ins) sd_ingredient(w, *i);
            result(w);
            sd_item(w, "crafting_table");
            displays.push_back(std::move(w));
        };
        if (type == "crafting_shaped") {
            r.shaped = true;
            auto& pat = j["pattern"];
            r.h = int(pat.size()), r.w = 0;
            for (auto& row : pat.arr) r.w = std::max(r.w, int(row.str.size()));
            std::map<char, Ingredient> key;
            for (auto& [k, v] : j["key"].obj) key[k[0]] = ingredient(v);
            Writer w;
            w.varint(RD_SHAPED).varint(r.w).varint(r.h).varint(r.w * r.h);
            for (auto& row : pat.arr)
                for (int x = 0; x < r.w; x++) {
                    char ch = x < int(row.str.size()) ? row.str[size_t(x)] : ' ';
                    if (ch == ' ') { w.varint(SD_EMPTY); r.slots.push_back(-1); continue; }
                    sd_ingredient(w, key[ch]);
                    r.slots.push_back(int(r.place.size()));
                    r.place.push_back(key[ch]);
                }
            sd_result(w, j["result"]);
            sd_item(w, "crafting_table");
            displays.push_back(std::move(w));
            r.category = crafting_category(j);
        } else if (type == "crafting_shapeless") {
            for (auto& x : j["ingredients"].arr) r.place.push_back(ingredient(x));
            std::vector<const Ingredient*> ins;
            for (auto& i : r.place) ins.push_back(&i);
            shapeless(ins, [&](Writer& w) { sd_result(w, j["result"]); });
            r.category = crafting_category(j);
        } else if (type == "crafting_transmute") {
            Ingredient input = ingredient(j["input"]), material = ingredient(j["material"]);
            IRange mc = j.find("material_count") ? irange(j.find("material_count")) : IRange{1, 1};
            int lo = mc.lo == INT64_MIN ? 1 : int(mc.lo), hi = mc.hi == INT64_MAX ? 8 : int(mc.hi);
            bool add = j.find("add_material_count_to_result") && j["add_material_count_to_result"].b;
            int count = j["result"].getInt("count", 1);
            int item = j["result"].find("id") ? reg_id("item", j["result"]["id"].asString())
                                              : (input.items && !input.items->empty() ? int((*input.items)[0]) : 0);
            r.place.push_back(input);
            for (int k = 0; k < hi; k++) r.place.push_back(material);
            std::vector<const Ingredient*> ins{&r.place[0]};
            for (int m = lo; m <= hi; m++) {
                ins.push_back(&r.place[1]);
                shapeless(ins, [&](Writer& w) { sd_stack(w, item, add ? m + count : count); });
            }
            r.category = crafting_category(j);
        } else if (type == "crafting_dye") {
            r.place = {ingredient(j["target"]), ingredient(j["dye"])};
            Writer dye;  // OnlyWithComponent(dye.display(), DYE)
            dye.varint(SD_ONLY_WITH_COMPONENT);
            sd_ingredient(dye, r.place[1]);
            dye.varint(component_ids["dye"]);
            Writer w;
            w.varint(RD_SHAPELESS).varint(2);
            sd_ingredient(w, r.place[0]);
            w.bytes(dye.b);
            w.varint(SD_DYED).bytes(dye.b);
            sd_ingredient(w, r.place[0]);
            sd_item(w, "crafting_table");
            displays.push_back(std::move(w));
            r.category = crafting_category(j);
        } else if (type == "crafting_imbue") {
            Ingredient material = ingredient(j["material"]), source = ingredient(j["source"]);
            for (int k = 0; k < 9; k++) r.place.push_back(k == 4 ? source : material);
            for (int k = 0; k < 9; k++) r.slots.push_back(k);
            r.shaped = true;
            Writer w;
            w.varint(RD_SHAPED).varint(3).varint(3).varint(9);
            for (int k = 0; k < 9; k++) {
                if (k == 4) w.varint(SD_WITH_ANY_POTION);
                sd_ingredient(w, r.place[size_t(k)]);
            }
            w.varint(SD_WITH_ANY_POTION);
            sd_result(w, j["result"]);
            sd_item(w, "crafting_table");
            displays.push_back(std::move(w));
            r.category = crafting_category(j);
        } else if (type == "smelting" || type == "blasting" || type == "smoking" || type == "campfire_cooking") {
            r.place.push_back(ingredient(j["ingredient"]));
            std::string c = j.find("category") ? j["category"].asString() : "misc";
            const char* icon = "furnace";
            if (type == "smelting") r.category = c == "blocks" ? C_FURNACE_BLOCKS : c == "food" ? C_FURNACE_FOOD : C_FURNACE_MISC;
            else if (type == "blasting") r.category = c == "blocks" ? C_BLAST_BLOCKS : C_BLAST_MISC, icon = "blast_furnace";
            else if (type == "smoking") r.category = C_SMOKER_FOOD, icon = "smoker";
            else r.category = C_CAMPFIRE, icon = "campfire";
            Writer w;
            w.varint(RD_FURNACE);
            sd_ingredient(w, r.place[0]);
            w.varint(SD_ANY_FUEL);
            sd_result(w, j["result"]);
            sd_item(w, icon);
            w.varint(j.getInt("cookingtime", type == "smelting" ? 200 : 100)).f32(j.find("experience") ? j["experience"].asFloat() : 0.0f);
            displays.push_back(std::move(w));
        } else if (type == "stonecutting") {
            r.place.push_back(ingredient(j["ingredient"]));
            r.category = C_STONECUTTER;
            Writer w;
            w.varint(RD_STONECUTTER);
            sd_ingredient(w, r.place[0]);
            sd_result(w, j["result"]);
            sd_item(w, "stonecutter");
            displays.push_back(std::move(w));
        } else if (type == "smithing_transform" || type == "smithing_trim") {
            r.category = C_SMITHING;
            std::optional<Ingredient> tmpl, base_, add;
            if (auto* x = j.find("template")) tmpl = ingredient(*x);
            base_ = ingredient(j["base"]);
            if (auto* x = j.find("addition")) add = ingredient(*x);
            for (auto* o : {&tmpl, &base_, &add}) {
                if (*o) r.slots.push_back(int(r.place.size())), r.place.push_back(**o);
                else r.slots.push_back(-1);
            }
            Writer w;
            w.varint(RD_SMITHING);
            sd_optional(w, tmpl ? &*tmpl : nullptr);
            sd_ingredient(w, *base_);
            sd_optional(w, add ? &*add : nullptr);
            if (type == "smithing_trim") {
                w.varint(SD_SMITHING_TRIM);
                sd_ingredient(w, *base_);
                sd_optional(w, add ? &*add : nullptr);
                w.varint(reg_id("trim_pattern", j["pattern"].asString()) + 1);  // Holder by registry id
            } else {
                sd_result(w, j["result"]);
            }
            sd_item(w, "smithing_table");
            displays.push_back(std::move(w));
        } else {
            r.special = true;  // unknown type: not learnable
            log_warn("recipe book: unsupported recipe type %s (%s)", type.c_str(), r.id.c_str());
            continue;
        }
        if (r.slots.empty()) for (size_t k = 0; k < r.place.size(); k++) r.slots.push_back(int(k));
        for (auto& d : displays) {
            DisplayDef def;
            def.recipe = int(ri);
            def.display = std::move(d.b);
            Writer e;  // RecipeDisplayEntry
            e.varint(int(recipe_displays.size())).bytes(def.display);
            e.boolean(r.group >= 0);
            if (r.group >= 0) e.varint(r.group);
            e.varint(r.category);
            e.boolean(true).varint(int(r.place.size()));  // craftingRequirements = placementInfo.ingredients
            for (auto& i : r.place) holder_set_bytes(e, i);
            def.entry = std::move(e.b);
            r.displays.push_back(int(recipe_displays.size()));
            recipe_displays.push_back(std::move(def));
        }
    }
    recipe_of_craft.assign(::recipes.size(), -1);
    for (size_t k = 0; k < ::recipes.size(); k++)
        if ((recipe_of_craft[k] = find_recipe(::recipes[k].id)) >= 0) recipe_defs[size_t(recipe_of_craft[k])].craft = int(k);
}

// ---------- ServerRecipeBook ----------
int add_recipes(Player& p, const std::vector<int>& rs) {
    State& s = st(p);
    Writer w;
    int n = 0;
    std::vector<int> unlocked;
    for (int r : rs) {
        if (r < 0 || s.known[size_t(r)] || recipe_defs[size_t(r)].special) continue;
        s.known[size_t(r)] = s.highlight[size_t(r)] = 1;
        for (int d : recipe_defs[size_t(r)].displays) {
            w.bytes(recipe_displays[size_t(d)].entry).u8(uint8_t((recipe_defs[size_t(r)].notify ? 1 : 0) | 2));
            n++;
        }
        unlocked.push_back(r);
    }
    for (int r : unlocked) trigger(p, T_RECIPE_UNLOCKED, [&](const Criterion& c) {
        return std::find(c.recipes.begin(), c.recipes.end(), r) != c.recipes.end();
    });
    if (n) {
        Writer pk;
        pk.varint(play_cb::recipe_book_add).varint(n).bytes(w.b).boolean(false);
        p.send(pk);
    }
    return n;
}
int remove_recipes(Player& p, const std::vector<int>& rs) {
    State& s = st(p);
    std::vector<int> ids;
    for (int r : rs) {
        if (r < 0 || !s.known[size_t(r)]) continue;
        s.known[size_t(r)] = s.highlight[size_t(r)] = 0;
        for (int d : recipe_defs[size_t(r)].displays) ids.push_back(d);
    }
    if (!ids.empty()) {
        Writer w;
        w.varint(play_cb::recipe_book_remove).varint(int(ids.size()));
        for (int d : ids) w.varint(d);
        p.send(w);
    }
    return int(ids.size());
}
static void send_settings(Player& p) {
    State& s = st(p);
    Writer w;
    w.varint(play_cb::recipe_book_settings);
    for (auto& b : s.book) w.boolean(b[0]).boolean(b[1]);
    p.send(w);
}
void send_recipe_book(Player& p) {  // ServerRecipeBook.sendInitialRecipeBook
    State& s = st(p);
    send_settings(p);
    Writer w;
    int n = 0;
    for (size_t r = 0; r < recipe_defs.size(); r++) {
        if (!s.known[r]) continue;
        for (int d : recipe_defs[r].displays) {
            w.bytes(recipe_displays[size_t(d)].entry).u8(s.highlight[r] ? 2 : 0);
            n++;
        }
    }
    Writer pk;
    pk.varint(play_cb::recipe_book_add).varint(n).bytes(w.b).boolean(true);
    p.send(pk);
}

// ---------- crafting results (ResultContainer.setRecipeUsed / ResultSlot.checkTakeAchievements) ----------
bool may_craft(Player& p, int recipe) {
    if (!rule("limited_crafting") || recipe < 0) return true;
    int r = size_t(recipe) < recipe_of_craft.size() ? recipe_of_craft[size_t(recipe)] : -1;
    return r >= 0 && st(p).known[size_t(r)];
}
void crafted(Player& p, int recipe, const ItemStack& result, const ItemStack* grid, int n) {
    award_stat(p, CRAFTED, result.item, result.count);  // ItemStack.onCraftedBy
    int r = recipe >= 0 && size_t(recipe) < recipe_of_craft.size() ? recipe_of_craft[size_t(recipe)] : -1;
    if (r < 0) return;
    std::vector<ItemStack> used;
    for (int i = 0; i < n; i++) if (!grid[i].empty()) used.push_back(grid[i]);
    trigger(p, T_RECIPE_CRAFTED, [&](const Criterion& c) {
        if (std::find(c.recipes.begin(), c.recipes.end(), r) == c.recipes.end()) return false;
        std::vector<ItemStack> left = used;
        for (auto& ip : c.items) {
            auto it = std::find_if(left.begin(), left.end(), [&](const ItemStack& s) { return ip.test(s); });
            if (it == left.end()) return false;
            left.erase(it);
        }
        return true;
    });
    add_recipes(p, {r});  // awardRecipes(List.of(recipe))
}

// ---------- ServerPlaceRecipe ----------
// StackedContents<Holder<Item>>: available item counts and the RecipePicker augmenting-path assignment.
struct Stacked {
    std::map<int, int> amounts;  // ponytail: ordered by item id; vanilla iterates a Reference2Int hash map
    void account(const ItemStack& s, int max_count) {
        if (!s.empty()) amounts[s.item] += std::min(max_count, s.count);
    }
    bool try_pick(const std::vector<Ingredient>& ings, int cap, std::vector<int>* out);
    int try_pick_all(const std::vector<Ingredient>& ings, int max_size, std::vector<int>* out) {
        int upper = INT32_MAX;  // getResultUpperBound
        for (auto& ing : ings) {
            int mx = 0;
            for (auto& [it, n] : amounts) if (n > mx && ing.test(it)) mx = n;
            upper = std::min(upper, mx);
            if (mx == 0) break;
        }
        int lo = 0, hi = int(std::min<int64_t>(max_size, upper)) + 1;
        while (true) {
            int mid = (lo + hi) / 2;
            if (try_pick(ings, mid, nullptr)) {
                if (hi - lo <= 1) {
                    if (mid > 0) try_pick(ings, mid, out);
                    return mid;
                }
                lo = mid;
            } else {
                hi = mid;
            }
        }
    }
};
bool Stacked::try_pick(const std::vector<Ingredient>& ings, int cap, std::vector<int>* out) {
    if (cap <= 0) return true;
    int ni = int(ings.size());
    std::vector<int> items;
    for (auto& [it, n] : amounts) {
        if (n <= 0) continue;
        for (auto& ing : ings) if (ing.test(it)) { items.push_back(it); break; }
    }
    int nt = int(items.size());
    std::vector<uint8_t> conn(size_t(ni * nt), 0), assigned(size_t(ni * nt), 0), sat(size_t(ni), 0), vis_ing(size_t(ni), 0), vis_item(size_t(nt), 0);
    for (int g = 0; g < ni; g++)
        for (int t = 0; t < nt; t++) conn[size_t(t * ni + g)] = ings[size_t(g)].test(items[size_t(t)]);
    std::vector<int> path;
    auto find_path = [&](int start) -> bool {  // findNewItemAssignmentPath
        path.clear();
        vis_item[size_t(start)] = 1;
        path.push_back(start);
        while (!path.empty()) {
            size_t len = path.size();
            if ((len - 1) % 2 == 0) {  // last is an item
                int t = path.back();
                for (int g = 0; g < ni; g++)
                    if (!vis_ing[size_t(g)] && conn[size_t(t * ni + g)] && !assigned[size_t(t * ni + g)]) {
                        vis_ing[size_t(g)] = 1;
                        path.push_back(g);
                        break;
                    }
            } else {
                int g = path.back();
                if (!sat[size_t(g)]) return true;
                for (int t = 0; t < nt; t++)
                    if (!vis_item[size_t(t)] && assigned[size_t(t * ni + g)]) {
                        vis_item[size_t(t)] = 1;
                        path.push_back(t);
                        break;
                    }
            }
            if (path.size() == len) path.pop_back();
        }
        return false;
    };
    int satisfied = 0;
    while (true) {
        std::fill(vis_ing.begin(), vis_ing.end(), 0);
        std::fill(vis_item.begin(), vis_item.end(), 0);
        bool found = false;
        for (int t = 0; t < nt && !found; t++)
            if (amounts[items[size_t(t)]] >= cap) found = find_path(t);
        if (!found) {
            bool valid = satisfied == ni;
            std::fill(sat.begin(), sat.end(), 0);
            for (int g = 0; g < ni; g++)
                for (int t = 0; t < nt; t++)
                    if (assigned[size_t(t * ni + g)]) {
                        assigned[size_t(t * ni + g)] = 0;
                        amounts[items[size_t(t)]] += cap;
                        if (valid && out) out->push_back(items[size_t(t)]);
                        break;
                    }
            return valid;
        }
        amounts[items[size_t(path[0])]] -= cap;
        sat[size_t(path.back())] = 1;
        satisfied++;
        for (size_t i = 0; i + 1 < path.size(); i++) {
            if (i % 2 == 0) assigned[size_t(path[i] * ni + path[i + 1])] = 1;
            else assigned[size_t(path[i + 1] * ni + path[i])] = 0;
        }
    }
}

static bool usable_for_crafting(const ItemStack& s) { return s.damage == 0 && !s.comp; }  // ponytail: any component counts as enchanted/named

// Returns true when the ghost recipe should be shown (PostPlaceAction.PLACE_GHOST_RECIPE).
static bool place_recipe(Player& p, const RecipeDef& r, bool use_max, bool creative) {
    int gw = grid_width(p);
    ItemStack* grid = ::grid(p);
    int ng = gw * gw;
    if (!creative) {  // testClearGrid
        int free = 0;
        for (int i = 0; i < 36; i++) free += p.inv[size_t(i)].empty();
        std::vector<ItemStack> free_slots;
        for (int i = 0; i < ng; i++) {
            ItemStack s = grid[i];
            if (s.empty()) continue;
            bool room = false;
            auto has_room = [&](int k) { auto& t = p.inv[size_t(k)]; return !t.empty() && t.same(s) && max_stack(t) > 1 && t.count < max_stack(t); };
            room = has_room(p.selected) || has_room(40);
            for (int k = 0; k < 36 && !room; k++) room = has_room(k);
            if (room) continue;
            if (int(free_slots.size()) > free) return false;
            for (auto& f : free_slots)
                if (f.item == s.item && f.count != max_stack(f) && f.count + s.count <= max_stack(f)) { f.count += s.count; s.count = 0; break; }
            if (s.count > 0) {
                if (int(free_slots.size()) >= free) return false;
                free_slots.push_back(s);
            }
        }
    }
    Stacked avail;
    for (int i = 0; i < 36; i++) if (usable_for_crafting(p.inv[size_t(i)])) avail.account(p.inv[size_t(i)], max_stack(p.inv[size_t(i)]));
    for (int i = 0; i < ng; i++) avail.account(grid[i], max_stack(grid[i]));
    auto clear_grid = [&] {
        for (int i = 0; i < ng; i++) {
            ItemStack s = grid[i];
            while (!s.empty()) {  // Inventory.placeItemBackInInventory
                int slot = -1;
                auto room = [&](int k) { auto& t = p.inv[size_t(k)]; return !t.empty() && t.same(s) && t.count < max_stack(t); };
                if (room(p.selected)) slot = p.selected;
                else if (room(40)) slot = 40;
                for (int k = 0; k < 36 && slot < 0; k++) if (room(k)) slot = k;
                for (int k = 0; k < 36 && slot < 0; k++) if (p.inv[size_t(k)].empty()) slot = k;
                if (slot < 0) { player_drop(p, s, false); break; }
                ItemStack& t = p.inv[size_t(slot)];
                int n = std::min(s.count, max_stack(s) - (t.empty() ? 0 : t.count));
                if (t.empty()) t = s.copy(n);
                else t.count += n;
                s.count -= n;
            }
            grid[i] = {};
        }
        p.g.result = {};
    };
    Stacked probe = avail;
    if (!probe.try_pick(r.place, 1, nullptr)) {
        clear_grid();
        update_result(p);
        return true;
    }
    // placeRecipe
    int craft = r.craft >= 0 ? r.craft : -1;
    bool matches_placed = craft >= 0 && recipe_matches(::recipes[size_t(craft)], craft_input(grid, gw));
    Stacked big = avail;
    int biggest = big.try_pick_all(r.place, INT32_MAX, nullptr);
    if (matches_placed)
        for (int i = 0; i < ng; i++)
            if (!grid[i].empty() && std::min(biggest, max_stack(grid[i])) < grid[i].count + 1) return false;
    int amount;
    if (use_max) amount = biggest;
    else if (matches_placed) {
        int smallest = INT32_MAX;
        for (int i = 0; i < ng; i++) if (!grid[i].empty() && smallest > grid[i].count) smallest = grid[i].count;
        amount = smallest == INT32_MAX ? smallest : smallest + 1;
    } else amount = 1;
    std::vector<int> used;
    Stacked pick = avail;
    if (!pick.try_pick(r.place, amount, &used)) return false;
    int adjusted = amount;
    for (int it : used) adjusted = std::min(adjusted, max_stack(it));
    if (adjusted != amount) {
        used.clear();
        Stacked again = avail;
        if (!again.try_pick(r.place, adjusted, &used)) return false;
    }
    clear_grid();
    // PlaceRecipeHelper.placeRecipe over the grid, centring small shaped recipes.
    int rw = r.shaped ? r.w : gw, rh = r.shaped ? r.h : gw;
    size_t next = 0;
    int gi = 0;
    for (int gy = 0; gy < gw; gy++) {
        if (rh < gw / 2.0f && mth::floor(gw / 2.0f - rh / 2.0f) > gy) gi += gw, gy++;
        for (int gx = 0; gx < gw; gx++) {
            if (next >= r.slots.size()) goto done;
            bool center = rw < gw / 2.0f;
            int start = mth::floor(gw / 2.0f - rw / 2.0f), total = rw;
            bool add = gx < rw;
            if (center) total = start + rw, add = start <= gx && gx < start + rw;
            if (add) {
                int ing = r.slots[next++];
                if (ing != -1 && size_t(ing) < used.size()) {
                    int item = used[size_t(ing)];
                    int remaining = adjusted;
                    while (remaining > 0) {  // moveItemToGrid
                        ItemStack& target = grid[gi];
                        int slot = -1;
                        for (int k = 0; k < 36 && slot < 0; k++) {
                            const ItemStack& s = p.inv[size_t(k)];
                            if (!s.empty() && s.item == item && usable_for_crafting(s) && (target.empty() || target.same(s))) slot = k;
                        }
                        if (slot < 0) break;
                        ItemStack& src = p.inv[size_t(slot)];
                        int n = std::min(remaining, src.count);
                        if (target.empty()) target = src.copy(n);
                        else target.count += n;
                        if ((src.count -= n) <= 0) src = {};
                        remaining -= n;
                    }
                }
            } else if (total == gx) {
                gi += gw - gx;
                break;
            }
            gi++;
        }
    }
done:
    update_result(p);
    return false;
}

void recipe_book_init() {
    on(play_sb::recipe_book_change_settings, [](Player& p, Reader& in, int) {
        int type = in.varint();
        bool open = in.u8(), filtering = in.u8();
        if (type >= 0 && type < 4) st(p).book[type][0] = open, st(p).book[type][1] = filtering;
    });
    on(play_sb::recipe_book_seen_recipe, [](Player& p, Reader& in, int) {
        int d = in.varint();
        if (d >= 0 && size_t(d) < recipe_displays.size()) st(p).highlight[size_t(recipe_displays[size_t(d)].recipe)] = 0;
    });
    // ServerGamePacketListenerImpl.handlePlaceRecipe: crafting table and the inventory 2x2 (RecipeBookMenu).
    on(play_sb::place_recipe, [](Player& p, Reader& in, int) {
        int container = in.varint(), d = in.varint();
        bool use_max = in.u8();
        if (p.gamemode == 3 || container != p.g.container || p.g.menu) return;
        if (d < 0 || size_t(d) >= recipe_displays.size()) return;
        const DisplayDef& dd = recipe_displays[size_t(d)];
        const RecipeDef& r = recipe_defs[size_t(dd.recipe)];
        if (!st(p).known[size_t(dd.recipe)] || r.craft < 0 || r.place.empty()) return;
        if (!table_open(p) && (r.shaped ? r.w > 2 || r.h > 2 : r.place.size() > 4)) return;  // doesn't fit the 2x2
        if (place_recipe(p, r, use_max, p.gamemode == 1))
            p.send(Writer().varint(play_cb::place_ghost_recipe).varint(container).bytes(dd.display));
    });
    reg("recipe", [](Cmd& c) {
        bool give = c.is("give");
        auto targets = c.players("targets");
        std::vector<int> list;
        if (c.is("*")) {
            for (size_t r = 0; r < recipe_defs.size(); r++) if (!recipe_defs[r].special) list.push_back(int(r));
        } else {
            int r = find_recipe(c.str("recipe"));
            if (r < 0 || recipe_defs[size_t(r)].special) throw err("argument.resource.not_found", {c.str("recipe"), "minecraft:recipe"});
            list.push_back(r);
        }
        int total = 0, nonzero = 0;
        PlayerPtr only;
        for (auto& t : targets) {
            int v = give ? add_recipes(*t, list) : remove_recipes(*t, list);
            total += v;
            if (v) nonzero++, only = nonzero == 1 ? t : nullptr;
        }
        std::string key = give ? "give" : "take";
        if (nonzero == 0) throw err("commands.recipe." + key + ".failed");
        c.success(only ? tr("commands.recipe." + key + ".success.single", {total, player_name(*only)})
                       : tr("commands.recipe." + key + ".success.multiple", {total, nonzero}),
                  true);
        return total;
    });
}
}  // namespace progress
