#pragma once
// Hoppers (HopperBlockEntity + HopperMenu): push into the container they face, pull from the container above
// or item entities in the box above, 8 tick cooldown. Container lookup (getContainerAt) covers block entity
// containers incl. double chests and worldly faces. ponytail: minecart/entity containers are not looked up.
#include "furnace.hpp"

namespace cont {

// A Container view: one block entity, or both halves of a double chest (CompoundContainer).
struct ContainerRef {
    std::shared_ptr<be::Live> a, b;
    explicit operator bool() const { return bool(a); }
    int size() const { return int(a->items.size() + (b ? b->items.size() : 0)); }
    be::Live& owner(int i) const { return b && i >= int(a->items.size()) ? *b : *a; }
    int local(int i) const { return b && i >= int(a->items.size()) ? i - int(a->items.size()) : i; }
    ItemStack& at(int i) const { return owner(i).items[size_t(local(i))]; }
    bool worldly() const { return !b && a->worldly(); }
    std::vector<int> slots(int dir) const {  // getSlots
        if (worldly() && dir >= 0) return a->face_slots(dir);
        std::vector<int> v(static_cast<size_t>(size()));
        for (int i = 0; i < size(); i++) v[size_t(i)] = i;
        return v;
    }
    bool empty() const {
        for (int i = 0; i < size(); i++)
            if (!at(i).empty()) return false;
        return true;
    }
    int max_stack() const { return a->max_stack(); }
    void set_changed() const { a->set_changed(); if (b) b->set_changed(); }
};

inline ContainerRef container_at(int x, int y, int z) {  // getBlockContainer
    int state = world.get_block(x, y, z);
    if (be::block_entity_type(state) < 0) return {};
    auto b = be::get(x, y, z);
    if (!b || b->items.empty()) return {};
    ContainerRef r{b, nullptr};
    if (is_chest_state(state))
        if (auto p = chest_partner(*b)) {  // ignoreBeingBlocked = true
            bool first = bs::R().value(state, "type") == "right";
            r = first ? ContainerRef{b, p} : ContainerRef{p, b};
        }
    return r;
}

struct HopperBE : be::Live {
    int cooldown = -1;
    int64_t ticked = 0;
    HopperBE() { items.resize(5); }
    bool ticks() const override { return true; }
    void removed_side_effects() override { be::drop_contents(*this); }
    void load(const Nbt& n) override { cooldown = int(comp::nbt_int(n, "TransferCooldown", -1)); }
    void save(Nbt& n) const override { n.set("TransferCooldown", Nbt::num(Nbt::Int, cooldown)); }
    bool full() const {
        for (auto& s : items)
            if (s.empty() || s.count != ::max_stack(s)) return false;
        return true;
    }
    void tick() override;  // pushItemsTick
};

inline bool place_ok(const ContainerRef& c, const ItemStack& s, int slot, int dir) {  // canPlaceItemInContainer
    be::Live& o = c.owner(slot);
    if (!o.can_place(c.local(slot), s)) return false;
    return !(c.worldly() && !o.can_place_face(c.local(slot), s, dir));
}

// tryMoveInItem. `from` is the source hopper (cooldown rule) or null.
inline ItemStack move_in(HopperBE* from, const ContainerRef& c, ItemStack s, int slot, int dir) {
    ItemStack& cur = c.at(slot);
    if (!place_ok(c, s, slot, dir)) return s;
    bool ok = false, was_empty = c.empty();
    if (cur.empty()) {
        cur = s;
        c.owner(slot).item_set(c.local(slot));
        s = {};
        ok = true;
    } else if (cur.same(s) && cur.count <= ::max_stack(cur)) {  // canMergeItems
        int n = std::min(s.count, ::max_stack(s) - cur.count);
        s.count -= n, cur.count += n;
        if (s.count <= 0) s = {};
        ok = n > 0;
    }
    if (ok) {
        if (auto* h = dynamic_cast<HopperBE*>(c.a.get()); h && was_empty && !c.b && h->cooldown <= 8)
            h->cooldown = 8 - (from && h->ticked >= from->ticked ? 1 : 0);
        c.set_changed();
    }
    return s;
}
inline ItemStack add_item(HopperBE* from, const ContainerRef& c, ItemStack s, int dir) {  // addItem(from, container, stack, dir)
    if (c.worldly() && dir >= 0) {
        for (int slot : c.a->face_slots(dir))
            if (!s.empty()) s = move_in(from, c, s, slot, dir);
    } else {
        for (int i = 0; i < c.size() && !s.empty(); i++) s = move_in(from, c, s, i, dir);
    }
    return s;
}
inline bool full_container(const ContainerRef& c, int dir) {  // isFullContainer
    for (int slot : c.slots(dir)) {
        const ItemStack& s = c.at(slot);
        if (s.count < (s.empty() ? 64 : ::max_stack(s))) return false;
    }
    return true;
}

inline void HopperBE::tick() {
    cooldown--;
    ticked = server_tick;
    if (cooldown > 0) return;
    cooldown = 0;
    if (bs::R().value(state, "enabled") != "true") return;
    ContainerRef self{std::static_pointer_cast<be::Live>(be::live_map()[be::key(x, y, z)]), nullptr};
    if (!self) return;
    bool changed = false;
    bool any = false;
    for (auto& s : items) any |= !s.empty();
    if (any) {  // ejectItems
        Dir f = facing(state);
        if (ContainerRef target = container_at(x + bs::DX[f], y + bs::DY[f], z + bs::DZ[f])) {
            int dir = bs::opposite(f);
            if (!full_container(target, dir))
                for (int slot = 0; slot < 5 && !changed; slot++) {
                    ItemStack& s = items[size_t(slot)];
                    if (s.empty()) continue;
                    ItemStack one = s.copy(1);
                    int original = s.count;
                    if (--s.count <= 0) s = {};
                    if (add_item(this, target, one, dir).empty()) {
                        target.set_changed();
                        changed = true;
                    } else {
                        s = one.copy(original);
                    }
                }
        }
    }
    if (!full()) {  // suckInItems
        bool sucked = false;
        if (ContainerRef src = container_at(x, y + 1, z)) {
            for (int slot : src.slots(bs::DOWN)) {
                ItemStack& s = src.at(slot);
                if (s.empty() || !src.owner(slot).can_take(src.local(slot), s)) continue;
                if (src.worldly() && !src.a->can_take_face(slot, s, bs::DOWN)) continue;
                int original = s.count;
                ItemStack one = s.copy(1);
                if (--s.count <= 0) s = {};
                if (add_item(nullptr, self, one, -1).empty()) {
                    src.set_changed();
                    sucked = true;
                    break;
                }
                s = one.copy(original);
            }
        } else {
            int above = world.get_block(x, y + 1, z);
            bool blocked = (bs::R().flags(above) & bs::FULL_COLLISION) && !bs::R().in(above, bs::R().tag("minecraft:does_not_block_hoppers"));
            if (!blocked)
                for_each_entity_in(AABB{double(x), y + 11.0 / 16, double(z), x + 1.0, y + 2.0, z + 1.0}, [&](Entity& o) {
                    if (sucked || o.removed || o.type != entity_types::item) return;  // getItemsAtAndAbove
                    auto& e = static_cast<ItemEntity&>(o);
                    if (e.stack.empty()) return;
                    ItemStack rest = add_item(nullptr, self, e.stack, -1);
                    if (rest.empty()) {
                        e.set_item({});
                        e.discard();
                        sucked = true;
                    } else if (rest.count != e.stack.count) {
                        e.set_item(rest);
                    }
                }, false);
        }
        changed |= sucked;
    }
    if (changed) {
        cooldown = 8;
        set_changed();
    }
}

inline bool use_hopper(Player& p, int state, int x, int y, int z) {
    if (be::block_entity_type(state) != block_entity_types::hopper) return false;
    auto b = be::get(x, y, z);
    if (!b) return true;
    unpack_loot(*b, &p);
    open_slots_menu(p, menu_over(std::make_shared<SlotsMenu>(), {b}), "hopper", title_of(*b, "container.hopper"));
    return true;
}

inline void register_hoppers() {
    be::factories[block_entity_types::hopper] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<HopperBE>(); };
    be::ticking_types.push_back(block_entity_types::hopper);
    be::empty_update_tag.push_back(block_entity_types::hopper);
}

}  // namespace cont
