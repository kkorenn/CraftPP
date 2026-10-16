#pragma once
// Chests (single/double, trapped, copper), barrels, ender chests, shulker boxes, dispensers/droppers:
// block entities, ChestMenu/ShulkerBoxMenu/DispenserMenu, opener counting (lid block events, sounds, barrel
// open state), placement states and item component round trips.
#include "common.hpp"

namespace cont {
namespace bet = block_entity_types;

// ---------- block entities ----------
struct ContainerBE : be::Live {
    explicit ContainerBE(int n) { items.resize(size_t(n)); }
    void removed_side_effects() override { be::drop_contents(*this); }  // Containers.dropContents
};

inline bool is_chest_state(int s) { return bs::R().block(bs::R().block_of(s)).is_a("ChestBlock"); }
inline std::string block_name(int s) { return bs::R().block(bs::R().block_of(s)).name; }

// ChestBlock.getConnectedDirection: LEFT -> facing.clockwise, RIGHT -> counter-clockwise.
inline Dir connected_dir(int s) {
    Dir f = facing(s);
    return bs::R().value(s, "type") == "left" ? clockwise(f) : counter_clockwise(f);
}
// Partner half of a double chest (same block, opposite type, same facing), or null.
inline std::shared_ptr<be::Live> chest_partner(const be::Live& b) {
    if (bs::R().value(b.state, "type") == "single") return nullptr;
    Dir d = connected_dir(b.state);
    int x = b.x + bs::DX[d], z = b.z + bs::DZ[d];
    int s = world.get_block(x, b.y, z);
    if (bs::R().block_of(s) != bs::R().block_of(b.state) || bs::R().value(s, "type") == "single" ||
        bs::R().value(s, "type") == bs::R().value(b.state, "type") || facing(s) != facing(b.state))
        return nullptr;
    return be::get(x, b.y, z);
}

struct ChestBE : ContainerBE {
    ChestBE() : ContainerBE(27) {}
    std::string sound(bool open) const {
        bool copper = block_name(state).find("copper") != std::string::npos;
        return std::string(copper ? "block.copper_chest." : "block.chest.") + (open ? "open" : "close");
    }
    void play(bool open) {  // ChestBlockEntity.playSound: LEFT halves are silent, RIGHT plays between halves
        std::string type(bs::R().value(state, "type"));
        if (type == "left") return;
        double sx = x + 0.5, sy = y + 0.5, sz = z + 0.5;
        if (type == "right") {
            Dir d = connected_dir(state);
            sx += bs::DX[d] * 0.5, sz += bs::DZ[d] * 0.5;
        }
        play_sound(sound(open).c_str(), sx, sy, sz, 0.5f, rand_float() * 0.1f + 0.9f);
    }
    void opened(Player&) override {
        if (openers == 1) play(true);
        block_event(x, y, z, 1, openers, state);
    }
    void closed(Player&) override {
        if (openers == 0) play(false);
        block_event(x, y, z, 1, openers, state);
        // ponytail: trapped chest power (TrappedChestBlockEntity.signalOpenCount) is left to the redstone port;
        // it reads `openers`.
    }
};
struct BarrelBE : ContainerBE {
    BarrelBE() : ContainerBE(27) {}
    void opened(Player&) override {
        if (openers != 1) return;
        play_sound("block.barrel.open", x + 0.5 + bs::DX[facing(state)] * 0.5, y + 0.5 + bs::DY[facing(state)] * 0.5,
                   z + 0.5 + bs::DZ[facing(state)] * 0.5, 0.5f, rand_float() * 0.1f + 0.9f);
        set_state(*this, with(state, "open", "true"));
    }
    void closed(Player&) override {
        if (openers != 0) return;
        play_sound("block.barrel.close", x + 0.5 + bs::DX[facing(state)] * 0.5, y + 0.5 + bs::DY[facing(state)] * 0.5,
                   z + 0.5 + bs::DZ[facing(state)] * 0.5, 0.5f, rand_float() * 0.1f + 0.9f);
        set_state(*this, with(state, "open", "false"));
    }
};
struct ShulkerBE : ContainerBE {
    ShulkerBE() : ContainerBE(27) {}
    void removed_side_effects() override {}  // contents leave with the item (loot copy_components)
    bool can_place(int, const ItemStack& s) const override { return !tagged("shulker_boxes", s.item); }
    void opened(Player&) override {
        block_event(x, y, z, 1, openers, state);
        if (openers == 1) play_sound("block.shulker_box.open", x + 0.5, y + 0.5, z + 0.5, 0.5f, rand_float() * 0.1f + 0.9f);
    }
    void closed(Player&) override {
        block_event(x, y, z, 1, openers, state);
        if (openers == 0) play_sound("block.shulker_box.close", x + 0.5, y + 0.5, z + 0.5, 0.5f, rand_float() * 0.1f + 0.9f);
    }
};
struct EnderChestBE : be::Live {  // no contents of its own: openers drive the lid
    void opened(Player&) override {
        if (openers == 1) play_sound("block.ender_chest.open", x + 0.5, y + 0.5, z + 0.5, 0.5f, rand_float() * 0.1f + 0.9f);
        block_event(x, y, z, 1, openers, state);
    }
    void closed(Player&) override {
        if (openers == 0) play_sound("block.ender_chest.close", x + 0.5, y + 0.5, z + 0.5, 0.5f, rand_float() * 0.1f + 0.9f);
        block_event(x, y, z, 1, openers, state);
    }
};
struct DispenserBE : ContainerBE {
    DispenserBE() : ContainerBE(9) {}
};

// ---------- loot tables in containers (RandomizableContainerBlockEntity.unpackLootTable) ----------
inline void (*fill_loot)(be::Live& b, Player* p) = nullptr;  // chest_loot.hpp
inline void unpack_loot(be::Live& b, Player* p) {
    if (b.loot_table.empty()) return;
    if (fill_loot) fill_loot(b, p);
    b.loot_table.clear();
    b.set_changed();
}

// ---------- menus ----------
struct ShulkerMenu : SlotsMenu {
    bool may_place(int, const ItemStack& s) override { return !tagged("shulker_boxes", s.item); }  // ShulkerBoxSlot
};
inline std::shared_ptr<SlotsMenu> menu_over(std::shared_ptr<SlotsMenu> m, std::initializer_list<std::shared_ptr<be::Live>> bes) {
    for (auto& b : bes) {
        m->holders.push_back(b);
        for (auto& s : b->items) m->slots.push_back(&s);
    }
    return m;
}

// ChestBlock.isChestBlockedAt (cats not checked: no cats yet).
inline bool blocked_above(int x, int y, int z) { return bs::R().redstone_conductor(world.get_block(x, y + 1, z)); }

inline bool open_chest(Player& p, std::shared_ptr<be::Live> b) {
    if (blocked_above(b->x, b->y, b->z)) return true;
    auto partner = chest_partner(*b);
    if (partner && blocked_above(partner->x, partner->y, partner->z)) return true;
    unpack_loot(*b, &p);
    if (!partner) {
        open_slots_menu(p, menu_over(std::make_shared<SlotsMenu>(), {b}), "generic_9x3", title_of(*b, "container.chest"));
        return true;
    }
    unpack_loot(*partner, &p);
    // DoubleBlockCombiner: the RIGHT half (BlockType.FIRST) is the top of the compound container.
    auto first = bs::R().value(b->state, "type") == "right" ? b : partner, second = first == b ? partner : b;
    std::vector<uint8_t> title = !first->custom_name.empty() ? first->custom_name
                                 : !second->custom_name.empty() ? second->custom_name
                                                                : comp::text_translate("container.chestDouble");
    open_slots_menu(p, menu_over(std::make_shared<SlotsMenu>(), {first, second}), "generic_9x6", title);
    return true;
}

// ShulkerBoxBlock.canOpen: the lid needs room (block in front must not collide).
inline bool shulker_can_open(const be::Live& b) {
    if (b.openers > 0) return true;
    Dir d = facing(b.state);
    int s = world.get_block(b.x + bs::DX[d], b.y + bs::DY[d], b.z + bs::DZ[d]);
    return bs::R().flags(s) & bs::NO_COLLISION;
}

struct EnderMenu : SlotsMenu {};
inline bool open_ender_chest(Player& p, std::shared_ptr<be::Live> b) {
    if (bs::R().redstone_conductor(world.get_block(b->x, b->y + 1, b->z))) return true;
    auto m = std::make_shared<EnderMenu>();
    m->holders.push_back(b);
    for (auto& s : p.g.ender) m->slots.push_back(&s);
    open_slots_menu(p, m, "generic_9x3", comp::text_translate("container.enderchest"));
    return true;
}

// useWithoutItem for the containers of this file. Returns false when the block isn't one of them.
inline bool use_container(Player& p, int state, int x, int y, int z) {
    int type = be::block_entity_type(state);
    if (type != bet::chest && type != bet::trapped_chest && type != bet::barrel && type != bet::ender_chest &&
        type != bet::shulker_box && type != bet::dispenser && type != bet::dropper)
        return false;
    auto b = be::get(x, y, z);
    if (!b) return true;
    switch (type) {
        case bet::chest: case bet::trapped_chest: return open_chest(p, b);
        case bet::ender_chest: return open_ender_chest(p, b);
        case bet::barrel:
            unpack_loot(*b, &p);
            open_slots_menu(p, menu_over(std::make_shared<SlotsMenu>(), {b}), "generic_9x3", title_of(*b, "container.barrel"));
            return true;
        case bet::shulker_box:
            if (!shulker_can_open(*b)) return true;
            unpack_loot(*b, &p);
            open_slots_menu(p, menu_over(std::make_shared<ShulkerMenu>(), {b}), "shulker_box", title_of(*b, "container.shulkerBox"));
            return true;
        default: {  // dispenser, dropper: DispenserMenu
            unpack_loot(*b, &p);
            auto m = menu_over(std::make_shared<SlotsMenu>(), {b});
            m->count_check = true;
            open_slots_menu(p, m, "generic_3x3", title_of(*b, type == bet::dropper ? "container.dropper" : "container.dispenser"));
            return true;
        }
    }
}

// Double chest comparator output: both halves (CompoundContainer).
inline int chest_signal(be::Live& b) {
    if (b.type == bet::chest || b.type == bet::trapped_chest)
        if (auto partner = chest_partner(b)) {
            std::vector<ItemStack> all = b.items;
            all.insert(all.end(), partner->items.begin(), partner->items.end());
            return be::signal_from_items(all.data(), int(all.size()), 99);
        }
    return be::signal_from_items(b.items.data(), int(b.items.size()), b.max_stack());
}

// ---------- placement (getStateForPlacement) ----------
inline int chest_partner_facing(int x, int y, int z, Dir towards, int block) {  // candidatePartnerFacing
    int s = world.get_block(x + bs::DX[towards], y, z + bs::DZ[towards]);
    if (bs::R().block_of(s) != block || bs::R().value(s, "type") != "single") return -1;
    return int(facing(s));
}
inline int place_chest(Player& p, int state, int x, int y, int z, int face) {
    int block = bs::R().block_of(state);
    Dir f = bs::opposite(horizontal_facing(p));
    std::string type = "single";
    bool secondary = p.g.sneaking;
    Dir clicked = Dir(face >= 0 && face < 6 ? face : 1);
    if (clicked >= bs::NORTH && secondary) {
        int nf = chest_partner_facing(x, y, z, bs::opposite(clicked), block);
        bool same_axis = nf >= 0 && (Dir(nf) == clicked || Dir(nf) == bs::opposite(clicked));
        if (nf >= 0 && !same_axis) {
            f = Dir(nf);
            type = counter_clockwise(f) == bs::opposite(clicked) ? "right" : "left";
        }
    }
    if (type == "single" && !secondary) {  // getChestType
        if (chest_partner_facing(x, y, z, clockwise(f), block) == int(f)) type = "left";
        else if (chest_partner_facing(x, y, z, counter_clockwise(f), block) == int(f)) type = "right";
    }
    return with(with(state, "facing", dir_name(f)), "type", type);
}
// ChestBlock.updateShape on the partner after a double chest forms / breaks.
inline void chest_link_partner(int x, int y, int z, int state) {
    if (!is_chest_state(state) || bs::R().value(state, "type") == "single") return;
    Dir d = connected_dir(state);
    int px = x + bs::DX[d], pz = z + bs::DZ[d];
    int ps = world.get_block(px, y, pz);
    if (bs::R().block_of(ps) != bs::R().block_of(state) || bs::R().value(ps, "type") != "single" || facing(ps) != facing(state)) return;
    change_block(px, y, pz, with(ps, "type", bs::R().value(state, "type") == "left" ? "right" : "left"));
}
inline void chest_unlink_partner(int x, int y, int z, int old_state) {
    if (!is_chest_state(old_state) || bs::R().value(old_state, "type") == "single") return;
    Dir d = connected_dir(old_state);
    int px = x + bs::DX[d], pz = z + bs::DZ[d];
    int ps = world.get_block(px, y, pz);
    if (bs::R().block_of(ps) == bs::R().block_of(old_state) && bs::R().value(ps, "type") != "single" && connected_dir(ps) == bs::opposite(d))
        change_block(px, y, pz, with(ps, "type", "single"));
}

// ---------- item <-> block entity components ----------
// BlockEntity.applyComponentsFromItemStack: custom_name, container.
inline void apply_item(be::Live& b, const ItemStack& it) {
    if (auto* n = comp::get(it, comp::custom_name())) b.custom_name = *n;
    if (auto* c = comp::get(it, comp::container()); c && !b.items.empty()) {
        auto v = comp::decode_contents(*c);
        for (size_t i = 0; i < b.items.size(); i++) b.items[i] = i < v.size() ? v[i] : ItemStack{};
    }
    b.set_changed();
}
// collectComponents, filtered by the loot table's include list.
inline void copy_to_item(const be::Live& b, const std::vector<std::string>& include, ItemStack& s) {
    for (auto& name : include) {
        if (name == "custom_name" && !b.custom_name.empty()) comp::set(s, comp::custom_name(), b.custom_name);
        else if (name == "container" && !b.items.empty()) {
            bool any = false;
            for (auto& i : b.items) any |= !i.empty();
            if (any) comp::set(s, comp::container(), comp::encode_contents(b.items.data(), int(b.items.size())));
        }
    }
}

inline void register_chests() {
    be::factories[bet::chest] = be::factories[bet::trapped_chest] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<ChestBE>(); };
    be::factories[bet::barrel] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<BarrelBE>(); };
    be::factories[bet::shulker_box] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<ShulkerBE>(); };
    be::factories[bet::ender_chest] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<EnderChestBE>(); };
    be::factories[bet::dispenser] = be::factories[bet::dropper] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<DispenserBE>(); };
    for (int t : {bet::chest, bet::trapped_chest, bet::barrel, bet::shulker_box, bet::ender_chest, bet::dispenser, bet::dropper})
        be::empty_update_tag.push_back(t);
    be::container_signal_hook = chest_signal;
}

}  // namespace cont
