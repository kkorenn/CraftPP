#pragma once
// CauldronInteractions bucket dispatch (vanilla 26.3). Tick thread only.
#include "level.hpp"

namespace lvl {

// ItemUtils.createFilledResult for component-free bucket stacks.
inline void cauldron_bucket_result(Player& player, ItemStack& held, int result) {
    ItemStack filled{result, 1, 0};
    if (player.gamemode == 1) {
        // Infinite materials: keep the held stack; add the result only if absent.
        for (const auto& stack : player.inv)
            if (!stack.empty() && stack.same(filled)) return;
        give_item(player, result, 1);
    } else if (--held.count <= 0) {
        held = filled;
    } else {
        give_item(player, result, 1);
    }
}

// 0 = try empty hand, 1 = success (swing), 2 = consume (no swing).
inline int use_cauldron_bucket(Player& player, const Pos& pos, int state, ItemStack& held) {
    if (held.empty()) return 0;
    const auto& name = block_of(state).name;
    bool empty = name == "minecraft:cauldron";
    bool water = name == "minecraft:water_cauldron";
    bool lava = name == "minecraft:lava_cauldron";
    bool snow = name == "minecraft:powder_snow_cauldron";
    if (!empty && !water && !lava && !snow) return 0;

    auto item_is = [&](const char* item) { return item_info[size_t(held.item)].name == std::string("minecraft:") + item; };
    int next = -1;
    const char* result = nullptr;
    const char* sound = nullptr;
    if (item_is("water_bucket")) {
        next = with(def("water_cauldron"), "level", 3);
        result = "minecraft:bucket";
        sound = "minecraft:item.bucket.empty";
    } else if (item_is("lava_bucket") || item_is("powder_snow_bucket")) {
        // Vanilla checks the fluid above, including waterlogged blocks.
        if (is_water(get(pos.above()))) return 2;
        bool filling_lava = item_is("lava_bucket");
        next = filling_lava ? def("lava_cauldron") : with(def("powder_snow_cauldron"), "level", 3);
        result = "minecraft:bucket";
        sound = filling_lava ? "minecraft:item.bucket.empty_lava" : "minecraft:item.bucket.empty_powder_snow";
    } else if (item_is("bucket")) {
        if (empty || ((water || snow) && geti(state, "level") != 3)) return 0;
        next = def("cauldron");
        result = water ? "minecraft:water_bucket" : lava ? "minecraft:lava_bucket" : "minecraft:powder_snow_bucket";
        sound = water ? "minecraft:item.bucket.fill" : lava ? "minecraft:item.bucket.fill_lava" : "minecraft:item.bucket.fill_powder_snow";
    } else {
        return 0;
    }
    cauldron_bucket_result(player, held, item_by_name.at(result));
    set(pos, next);
    ::play_sound(sound, SRC_BLOCKS, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5);
    return 1;
}

}  // namespace lvl
