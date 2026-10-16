#pragma once
// Player inventory API, menus (InventoryMenu, CraftingMenu), container_click, crafting recipes.
//
// API for other modules (tick thread only):
//   give_item(p, item, count[, damage])  -> bool   add to inventory (Inventory.add); leftovers dropped at the player
//   inventory_add(p, stack)               -> int    add as much as fits, `stack` keeps the rest
//   clear_inventory(p, item=-1, max=-1)   -> int    remove matching items (all if item < 0), returns count removed
//   count_item(p, item)                   -> int
//   set_inv(p, slot, stack)                         set an Inventory slot (0-8 hotbar, 9-35, 36-39 armor, 40 offhand)
// Clients are updated automatically at the end of the tick (sync_menu diff), so callers never send slot packets.
#include "entity.hpp"

// ---------- recipes ----------
struct Recipe {
    int w = 0, h = 0;                       // shaped size; 0 = shapeless
    std::vector<std::vector<uint16_t>> in;  // shaped: w*h cells (empty = must be empty); shapeless: ingredients
    bool symmetric = false;
    ItemStack result;
};
inline std::vector<Recipe> recipes;

inline void load_recipes(const Json& rs) {
    auto ids = [](const Json& a) {
        std::vector<uint16_t> v;
        for (auto& x : a.arr) v.push_back(uint16_t(x.asInt()));
        return v;
    };
    for (auto& r : rs.arr) {
        Recipe rc;
        rc.result = {r["r"][0].asInt(), r["r"][1].asInt(), 0};
        if (auto* k = r.find("k")) {
            rc.w = r["w"].asInt(), rc.h = r["h"].asInt();
            for (auto& c : k->arr) rc.in.push_back(ids(c));
            rc.symmetric = true;
            for (int y = 0; y < rc.h; y++)
                for (int x = 0; x < rc.w / 2; x++)
                    if (rc.in[size_t(y * rc.w + x)] != rc.in[size_t(y * rc.w + rc.w - 1 - x)]) rc.symmetric = false;
        } else {
            for (auto& c : r["i"].arr) rc.in.push_back(ids(c));
        }
        recipes.push_back(std::move(rc));
    }
}

// CraftingInput.ofPositioned: trimmed bounding box of the non-empty grid cells.
struct CraftInput {
    int w = 0, h = 0, left = 0, top = 0, n = 0;
    std::array<int, 9> items{};  // item ids (-1 empty), row-major w*h
};
inline CraftInput craft_input(const ItemStack* grid, int gw) {
    CraftInput c;
    int x0 = gw, y0 = gw, x1 = -1, y1 = -1;
    for (int y = 0; y < gw; y++)
        for (int x = 0; x < gw; x++)
            if (!grid[y * gw + x].empty()) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    if (x1 < 0) return c;
    c.left = x0, c.top = y0, c.w = x1 - x0 + 1, c.h = y1 - y0 + 1;
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++) {
            const ItemStack& s = grid[(y + y0) * gw + x + x0];
            c.items[size_t(y * c.w + x)] = s.empty() ? -1 : s.item;
            c.n += !s.empty();
        }
    return c;
}

inline bool recipe_matches(const Recipe& r, const CraftInput& c) {
    if (r.w == 0) {  // shapeless: each ingredient used by exactly one item (small bipartite match)
        if (int(r.in.size()) != c.n) return false;
        int items[9], k = 0;
        for (int i = 0; i < c.w * c.h; i++) if (c.items[size_t(i)] >= 0) items[k++] = c.items[size_t(i)];
        uint32_t used = 0;
        auto rec = [&](auto& self, int i) -> bool {
            if (i == k) return true;
            for (size_t j = 0; j < r.in.size(); j++)
                if (!(used >> j & 1) && in_sorted(r.in[j], items[i])) {
                    used |= 1u << j;
                    if (self(self, i + 1)) return true;
                    used &= ~(1u << j);
                }
            return false;
        };
        return rec(rec, 0);
    }
    if (r.w != c.w || r.h != c.h) return false;
    for (int mirror = 0; mirror < (r.symmetric ? 1 : 2); mirror++) {
        bool ok = true;
        for (int y = 0; y < r.h && ok; y++)
            for (int x = 0; x < r.w && ok; x++) {
                const auto& ing = r.in[size_t(y * r.w + (mirror ? r.w - 1 - x : x))];
                int it = c.items[size_t(y * c.w + x)];
                ok = ing.empty() ? it < 0 : it >= 0 && in_sorted(ing, it);
            }
        if (ok) return true;
    }
    return false;
}
inline const Recipe* find_recipe(const ItemStack* grid, int gw) {
    CraftInput c = craft_input(grid, gw);
    if (c.n == 0) return nullptr;
    for (auto& r : recipes)
        if (recipe_matches(r, c)) return &r;
    return nullptr;
}

