#pragma once
// Block entities and container menus: entry point. containers_init() (game_init) loads data/containers.json,
// registers block entity types and installs the hooks in world.hpp / survival.hpp / loot.hpp / server.hpp.
#include "hopper.hpp"
#include "brewing.hpp"

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

// Player file lines (survival.hpp): "xpseed <seed>", "ender <slot> <item> <count> [patch hex]".
inline void save_player_containers(const Player& p, std::ostream& f) {
    f << "xpseed " << p.g.enchant_seed << '\n';  // Player XpSeed
    for (size_t i = 0; i < p.g.ender.size(); i++) {
        const ItemStack& s = p.g.ender[i];
        if (s.empty()) continue;
        f << "ender " << i << ' ' << s.item << ' ' << s.count;
        if (s.damage || s.comp) f << ' ' << comp::patch_hex(s);
        f << '\n';
    }
}
inline bool load_player_containers(Player& p, const std::string& key, std::istream& f) {
    if (key == "xpseed") return bool(f >> p.g.enchant_seed) || true;
    if (key != "ender") return false;
    std::string line;
    std::getline(f, line);
    std::istringstream in(line);
    int slot, item, count;
    std::string hex;
    if (!(in >> slot >> item >> count) || slot < 0 || slot >= 27 || item < 0 || item >= ITEM_COUNT) return true;
    ItemStack s{item, count};
    if (in >> hex) comp::patch_from_hex(s, hex);
    p.g.ender[size_t(slot)] = s;
    return true;
}

inline void containers_init() {
    if (!cont::load_data()) _exit(1);
    cont::register_chests();
    cont::register_furnaces();
    cont::register_hoppers();
    cont::load_enchant_data(cont::data_json);
    cont::load_brewing_data(cont::data_json);
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
