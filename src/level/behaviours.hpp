#pragma once
// Block behaviours (BlockBehaviour overrides of vanilla 26.3 block classes), dispatched by a per-block kind
// derived from the class chain in data/level.json. Tick thread only.
#include "fluids.hpp"

#include <unordered_set>

namespace lvl {

// place_rules.hpp (walls, chests, wire, rails, banners, skulls, coral, vines, ...).
inline bool can_survive_more(int s, const Pos& p);
inline int update_shape_more(int s, const Pos& p, int dir, int ns);
inline void rail_on_place(int state, const Pos& p, int old, bool piston);
inline bool veg_may_place_on_more(int plant, int below, int* r);

enum Kind : uint8_t {
    K_NONE, K_LIQUID, K_BUBBLE_COLUMN, K_FALLING, K_CONCRETE_POWDER, K_ANVIL,
    K_VEGETATION, K_CROP, K_STEM, K_ATTACHED_STEM, K_SAPLING, K_MUSHROOM, K_NETHER_WART, K_BERRY_BUSH, K_LILY_PAD,
    K_DOUBLE_PLANT, K_TORCH, K_WALL_TORCH, K_DOOR, K_BED, K_SNOW_LAYER, K_CARPET, K_RAIL, K_LADDER, K_LANTERN,
    K_FACE_ATTACHED, K_PRESSURE_PLATE, K_STANDING_SIGN, K_WALL_SIGN, K_FENCE, K_BARS, K_FENCE_GATE, K_STAIRS,
    K_SNOWY, K_SPREADING, K_LEAVES, K_ICE, K_FROSTED_ICE, K_FARMLAND, K_FIRE, K_SOUL_FIRE, K_SUGAR_CANE, K_CACTUS,
    K_CACTUS_FLOWER, K_GROWING_HEAD, K_GROWING_BODY, K_BAMBOO, K_BAMBOO_SAPLING, K_SEAGRASS, K_TALL_SEAGRASS,
    K_SPONGE, K_TRAPDOOR, K_DRY_VEGETATION,
};
inline std::vector<uint8_t> kinds;  // per block
inline int kind(int s) { return kinds[size_t(block(s))]; }

inline void init_kinds() {
    kinds.assign(block_info.size(), K_NONE);
    static const std::pair<const char*, Kind> by_class[] = {  // most specific first
        {"LiquidBlock", K_LIQUID}, {"BubbleColumnBlock", K_BUBBLE_COLUMN}, {"ConcretePowderBlock", K_CONCRETE_POWDER},
        {"AnvilBlock", K_ANVIL}, {"FallingBlock", K_FALLING}, {"AttachedStemBlock", K_ATTACHED_STEM},
        {"StemBlock", K_STEM}, {"CropBlock", K_CROP}, {"SaplingBlock", K_SAPLING}, {"MushroomBlock", K_MUSHROOM},
        {"NetherWartBlock", K_NETHER_WART}, {"SweetBerryBushBlock", K_BERRY_BUSH}, {"LilyPadBlock", K_LILY_PAD},
        {"DryVegetationBlock", K_DRY_VEGETATION}, {"TallSeagrassBlock", K_TALL_SEAGRASS}, {"SeagrassBlock", K_SEAGRASS},
        {"DoublePlantBlock", K_DOUBLE_PLANT}, {"CactusFlowerBlock", K_CACTUS_FLOWER},
        {"WallTorchBlock", K_WALL_TORCH}, {"RedstoneWallTorchBlock", K_WALL_TORCH}, {"BaseTorchBlock", K_TORCH},
        {"DoorBlock", K_DOOR}, {"AbstractBedBlock", K_BED}, {"SnowLayerBlock", K_SNOW_LAYER}, {"CarpetBlock", K_CARPET},
        {"BaseRailBlock", K_RAIL}, {"LadderBlock", K_LADDER}, {"LanternBlock", K_LANTERN},
        {"FaceAttachedHorizontalDirectionalBlock", K_FACE_ATTACHED}, {"BasePressurePlateBlock", K_PRESSURE_PLATE},
        {"StandingSignBlock", K_STANDING_SIGN}, {"WallSignBlock", K_WALL_SIGN}, {"FenceGateBlock", K_FENCE_GATE},
        {"FenceBlock", K_FENCE}, {"IronBarsBlock", K_BARS}, {"StairBlock", K_STAIRS}, {"TrapDoorBlock", K_TRAPDOOR},
        {"SpreadingSnowyBlock", K_SPREADING}, {"SnowyBlock", K_SNOWY}, {"LeavesBlock", K_LEAVES},
        {"FrostedIceBlock", K_FROSTED_ICE}, {"IceBlock", K_ICE}, {"FarmlandBlock", K_FARMLAND},
        {"SoulFireBlock", K_SOUL_FIRE}, {"FireBlock", K_FIRE}, {"SugarCaneBlock", K_SUGAR_CANE}, {"CactusBlock", K_CACTUS},
        {"GrowingPlantHeadBlock", K_GROWING_HEAD}, {"GrowingPlantBodyBlock", K_GROWING_BODY},
        {"BambooStalkBlock", K_BAMBOO}, {"BambooSaplingBlock", K_BAMBOO_SAPLING}, {"SpongeBlock", K_SPONGE},
        {"VegetationBlock", K_VEGETATION},
    };
    for (size_t b = 0; b < block_info.size(); b++) {
        int s = def(int(b));
        for (auto& [c, k] : by_class)
            if (has_class(s, c)) { kinds[b] = k; break; }
    }
}

// ---------- shared helpers ----------
inline bool can_support_center(const Pos& p, int dir) { return sturdy(get(p), dir, CENTER); }  // Block.canSupportCenter
inline bool can_support_rigid(const Pos& p) { return sturdy(get(p), UP, RIGID); }
inline bool face_sturdy(const Pos& p, int dir) { return sturdy(get(p), dir, FULL); }
inline bool is_empty_block(const Pos& p) { return is_air(get(p)); }  // LevelReader.isEmptyBlock
inline bool face_full(int s, int dir) {  // Block.isFaceFull(collision shape, dir)
    static const std::vector<Box> none;
    bool positive = dir == UP || dir == SOUTH || dir == EAST;
    return positive ? merged_face_occludes(collision(s), none, dir) : merged_face_occludes(none, collision(s), dir);
}
inline int air() { return blocks::air; }
inline int age_of(int s) { return geti(s, "age"); }
inline int max_age(int s) {  // last value of the "age" property
    for (auto& p : block_of(s).props)
        if (p.name == "age") return std::stoi(p.values.back());
    return 0;
}
inline int raw_brightness0(const Pos& p) { return raw_light(p, 0); }                       // getRawBrightness(pos, 0)
inline int max_local_brightness(const Pos& p) { return raw_light(p, sky_darken()); }        // getMaxLocalRawBrightness
inline int block_brightness(const Pos& p) { return block_light(p); }                        // getBrightness(BLOCK)

// ---------- bubble column ----------
inline bool bubble_can_occupy(int s) {
    if (block(s) == ID.bubble_column) return true;
    return block(s) == ID.water && fluid_type(s) == WATER && fluid_amount(s) >= 8;
}
inline void bubble_update_column(const Pos& at, int occupy, int below) {  // BubbleColumnBlock.updateColumn
    if (!bubble_can_occupy(occupy)) return;
    int col;
    if (block(below) == ID.bubble_column) col = below;
    else if (is(below, tag("enables_bubble_column_push_up"))) col = with(def(ID.bubble_column), "drag", false);
    else if (is(below, tag("enables_bubble_column_drag_down"))) col = with(def(ID.bubble_column), "drag", true);
    else col = block(occupy) == ID.bubble_column ? blocks::water : occupy;
    set(at, col, UPDATE_CLIENTS);
    for (Pos p = at.above(); bubble_can_occupy(get(p)); p = p.above())
        if (!set(p, col, UPDATE_CLIENTS)) return;
}
inline bool bubble_can_survive(const Pos& p) {
    int b = get(p.below());
    return block(b) == ID.bubble_column || is(b, tag("enables_bubble_column_push_up")) || is(b, tag("enables_bubble_column_drag_down"));
}

// ---------- falling blocks (FallingBlock + falling.hpp entity) ----------
inline bool falling_is_free(int s) {  // FallingBlock.isFree
    return is_air(s) || is(s, tag("fire")) || liquid(s) || can_be_replaced(s);
}
inline void start_falling(const Pos& p, int state);  // falling.hpp: FallingBlockEntity.fall
inline int concrete_of(int powder) {
    std::string n = block_of(powder).name;  // minecraft:white_concrete_powder
    auto it = block_by_name.find(n.substr(0, n.size() - 7));
    return it == block_by_name.end() ? powder : def(it->second);
}
inline bool concrete_can_solidify(int s) { return is_water(s); }
inline bool concrete_touches_liquid(const Pos& p) {
    for (int d = 0; d < 6; d++) {
        int bs = get(p);
        if (d != DOWN || concrete_can_solidify(bs)) {
            Pos t = p.rel(d);
            bs = get(t);
            if (concrete_can_solidify(bs) && !sturdy(bs, OPP[d])) return true;
        }
    }
    return false;
}

// ---------- fire (FireBlock / SoulFireBlock / BaseFireBlock) ----------
inline int burn_odds(int s) { return waterlogged(s) ? 0 : T.burn[size_t(block(s))]; }
inline int ignite_odds(int s) { return waterlogged(s) ? 0 : T.ignite[size_t(block(s))]; }
inline bool can_burn(int s) { return ignite_odds(s) > 0; }
inline bool fire_valid_location(const Pos& p) {
    for (int d = 0; d < 6; d++)
        if (can_burn(get(p.rel(d)))) return true;
    return false;
}
inline int fire_placement_state(const Pos& p) {  // FireBlock.getStateForPlacement(level, pos)
    int below = get(p.below());
    int s = def(ID.fire);
    if (!can_burn(below) && !sturdy(below, UP)) {
        static const char* props[6] = {nullptr, "up", "north", "south", "west", "east"};
        for (int d = 1; d < 6; d++) s = with(s, props[d], can_burn(get(p.rel(d))));
    }
    return s;
}
inline bool soul_fire_base(int s) { return is(s, tag("soul_fire_base_blocks")); }
inline int fire_state_for(const Pos& p) {  // BaseFireBlock.getState
    return soul_fire_base(get(p.below())) ? def(ID.soul_fire) : fire_placement_state(p);
}
inline int fire_state_with_age(const Pos& p, int age) {
    int s = fire_state_for(p);
    return block(s) == ID.fire ? with(s, "age", age) : s;
}
inline int kind_of_block(int b) { return kinds[size_t(b)]; }
inline bool fire_can_survive(int s, const Pos& p) {
    if (kind(s) == K_SOUL_FIRE) return soul_fire_base(get(p.below()));
    return face_sturdy(p.below(), UP) || fire_valid_location(p);
}
inline bool is_near_rain(const Pos& p) {
    return raining_at(p) || raining_at(p.rel(WEST)) || raining_at(p.rel(EAST)) || raining_at(p.rel(NORTH)) || raining_at(p.rel(SOUTH));
}
inline int fire_ignite_odds_at(const Pos& p) {
    if (!is_empty_block(p)) return 0;
    int o = 0;
    for (int d = 0; d < 6; d++) o = std::max(o, ignite_odds(get(p.rel(d))));
    return o;
}
inline void fire_check_burn_out(const Pos& p, int chance, int age) {
    auto& r = rng();
    int odds = burn_odds(get(p));
    if (r.nextInt(chance) < odds) {
        if (r.nextInt(age + 10) < 5 && !raining_at(p)) set(p, fire_state_with_age(p, std::min(age + r.nextInt(5) / 4, 15)));
        else remove(p);
        // ponytail: TNT is removed instead of primed (no TNT entity yet).
    }
}
inline bool can_spread_fire_around(const Pos& p);
inline void fire_tick(int state, const Pos& p) {
    auto& r = rng();
    schedule_block(p, block(state), 30 + r.nextInt(10));
    if (!can_spread_fire_around(p)) return;
    if (!fire_can_survive(state, p)) remove(p);
    int below = get(p.below());
    bool infini = is(below, tag("infiniburn_overworld"));
    int age = geti(state, "age");
    if (!infini && raining() && is_near_rain(p) && r.nextFloat() < 0.2f + age * 0.03f) {
        remove(p);
        return;
    }
    int nage = std::min(15, age + r.nextInt(3) / 2);
    if (age != nage) {
        state = with(state, "age", nage);
        set(p, state, UPDATE_NONE);
    }
    if (!infini) {
        if (!fire_valid_location(p)) {
            if (!face_sturdy(p.below(), UP) || age > 3) remove(p);
            return;
        }
        if (age == 15 && r.nextInt(4) == 0 && !can_burn(get(p.below()))) {
            remove(p);
            return;
        }
    }
    fire_check_burn_out(p.rel(EAST), 300, age);
    fire_check_burn_out(p.rel(WEST), 300, age);
    fire_check_burn_out(p.below(), 250, age);
    fire_check_burn_out(p.above(), 250, age);
    fire_check_burn_out(p.rel(NORTH), 300, age);
    fire_check_burn_out(p.rel(SOUTH), 300, age);
    for (int xx = -1; xx <= 1; xx++)
        for (int zz = -1; zz <= 1; zz++)
            for (int yy = -1; yy <= 4; yy++) {
                if (xx == 0 && yy == 0 && zz == 0) continue;
                int rate = 100 + (yy > 1 ? (yy - 1) * 100 : 0);
                Pos t = p.off(xx, yy, zz);
                int ig = fire_ignite_odds_at(t);
                if (ig <= 0) continue;
                int odds = (ig + 40 + world_difficulty * 7) / (age + 30);
                if (odds > 0 && r.nextInt(rate) <= odds && (!raining() || !is_near_rain(t)))
                    set(t, fire_state_with_age(t, std::min(15, age + r.nextInt(5) / 4)));
            }
}

// ---------- plants ----------
inline bool veg_may_place_on(int plant, int below, const Pos& bp) {  // VegetationBlock.mayPlaceOn + overrides
    if (int r; veg_may_place_on_more(plant, below, &r)) return r;
    switch (kind(plant)) {
        case K_CROP: return is(below, tag("supports_crops"));
        case K_STEM:
        case K_ATTACHED_STEM: {
            bool melon = block_of(plant).name.find("melon") != std::string::npos;
            return is(below, tag(melon ? "supports_melon_stem" : "supports_pumpkin_stem"));
        }
        case K_MUSHROOM: return solid_render(below);
        case K_NETHER_WART: return is(below, tag("supports_nether_wart"));
        case K_DRY_VEGETATION: return is(below, tag("supports_dry_vegetation"));
        case K_SEAGRASS:
        case K_TALL_SEAGRASS: return sturdy(below, UP) && !is(below, tag("cannot_support_seagrass"));
        case K_CACTUS_FLOWER: return is(below, tag("support_override_cactus_flower")) || sturdy(below, UP, CENTER);
        case K_LILY_PAD: {
            int above = get(bp.above());
            return (fluid_type(below) == WATER || is(below, tag("supports_lily_pad"))) && !has_fluid(above);
        }
        default: break;
    }
    const std::string& n = block_of(plant).name;
    if (n == "minecraft:azalea" || n == "minecraft:flowering_azalea") return is(below, tag("supports_azalea"));
    if (n == "minecraft:mangrove_propagule") return is(below, tag("supports_mangrove_propagule"));
    if (n == "minecraft:wither_rose") return is(below, tag("supports_wither_rose"));
    if (n == "minecraft:nether_sprouts") return is(below, tag("supports_nether_sprouts"));
    if (n == "minecraft:warped_fungus") return is(below, tag("supports_warped_fungus"));
    if (n == "minecraft:crimson_fungus") return is(below, tag("supports_crimson_fungus"));
    if (n == "minecraft:warped_roots") return is(below, tag("supports_warped_roots"));
    if (n == "minecraft:crimson_roots") return is(below, tag("supports_crimson_roots"));
    return is(below, tag("supports_vegetation"));
}
inline bool veg_can_survive(int s, const Pos& p) {
    Pos bp = p.below();
    int below = get(bp);
    switch (kind(s)) {
        case K_CROP: return raw_brightness0(p) >= 8 && veg_may_place_on(s, below, bp);
        case K_MUSHROOM:
            if (is(below, tag("overrides_mushroom_light_requirement"))) return true;
            return raw_brightness0(p) < 13 && veg_may_place_on(s, below, bp);
        case K_DOUBLE_PLANT:
            if (get(s, "half") == "upper") return block(below) == block(s) && get(below, "half") == "lower";
            break;
        case K_TALL_SEAGRASS:
            if (get(s, "half") == "upper") return block(below) == block(s) && get(below, "half") == "lower";
            break;
        default: break;
    }
    return veg_may_place_on(s, below, bp);
}
// CropBlock.getGrowthSpeed
inline float growth_speed(int b, const Pos& p) {
    float speed = 1.0f;
    Pos below = p.below();
    for (int xx = -1; xx <= 1; xx++)
        for (int zz = -1; zz <= 1; zz++) {
            float bs = 0;
            int s = get(below.off(xx, 0, zz));
            if (is(s, tag("grows_crops"))) {
                bs = 1.0f;
                if (has(s, "moisture") && geti(s, "moisture") > 0) bs = 3.0f;
            }
            if (xx != 0 || zz != 0) bs /= 4.0f;
            speed += bs;
        }
    auto isb = [&](const Pos& q) { return block(get(q)) == b; };
    Pos n = p.rel(NORTH), s = p.rel(SOUTH), w = p.rel(WEST), e = p.rel(EAST);
    bool horizontal = isb(w) || isb(e), vertical = isb(n) || isb(s);
    if (horizontal && vertical) speed /= 2.0f;
    else if (isb(w.rel(NORTH)) || isb(e.rel(NORTH)) || isb(e.rel(SOUTH)) || isb(w.rel(SOUTH))) speed /= 2.0f;
    return speed;
}

// ---------- double blocks ----------
inline void prevent_drop_from_bottom_part(const Pos& p, int s) {  // DoublePlantBlock.preventDropFromBottomPart
    if (get(s, "half") != "upper") return;
    Pos bp = p.below();
    int bs = get(bp);
    if (block(bs) == block(s) && get(bs, "half") == "lower") {
        set(bp, is_water(bs) ? blocks::water : blocks::air, UPDATE_ALL | UPDATE_SUPPRESS_DROPS);
        level_event(2001, bp, bs);
    }
}
inline int bed_neighbour_dir(int s) {
    int f = getdir(s);
    return get(s, "part") == "foot" ? f : OPP[f];
}

// ---------- fences / bars / stairs ----------
inline bool exception_for_connection(int s) {  // Block.isExceptionForConnection
    const std::string& n = block_of(s).name;
    return has_class(s, "LeavesBlock") || n == "minecraft:barrier" || n == "minecraft:carved_pumpkin" ||
           n == "minecraft:jack_o_lantern" || n == "minecraft:melon" || n == "minecraft:pumpkin" || is(s, tag("shulker_boxes"));
}
inline bool fence_connects(int fence, int s, bool face_solid, int dir) {  // FenceBlock.connectsTo
    bool same = is(s, tag("fences")) && is(s, tag("wooden_fences")) == is(fence, tag("wooden_fences"));
    bool gate = kind(s) == K_FENCE_GATE && axis(getdir(s)) == axis(clockwise(dir));
    return (!exception_for_connection(s) && face_solid) || same || gate;
}
inline bool bars_attach(int s, bool face_solid) {
    return (!exception_for_connection(s) && face_solid) || kind(s) == K_BARS || is(s, tag("walls"));
}
inline bool cross_connect(int s, const Pos& p, int d) {
    int ns = get(p.rel(d));
    bool solid = sturdy(ns, OPP[d]);
    return kind(s) == K_FENCE ? fence_connects(s, ns, solid, OPP[d]) : bars_attach(ns, solid);
}
inline bool is_stairs(int s) { return kind(s) == K_STAIRS; }
inline const char* stairs_shape(int s, const Pos& p) {  // StairBlock.getStairsShape
    int facing = getdir(s);
    auto half = get(s, "half");
    auto can_take = [&](int d) {
        int n = get(p.rel(d));
        return !is_stairs(n) || getdir(n) != facing || get(n, "half") != half;
    };
    int behind = get(p.rel(facing));
    if (is_stairs(behind) && get(behind, "half") == half) {
        int bf = getdir(behind);
        if (axis(bf) != axis(facing) && can_take(OPP[bf])) return bf == ccw(facing) ? "outer_left" : "outer_right";
    }
    int front = get(p.rel(OPP[facing]));
    if (is_stairs(front) && get(front, "half") == half) {
        int ff = getdir(front);
        if (axis(ff) != axis(facing) && can_take(ff)) return ff == ccw(facing) ? "inner_left" : "inner_right";
    }
    return "straight";
}

// ---------- leaves ----------
inline int leaf_distance_at(int s) {
    if (is(s, tag("prevents_nearby_leaf_decay"))) return 0;
    return has(s, "distance") ? geti(s, "distance") : 7;
}
inline int leaves_update_distance(int s, const Pos& p) {
    int d = 7;
    for (int k = 0; k < 6 && d > 1; k++) d = std::min(d, leaf_distance_at(get(p.rel(k))) + 1);
    return with(s, "distance", d);
}

// ---------- snow / spreading dirt ----------
inline bool snow_can_survive(const Pos& p) {
    int below = get(p.below());
    if (is(below, tag("cannot_support_snow_layer"))) return false;
    if (is(below, tag("support_override_snow_layer"))) return true;
    return face_full(below, UP) || (block(below) == ID.snow && geti(below, "layers") == 8);
}
inline bool snowy_setting(int above) { return is(above, tag("snow")); }
inline bool spreading_can_stay_alive(const Pos& p) {
    int above = get(p.above());
    if (block(above) == ID.snow && geti(above, "layers") == 1) return true;
    if (has_fluid(above) && fluid_amount(above) == 8) return false;
    return flags(above) & F_GRASS_LIVES_UNDER;
}
inline bool spreading_can_propagate(const Pos& p) { return spreading_can_stay_alive(p) && !is_water(get(p.above())); }

// ---------- growing plants (kelp, vines) ----------
struct Growing { const char* head; const char* body; int dir; double prob; };
inline const Growing* growing_of(int s) {
    static const Growing g[] = {{"kelp", "kelp_plant", UP, 0.14}, {"weeping_vines", "weeping_vines_plant", DOWN, 0.1},
                                {"twisting_vines", "twisting_vines_plant", UP, 0.1}, {"cave_vines", "cave_vines_plant", DOWN, 0.1}};
    const std::string& n = block_of(s).name;
    for (auto& x : g)
        if (n == std::string("minecraft:") + x.head || n == std::string("minecraft:") + x.body) return &x;
    return nullptr;
}
inline bool growing_is_kelp(const Growing& g) { return std::string(g.head) == "kelp"; }
inline bool growing_can_grow_into(const Growing& g, int s) {
    return growing_is_kelp(g) ? block(s) == ID.water : is_air(s);  // KelpBlock / NetherVines / CaveVines
}
inline bool growing_can_survive(int s, const Pos& p) {
    const Growing* g = growing_of(s);
    if (!g) return true;
    Pos ap = p.rel(OPP[g->dir]);
    int at = get(ap);
    if (growing_is_kelp(*g) && is(at, tag("cannot_support_kelp"))) return false;
    const std::string& n = block_of(at).name;
    return n == std::string("minecraft:") + g->head || n == std::string("minecraft:") + g->body || sturdy(at, g->dir);
}

// ==================================================================================================
// BlockBehaviour.canSurvive
// ==================================================================================================
inline bool can_survive(int state, Pos pos) {
    switch (kind(state)) {
        case K_BUBBLE_COLUMN: return bubble_can_survive(pos);
        case K_VEGETATION: case K_CROP: case K_STEM: case K_ATTACHED_STEM: case K_SAPLING: case K_MUSHROOM:
        case K_NETHER_WART: case K_BERRY_BUSH: case K_LILY_PAD: case K_DOUBLE_PLANT: case K_DRY_VEGETATION:
        case K_SEAGRASS: case K_TALL_SEAGRASS: case K_CACTUS_FLOWER:
            return veg_can_survive(state, pos);
        case K_TORCH: return can_support_center(pos.below(), UP);
        case K_WALL_TORCH: { int f = getdir(state); return face_sturdy(pos.rel(OPP[f]), f); }
        case K_DOOR: {
            int b = get(pos.below());
            return get(state, "half") == "lower" ? sturdy(b, UP) : block(b) == block(state);
        }
        case K_SNOW_LAYER: return snow_can_survive(pos);
        case K_CARPET: return !is_empty_block(pos.below());
        case K_RAIL: return can_support_rigid(pos.below());
        case K_LADDER: { int f = getdir(state); return face_sturdy(pos.rel(OPP[f]), f); }
        case K_LANTERN: {
            int d = getb(state, "hanging") ? UP : DOWN;  // getConnectedDirection().getOpposite()
            return can_support_center(pos.rel(d), OPP[d]);
        }
        case K_FACE_ATTACHED: {
            auto face = get(state, "face");
            int conn = face == "ceiling" ? DOWN : face == "floor" ? UP : getdir(state);
            int d = OPP[conn];
            return face_sturdy(pos.rel(d), OPP[d]);
        }
        case K_PRESSURE_PLATE: return can_support_rigid(pos.below()) || can_support_center(pos.below(), UP);
        case K_STANDING_SIGN: return is_solid(get(pos.below()));
        case K_WALL_SIGN: return is_solid(get(pos.rel(OPP[getdir(state)])));
        case K_FIRE: case K_SOUL_FIRE: return fire_can_survive(state, pos);
        case K_SUGAR_CANE: {
            int b = get(pos.below());
            if (block(b) == block(state)) return true;
            if (!is(b, tag("supports_sugar_cane"))) return false;
            for (int d : HORIZONTAL) {
                int n = get(pos.below().rel(d));
                if (is_water(n) || is(n, tag("supports_sugar_cane_adjacently"))) return true;
            }
            return false;
        }
        case K_CACTUS: {
            for (int d : HORIZONTAL) {
                int n = get(pos.rel(d));
                if (is_solid(n) || is_lava(n)) return false;
            }
            int b = get(pos.below());
            return (block(b) == block(state) || is(b, tag("supports_cactus"))) && !liquid(get(pos.above()));
        }
        case K_FARMLAND: return !is_solid(get(pos.above())) || is(get(pos.above()), tag("maintains_farmland"));
        case K_GROWING_HEAD: case K_GROWING_BODY: return growing_can_survive(state, pos);
        case K_BAMBOO: case K_BAMBOO_SAPLING: return is(get(pos.below()), tag("supports_bamboo"));
        default: return can_survive_more(state, pos);
    }
}

// ==================================================================================================
// BlockBehaviour.updateShape
// ==================================================================================================
// Nether portal hooks (game/portal.hpp): BaseFireBlock.onPlace lighting a frame, NetherPortalBlock.updateShape.
inline bool (*fire_lights_portal)(int x, int y, int z) = nullptr;
inline int (*portal_update_shape)(int state, int x, int y, int z, int dir, int nstate) = nullptr;

inline int update_shape(int state, Pos pos, int dir, Pos npos, int nstate) {
    (void)npos;
    if (is_nether_portal(uint16_t(state)) && portal_update_shape) return portal_update_shape(state, pos.x, pos.y, pos.z, dir, nstate);
    int k = kind(state);
    switch (k) {
        case K_LIQUID: return liquid_update_shape(state, pos, dir, nstate);
        case K_BUBBLE_COLUMN:
            schedule_fluid(pos, WATER, tick_delay(WATER));
            if (!bubble_can_survive(pos) || dir == DOWN || (dir == UP && block(nstate) != ID.bubble_column && bubble_can_occupy(nstate)))
                schedule_block(pos, block(state), 5);
            return state;
        case K_CONCRETE_POWDER:
            if (concrete_touches_liquid(pos)) return concrete_of(state);
            [[fallthrough]];
        case K_FALLING:
        case K_ANVIL:
            schedule_block(pos, block(state), 2);
            schedule_waterlogged(state, pos);
            return state;
        case K_VEGETATION: case K_CROP: case K_STEM: case K_SAPLING: case K_MUSHROOM: case K_NETHER_WART:
        case K_BERRY_BUSH: case K_LILY_PAD: case K_DRY_VEGETATION: case K_CACTUS_FLOWER: case K_SEAGRASS:
            if (k == K_SEAGRASS) schedule_fluid(pos, WATER, tick_delay(WATER));
            if (!can_survive(state, pos)) return air();
            schedule_waterlogged(state, pos);
            return state;
        case K_ATTACHED_STEM: {
            bool melon = block_of(state).name.find("melon") != std::string::npos;
            if (block_of(nstate).name != (melon ? "minecraft:melon" : "minecraft:pumpkin") && dir == getdir(state))
                return with(def(melon ? "melon_stem" : "pumpkin_stem"), "age", 7);
            return can_survive(state, pos) ? state : air();
        }
        case K_DOUBLE_PLANT:
        case K_TALL_SEAGRASS: {
            bool lower = get(state, "half") == "lower";
            if (k == K_TALL_SEAGRASS) schedule_fluid(pos, WATER, tick_delay(WATER));
            if (axis(dir) != 1 || lower != (dir == UP) || (block(nstate) == block(state) && get(nstate, "half") != get(state, "half"))) {
                if (lower && dir == DOWN && !can_survive(state, pos)) return air();
                schedule_waterlogged(state, pos);
                return state;
            }
            return air();
        }
        case K_DOOR: {
            bool lower = get(state, "half") == "lower";
            if (axis(dir) != 1 || lower != (dir == UP)) return lower && dir == DOWN && !can_survive(state, pos) ? air() : state;
            return kind(nstate) == K_DOOR && get(nstate, "half") != get(state, "half") ? with(nstate, "half", get(state, "half")) : air();
        }
        case K_BED:
            if (dir == bed_neighbour_dir(state))
                return block(nstate) == block(state) && get(nstate, "part") != get(state, "part") ? with(state, "occupied", getb(nstate, "occupied")) : air();
            return state;
        case K_TORCH: return dir == DOWN && !can_survive(state, pos) ? air() : state;
        case K_WALL_TORCH:
        case K_WALL_SIGN:
        case K_LADDER:
            if (OPP[dir] == getdir(state) && !can_survive(state, pos)) return air();
            schedule_waterlogged(state, pos);
            return state;
        case K_LANTERN: {
            schedule_waterlogged(state, pos);
            int conn_opp = getb(state, "hanging") ? UP : DOWN;
            return conn_opp == dir && !can_survive(state, pos) ? air() : state;
        }
        case K_FACE_ATTACHED: {
            auto face = get(state, "face");
            int conn = face == "ceiling" ? DOWN : face == "floor" ? UP : getdir(state);
            return OPP[conn] == dir && !can_survive(state, pos) ? air() : state;
        }
        case K_PRESSURE_PLATE:
        case K_STANDING_SIGN:
            schedule_waterlogged(state, pos);
            return dir == DOWN && !can_survive(state, pos) ? air() : state;
        case K_SNOW_LAYER:
        case K_CARPET: return !can_survive(state, pos) ? air() : state;
        case K_FENCE:
        case K_BARS:
            schedule_waterlogged(state, pos);
            if (axis(dir) == 1) return state;
            return with(state, DIR_NAME[dir], cross_connect(state, pos, dir));
        case K_FENCE_GATE:
            if (axis(clockwise(getdir(state))) != axis(dir)) return state;
            return with(state, "in_wall", is(nstate, tag("walls")) || is(get(pos.rel(OPP[dir])), tag("walls")));
        case K_STAIRS:
            schedule_waterlogged(state, pos);
            return axis(dir) != 1 ? with(state, "shape", stairs_shape(state, pos)) : state;
        case K_SNOWY:
        case K_SPREADING: return dir == UP ? with(state, "snowy", snowy_setting(nstate)) : state;
        case K_LEAVES: {
            schedule_waterlogged(state, pos);
            int dn = leaf_distance_at(nstate) + 1;
            if (dn != 1 || geti(state, "distance") != dn) schedule_block(pos, block(state), 1);
            return state;
        }
        case K_FARMLAND:
            if (dir == UP && !can_survive(state, pos)) schedule_block(pos, block(state), 1);
            return state;
        case K_FIRE: return can_survive(state, pos) ? fire_state_with_age(pos, geti(state, "age")) : air();
        case K_SOUL_FIRE: return can_survive(state, pos) ? state : air();
        case K_SUGAR_CANE:
        case K_CACTUS:
            if (!can_survive(state, pos)) schedule_block(pos, block(state), 1);
            return state;
        case K_GROWING_HEAD: {
            const Growing* g = growing_of(state);
            if (!g) return state;
            int body = def(g->body);
            if (dir == OPP[g->dir]) {
                if (!can_survive(state, pos)) schedule_block(pos, block(state), 1);
                else {
                    int n = get(pos.rel(g->dir));
                    if (block(n) == block(state) || block(n) == block(body)) return body;
                }
            }
            if (dir != g->dir || (block(nstate) != block(state) && block(nstate) != block(body))) {
                if (growing_is_kelp(*g)) schedule_fluid(pos, WATER, tick_delay(WATER));
                return state;
            }
            return body;
        }
        case K_GROWING_BODY: {
            const Growing* g = growing_of(state);
            if (!g) return state;
            int head = def(g->head);
            if (dir == OPP[g->dir] && !can_survive(state, pos)) schedule_block(pos, block(state), 1);
            if (dir == g->dir && block(nstate) != block(state) && block(nstate) != block(head))
                return has(head, "age") ? with(head, "age", rng().nextInt(25)) : head;
            if (growing_is_kelp(*g)) schedule_fluid(pos, WATER, tick_delay(WATER));
            return state;
        }
        case K_BAMBOO:
            if (!can_survive(state, pos)) schedule_block(pos, block(state), 1);
            return dir == UP && block(nstate) == ID.bamboo && geti(nstate, "age") > geti(state, "age") ? with(state, "age", 1 - geti(state, "age")) : state;
        case K_BAMBOO_SAPLING:
            if (!can_survive(state, pos)) return air();
            return dir == UP && block(nstate) == ID.bamboo ? def(ID.bamboo) : state;
        default:
            if (int r = update_shape_more(state, pos, dir, nstate); r >= 0) return r;
            schedule_waterlogged(state, pos);
            return state;
    }
}

// ==================================================================================================
// neighborChanged / onPlace / affectNeighborsAfterRemoval
// ==================================================================================================
inline void sponge_try_absorb(const Pos& p);
inline void neighbor_changed(int state, Pos pos, int src_block, bool piston) {
    (void)src_block;
    switch (kind(state)) {
        case K_LIQUID: liquid_neighbor_changed(state, pos); break;
        case K_RAIL: {
            if (block(get(pos)) != block(state)) break;
            auto shape = get(state, "shape");
            bool rm = !can_support_rigid(pos.below());
            if (!rm && shape.rfind("ascending_", 0) == 0) rm = !can_support_rigid(pos.rel(dir_by_name(shape.substr(10))));
            if (rm) {
                drop_resources(state, pos);
                remove(pos, piston);
            }
            break;  // ponytail: rail shapes don't re-connect (RailState); powered rails need redstone
        }
        case K_SPONGE:
            if (block(state) == ID.sponge) sponge_try_absorb(pos);
            break;
        default: break;
    }
}

inline void on_place(int state, Pos pos, int old, bool piston) {
    (void)piston;
    switch (kind(state)) {
        case K_LIQUID: liquid_on_place(state, pos); break;
        case K_FALLING:
        case K_ANVIL:
        case K_CONCRETE_POWDER: schedule_block(pos, block(state), 2); break;
        case K_FIRE:
        case K_SOUL_FIRE:
            if (block(old) != block(state) && fire_lights_portal && fire_lights_portal(pos.x, pos.y, pos.z)) break;
            if (block(old) != block(state) && !fire_can_survive(state, pos)) remove(pos);  // BaseFireBlock.onPlace
            else if (kind(state) == K_FIRE) schedule_block(pos, block(state), 30 + rng().nextInt(10));
            break;
        case K_SPONGE:
            if (block(old) != block(state) && block(state) == ID.sponge) sponge_try_absorb(pos);
            break;
        case K_RAIL: rail_on_place(state, pos, old, piston); break;
        default: break;
    }
}

inline void after_removal(int state, Pos pos, bool piston) { (void)state, (void)pos, (void)piston; }

// SpongeBlock.removeWaterBreadthFirstSearch: BlockPos.breadthFirstTraversal(pos, 6, 65, all directions).
inline void sponge_try_absorb(const Pos& p) {
    struct Q { Pos p; int depth; };
    std::deque<Q> q{{p, 0}};
    std::unordered_set<int64_t> seen{pos_key(p)};
    int visited = 0;
    while (!q.empty()) {
        Q cur = q.front();
        q.pop_front();
        if (!(cur.p == p)) {
            int s = get(cur.p);
            if (!is_water(s)) continue;
            if (kind(s) == K_LIQUID || kind(s) == K_BUBBLE_COLUMN) set(cur.p, blocks::air);
            else if (waterlogged(s)) set(cur.p, with(s, "waterlogged", false));
            else if (kind(s) == K_SEAGRASS || kind(s) == K_TALL_SEAGRASS || kind(s) == K_GROWING_HEAD || kind(s) == K_GROWING_BODY) {
                drop_resources(s, cur.p);
                set(cur.p, blocks::air);
            } else continue;
            if (++visited >= 65) break;
        }
        if (cur.depth < 6)
            for (int d = 0; d < 6; d++) {
                Pos n = cur.p.rel(d);
                if (seen.insert(pos_key(n)).second) q.push_back({n, cur.depth + 1});
            }
    }
    if (visited > 0) {
        set(p, def(ID.wet_sponge), UPDATE_CLIENTS);
        level_event(2001, p, blocks::water);
    }
}

// ==================================================================================================
// scheduled ticks
// ==================================================================================================
inline void block_tick(int state, Pos pos) {
    auto& r = rng();
    switch (kind(state)) {
        case K_LIQUID:
            if (bubble_column_occupies(state)) bubble_update_column(pos, get(pos), get(pos.below()));
            break;
        case K_BUBBLE_COLUMN: bubble_update_column(pos, state, get(pos.below())); break;
        case K_FALLING:
        case K_ANVIL:
        case K_CONCRETE_POWDER:
            if (falling_is_free(get(pos.below())) && pos.y >= world.min_y()) start_falling(pos, state);
            break;
        case K_LEAVES: set(pos, leaves_update_distance(state, pos)); break;
        case K_FARMLAND:
            if (!can_survive(state, pos)) set(pos, blocks::dirt);  // turnToBaseBlock (ponytail: no pushEntitiesUp)
            break;
        case K_FIRE: fire_tick(state, pos); break;
        case K_SUGAR_CANE: case K_CACTUS: case K_BAMBOO: case K_GROWING_HEAD: case K_GROWING_BODY:
            if (!can_survive(state, pos)) destroy(pos, true);
            break;
        case K_FROSTED_ICE: {  // FrostedIceBlock.tick (ponytail: neighbour-melt chain not ported)
            int age = geti(state, "age");
            if ((r.nextInt(3) == 0 || age > 3) && block_brightness(pos) > 11 - age - light_dampening(state)) {
                if (age < 3) set(pos, with(state, "age", age + 1), UPDATE_CLIENTS);
                else set(pos, blocks::water);
            }
            schedule_block(pos, block(state), 20 + r.nextInt(20));
            break;
        }
        default: break;
    }
}

// ==================================================================================================
// random ticks
// ==================================================================================================
inline void random_tick(int state, Pos pos) {
    auto& r = rng();
    int k = kind(state);
    int b = block(state);
    if (T.age[size_t(b)] >= 0 && T.next_age[size_t(b)] >= 0) {  // ChangeOverTimeBlock.changeOverTime (copper)
        if (!(r.nextFloat() < 0.05688889f)) return;
        int own = T.age[size_t(b)], same = 0, older = 0;
        for (int dx = -4; dx <= 4; dx++)
            for (int dy = -4; dy <= 4; dy++)
                for (int dz = -4; dz <= 4; dz++) {
                    if (std::abs(dx) + std::abs(dy) + std::abs(dz) > 4 || (dx == 0 && dy == 0 && dz == 0)) continue;
                    int a = T.age[size_t(block(get(pos.off(dx, dy, dz))))];
                    if (a < 0) continue;
                    if (a < own) return;  // a younger neighbour: abort
                    if (a > own) older++;
                    else same++;
                }
        float chance = float(older + 1) / float(older + same + 1);
        if (r.nextFloat() < chance * chance * (own == 0 ? 0.75f : 1.0f)) {
            int ns = def(T.next_age[size_t(b)]);
            for (auto& p : block_of(state).props) ns = with(ns, p.name, get(state, p.name));  // withPropertiesOf
            set(pos, ns);
        }
        return;
    }
    switch (k) {
        case K_LIQUID: fluid_random_tick(pos, state); break;
        case K_CROP: {
            if (raw_brightness0(pos) < 9) break;
            int age = age_of(state), mx = max_age(state);
            if (age < mx && r.nextInt(int(25.0f / growth_speed(b, pos)) + 1) == 0) set(pos, with(state, "age", age + 1), UPDATE_CLIENTS);
            break;
        }
        case K_STEM: {
            if (raw_brightness0(pos) < 9) break;
            if (r.nextInt(int(25.0f / growth_speed(b, pos)) + 1) != 0) break;
            int age = age_of(state);
            if (age < 7) {
                set(pos, with(state, "age", age + 1), UPDATE_CLIENTS);
            } else {
                int d = HORIZONTAL[r.nextInt(4)];
                Pos t = pos.rel(d);
                bool melon = block_of(state).name.find("melon") != std::string::npos;
                if (is_air(get(t)) && is(get(t.below()), tag(melon ? "supports_melon_stem_fruit" : "supports_pumpkin_stem_fruit"))) {
                    set(t, def(melon ? "melon" : "pumpkin"));
                    set(pos, withdir(def(melon ? "attached_melon_stem" : "attached_pumpkin_stem"), "facing", d));
                }
            }
            break;
        }
        case K_SAPLING:
            if (max_local_brightness(pos.above()) >= 9 && r.nextInt(7) == 0 && geti(state, "stage") == 0)
                set(pos, with(state, "stage", 1), UPDATE_NONE);
            // ponytail: stage 1 -> tree needs the tree features (TreeGrower); the sapling stays at stage 1.
            break;
        case K_MUSHROOM: {
            if (r.nextInt(25) != 0) break;
            int n = 0;
            for (int dx = -4; dx <= 4; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dz = -4; dz <= 4; dz++)
                        if (block(get(pos.off(dx, dy, dz))) == b && ++n > 4) return;
            auto off = [&](const Pos& p) { return p.off(r.nextInt(3) - 1, r.nextInt(2) - r.nextInt(2), r.nextInt(3) - 1); };
            Pos o = off(pos);
            for (int i = 0; i < 4; i++) {
                if (is_empty_block(o) && can_survive(state, o)) pos = o;
                o = off(pos);
            }
            if (is_empty_block(o) && can_survive(state, o)) set(o, state, UPDATE_CLIENTS);
            break;
        }
        case K_NETHER_WART: {
            int age = age_of(state);
            if (age < 3 && r.nextInt(10) == 0) set(pos, with(state, "age", age + 1), UPDATE_CLIENTS);
            break;
        }
        case K_BERRY_BUSH: {
            int age = age_of(state);
            if (age < 3 && r.nextInt(5) == 0 && raw_brightness0(pos.above()) >= 9) set(pos, with(state, "age", age + 1), UPDATE_CLIENTS);
            break;
        }
        case K_SUGAR_CANE: {
            if (!is_empty_block(pos.above())) break;
            int h = 1;
            while (block(get(pos.below(h))) == b) h++;
            if (h < 3) {
                int age = age_of(state);
                if (age == 15) {
                    set(pos.above(), def(b));
                    set(pos, with(state, "age", 0), UPDATE_NONE);
                } else {
                    set(pos, with(state, "age", age + 1), UPDATE_NONE);
                }
            }
            break;
        }
        case K_CACTUS: {
            Pos above = pos.above();
            if (!is_empty_block(above)) break;
            int h = 1, age = age_of(state);
            while (block(get(pos.below(h))) == b)
                if (++h == 3 && age == 15) return;
            if (age == 8 && can_survive(def(b), above)) {
                if (r.nextDouble() <= (h >= 3 ? 0.25 : 0.1)) set(above, def(ID.cactus_flower));
            } else if (age == 15 && h < 3) {
                set(above, def(b));
                int ns = with(state, "age", 0);
                set(pos, ns, UPDATE_NONE);
                neighbor_changed_full(ns, above, b, false);
            }
            if (age < 15) set(pos, with(state, "age", age + 1), UPDATE_NONE);
            break;
        }
        case K_GROWING_HEAD: {
            const Growing* g = growing_of(state);
            if (!g || age_of(state) >= 25 || !(r.nextDouble() < g->prob)) break;
            Pos t = pos.rel(g->dir);
            if (growing_can_grow_into(*g, get(t))) set(t, with(state, "age", age_of(state) + 1));
            break;
        }
        case K_BAMBOO: {
            if (geti(state, "stage") != 0) break;
            if (r.nextInt(3) != 0 || !is_empty_block(pos.above()) || raw_brightness0(pos.above()) < 9) break;
            int below = 0;
            while (below < 16 && block(get(pos.below(below + 1))) == ID.bamboo) below++;
            int h = below + 1;
            if (h >= 16) break;
            int bs = get(pos.below()), b2 = get(pos.below(2));
            const char* leaves = "none";
            if (h >= 1) {
                if (block(bs) != ID.bamboo || get(bs, "leaves") == "none") leaves = "small";
                else {
                    leaves = "large";
                    if (block(b2) == ID.bamboo) {
                        set(pos.below(), with(bs, "leaves", "small"));
                        set(pos.below(2), with(b2, "leaves", "none"));
                    }
                }
            }
            int age = geti(state, "age") != 1 && block(b2) != ID.bamboo ? 0 : 1;
            int stage = (h < 11 || !(r.nextFloat() < 0.25f)) && h != 15 ? 0 : 1;
            set(pos.above(), with(with(with(def(ID.bamboo), "age", age), "leaves", leaves), "stage", stage));
            break;
        }
        case K_BAMBOO_SAPLING:
            if (r.nextInt(3) == 0 && is_empty_block(pos.above()) && raw_brightness0(pos.above()) >= 9)
                set(pos.above(), with(def(ID.bamboo), "leaves", "small"));
            break;
        case K_SPREADING: {
            if (!spreading_can_stay_alive(pos)) {
                set(pos, blocks::dirt);
                break;
            }
            if (max_local_brightness(pos.above()) < 9) break;
            int ds = def(b);
            for (int i = 0; i < 4; i++) {
                Pos t = pos.off(r.nextInt(3) - 1, r.nextInt(5) - 3, r.nextInt(3) - 1);
                if (block(get(t)) == ID.dirt && spreading_can_propagate(t)) set(t, with(ds, "snowy", snowy_setting(get(t.above()))));
            }
            break;
        }
        case K_LEAVES:
            if (!getb(state, "persistent") && geti(state, "distance") == 7) {
                drop_resources(state, pos);
                remove(pos);
            }
            break;
        case K_ICE:
            if (block_brightness(pos) > 11 - light_dampening(state)) {  // IceBlock.melt (overworld: no evaporation)
                set(pos, blocks::water);
                neighbor_changed_at(pos, ID.water);
            }
            break;
        case K_SNOW_LAYER:
            if (block_brightness(pos) > 11) {
                drop_resources(state, pos);
                remove(pos);
            }
            break;
        case K_FARMLAND: {
            int m = geti(state, "moisture");
            bool near_water = false;
            for (int dx = -4; dx <= 4 && !near_water; dx++)
                for (int dy = 0; dy <= 1 && !near_water; dy++)
                    for (int dz = -4; dz <= 4 && !near_water; dz++) near_water = is_water(get(pos.off(dx, dy, dz)));
            if (!near_water && !raining_at(pos.above())) {
                if (m > 0) set(pos, with(state, "moisture", m - 1), UPDATE_CLIENTS);
                else if (!is(get(pos.above()), tag("maintains_farmland"))) set(pos, blocks::dirt);
            } else if (m < 7) {
                set(pos, with(state, "moisture", 7), UPDATE_CLIENTS);
            }
            break;
        }
        default: break;
    }
}

// ==================================================================================================
// precipitation (ServerLevel.tickPrecipitation, Biome.shouldFreeze / shouldSnow)
// ==================================================================================================
inline bool should_freeze(const Pos& p, bool check_neighbors = true) {
    if (warm_enough_to_rain(p)) return false;
    if (!in_height(p.y) || block_brightness(p) >= 10) return false;
    int s = get(p);
    if (!(fluid_type(s) == WATER && kind(s) == K_LIQUID)) return false;
    if (!check_neighbors) return true;
    bool surrounded = is_water_at(p.rel(WEST)) && is_water_at(p.rel(EAST)) && is_water_at(p.rel(NORTH)) && is_water_at(p.rel(SOUTH));
    return !surrounded;
}
inline bool should_snow(const Pos& p) {
    if (precipitation_at(p) != SNOW) return false;
    if (!in_height(p.y) || block_brightness(p) >= 10) return false;
    int s = get(p);
    return (is_air(s) || block(s) == ID.snow) && snow_can_survive(p);
}
inline void tick_precipitation(const Pos& p0) {
    Pos top{p0.x, motion_blocking_height(p0.x, p0.z), p0.z};
    Pos below = top.below();
    if (should_freeze(below)) set(below, def(ID.ice));
    if (!raining()) return;
    int maxh = rule("max_snow_accumulation_height");
    if (maxh > 0 && should_snow(top)) {
        int s = get(top);
        if (block(s) == ID.snow) {
            int layers = geti(s, "layers");
            if (layers < std::min(maxh, 8)) set(top, with(s, "layers", layers + 1));  // ponytail: pushEntitiesUp
        } else {
            set(top, def(ID.snow));
        }
    }
    // ponytail: handlePrecipitation (cauldrons filling) needs cauldron behaviours.
}

// ==================================================================================================
// player breaking (Block.playerWillDestroy)
// ==================================================================================================
inline void player_will_destroy(Player& p, const Pos& pos, int state) {
    bool prevents_drops = p.gamemode == 1;  // Player.preventsBlockDrops
    switch (kind(state)) {
        case K_DOOR:
            if (prevents_drops || (block_of(state).requires_tool && !tool_correct(p.inv[size_t(p.selected)].item, state)))
                prevent_drop_from_bottom_part(pos, state);
            break;
        case K_DOUBLE_PLANT:
        case K_TALL_SEAGRASS:
            if (prevents_drops) prevent_drop_from_bottom_part(pos, state);
            break;
        case K_BED:
            if (prevents_drops && get(state, "part") == "foot") {
                Pos hp = pos.rel(bed_neighbour_dir(state));
                int hs = get(hp);
                if (block(hs) == block(state) && get(hs, "part") == "head") {
                    set(hp, blocks::air, UPDATE_ALL | UPDATE_SUPPRESS_DROPS);
                    level_event(2001, hp, hs);
                }
            }
            break;
        default: break;
    }
}

inline bool can_spread_fire_around(const Pos& p) {  // ServerLevel.canSpreadFireAround
    int r = rule("fire_spread_radius_around_player");
    if (r < 0) return true;
    if (r == 0) return false;
    for (auto& pl : players) {
        if (pl->gamemode == 3) continue;
        double dx = pl->x - p.x, dz = pl->z - p.z;
        if (dx * dx + dz * dz <= double(r) * r) return true;
    }
    return false;
}

}  // namespace lvl