// ---------- inventory ----------
inline void set_inv(Player& p, int slot, const ItemStack& s) { p.inv[size_t(slot)] = s.empty() ? ItemStack{} : s; }

// Inventory.add: fills matching stacks (selected, offhand, 0..35), then empty main slots.
inline int inventory_add(Player& p, ItemStack& s) {
    if (s.empty()) return 0;
    int start = s.count;
    if (p.gamemode == 1) {  // creative: Inventory.add swallows the whole stack
        bool have = false;
        for (int i = 0; i < 36 && !have; i++) have = p.inv[size_t(i)].same(s);
        if (!have)
            for (int i = 0; i < 36; i++)
                if (p.inv[size_t(i)].empty()) { p.inv[size_t(i)] = {s.item, s.count, s.damage}; break; }
        s.count = 0;
        return start;
    }
    auto room = [&](int i) {
        auto& t = p.inv[size_t(i)];
        return !t.empty() && t.same(s) && t.count < max_stack(t);
    };
    while (s.count > 0) {
        int slot = -1;
        if (room(p.selected)) slot = p.selected;
        else if (room(40)) slot = 40;
        else
            for (int i = 0; i < 36 && slot < 0; i++) if (room(i)) slot = i;
        if (slot < 0)
            for (int i = 0; i < 36 && slot < 0; i++) if (p.inv[size_t(i)].empty()) slot = i;
        if (slot < 0) break;
        auto& t = p.inv[size_t(slot)];
        if (t.empty()) t = {s.item, 0, s.damage};
        int n = std::min(s.count, max_stack(s) - t.count);
        t.count += n;
        s.count -= n;
    }
    return start - s.count;
}

inline bool give_item(Player& p, int item, int count, int damage) {
    if (item < 0 || item >= int(item_info.size()) || count <= 0) return false;
    bool all = true;
    while (count > 0) {
        ItemStack s{item, std::min(count, max_stack(item)), damage};
        count -= s.count;
        inventory_add(p, s);
        if (s.count > 0) { all = false; player_drop(p, s, false); }
    }
    return all;
}
inline bool give_item(Player& p, int item, int count) { return give_item(p, item, count, 0); }

inline int count_item(const Player& p, int item) {
    int n = 0;
    for (auto& s : p.inv) if (!s.empty() && s.item == item) n += s.count;
    return n;
}
inline int clear_inventory(Player& p, int item = -1, int max = -1) {
    int removed = 0;
    auto take = [&](ItemStack& s) {
        if (s.empty() || (item >= 0 && s.item != item)) return;
        int n = max < 0 ? s.count : std::min(s.count, max - removed);
        s.count -= n;
        removed += n;
        if (s.count <= 0) s = {};
    };
    for (auto& s : p.inv) take(s);
    take(p.g.carried);
    for (auto& s : p.g.craft2) take(s);
    return removed;
}

// ---------- menus ----------
// Slot layouts: InventoryMenu: 0 result, 1-4 grid, 5-8 armor (head..feet = inv 39..36), 9-35 main, 36-44 hotbar,
// 45 offhand. CraftingMenu: 0 result, 1-9 grid, 10-36 main, 37-45 hotbar.
enum SlotKind : uint8_t { S_RESULT, S_GRID, S_INV, S_ARMOR };
struct SlotRef {
    ItemStack* s = nullptr;
    SlotKind kind = S_INV;
    int inv = -1;  // inventory index for S_INV/S_ARMOR
};
inline int menu_size(const Player& p) { return p.g.container ? 46 : 46; }
inline SlotRef menu_slot(Player& p, int i) {
    auto inv = [&](int k, SlotKind kind = S_INV) { return SlotRef{&p.inv[size_t(k)], kind, k}; };
    if (i == 0) return {&p.g.result, S_RESULT};
    if (p.g.container == 0) {
        if (i <= 4) return {&p.g.craft2[size_t(i - 1)], S_GRID};
        if (i <= 8) return inv(39 - (i - 5), S_ARMOR);
        if (i <= 35) return inv(i);
        if (i <= 44) return inv(i - 36);
        return inv(40);
    }
    if (i <= 9) return {&p.g.craft3[size_t(i - 1)], S_GRID};
    if (i <= 36) return inv(i - 1);
    return inv(i - 37);
}
inline int grid_width(const Player& p) { return p.g.container ? 3 : 2; }
inline ItemStack* grid(Player& p) { return p.g.container ? p.g.craft3.data() : p.g.craft2.data(); }

