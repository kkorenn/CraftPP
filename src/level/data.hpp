#pragma once
// Block physics tables for the level tick system, from data/level.json (tools/gen_level_data.py, which dumps
// them from the vanilla jar at build time): per-state collision boxes and flags (legacy solid/liquid, face
// sturdiness, random ticking, light, fluid state), per-block class chains, fire odds and block tags.
// Also block-state property helpers (BlockState.getValue / setValue) over game/data.hpp's BlockInfo.
#include "../game/data.hpp"

#include <cstring>
#include <string_view>

namespace lvl {

// Direction (vanilla ordinal order).
enum Dir : int { DOWN, UP, NORTH, SOUTH, WEST, EAST };
inline constexpr int DX[6] = {0, 0, 0, 0, -1, 1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};
inline constexpr int OPP[6] = {UP, DOWN, SOUTH, NORTH, EAST, WEST};
inline constexpr int HORIZONTAL[4] = {NORTH, EAST, SOUTH, WEST};  // Direction.Plane.HORIZONTAL
inline constexpr const char* DIR_NAME[6] = {"down", "up", "north", "south", "west", "east"};
inline int axis(int d) { return d <= UP ? 1 : d <= SOUTH ? 2 : 0; }  // 0 x, 1 y, 2 z
inline int clockwise(int d) { return d == NORTH ? EAST : d == EAST ? SOUTH : d == SOUTH ? WEST : NORTH; }
inline int ccw(int d) { return d == NORTH ? WEST : d == WEST ? SOUTH : d == SOUTH ? EAST : NORTH; }
inline int dir_by_name(std::string_view n) {
    for (int i = 0; i < 6; i++)
        if (n == DIR_NAME[i]) return i;
    return -1;
}

struct Pos {
    int x, y, z;
    Pos rel(int d, int n = 1) const { return {x + DX[d] * n, y + DY[d] * n, z + DZ[d] * n}; }
    Pos above(int n = 1) const { return {x, y + n, z}; }
    Pos below(int n = 1) const { return {x, y - n, z}; }
    Pos off(int dx, int dy, int dz) const { return {x + dx, y + dy, z + dz}; }
    bool operator==(const Pos&) const = default;
};

// Per-state flag bits (BlockDump.java).
enum : uint64_t {
    F_AIR = 1, F_REPLACEABLE = 2, F_SOLID = 4, F_LIQUID = 8, F_RANDOM_TICKS = 16, F_LAVA_IGNITES = 32,
    F_FULL_COLLISION = 64, F_BLOCK_ENTITY = 128, F_SOLID_RENDER = 256, F_CONDUCTOR = 512, F_SIGNAL_SOURCE = 1024,
    F_SKYLIGHT = 2048, F_ANALOG = 4096, F_SUFFOCATING = 8192, F_GRASS_LIVES_UNDER = 16384, F_CAN_OCCLUDE = 32768,
};
enum Support { FULL, CENTER, RIGID };
// Fluids registry ids (Fluids: empty, flowing_water, water, flowing_lava, lava).
enum Fluid : int { EMPTY = 0, FLOWING_WATER = 1, WATER = 2, FLOWING_LAVA = 3, LAVA = 4 };

struct Box { double x0, y0, z0, x1, y1, z1; };

struct Tables {
    std::vector<uint64_t> flags;           // per state
    std::vector<uint16_t> shape;           // per state -> shapes
    std::vector<std::vector<Box>> shapes;  // collision shapes (Shapes.toAabbs)
    std::vector<std::string> cls;          // per block: "DoorBlock<..." class chain below Block
    std::vector<uint8_t> burn, ignite;     // per block: FireBlock odds
    std::vector<int16_t> next_age;         // per block: ChangeOverTimeBlock.getNext (copper weathering), -1 none
    std::vector<int8_t> age;               // per block: ChangeOverTimeBlock age ordinal, -1 not ageing
    std::map<std::string, std::vector<bool>> tags;  // block tags by short name, per block index
    std::vector<bool> empty_tag;
};
inline Tables T;

inline uint64_t flags(int s) { return T.flags[size_t(s)]; }
inline bool is_air(int s) { return flags(s) & F_AIR; }
inline bool can_be_replaced(int s) { return flags(s) & F_REPLACEABLE; }
inline bool is_solid(int s) { return flags(s) & F_SOLID; }  // BlockState.isSolid (legacy)
inline bool liquid(int s) { return flags(s) & F_LIQUID; }
inline bool random_ticks(int s) { return flags(s) & F_RANDOM_TICKS; }
inline bool ignited_by_lava(int s) { return flags(s) & F_LAVA_IGNITES; }
inline bool full_collision(int s) { return flags(s) & F_FULL_COLLISION; }
inline bool solid_render(int s) { return flags(s) & F_SOLID_RENDER; }
inline bool sturdy(int s, int d, int type = FULL) { return flags(s) >> (16 + type * 6 + d) & 1; }
inline int light_dampening(int s) { return int(flags(s) >> 34 & 15); }
inline int light_emission(int s) { return int(flags(s) >> 38 & 15); }
inline int fluid_type(int s) { return int(flags(s) >> 42 & 7); }
inline int fluid_amount(int s) { return int(flags(s) >> 45 & 15); }
inline bool fluid_falling(int s) { return flags(s) >> 49 & 1; }
inline bool has_fluid(int s) { return fluid_type(s) != EMPTY; }
inline bool is_water(int s) { int t = fluid_type(s); return t == WATER || t == FLOWING_WATER; }
inline bool is_lava(int s) { int t = fluid_type(s); return t == LAVA || t == FLOWING_LAVA; }
inline bool fluid_source(int s) { int t = fluid_type(s); return t == WATER || t == LAVA; }
inline const std::vector<Box>& collision(int s) { return T.shapes[T.shape[size_t(s)]]; }
inline bool no_collision(int s) { return collision(s).empty(); }

inline int block(int s) { return block_index(s); }
inline int block_id(const char* name) {  // "stone" -> block index
    auto it = block_by_name.find(std::string("minecraft:") + name);
    return it == block_by_name.end() ? -1 : it->second;
}
inline std::vector<int> default_states;  // per block (blocks.json "default")
inline const std::string& cls(int s) { return T.cls[size_t(block(s))]; }
inline bool has_class(int s, const char* c) {  // instanceof (simple name anywhere in the chain)
    const std::string& ch = cls(s);
    size_t n = strlen(c), at = 0;
    while ((at = ch.find(c, at)) != std::string::npos) {
        if ((at == 0 || ch[at - 1] == '<') && (at + n == ch.size() || ch[at + n] == '<')) return true;
        at += n;
    }
    return false;
}
inline const std::vector<bool>& tag(const char* name) {
    auto it = T.tags.find(name);
    return it == T.tags.end() ? T.empty_tag : it->second;
}
inline bool is(int s, const std::vector<bool>& t) { return !t.empty() && t[size_t(block(s))]; }

// ---------- state properties ----------
inline int prop_index(int s, std::string_view prop, int* stride, int* n) {
    const BlockInfo& b = block_of(s);
    int st = 1;
    for (int k = int(b.props.size()) - 1; k >= 0; k--) {
        int cnt = int(b.props[size_t(k)].values.size());
        if (b.props[size_t(k)].name == prop) {
            *stride = st, *n = cnt;
            return k;
        }
        st *= cnt;
    }
    return -1;
}
inline bool has(int s, std::string_view prop) {
    int st, n;
    return prop_index(s, prop, &st, &n) >= 0;
}
inline std::string_view get(int s, std::string_view prop) {
    int st, n, k = prop_index(s, prop, &st, &n);
    if (k < 0) return {};
    const BlockInfo& b = block_of(s);
    return b.props[size_t(k)].values[size_t((s - b.first) / st % n)];
}
inline int geti(int s, std::string_view prop) {
    auto v = get(s, prop);
    int r = 0;
    for (char c : v) r = r * 10 + (c - '0');
    return r;
}
inline bool getb(int s, std::string_view prop) { return get(s, prop) == "true"; }
inline int getdir(int s, std::string_view prop = "facing") { return dir_by_name(get(s, prop)); }
inline int with(int s, std::string_view prop, std::string_view value) {
    int st, n, k = prop_index(s, prop, &st, &n);
    if (k < 0) return s;
    const BlockInfo& b = block_of(s);
    auto& vals = b.props[size_t(k)].values;
    for (int i = 0; i < n; i++)
        if (vals[size_t(i)] == value) {
            int cur = (s - b.first) / st % n;
            return s + (i - cur) * st;
        }
    return s;
}
inline int with(int s, std::string_view prop, int v) { return with(s, prop, std::to_string(v)); }
inline int with(int s, std::string_view prop, bool v) { return with(s, prop, std::string_view(v ? "true" : "false")); }
inline int with(int s, std::string_view prop, const char* v) { return with(s, prop, std::string_view(v)); }
inline int withdir(int s, std::string_view prop, int d) { return with(s, prop, std::string_view(DIR_NAME[d])); }
inline bool waterlogged(int s) { return getb(s, "waterlogged"); }

inline int def(int b) { return default_states[size_t(b)]; }
inline int def(const char* name) { return def(block_id(name)); }
inline bool is_block(int s, int b) { return block(s) == b; }

// Block indices used all over (resolved once at load).
struct Ids {
    int air, cave_air, void_air, water, lava, stone, cobblestone, obsidian, basalt, soul_soil, blue_ice, bubble_column,
        sand, red_sand, gravel, dirt, grass_block, mycelium, farmland, ice, frosted_ice, packed_ice, snow,
        snow_block, powder_snow, fire, soul_fire, netherrack, soul_sand, magma_block, kelp, kelp_plant, seagrass,
        tall_seagrass, sugar_cane, cactus, bamboo, bamboo_sapling, dirt_path, mud, coarse_dirt, rooted_dirt,
        podzol, moss_block, clay, sponge, wet_sponge, scaffolding, structure_void, nether_portal, end_portal,
        end_gateway, ladder, bedrock, cactus_flower, lily_pad, vine, pumpkin, melon, attached_pumpkin_stem,
        attached_melon_stem, pumpkin_stem, melon_stem, tnt, glass, end_stone, brown_mushroom, red_mushroom,
        mycelium_, cobweb, short_grass, tall_grass, fern, large_fern, dead_bush, moss_carpet, crimson_nylium,
        warped_nylium, light;
};
inline Ids ID;

inline bool load(const std::string& path = "data/level.json") {
    Json j;
    try {
        j = Json::parseFile(path);
    } catch (const std::exception& e) {
        log_error("%s: %s (run: python3 tools/gen_level_data.py)", path.c_str(), e.what());
        return false;
    }
    for (auto& sh : j["shapes"].arr) {
        std::vector<Box> v;
        for (size_t i = 0; i + 5 < sh.arr.size(); i += 6)
            v.push_back({sh[i].asDouble(), sh[i + 1].asDouble(), sh[i + 2].asDouble(), sh[i + 3].asDouble(),
                         sh[i + 4].asDouble(), sh[i + 5].asDouble()});
        T.shapes.push_back(std::move(v));
    }
    for (auto& s : j["states"].arr) {
        T.shape.push_back(uint16_t(s[0].asInt()));
        T.flags.push_back(uint64_t(s[1].num));
    }
    for (auto& b : j["blocks"].arr) {
        T.cls.push_back(b[0].asString());
        T.burn.push_back(uint8_t(b[1].asInt()));
        T.ignite.push_back(uint8_t(b[2].asInt()));
        default_states.push_back(b[3].asInt());
        T.next_age.push_back(int16_t(b[4].asInt()));
        T.age.push_back(int8_t(b[5].asInt()));
    }
    for (auto& [k, v] : j["tags"].obj) {
        std::vector<bool> t(block_info.size());
        for (auto& x : v.arr) t[size_t(x.asInt())] = true;
        T.tags[k] = std::move(t);
    }
#define R(n) ID.n = block_id(#n);
    R(air) R(cave_air) R(void_air) R(water) R(lava) R(stone) R(cobblestone) R(obsidian) R(basalt) R(soul_soil)
    R(blue_ice) R(bubble_column) R(sand) R(red_sand) R(gravel) R(dirt) R(grass_block) R(mycelium) R(farmland) R(ice)
    R(frosted_ice) R(packed_ice) R(snow) R(snow_block) R(powder_snow) R(fire) R(soul_fire) R(netherrack) R(soul_sand)
    R(magma_block) R(kelp) R(kelp_plant) R(seagrass) R(tall_seagrass) R(sugar_cane) R(cactus) R(bamboo)
    R(bamboo_sapling) R(dirt_path) R(mud) R(coarse_dirt) R(rooted_dirt) R(podzol) R(moss_block) R(clay) R(sponge)
    R(wet_sponge) R(scaffolding) R(structure_void) R(nether_portal) R(end_portal) R(end_gateway) R(ladder) R(bedrock)
    R(cactus_flower) R(lily_pad) R(vine) R(pumpkin) R(melon) R(attached_pumpkin_stem) R(attached_melon_stem)
    R(pumpkin_stem) R(melon_stem) R(tnt) R(glass) R(end_stone) R(brown_mushroom) R(red_mushroom) R(cobweb)
    R(short_grass) R(tall_grass) R(fern) R(large_fern) R(dead_bush) R(moss_carpet) R(crimson_nylium) R(warped_nylium)
    R(light)
#undef R
    return T.flags.size() == size_t(BLOCK_STATE_COUNT);
}

}  // namespace lvl
