#pragma once
// Block entities and container menus: entry point. containers_init() (game_init) loads data/containers.json,
// registers block entity types and installs the hooks in world.hpp / survival.hpp / loot.hpp / server.hpp.
#include "hopper.hpp"
#include "item_nbt.hpp"

namespace cont {

// Block interaction (BlockState.useItemOn / useWithoutItem). True = consumed.
inline bool use_block(Player& p, int hand, int x, int y, int z, int) {
    int state = world.get_block(x, y, z);
    bool have_items = !p.inv[size_t(p.selected)].empty() || !p.inv[40].empty();
    if (p.g.sneaking && have_items) return false;  // secondary use: place against it instead
    if (hand != 0) return false;                   // useWithoutItem is main hand only
    return use_container(p, state, x, y, z) || use_furnace(p, state, x, y, z) || use_hopper(p, state, x, y, z) ||
           use_station(p, state, x, y, z) || use_brewing_or_beacon(p, state, x, y, z);
}

inline void placed(Player&, int x, int y, int z, const ItemStack& item) {
    int state = world.get_block(x, y, z);
    if (be::block_entity_type(state) < 0) return;
    if (!comp::get(item, comp::custom_name()) && !comp::get(item, comp::container())) return;
    if (auto b = be::get(x, y, z)) apply_item(*b, item);
}

inline void block_changed(World* w, int x, int y, int z, int old_state, int new_state) {
    InLevel in(w);
    be::block_changed(x, y, z, old_state, new_state);
}

inline void loot_copy(int x, int y, int z, const std::vector<std::string>& include, ItemStack& s) {
    if (auto b = be::removed_at(x, y, z)) copy_to_item(*b, include, s);
}

inline void player_tick(Player& p) {
    if (p.g.menu && !p.g.menu->still_valid(p)) close_container(p, true);
}

}  // namespace cont

// Player NBT (survival.hpp hooks): EnderItems [{Slot, id, count, components}], XpSeed.
inline void save_player_containers_nbt(const Player& p, Nbt& t) {
    t.set("EnderItems", comp::items_nbt(p.g.ender.data(), int(p.g.ender.size())));
    t.set("XpSeed", Nbt::num(Nbt::Int, p.g.enchant_seed));
}
inline void load_player_containers_nbt(Player& p, const Nbt& t) {
    p.g.ender.fill({});
    comp::items_from_nbt(t, "EnderItems", p.g.ender.data(), int(p.g.ender.size()));
    p.g.enchant_seed = int(comp::nbt_int(t, "XpSeed"));
}

inline void containers_init() {
    if (!cont::load_data()) _exit(1);
    cont::register_chests();
    cont::register_furnaces();
    cont::register_hoppers();
    cont::load_enchant_data(cont::data_json);
    cont::load_brewing_data(cont::data_json);
    {
        auto& pot = cont::data_json["potions"];
        cont::potion_names.resize(pot.obj.size());
        for (auto& [k, v] : pot.obj)
            if (size_t(v.asInt()) < cont::potion_names.size()) cont::potion_names[size_t(v.asInt())] = k;
        for (auto& v : cont::data_json["banner"]["ids"].arr) cont::banner_names.push_back(v.asString());
    }
    item_components_to_nbt = cont::components_to_nbt;
    item_components_from_nbt = cont::components_from_nbt;
    save_player_containers = save_player_containers_nbt;
    load_player_containers = load_player_containers_nbt;
    cont::register_brewing_beacon();
    cont::data_json = Json();
    static const Writer recipes = cont::update_recipes_packet();
    on_player_join([](Player& p) {
        p.send(recipes);
        if (!p.g.enchant_seed) p.g.enchant_seed = int32_t(game_rng());
    });
    on(play_sb::rename_item, [](Player& p, Reader& in, int) {
        std::string name = in.str(32767);
        if (auto* m = dynamic_cast<cont::AnvilMenu*>(p.g.menu.get())) m->rename(p, name);
    });
    block_changed_hook = cont::block_changed;
    chunk_loaded_hook = be::chunk_loaded;
    block_entity_update_tag = be::update_tag;
    use_block_hook = cont::use_block;
    lvl::block_item_placed_hook = cont::placed;
    loot_copy_block_entity = cont::loot_copy;
    before_world_save = be::flush_all;
    on_tick(be::tick);
    on_player_tick(cont::player_tick);
    on(play_sb::container_button_click, [](Player& p, Reader& in, int) {
        int id = in.varint(), button = in.varint();
        if (id == p.g.container && p.g.menu && !p.g.dead && p.gamemode != 3) p.g.menu->button(p, button);
    });
}