inline bool may_place(const SlotRef& r, const ItemStack& s) {
    if (r.kind == S_RESULT) return false;
    if (r.kind == S_ARMOR) return s.item >= 0 && item_info[size_t(s.item)].equip == r.inv;
    return true;
}
inline int slot_max(const SlotRef& r, const ItemStack& s) {
    return r.kind == S_ARMOR ? 1 : std::min(99, max_stack(s));
}

inline void update_result(Player& p) {
    const Recipe* r = find_recipe(grid(p), grid_width(p));
    p.g.result = r ? r->result : ItemStack{};
}

// ResultSlot.onTake: consume one of each grid item, leaving crafting remainders (buckets, bottles).
inline void take_result(Player& p) {
    ItemStack* g = grid(p);
    int n = grid_width(p) * grid_width(p);
    for (int i = 0; i < n; i++) {
        if (g[i].empty()) continue;
        int rem = item_info[size_t(g[i].item)].craft_remainder;
        if (--g[i].count <= 0) g[i] = {};
        if (rem < 0) continue;
        ItemStack r{rem, 1, 0};
        if (g[i].empty()) g[i] = r;
        else if (g[i].same(r)) g[i].count++;
        else if (inventory_add(p, r), r.count > 0) player_drop(p, r, false);
    }
    update_result(p);
}

// Slot.safeInsert: moves up to `amount` of `in` into the slot.
inline void safe_insert(const SlotRef& r, ItemStack& in, int amount) {
    if (in.empty() || !may_place(r, in)) return;
    ItemStack& t = *r.s;
    int n = std::min({amount, in.count, slot_max(r, in) - (t.empty() ? 0 : t.count)});
    if (n <= 0) return;
    if (t.empty()) t = {in.item, n, in.damage};
    else if (t.same(in)) t.count += n;
    else return;
    in.count -= n;
    if (in.count <= 0) in = {};
}

// Slot.tryRemove + onTake. Result slot: always the whole stack, only if it all fits (allowModification false).
inline ItemStack try_remove(Player& p, const SlotRef& r, int amount, int max_amount) {
    ItemStack& t = *r.s;
    if (t.empty()) return {};
    if (r.kind == S_RESULT) {
        if (max_amount < t.count) return {};
        ItemStack got = t;
        take_result(p);
        return got;
    }
    int n = std::min({amount, max_amount, t.count});
    ItemStack got{t.item, n, t.damage};
    if ((t.count -= n) <= 0) t = {};
    return got;
}

// AbstractContainerMenu.moveItemStackTo over menu slots [a, b).
inline bool move_to(Player& p, ItemStack& s, int a, int b, bool backwards) {
    bool changed = false;
    if (max_stack(s) > 1)
        for (int i = backwards ? b - 1 : a; !s.empty() && (backwards ? i >= a : i < b); i += backwards ? -1 : 1) {
            SlotRef r = menu_slot(p, i);
            ItemStack& t = *r.s;
            if (t.empty() || !t.same(s)) continue;
            int mx = slot_max(r, t), total = t.count + s.count;
            if (total <= mx) t.count = total, s = {}, changed = true;
            else if (t.count < mx) s.count -= mx - t.count, t.count = mx, changed = true;
        }
    if (!s.empty())
        for (int i = backwards ? b - 1 : a; backwards ? i >= a : i < b; i += backwards ? -1 : 1) {
            SlotRef r = menu_slot(p, i);
            if (r.s->empty() && may_place(r, s)) {
                int n = std::min(s.count, slot_max(r, s));
                *r.s = {s.item, n, s.damage};
                if ((s.count -= n) <= 0) s = {};
                changed = true;
                break;
            }
        }
    return changed;
}

