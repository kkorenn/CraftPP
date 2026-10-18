#pragma once
// Live feature placement (sapling tree growth, huge mushrooms, bonemeal grass).
// Level subclass with dry-run/commit buffering.
#include "../worldgen/features.hpp"
#include "../level/level.hpp"
#include "../level/placement.hpp"
#include "../level/behaviours.hpp"

#include <unordered_map>
#include <memory>
#include <string>

namespace blocks_world {

class LiveLevel : public worldgen::feat::Level {
public:
    std::map<worldgen::feat::P, uint16_t> buffer;
    WorldgenRandom rng;

    LiveLevel(int64_t seed = 0) : rng(seed) {}

    uint16_t get(const worldgen::feat::P& p) const override {
        auto it = buffer.find(p);
        if (it != buffer.end()) return it->second;
        return uint16_t(lvl::get(p.x, p.y, p.z));
    }

    bool set(const worldgen::feat::P& p, int state) override {
        buffer[p] = uint16_t(state);
        return true;
    }

    bool can_write(const worldgen::feat::P& p) const override {
        return p.y >= world.min_y() && p.y < world.max_y();
    }

    int height(worldgen::Heightmap, int x, int z) const override {
        for (int y = world.max_y() - 1; y >= world.min_y(); y--) {
            uint16_t s = get(worldgen::feat::P{x, y, z});
            if (!bs::R().is_air(s)) return y + 1;
        }
        return world.min_y();
    }

    int biome(const worldgen::feat::P& p) const override {
        return lvl::biome_at(lvl::Pos{p.x, p.y, p.z});
    }

    RandomSource& random() override { return rng; }
    int64_t seed() const override { return rng.nextLong(); }
    int sea_level() const override { return world.sea_level; }
    int min_y() const override { return world.min_y(); }
    int max_y() const override { return world.max_y(); }

    void commit() {
        for (const auto& [p, state] : buffer) {
            lvl::set(lvl::Pos{p.x, p.y, p.z}, state, lvl::UPDATE_ALL);
        }
    }
};

inline const worldgen::feat::Feature* get_or_load_feature(const std::string& name) {
    return worldgen::feat::load_feature_by_name(name);
}

inline bool has_nearby_flowers(const lvl::Pos& p) {
    for (int dx = -2; dx <= 2; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -2; dz <= 2; dz++) {
                int s = lvl::get(p.x + dx, p.y + dy, p.z + dz);
                if (lvl::is(s, lvl::tag("flowers"))) return true;
            }
        }
    }
    return false;
}

inline bool grow_tree(const lvl::Pos& pos, RandomSource& rnd) {
    int s = lvl::get(pos);
    const std::string& bname = block_of(s).name;
    auto find_2x2 = [&](const lvl::Pos& p, int target_block, lvl::Pos& origin) -> bool {
        for (int dx = 0; dx >= -1; dx--) {
            for (int dz = 0; dz >= -1; dz--) {
                bool match = true;
                for (int x = 0; x <= 1; x++) {
                    for (int z = 0; z <= 1; z++) {
                        if (lvl::block(lvl::get(p.x + dx + x, p.y, p.z + dz + z)) != target_block) {
                            match = false;
                            break;
                        }
                    }
                    if (!match) break;
                }
                if (match) {
                    origin = lvl::Pos{p.x + dx, p.y, p.z + dz};
                    return true;
                }
            }
        }
        return false;
    };

    lvl::Pos origin = pos;
    bool is_2x2 = false;
    std::string feat_name;
    int b = lvl::block(s);
    bool flowers = has_nearby_flowers(pos);

    if (bname == "minecraft:oak_sapling") {
        if (flowers) feat_name = (rnd.nextInt(10) == 0 ? "fancy_oak_bees_005" : "oak_bees_005");
        else feat_name = (rnd.nextInt(10) == 0 ? "fancy_oak" : "oak");
    } else if (bname == "minecraft:spruce_sapling") {
        if (find_2x2(pos, b, origin)) {
            is_2x2 = true;
            feat_name = (rnd.nextInt(2) == 0 ? "mega_spruce" : "mega_pine");
        } else {
            feat_name = "spruce";
        }
    } else if (bname == "minecraft:birch_sapling") {
        feat_name = flowers ? "birch_bees_005" : "birch";
    } else if (bname == "minecraft:jungle_sapling") {
        if (find_2x2(pos, b, origin)) {
            is_2x2 = true;
            feat_name = "mega_jungle_tree";
        } else {
            feat_name = "jungle_tree_no_vine";
        }
    } else if (bname == "minecraft:acacia_sapling") {
        feat_name = "acacia";
    } else if (bname == "minecraft:cherry_sapling") {
        feat_name = flowers ? "cherry_bees_005" : "cherry";
    } else if (bname == "minecraft:dark_oak_sapling") {
        if (!find_2x2(pos, b, origin)) return false;
        is_2x2 = true;
        feat_name = "dark_oak";
    } else if (bname == "minecraft:pale_oak_sapling") {
        if (!find_2x2(pos, b, origin)) return false;
        is_2x2 = true;
        feat_name = "pale_oak_bonemeal";
    } else if (bname == "minecraft:mangrove_propagule") {
        feat_name = (rnd.nextFloat() < 0.85f ? "tall_mangrove" : "mangrove");
    } else if (bname == "minecraft:azalea") {
        feat_name = "azalea_tree";
    } else {
        return false;
    }

    auto feat = get_or_load_feature(feat_name);
    if (!feat) return false;

    LiveLevel ll(rnd.nextLong());
    if (is_2x2) {
        for (int x = 0; x <= 1; x++)
            for (int z = 0; z <= 1; z++)
                ll.set(worldgen::feat::P{origin.x + x, origin.y, origin.z + z}, blocks::air);
    } else {
        ll.set(worldgen::feat::P{origin.x, origin.y, origin.z}, blocks::air);
    }

    if (feat->place(ll, rnd, worldgen::feat::P{origin.x, origin.y, origin.z})) {
        ll.commit();
        return true;
    }
    return false;
}