// InventoryMenu / CraftingMenu.quickMoveStack. Returns the stack before moving (empty = nothing moved).
inline ItemStack quick_move(Player& p, int i) {
    SlotRef r = menu_slot(p, i);
    if (r.s->empty()) return {};
    ItemStack stack = *r.s, clicked = stack;
    bool inv_menu = p.g.container == 0;
    int eq = item_info[size_t(stack.item)].equip;
    bool ok;
    if (inv_menu) {
        if (i == 0) ok = move_to(p, stack, 9, 45, true);
        else if (i < 9) ok = move_to(p, stack, 9, 45, false);
        else if (eq >= 36 && eq <= 39 && menu_slot(p, 8 - (eq - 36)).s->empty())
            ok = move_to(p, stack, 8 - (eq - 36), 9 - (eq - 36), false);
        else if (eq == 40 && p.inv[40].empty()) ok = move_to(p, stack, 45, 46, false);
        else if (i < 36) ok = move_to(p, stack, 36, 45, false);
        else if (i < 45) ok = move_to(p, stack, 9, 36, false);
        else ok = move_to(p, stack, 9, 45, false);
    } else {
        if (i == 0) ok = move_to(p, stack, 10, 46, true);
        else if (i >= 10) {
            ok = move_to(p, stack, 1, 10, false);
            if (!ok) ok = i < 37 ? move_to(p, stack, 37, 46, false) : move_to(p, stack, 10, 37, false);
        } else ok = move_to(p, stack, 10, 46, false);
    }
    if (!ok) return {};
    if (stack.count == clicked.count) return {};
    if (r.kind == S_RESULT) {
        take_result(p);  // consumes ingredients; whatever didn't fit is dropped
        if (!stack.empty()) player_drop(p, stack, false);
    } else {
        *r.s = stack;
    }
    return clicked;
}

// Container click (AbstractContainerMenu.doClick). Click types: 0 pickup, 1 quick move, 2 swap, 3 clone,
// 4 throw, 5 quick craft, 6 pickup all.
inline void do_click(Player& p, int slot, int button, int type) {
    auto& g = p.g;
    ItemStack& carried = g.carried;
    int n = menu_size(p);
    bool valid = slot >= 0 && slot < n;
    if (type == 5) {
        int expected = g.qc_status;
        g.qc_status = button & 3;
        auto reset = [&] { g.qc_status = 0; g.qc_slots = 0; };
        if ((expected != 1 || g.qc_status != 2) && expected != g.qc_status) reset();
        else if (carried.empty()) reset();
        else if (g.qc_status == 0) {
            g.qc_type = button >> 2 & 3;
            if (g.qc_type == 0 || g.qc_type == 1 || (g.qc_type == 2 && p.gamemode == 1)) g.qc_status = 1, g.qc_slots = 0;
            else reset();
        } else if (g.qc_status == 1) {
            if (!valid) return;
            SlotRef r = menu_slot(p, slot);
            bool fits = r.s->empty() || (r.s->same(carried) && r.s->count <= max_stack(carried));
            if (fits && may_place(r, carried) && (g.qc_type == 2 || carried.count > std::popcount(g.qc_slots)))
                g.qc_slots |= 1ull << slot;
        } else if (g.qc_status == 2) {
            int cnt = std::popcount(g.qc_slots);
            if (cnt == 1) {
                int s = std::countr_zero(g.qc_slots);
                int t = g.qc_type;
                reset();
                do_click(p, s, t, 0);
                return;
            }
            if (cnt > 0) {
                ItemStack source = carried;
                int remaining = carried.count;
                for (int s = 0; s < n; s++) {
                    if (!(g.qc_slots >> s & 1)) continue;
                    SlotRef r = menu_slot(p, s);
                    bool fits = r.s->empty() || (r.s->same(carried) && r.s->count <= max_stack(carried));
                    if (!fits || !may_place(r, carried) || (g.qc_type != 2 && carried.count < cnt)) continue;
                    int carry = r.s->empty() ? 0 : r.s->count;
                    int mx = std::min(max_stack(source), slot_max(r, source));
                    int place = g.qc_type == 0 ? source.count / cnt : g.qc_type == 1 ? 1 : max_stack(source);
                    int nc = std::min(place + carry, mx);
                    remaining -= nc - carry;
                    *r.s = {source.item, nc, source.damage};
                }
                source.count = remaining;
                carried = source.count > 0 ? source : ItemStack{};
            }
            reset();
        } else {
            reset();
        }
        return;
    }
    if (g.qc_status != 0) { g.qc_status = 0; g.qc_slots = 0; return; }
    if ((type == 0 || type == 1) && (button == 0 || button == 1)) {
        bool primary = button == 0;
        if (slot == -999) {
            if (carried.empty()) return;
            if (primary) { player_drop(p, carried, true); carried = {}; }
            else { player_drop(p, {carried.item, 1, carried.damage}, true); if (--carried.count <= 0) carried = {}; }
            return;
        }
        if (!valid) return;
        if (type == 1) {
            ItemStack c = quick_move(p, slot);
            while (!c.empty() && menu_slot(p, slot).s->item == c.item) c = quick_move(p, slot);
            return;
        }
        SlotRef r = menu_slot(p, slot);
        ItemStack& clicked = *r.s;
        if (clicked.empty()) {
            if (!carried.empty()) safe_insert(r, carried, primary ? carried.count : 1);
        } else if (carried.empty()) {
            carried = try_remove(p, r, primary ? clicked.count : (clicked.count + 1) / 2, INT32_MAX);
        } else if (may_place(r, carried)) {
            if (clicked.same(carried)) safe_insert(r, carried, primary ? carried.count : 1);
            else if (carried.count <= slot_max(r, carried)) std::swap(clicked, carried);
        } else if (clicked.same(carried)) {
            ItemStack got = try_remove(p, r, clicked.count, max_stack(carried) - carried.count);
            carried.count += got.count;
        }
        if (r.kind == S_GRID) update_result(p);
        return;
    }
    if (type == 2 && valid && ((button >= 0 && button < 9) || button == 40)) {
        ItemStack& src = p.inv[size_t(button)];
        SlotRef t = menu_slot(p, slot);
        if (src.empty() && t.s->empty()) return;
        if (src.empty()) {
            src = try_remove(p, t, t.s->count, INT32_MAX);
        } else if (t.s->empty()) {
            if (may_place(t, src)) {
                int mx = slot_max(t, src);
                if (src.count > mx) *t.s = {src.item, mx, src.damage}, src.count -= mx;
                else *t.s = src, src = {};
            }
        } else if (t.kind != S_RESULT && may_place(t, src)) {
            int mx = slot_max(t, src);
            if (src.count > mx) {
                ItemStack old = *t.s;
                *t.s = {src.item, mx, src.damage};
                src.count -= mx;
                if (inventory_add(p, old), old.count > 0) player_drop(p, old, true);
            } else {
                std::swap(src, *t.s);
            }
        }
        if (t.kind == S_GRID) update_result(p);
        return;
    }
    if (type == 3 && valid && p.gamemode == 1 && carried.empty()) {
        ItemStack s = *menu_slot(p, slot).s;
        if (!s.empty()) carried = {s.item, max_stack(s), s.damage};
        return;
    }
    if (type == 4 && valid && carried.empty() && p.gamemode != 3) {
        SlotRef r = menu_slot(p, slot);
        for (bool first = true; first || button == 1; first = false) {
            if (r.s->empty()) break;
            int item = r.s->item;
            ItemStack s = try_remove(p, r, button == 0 ? 1 : r.s->count, INT32_MAX);
            if (s.empty()) break;
            player_drop(p, s, true);
            if (r.s->item != item) break;
        }
        if (r.kind == S_GRID) update_result(p);
        return;
    }
    if (type == 6 && valid && !carried.empty()) {
        SlotRef r0 = menu_slot(p, slot);
        if (!r0.s->empty()) return;
        int start = button == 0 ? 0 : n - 1, step = button == 0 ? 1 : -1;
        for (int pass = 0; pass < 2; pass++)
            for (int i = start; i >= 0 && i < n && carried.count < max_stack(carried); i += step) {
                SlotRef r = menu_slot(p, i);
                if (r.s->empty() || !r.s->same(carried) || r.kind == S_RESULT) continue;  // canTakeItemForPickAll
                if (pass == 0 && r.s->count == max_stack(*r.s)) continue;
                ItemStack got = try_remove(p, r, r.s->count, max_stack(carried) - carried.count);
                carried.count += got.count;
                if (r.kind == S_GRID) update_result(p);
            }
    }
}