inline bool grow_mushroom(const lvl::Pos& pos, RandomSource& rnd) {
    int s = lvl::get(pos);
    const std::string& bname = block_of(s).name;
    std::string feat_name;
    if (bname == "minecraft:brown_mushroom") feat_name = "huge_brown_mushroom";
    else if (bname == "minecraft:red_mushroom") feat_name = "huge_red_mushroom";
    else return false;

    auto feat = get_or_load_feature(feat_name);
    if (!feat) return false;

    LiveLevel ll(rnd.nextLong());
    ll.set(worldgen::feat::P{pos.x, pos.y, pos.z}, blocks::air);
    if (feat->place(ll, rnd, worldgen::feat::P{pos.x, pos.y, pos.z})) {
        ll.commit();
        return true;
    }
    return false;
}

inline int pick_flower(int biome_id, RandomSource& rnd) {
    (void)biome_id;
    static const int flowers[] = {
        blocks::dandelion, blocks::poppy, blocks::allium, blocks::azure_bluet,
        blocks::red_tulip, blocks::orange_tulip, blocks::white_tulip, blocks::pink_tulip,
        blocks::oxeye_daisy, blocks::cornflower, blocks::lily_of_the_valley
    };
    return flowers[rnd.nextInt(11)];
}

inline bool bonemeal_grass(const lvl::Pos& pos, RandomSource& rnd) {
    lvl::Pos above = pos.above();
    int short_grass_state = lvl::def("short_grass");
    int tall_grass_lower = lvl::def("tall_grass");
    int tall_grass_upper = lvl::with(tall_grass_lower, "half", "upper");

    for (int attempt = 0; attempt < 128; attempt++) {
        lvl::Pos test_pos = above;
        int randomize_count = attempt / 16;
        bool stop = false;
        for (int i = 0; i < randomize_count; i++) {
            int dx = rnd.nextIntBetweenInclusive(-1, 1);
            int dy = rnd.nextIntBetweenInclusive(-1, 1) * rnd.nextInt(3) / 2;
            int dz = rnd.nextIntBetweenInclusive(-1, 1);
            test_pos = test_pos.off(dx, dy, dz);
            int below_state = lvl::get(test_pos.below());
            if (lvl::block(below_state) != lvl::ID.grass_block || !lvl::is_air(lvl::get(test_pos))) {
                stop = true;
                break;
            }
        }
        if (stop) continue;

        int cur = lvl::get(test_pos);
        if (lvl::block(cur) == lvl::ID.short_grass && rnd.nextFloat() < 0.1f) {
            lvl::Pos up = test_pos.above();
            if (lvl::is_air(lvl::get(up)) && lvl::in_height(up.y)) {
                lvl::set(test_pos, tall_grass_lower, lvl::UPDATE_ALL);
                lvl::set(up, tall_grass_upper, lvl::UPDATE_ALL);
            }
        } else if (lvl::is_air(cur) && lvl::in_height(test_pos.y) && lvl::block(lvl::get(test_pos.below())) == lvl::ID.grass_block) {
            if (rnd.nextFloat() < 0.125f) {
                int flower = pick_flower(lvl::biome_at(test_pos), rnd);
                lvl::set(test_pos, flower, lvl::UPDATE_ALL);
            } else {
                lvl::set(test_pos, short_grass_state, lvl::UPDATE_ALL);
            }
        }
    }
    return true;
}

}  // namespace blocks_world