// ---------- sync (end of tick) ----------
inline constexpr int STALE = -2;  // remote slot marker: resend
inline void send_menu_content(Player& p) {
    Writer w;
    w.varint(play_cb::container_set_content).varint(p.g.container).varint(++p.g.state_id).varint(menu_size(p));
    for (int i = 0; i < menu_size(p); i++) {
        ItemStack s = *menu_slot(p, i).s;
        write_slot(w, s);
        p.g.remote[size_t(i)] = s;
    }
    write_slot(w, p.g.carried);
    p.g.remote_carried = p.g.carried;
    p.g.remote_valid = true;
    p.send(w);
}
// broadcastChanges: send every slot that differs from what the client was last told.
inline void sync_menu(Player& p) {
    if (!p.g.remote_valid) return send_menu_content(p);
    for (int i = 0; i < menu_size(p); i++) {
        const ItemStack& s = *menu_slot(p, i).s;
        if (p.g.remote[size_t(i)].item != STALE && s == p.g.remote[size_t(i)]) continue;
        Writer w;
        w.varint(play_cb::container_set_slot).varint(p.g.container).varint(++p.g.state_id).i16(int16_t(i));
        write_slot(w, s);
        p.send(w);
        p.g.remote[size_t(i)] = s;
    }
    if (p.g.remote_carried.item == STALE || !(p.g.carried == p.g.remote_carried)) {
        Writer w;
        w.varint(play_cb::set_cursor_item);
        write_slot(w, p.g.carried);
        p.send(w);
        p.g.remote_carried = p.g.carried;
    }
}

// Closing a menu returns the grid and cursor to the inventory (AbstractContainerMenu.removed / clearContainer).
inline void return_grid(Player& p) {
    auto give_back = [&](ItemStack& s) {
        if (s.empty()) return;
        if (inventory_add(p, s), s.count > 0) player_drop(p, s, false);
        s = {};
    };
    if (p.g.container) for (auto& s : p.g.craft3) give_back(s);
    else for (auto& s : p.g.craft2) give_back(s);
    give_back(p.g.carried);
    update_result(p);
}
inline void close_container(Player& p, bool notify_client) {
    return_grid(p);
    if (p.g.container == 0) return;
    if (notify_client) p.send(Writer().varint(play_cb::container_close).varint(p.g.container));
    p.g.container = 0;
    update_result(p);
    p.g.remote_valid = false;
}
inline void open_crafting_table(Player& p, int x, int y, int z) {
    if (p.g.container) close_container(p, true);
    else return_grid(p);
    p.g.next_container = p.g.next_container % 100 + 1;
    p.g.container = p.g.next_container;
    p.g.table_x = x, p.g.table_y = y, p.g.table_z = z;
    Writer w;
    w.varint(play_cb::open_screen).varint(p.g.container).varint(menu_ids["crafting"]);
    // {translate:"container.crafting"}
    w.u8(0x0A).u8(0x08).u8(0).u8(9).bytes("translate", 9).u8(0).u8(18).bytes("container.crafting", 18).u8(0);
    p.send(w);
    update_result(p);
    p.g.remote_valid = false;
}

// ServerboundContainerClickPacket: container, stateId, slot, button, type, changed slots, carried (hashed).
inline void handle_container_click(Player& p, Reader& in) {
    int container = in.varint();
    int state_id = in.varint();
    int slot = int16_t(in.u16());
    int button = int8_t(in.u8());
    int type = in.varint();
    // The hashed stacks the client predicted are skipped: the server is authoritative and resends what it
    // changed; slots the client says it changed are marked stale so they're resent even if unchanged here.
    int changed = in.varint();
    if (changed < 0 || changed > 128) throw std::runtime_error("too many changed slots");
    auto skip_hashed = [&] {
        if (!in.u8()) return;
        in.varint(); in.varint();
        for (int k = in.varint(); k > 0; k--) in.varint(), in.i32();
        for (int k = in.varint(); k > 0; k--) in.varint();
    };
    std::array<int, 128> stale{};
    for (int k = 0; k < changed; k++) {
        stale[size_t(k)] = int16_t(in.u16());
        skip_hashed();
    }
    skip_hashed();
    if (container != p.g.container) return;
    if (p.g.dead || p.gamemode == 3) { p.g.remote_valid = false; return; }  // sendAllDataToRemote
    for (int k = 0; k < changed; k++)
        if (stale[size_t(k)] >= 0 && stale[size_t(k)] < MENU_SLOTS) p.g.remote[size_t(stale[size_t(k)])].item = STALE;
    p.g.remote_carried.item = STALE;
    do_click(p, slot, button, type);
    if (state_id != p.g.state_id) p.g.remote_valid = false;  // client was out of date: full resync
}
