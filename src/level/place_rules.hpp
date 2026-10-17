#pragma once
// Placement and shape rules for the block families placement.hpp / behaviours.hpp don't dispatch themselves:
// Block.getStateForPlacement and canBeReplaced(state, context) per vanilla 26.3 class, plus updateShape /
// canSurvive for the blocks those rules create (walls, chests, redstone wire, rails (RailState), tripwire,
// chorus plants, huge mushrooms, vines, multiface, banners, skulls, coral, hanging signs, bells, ...).
// Included at the end of placement.hpp; behaviours.hpp calls in through the forward declarations there.
// Tick thread only.

#include <optional>

namespace lvl {

enum PKind : uint8_t {
    P_NONE, P_WALL, P_CHEST, P_WIRE, P_TRIPWIRE, P_TRIPWIRE_HOOK, P_CHORUS_PLANT, P_HUGE_MUSHROOM, P_VINE, P_MULTIFACE,
    P_BANNER, P_WALL_BANNER, P_SKULL, P_WALL_SKULL, P_CORAL_PLANT, P_CORAL_WALL_FAN, P_CEILING_HANGING_SIGN,
    P_WALL_HANGING_SIGN, P_BELL, P_COCOA, P_SHELF_MUSHROOM, P_CANDLE, P_SEA_PICKLE, P_TURTLE_EGG, P_SEGMENTED,
    P_SCAFFOLDING, P_ROD, P_LIGHTNING_ROD, P_AMETHYST, P_SHULKER, P_HOPPER, P_OBSERVER, P_NEAREST_OPP, P_HORIZONTAL,
    P_HORIZONTAL_OPP, P_CAMPFIRE, P_PILLAR, P_CREAKING_HEART, P_CRAFTER, P_JIGSAW, P_BIG_DRIPLEAF, P_SMALL_DRIPLEAF,
    P_MANGROVE_PROPAGULE, P_WATERLOGGED_ONLY, P_CORAL_FULL_WATER, P_STANDING_SIGN, P_PITCHER_CROP, P_POTENT_SULFUR,
    P_LEAF_LITTER,
};
inline std::vector<uint8_t> pkinds;
inline int pkind(int s) {
    if (pkinds.empty()) {
        static const std::pair<const char*, PKind> by_class[] = {  // most specific first
            {"WallBlock", P_WALL}, {"ChestBlock", P_CHEST}, {"RedstoneWireBlock", P_WIRE}, {"TripWireBlock", P_TRIPWIRE},
            {"TripWireHookBlock", P_TRIPWIRE_HOOK}, {"ChorusPlantBlock", P_CHORUS_PLANT}, {"HugeMushroomBlock", P_HUGE_MUSHROOM},
            {"VineBlock", P_VINE}, {"MultifaceBlock", P_MULTIFACE}, {"WallBannerBlock", P_WALL_BANNER},
            {"BannerBlock", P_BANNER}, {"WallSkullBlock", P_WALL_SKULL}, {"SkullBlock", P_SKULL},
            {"BaseCoralWallFanBlock", P_CORAL_WALL_FAN}, {"BaseCoralPlantTypeBlock", P_CORAL_PLANT},
            {"CeilingHangingSignBlock", P_CEILING_HANGING_SIGN}, {"WallHangingSignBlock", P_WALL_HANGING_SIGN},
            {"StandingSignBlock", P_STANDING_SIGN}, {"BellBlock", P_BELL}, {"CocoaBlock", P_COCOA},
            {"ShelfMushroomBlock", P_SHELF_MUSHROOM}, {"CandleBlock", P_CANDLE}, {"SeaPickleBlock", P_SEA_PICKLE},
            {"TurtleEggBlock", P_TURTLE_EGG}, {"FlowerBedBlock", P_SEGMENTED}, {"LeafLitterBlock", P_LEAF_LITTER},
            {"ScaffoldingBlock", P_SCAFFOLDING}, {"LightningRodBlock", P_LIGHTNING_ROD}, {"EndRodBlock", P_ROD},
            {"AmethystClusterBlock", P_AMETHYST}, {"ShulkerBoxBlock", P_SHULKER}, {"HopperBlock", P_HOPPER},
            {"ObserverBlock", P_OBSERVER}, {"BarrelBlock", P_NEAREST_OPP}, {"DispenserBlock", P_NEAREST_OPP},
            {"CommandBlock", P_NEAREST_OPP}, {"PistonBaseBlock", P_NEAREST_OPP}, {"CampfireBlock", P_CAMPFIRE},
            {"CalibratedSculkSensorBlock", P_HORIZONTAL}, {"DecoratedPotBlock", P_HORIZONTAL},
            {"BigDripleafBlock", P_BIG_DRIPLEAF}, {"SmallDripleafBlock", P_SMALL_DRIPLEAF},
            {"MangrovePropaguleBlock", P_MANGROVE_PROPAGULE}, {"PitcherCropBlock", P_PITCHER_CROP},
            {"AbstractFurnaceBlock", P_HORIZONTAL_OPP}, {"BeehiveBlock", P_HORIZONTAL_OPP},
            {"ChiseledBookShelfBlock", P_HORIZONTAL_OPP}, {"CarvedPumpkinBlock", P_HORIZONTAL_OPP},
            {"GlazedTerracottaBlock", P_HORIZONTAL_OPP}, {"DiodeBlock", P_HORIZONTAL_OPP}, {"LoomBlock", P_HORIZONTAL_OPP},
            {"LecternBlock", P_HORIZONTAL_OPP}, {"StonecutterBlock", P_HORIZONTAL_OPP}, {"VaultBlock", P_HORIZONTAL_OPP},
            {"EndPortalFrameBlock", P_HORIZONTAL_OPP}, {"EnderChestBlock", P_HORIZONTAL_OPP},
            {"CopperGolemStatueBlock", P_HORIZONTAL_OPP}, {"DriedGhastBlock", P_HORIZONTAL_OPP},
            {"ShelfBlock", P_HORIZONTAL_OPP}, {"CreakingHeartBlock", P_CREAKING_HEART},
            {"InfestedRotatedPillarBlock", P_PILLAR}, {"RotatedPillarBlock", P_PILLAR}, {"CrafterBlock", P_CRAFTER},
            {"JigsawBlock", P_JIGSAW}, {"PotentSulfurBlock", P_POTENT_SULFUR}, {"ConduitBlock", P_CORAL_FULL_WATER},
            {"BarrierBlock", P_WATERLOGGED_ONLY}, {"HeavyCoreBlock", P_WATERLOGGED_ONLY},
            {"MangroveRootsBlock", P_WATERLOGGED_ONLY}, {"SculkShriekerBlock", P_WATERLOGGED_ONLY},
            {"SculkSensorBlock", P_WATERLOGGED_ONLY}, {"WaterloggedTransparentBlock", P_WATERLOGGED_ONLY},
            {"HangingRootsBlock", P_WATERLOGGED_ONLY},
        };
        pkinds.assign(block_info.size(), P_NONE);
        for (size_t b = 0; b < block_info.size(); b++)
            for (auto& [c, k] : by_class)
                if (has_class(def(int(b)), c)) { pkinds[b] = k; break; }
    }
    return pkinds[size_t(block(s))];
}

// ---------- shared helpers ----------
inline bool water_source(int s) { return fluid_type(s) == WATER; }                       // getFluidState().is(Fluids.WATER)
inline bool water_full(int s) { return is_water(s) && fluid_amount(s) == 8; }             // is(FluidTags.WATER) && isFull()
inline int set_wl(int s, bool w) { return has(s, "waterlogged") ? with(s, "waterlogged", w) : s; }
inline int rot_segment(float deg) { return int(std::floor(deg * (16.0f / 360.0f) + 0.5f)) & 15; }  // RotationSegment.convertToSegment
inline int rot_segment(int dir) {  // convertToSegment(Direction): get2DDataValue << 2
    static constexpr int D2[6] = {0, 0, 2, 0, 1, 3};
    return D2[dir] << 2;
}
inline int segment_dir(int seg) { return seg == 0 ? NORTH : seg == 4 ? EAST : seg == 8 ? SOUTH : seg == 12 ? WEST : -1; }
inline bool same_item_block(int s, const PlaceCtx& c) { int ib = item_block(c.item); return ib >= 0 && block(ib) == block(s); }
inline bool sneaking(const PlaceCtx& c) { return c.player.g.sneaking; }  // isSecondaryUseActive
inline bool attach_face_full(int s, int dir) { return sturdy(s, dir) || face_full(s, dir); }  // MultifaceBlock.canAttachTo
inline bool collision_face_up_nonempty(int s) {  // !getCollisionShape().getFaceShape(UP).isEmpty()
    for (auto& b : collision(s))
        if (b.y1 > 0.9999999) return true;
    return false;
}
// The xz projection of the collision boxes touching y=0 (getFaceShape(DOWN)) covers [x0,x1]x[z0,z1] (16ths).
inline bool down_face_covers(int s, double x0, double z0, double x1, double z1) {
    x0 /= 16, z0 /= 16, x1 /= 16, z1 /= 16;
    std::vector<double> xs{x0, x1}, zs{z0, z1};
    const auto& bs = collision(s);
    for (auto& b : bs)
        if (b.y0 < 1.0E-7) {
            if (b.x0 > x0 && b.x0 < x1) xs.push_back(b.x0);
            if (b.x1 > x0 && b.x1 < x1) xs.push_back(b.x1);
            if (b.z0 > z0 && b.z0 < z1) zs.push_back(b.z0);
            if (b.z1 > z0 && b.z1 < z1) zs.push_back(b.z1);
        }
    std::sort(xs.begin(), xs.end()), std::sort(zs.begin(), zs.end());
    for (size_t i = 0; i + 1 < xs.size(); i++)
        for (size_t k = 0; k + 1 < zs.size(); k++) {
            double cx = (xs[i] + xs[i + 1]) / 2, cz = (zs[k] + zs[k + 1]) / 2;
            if (xs[i + 1] - xs[i] < 1e-9 || zs[k + 1] - zs[k] < 1e-9) continue;
            bool in = false;
            for (auto& b : bs)
                if (b.y0 < 1.0E-7 && cx > b.x0 && cx < b.x1 && cz > b.z0 && cz < b.z1) { in = true; break; }
            if (!in) return false;
        }
    return true;
}

// ---------- walls (WallBlock) ----------
inline bool wall_connects(int s, bool face_solid, int dir) {  // connectsTo(state, faceSolid, direction)
    bool gate = kind(s) == K_FENCE_GATE && axis(getdir(s)) == axis(clockwise(dir));
    return is(s, tag("walls")) || (!exception_for_connection(s) && face_solid) || kind(s) == K_BARS || gate;
}
inline bool wall_side_connects(const Pos& p, int d) {
    int n = get(p.rel(d));
    return wall_connects(n, sturdy(n, OPP[d]), OPP[d]);
}
// updateShape(level, state, topPos, topNeighbour, n, e, s, w): sides LOW/TALL by what covers them from above, then the post.
inline int wall_shape(int s, int top, const bool conn[4]) {
    static constexpr int DIRS[4] = {NORTH, EAST, SOUTH, WEST};
    static constexpr double TEST[4][4] = {{7, 0, 9, 9}, {7, 7, 16, 9}, {7, 7, 9, 16}, {0, 7, 9, 9}};  // TEST_SHAPES_WALL
    for (int i = 0; i < 4; i++) {
        const char* v = !conn[i] ? "none" : down_face_covers(top, TEST[i][0], TEST[i][1], TEST[i][2], TEST[i][3]) ? "tall" : "low";
        s = with(s, DIR_NAME[DIRS[i]], v);
    }
    bool up;  // shouldRaisePost
    if (pkind(top) == P_WALL && getb(top, "up")) up = true;
    else {
        auto n = get(s, "north"), so = get(s, "south"), e = get(s, "east"), w = get(s, "west");
        bool nn = n == "none", sn = so == "none", en = e == "none", wn = w == "none";
        if ((nn && sn && wn && en) || nn != sn || wn != en) up = true;
        else if ((n == "tall" && so == "tall") || (e == "tall" && w == "tall")) up = false;
        else up = is(top, tag("wall_post_override")) || down_face_covers(top, 7, 7, 9, 9);
    }
    return with(s, "up", up);
}
inline int wall_update_shape(int s, const Pos& p, int dir, int ns) {
    schedule_waterlogged(s, p);
    if (dir == DOWN) return s;
    bool conn[4];
    static constexpr int DIRS[4] = {NORTH, EAST, SOUTH, WEST};
    for (int i = 0; i < 4; i++)
        conn[i] = dir == DIRS[i] ? wall_connects(ns, sturdy(ns, OPP[dir]), OPP[dir]) : get(s, DIR_NAME[DIRS[i]]) != "none";
    return wall_shape(s, dir == UP ? ns : get(p.above()), conn);
}

// ---------- chests (ChestBlock) ----------
inline bool chest_can_connect(int self, int s) {  // chestCanConnectTo
    if (has_class(self, "CopperChestBlock")) return is(s, tag("copper_chests")) && has(s, "type");
    return block(s) == block(self);
}
inline int chest_connected_dir(int s) { return get(s, "type") == "left" ? clockwise(getdir(s)) : ccw(getdir(s)); }
inline int chest_partner_facing(int self, const Pos& p, int d) {  // candidatePartnerFacing, -1 = null
    int n = get(p.rel(d));
    return chest_can_connect(self, n) && get(n, "type") == "single" ? getdir(n) : -1;
}
inline int chest_place(int s, PlaceCtx& c) {
    const char* type = "single";
    int facing = OPP[horizontal_direction(c.player)];
    bool secondary = sneaking(c);
    if (axis(c.face) != 1 && secondary) {
        int nf = chest_partner_facing(s, c.pos, OPP[c.face]);
        if (nf >= 0 && axis(nf) != axis(c.face)) {
            facing = nf;
            type = ccw(nf) == OPP[c.face] ? "right" : "left";
        }
    }
    if (std::string_view(type) == "single" && !secondary) {
        if (facing == chest_partner_facing(s, c.pos, clockwise(facing))) type = "left";
        else if (facing == chest_partner_facing(s, c.pos, ccw(facing))) type = "right";
    }
    // ponytail: copper chests don't adopt the least oxidized partner (getLeastOxidizedChestOfConnectedBlocks).
    return with(with(withdir(s, "facing", facing), "type", type), "waterlogged", water_source(get(c.pos)));
}
inline int chest_update_shape(int s, const Pos& p, int dir, int ns) {
    schedule_waterlogged(s, p);
    if (chest_can_connect(s, ns) && axis(dir) != 1) {
        auto nt = get(ns, "type");
        if (get(s, "type") == "single" && nt != "single" && getdir(s) == getdir(ns) && chest_connected_dir(ns) == OPP[dir])
            return with(s, "type", nt == "left" ? "right" : "left");
    } else if (get(s, "type") != "single" && chest_connected_dir(s) == dir) {
        return with(s, "type", "single");
    }
    return s;
}

// ---------- redstone wire (RedStoneWireBlock: shape only, no power) ----------
inline bool conductor(int s) { return flags(s) & F_CONDUCTOR; }
inline bool signal_source(int s) { return flags(s) & F_SIGNAL_SOURCE; }
inline bool wire_should_connect(int s, int dir) {  // shouldRedstoneWireConnectTo; dir -1 = null
    if (pkind(s) == P_WIRE) return true;
    if (pkind(s) == P_OBSERVER) return dir == getdir(s);
    if (has_class(s, "RepeaterBlock")) return dir >= 0 && axis(dir) == axis(getdir(s));
    return signal_source(s) && dir >= 0;
}
inline bool wire_survives_on(int s) { return sturdy(s, UP) || pkind(s) == P_HOPPER && block_of(s).name == "minecraft:hopper"; }
inline const char* wire_side(const Pos& p, int d, bool up_ok) {  // getConnectingSide
    Pos rp = p.rel(d);
    int rs = get(rp);
    if (up_ok) {
        bool placeable_above = kind(rs) == K_TRAPDOOR || wire_survives_on(rs);
        if (placeable_above && wire_should_connect(get(rp.above()), -1)) return sturdy(rs, OPP[d]) ? "up" : "side";
    }
    return !wire_should_connect(rs, d) && (conductor(rs) || !wire_should_connect(get(rp.below()), -1)) ? "none" : "side";
}
inline bool wire_conn(int s, int d) { return get(s, DIR_NAME[d]) != "none"; }
inline bool wire_dot(int s) { for (int d : HORIZONTAL) if (wire_conn(s, d)) return false; return true; }
inline bool wire_cross(int s) { for (int d : HORIZONTAL) if (!wire_conn(s, d)) return false; return true; }
inline int wire_connection_state(int s, const Pos& p) {  // getConnectionState
    bool was_dot = wire_dot(s);
    int t = with(def(block(s)), "power", geti(s, "power"));
    bool up_ok = !conductor(get(p.above()));
    for (int d : HORIZONTAL)
        if (!wire_conn(t, d)) t = with(t, DIR_NAME[d], wire_side(p, d, up_ok));
    if (was_dot && wire_dot(t)) return t;
    bool n = wire_conn(t, NORTH), so = wire_conn(t, SOUTH), e = wire_conn(t, EAST), w = wire_conn(t, WEST);
    bool ns_empty = !n && !so, ew_empty = !e && !w;
    if (!w && ns_empty) t = with(t, "west", "side");
    if (!e && ns_empty) t = with(t, "east", "side");
    if (!n && ew_empty) t = with(t, "north", "side");
    if (!so && ew_empty) t = with(t, "south", "side");
    return t;
}
inline int wire_cross_state(int b) {
    int s = def(b);
    for (int d : HORIZONTAL) s = with(s, DIR_NAME[d], "side");
    return s;
}
inline int wire_update_shape(int s, const Pos& p, int dir, int ns) {
    if (dir == DOWN) return wire_survives_on(ns) ? s : air();
    if (dir == UP) return wire_connection_state(s, p);
    const char* side = wire_side(p, dir, !conductor(get(p.above())));
    if ((std::string_view(side) != "none") == wire_conn(s, dir) && !wire_cross(s)) return with(s, DIR_NAME[dir], side);
    return wire_connection_state(with(with(wire_cross_state(block(s)), "power", geti(s, "power")), DIR_NAME[dir], side), p);
}
// RedStoneWireBlock.updateIndirectNeighbourShapes (Level.setBlock calls it after updateNeighbourShapes).
inline void wire_indirect_shapes(int s, const Pos& p, int flags, int limit) {
    if (pkind(s) != P_WIRE) return;
    for (int d : HORIZONTAL) {
        if (!wire_conn(s, d) || pkind(get(p.rel(d))) == P_WIRE) continue;
        for (int dy : {-1, 1}) {
            Pos q = p.rel(d).above(dy);
            if (pkind(get(q)) == P_WIRE) {
                Pos np = q.rel(OPP[d]);
                shape_update(OPP[d], get(np), q, np, flags, limit);
            }
        }
    }
}

// ---------- rails (RailState) ----------
inline bool is_rail(int s) { return kind(s) == K_RAIL; }
struct Rail {
    Pos pos;
    int state;
    bool straight;
    std::vector<Pos> conn;
    void update_conn(std::string_view shape) {
        conn.clear();
        if (shape == "north_south") conn = {pos.rel(NORTH), pos.rel(SOUTH)};
        else if (shape == "east_west") conn = {pos.rel(WEST), pos.rel(EAST)};
        else if (shape == "ascending_east") conn = {pos.rel(WEST), pos.rel(EAST).above()};
        else if (shape == "ascending_west") conn = {pos.rel(WEST).above(), pos.rel(EAST)};
        else if (shape == "ascending_north") conn = {pos.rel(NORTH).above(), pos.rel(SOUTH)};
        else if (shape == "ascending_south") conn = {pos.rel(NORTH), pos.rel(SOUTH).above()};
        else if (shape == "south_east") conn = {pos.rel(EAST), pos.rel(SOUTH)};
        else if (shape == "south_west") conn = {pos.rel(WEST), pos.rel(SOUTH)};
        else if (shape == "north_west") conn = {pos.rel(WEST), pos.rel(NORTH)};
        else if (shape == "north_east") conn = {pos.rel(EAST), pos.rel(NORTH)};
    }
    Rail(const Pos& p, int s) : pos(p), state(s), straight(!has_class(s, "RailBlock")) { update_conn(get(s, "shape")); }
    static bool rail_at(const Pos& p) { return is_rail(get(p)); }
    static std::optional<Rail> at(const Pos& p) {  // getRail
        for (int dy : {0, 1, -1}) {
            int s = get(p.above(dy));
            if (is_rail(s)) return Rail(p.above(dy), s);
        }
        return std::nullopt;
    }
    bool has_conn(const Pos& p) const {
        for (auto& c : conn)
            if (c.x == p.x && c.z == p.z) return true;
        return false;
    }
    bool connects_to(const Rail& r) const { return has_conn(r.pos); }
    void remove_soft() {
        for (size_t i = 0; i < conn.size(); i++) {
            auto r = at(conn[i]);
            if (r && r->connects_to(*this)) conn[i] = r->pos;
            else conn.erase(conn.begin() + std::ptrdiff_t(i--));
        }
    }
    bool can_connect_to(const Rail& r) const { return connects_to(r) || conn.size() != 2; }
    void set_shape(const char* shape) { state = with(state, "shape", shape); set(pos, state, UPDATE_ALL); }  // setBlockAndUpdate
    const char* ascend(const char* shape) const {
        if (std::string_view(shape) == "north_south") {
            if (rail_at(pos.rel(NORTH).above())) shape = "ascending_north";
            if (rail_at(pos.rel(SOUTH).above())) shape = "ascending_south";
        }
        if (std::string_view(shape) == "east_west") {
            if (rail_at(pos.rel(EAST).above())) shape = "ascending_east";
            if (rail_at(pos.rel(WEST).above())) shape = "ascending_west";
        }
        return shape;
    }
    void connect_to(const Rail& r) {
        conn.push_back(r.pos);
        bool n = has_conn(pos.rel(NORTH)), s = has_conn(pos.rel(SOUTH)), w = has_conn(pos.rel(WEST)), e = has_conn(pos.rel(EAST));
        const char* shape = nullptr;
        if (n || s) shape = "north_south";
        if (w || e) shape = "east_west";
        if (!straight) {
            if (s && e && !n && !w) shape = "south_east";
            if (s && w && !n && !e) shape = "south_west";
            if (n && w && !s && !e) shape = "north_west";
            if (n && e && !s && !w) shape = "north_east";
        }
        if (shape) shape = ascend(shape);
        set_shape(shape ? shape : "north_south");
    }
    bool has_neighbor_rail(const Pos& p) const {
        auto r = at(p);
        if (!r) return false;
        r->remove_soft();
        return r->can_connect_to(*this);
    }
    void place(bool signal, bool first, const std::string& def_shape) {
        bool n = has_neighbor_rail(pos.rel(NORTH)), s = has_neighbor_rail(pos.rel(SOUTH));
        bool w = has_neighbor_rail(pos.rel(WEST)), e = has_neighbor_rail(pos.rel(EAST));
        const char* shape = nullptr;
        bool ns = n || s, we = w || e;
        if (ns && !we) shape = "north_south";
        if (we && !ns) shape = "east_west";
        bool se = s && e, sw = s && w, ne = n && e, nw = n && w;
        if (!straight) {
            if (se && !n && !w) shape = "south_east";
            if (sw && !n && !e) shape = "south_west";
            if (nw && !s && !e) shape = "north_west";
            if (ne && !s && !w) shape = "north_east";
        }
        if (!shape) {
            if (ns && we) shape = def_shape.c_str();
            else if (ns) shape = "north_south";
            else if (we) shape = "east_west";
            if (!straight) {
                if (signal) {
                    if (se) shape = "south_east";
                    if (sw) shape = "south_west";
                    if (ne) shape = "north_east";
                    if (nw) shape = "north_west";
                } else {
                    if (nw) shape = "north_west";
                    if (ne) shape = "north_east";
                    if (sw) shape = "south_west";
                    if (se) shape = "south_east";
                }
            }
        }
        if (shape) shape = ascend(shape);
        if (!shape) shape = def_shape.c_str();
        update_conn(shape);
        state = with(state, "shape", shape);
        if (first || get(pos) != state) {
            set(pos, state, UPDATE_ALL);
            for (size_t i = 0; i < conn.size(); i++) {
                auto r = at(conn[i]);
                if (!r) continue;
                r->remove_soft();
                if (r->can_connect_to(*this)) r->connect_to(*this);
            }
        }
    }
};
inline bool has_neighbor_signal(const Pos& p);  // redstone.hpp
// BaseRailBlock.onPlace -> updateState -> updateDir(first = true).
inline void rail_on_place(int state, const Pos& p, int old, bool piston) {
    if (block(old) == block(state)) return;
    Rail r(p, state);
    r.place(has_neighbor_signal(p), true, std::string(get(state, "shape")));
    if (r.straight) neighbor_changed_full(r.state, p, block(state), piston);
}

// ---------- other shape helpers ----------
inline bool tripwire_connects(int self, int s, int dir) {  // TripWireBlock.shouldConnectTo
    if (pkind(s) == P_TRIPWIRE_HOOK) return getdir(s) == OPP[dir];
    return block(s) == block(self);
}
inline bool chorus_connects(int self, int s, int dir) {
    return block(s) == block(self) || block_of(s).name == "minecraft:chorus_flower" || (dir == DOWN && is(s, tag("supports_chorus_plant")));
}
inline bool chorus_can_survive(int s, const Pos& p) {
    int below = get(p.below());
    bool above_or_below = !is_air(get(p.above())) && !is_air(below);
    for (int d : HORIZONTAL) {
        Pos np = p.rel(d);
        if (block(get(np)) == block(s)) {
            if (above_or_below) return false;
            int b = get(np.below());
            if (block(b) == block(s) || is(b, tag("supports_chorus_plant"))) return true;
        }
    }
    return block(below) == block(s) || is(below, tag("supports_chorus_plant"));
}
inline bool vine_can_support_at(int s, const Pos& p, int d) {  // VineBlock.canSupportAtFace
    if (d == DOWN) return false;
    if (attach_face_full(get(p.rel(d)), OPP[d])) return true;
    if (axis(d) == 1) return false;
    int above = get(p.above());
    return block(above) == block(s) && getb(above, DIR_NAME[d]);
}
inline int vine_updated(int s, const Pos& p) {  // getUpdatedState
    if (getb(s, "up")) s = with(s, "up", attach_face_full(get(p.above()), DOWN));
    int above = -1;
    for (int d : HORIZONTAL) {
        if (!getb(s, DIR_NAME[d])) continue;
        bool ok = vine_can_support_at(s, p, d);
        if (!ok) {
            if (above < 0) above = get(p.above());
            ok = block(above) == block(s) && getb(above, DIR_NAME[d]);
        }
        s = with(s, DIR_NAME[d], ok);
    }
    return s;
}
inline int count_faces(int s) {
    int n = 0;
    for (int d = 0; d < 6; d++) n += has(s, DIR_NAME[d]) && getb(s, DIR_NAME[d]);
    return n;
}
inline bool bell_can_attach_face(const Pos& p, int dir) { return face_sturdy(p.rel(dir), OPP[dir]); }  // FaceAttached.canAttach
inline int bell_connected_opp(int s) {  // getConnectedDirection(state).getOpposite()
    auto a = get(s, "attachment");
    if (a == "floor") return DOWN;
    if (a == "ceiling") return UP;
    return OPP[getdir(s)];
}
inline bool hanging_wall_can_attach(int s, const Pos& at, int face) {  // WallHangingSignBlock.canAttachTo
    int a = get(at);
    if (is(a, tag("wall_hanging_signs"))) return axis(getdir(a)) == axis(getdir(s));
    return sturdy(a, face);
}
inline bool hanging_wall_can_place(int s, const Pos& p) {  // WallHangingSignBlock.canPlace
    int cw = clockwise(getdir(s)), cc = ccw(getdir(s));
    return hanging_wall_can_attach(s, p.rel(cw), cc) || hanging_wall_can_attach(s, p.rel(cc), cw);
}
inline int scaffolding_distance(const Pos& p) {  // ScaffoldingBlock.getDistance
    int below = get(p.below());
    int dist = 7;
    if (block(below) == ID.scaffolding) dist = geti(below, "distance");
    else if (sturdy(below, UP)) return 0;
    for (int d : HORIZONTAL) {
        int n = get(p.rel(d));
        if (block(n) == ID.scaffolding) {
            dist = std::min(dist, geti(n, "distance") + 1);
            if (dist == 1) break;
        }
    }
    return dist;
}
inline int potent_sulfur_state(int s, const Pos& p) {  // PotentSulfurBlock.validBlockState
    if (!water_source(get(p.above()))) return with(s, "potent_sulfur_state", "dry");
    int below = get(p.below());
    bool src = !has_fluid(below) || fluid_source(below);
    if (is(below, tag("causes_continuous_geyser_eruptions")) && src) return with(s, "potent_sulfur_state", "continuous");
    if (is(below, tag("causes_periodic_geyser_eruptions")) && src) {
        // ponytail: the block entity's eruption countdown isn't reset (resetCountdown).
        return get(s, "potent_sulfur_state") == "erupting" ? s : with(s, "potent_sulfur_state", "dormant");
    }
    return with(s, "potent_sulfur_state", "wet");
}

// VegetationBlock.mayPlaceOn overrides behaviours.hpp doesn't know; false = not overridden.
inline bool veg_may_place_on_more(int plant, int below, int* r) {
    if (pkind(plant) == P_SEA_PICKLE) *r = collision_face_up_nonempty(below) || sturdy(below, UP);
    else if (pkind(plant) == P_LEAF_LITTER) *r = sturdy(below, UP);
    else return false;
    return true;
}

// ---------- canSurvive / updateShape for the families above (behaviours.hpp default cases) ----------
inline bool can_survive_more(int s, const Pos& p) {
    switch (pkind(s)) {
        case P_WALL_BANNER: return is_solid(get(p.rel(OPP[getdir(s)])));
        case P_BANNER: return is_solid(get(p.below()));
        case P_CORAL_WALL_FAN: { int f = getdir(s); return face_sturdy(p.rel(OPP[f]), f); }
        case P_CORAL_PLANT: return face_sturdy(p.below(), UP);
        case P_CEILING_HANGING_SIGN: return sturdy(get(p.above()), DOWN, CENTER);
        case P_BELL: {
            int c = bell_connected_opp(s);
            return c == UP ? can_support_center(p.above(), DOWN) : bell_can_attach_face(p, c);
        }
        case P_COCOA: return is(get(p.rel(getdir(s))), tag("supports_cocoa"));
        case P_SHELF_MUSHROOM: case P_TRIPWIRE_HOOK: { int f = getdir(s); return face_sturdy(p.rel(OPP[f]), f); }
        case P_CANDLE: return can_support_center(p.below(), UP);
        case P_WIRE: return wire_survives_on(get(p.below()));
        case P_CHORUS_PLANT: return chorus_can_survive(s, p);
        case P_VINE: return count_faces(vine_updated(s, p)) > 0;
        case P_MULTIFACE: {
            bool any = false;
            for (int d = 0; d < 6; d++)
                if (has(s, DIR_NAME[d]) && getb(s, DIR_NAME[d])) {
                    if (!attach_face_full(get(p.rel(d)), OPP[d])) return false;
                    any = true;
                }
            return any;
        }
        case P_SCAFFOLDING: return scaffolding_distance(p) < 7;
        case P_AMETHYST: { int f = getdir(s); return face_sturdy(p.rel(OPP[f]), f); }
        default: return true;
    }
}
// Returns the new state, or -1 when behaviours.hpp's default (waterlogged tick) applies.
inline int update_shape_more(int s, const Pos& p, int dir, int ns) {
    auto pop = [&](bool cond) { return cond && !can_survive(s, p) ? air() : (schedule_waterlogged(s, p), s); };
    switch (pkind(s)) {
        case P_WALL: return wall_update_shape(s, p, dir, ns);
        case P_CHEST: return chest_update_shape(s, p, dir, ns);
        case P_WIRE: return wire_update_shape(s, p, dir, ns);
        case P_TRIPWIRE: return axis(dir) != 1 ? with(s, DIR_NAME[dir], tripwire_connects(s, ns, dir)) : s;
        case P_CHORUS_PLANT:
            if (!can_survive(s, p)) {
                schedule_block(p, block(s), 1);
                return s;
            }
            return with(s, DIR_NAME[dir], chorus_connects(s, ns, dir));
        case P_HUGE_MUSHROOM: return block(ns) == block(s) ? with(s, DIR_NAME[dir], false) : s;
        case P_VINE: {
            if (dir == DOWN) return s;
            int t = vine_updated(s, p);
            return count_faces(t) == 0 ? air() : t;
        }
        case P_MULTIFACE: {
            schedule_waterlogged(s, p);
            if (count_faces(s) == 0) return air();
            if (getb(s, DIR_NAME[dir]) && !attach_face_full(ns, OPP[dir])) {
                int t = with(s, DIR_NAME[dir], false);
                return count_faces(t) ? t : air();
            }
            return s;
        }
        case P_WALL_BANNER: case P_CORAL_WALL_FAN: case P_SHELF_MUSHROOM: case P_TRIPWIRE_HOOK: case P_AMETHYST:
            if (pkind(s) == P_CORAL_WALL_FAN || pkind(s) == P_AMETHYST) schedule_waterlogged(s, p);
            return OPP[dir] == getdir(s) && !can_survive(s, p) ? air() : s;
        case P_BANNER: case P_CORAL_PLANT: return pop(dir == DOWN);
        case P_CEILING_HANGING_SIGN: return pop(dir == UP);
        case P_WALL_HANGING_SIGN:
            schedule_waterlogged(s, p);
            return axis(dir) == axis(clockwise(getdir(s))) && !can_survive(s, p) ? air() : s;
        case P_COCOA: return dir == getdir(s) && !can_survive(s, p) ? air() : s;
        case P_BELL: {
            auto a = get(s, "attachment");
            int conn = bell_connected_opp(s);
            if (conn == dir && !can_survive(s, p) && a != "double_wall") return air();
            if (axis(dir) == axis(getdir(s))) {
                if (a == "double_wall" && !sturdy(ns, dir)) return withdir(with(s, "attachment", "single_wall"), "facing", OPP[dir]);
                if (a == "single_wall" && OPP[conn] == dir && sturdy(ns, getdir(s))) return with(s, "attachment", "double_wall");
            }
            return s;
        }
        case P_SCAFFOLDING:
            schedule_waterlogged(s, p);
            schedule_block(p, block(s), 1);
            return s;
        case P_CREAKING_HEART: schedule_block(p, block(s), 1); return s;
        case P_POTENT_SULFUR: return potent_sulfur_state(s, p);
        default: break;
    }
    if (block_of(s).name == "minecraft:note_block" && axis(dir) == 1) return s;  // ponytail: instrument table not generated
    return -1;
}

// ---------- BlockBehaviour.canBeReplaced(state, context) overrides; -1 = not overridden ----------
inline int can_be_replaced_more(int s, const PlaceCtx& c) {
    switch (pkind(s)) {
        case P_CANDLE: if (!sneaking(c) && same_item_block(s, c) && geti(s, "candles") < 4) return 1; break;
        case P_SEA_PICKLE: if (!sneaking(c) && same_item_block(s, c) && geti(s, "pickles") < 4) return 1; break;
        case P_TURTLE_EGG: if (!sneaking(c) && same_item_block(s, c) && geti(s, "eggs") < 4) return 1; break;
        case P_SEGMENTED: if (!sneaking(c) && same_item_block(s, c) && geti(s, "flower_amount") < 4) return 1; break;
        case P_LEAF_LITTER: if (!sneaking(c) && same_item_block(s, c) && geti(s, "segment_amount") < 4) return 1; break;
        case P_SCAFFOLDING: return same_item_block(s, c);
        case P_PITCHER_CROP: return 0;
        case P_VINE: {  // the clicked pos as the context sees it while it is being built (replaceClicked still false)
            int cl = get(c.replace_clicked ? c.hit : c.hit.rel(c.face));
            if (block(cl) == block(s)) return count_faces(cl) < 5;
            break;
        }
        case P_MULTIFACE: return !same_item_block(s, c) || count_faces(s) < 6 ? 1 : 0;  // hasAnyVacantFace
        default: return -1;
    }
    return -1;
}

// ---------- Block.getStateForPlacement; returns false when placement.hpp's own rules apply ----------
inline bool place_more(int b, PlaceCtx& c, int& out) {
    int s = def(b);
    const Pos& p = c.pos;
    int cur = get(p);
    int hdir = horizontal_direction(c.player);
    float yaw = c.player.yaw;
    auto nearest_horizontal = [&](auto&& ok) {  // first horizontal looking direction whose opposite facing works
        for (int d : c.nearest())
            if (axis(d) != 1) {
                int t = withdir(s, "facing", OPP[d]);
                if (ok(t, d)) return t;
            }
        return -1;
    };
    switch (pkind(s)) {
        case P_WALL: {
            bool conn[4] = {wall_side_connects(p, NORTH), wall_side_connects(p, EAST), wall_side_connects(p, SOUTH), wall_side_connects(p, WEST)};
            out = wall_shape(with(s, "waterlogged", water_source(cur)), get(p.above()), conn);
            return true;
        }
        case P_CHEST: out = chest_place(s, c); return true;
        case P_WIRE: out = wire_connection_state(wire_cross_state(b), p); return true;
        case P_TRIPWIRE:
            for (int d : HORIZONTAL) s = with(s, DIR_NAME[d], tripwire_connects(s, get(p.rel(d)), d));
            out = s;
            return true;
        case P_TRIPWIRE_HOOK:
            s = with(with(s, "powered", false), "attached", false);
            out = nearest_horizontal([&](int t, int) { return can_survive(t, p); });
            return true;
        case P_CHORUS_PLANT:
            for (int d = 0; d < 6; d++) s = with(s, DIR_NAME[d], chorus_connects(s, get(p.rel(d)), d));
            out = s;
            return true;
        case P_HUGE_MUSHROOM:
            for (int d = 0; d < 6; d++) s = with(s, DIR_NAME[d], block(get(p.rel(d))) != b);
            out = s;
            return true;
        case P_VINE: {
            bool clicked_vine = block(cur) == b;
            int r = clicked_vine ? cur : s;
            out = clicked_vine ? r : -1;
            for (int d : c.nearest()) {
                if (d == DOWN) continue;
                bool occupied = clicked_vine && getb(cur, DIR_NAME[d]);
                if (!occupied && vine_can_support_at(s, p, d)) { out = with(r, DIR_NAME[d], true); break; }
            }
            return true;
        }
        case P_MULTIFACE:  // MultifaceBlock.getStateForPlacement(oldState, level, pos, direction) per looking direction
            out = -1;
            for (int d : c.nearest()) {
                if (!has(s, DIR_NAME[d]) || (block(cur) == b && getb(cur, DIR_NAME[d]))) continue;
                if (!attach_face_full(get(p.rel(d)), OPP[d])) continue;
                int t = block(cur) == b ? cur : water_source(cur) ? with(s, "waterlogged", true) : s;
                out = with(t, DIR_NAME[d], true);
                break;
            }
            return true;
        case P_BANNER: out = with(s, "rotation", rot_segment(yaw + 180.0f)); return true;
        case P_STANDING_SIGN: out = with(with(s, "rotation", rot_segment(yaw + 180.0f)), "waterlogged", water_source(cur)); return true;
        case P_SKULL: out = with(with(s, "powered", false), "rotation", rot_segment(yaw)); return true;  // ponytail: no redstone signal
        case P_WALL_BANNER: out = nearest_horizontal([&](int t, int) { return can_survive(t, p); }); return true;
        case P_WALL_SKULL:
            s = with(s, "powered", false);
            out = nearest_horizontal([&](int, int d) { return !can_be_replaced_ctx(get(p.rel(d)), c); });
            return true;
        case P_CORAL_PLANT: out = with(s, "waterlogged", water_full(cur)); return true;
        case P_CORAL_WALL_FAN:
            s = with(s, "waterlogged", water_full(cur));
            out = nearest_horizontal([&](int t, int) { return can_survive(t, p); });
            return true;
        case P_CORAL_FULL_WATER: out = with(s, "waterlogged", water_full(cur)); return true;
        case P_CEILING_HANGING_SIGN: {
            int above = get(p.above());
            bool below_sign = is(above, tag("all_hanging_signs"));
            int dir = horizontal_direction(c.player);  // Direction.fromYRot(getRotation())
            bool middle = !face_full(above, DOWN) || sneaking(c);
            if (below_sign && !sneaking(c)) {
                if (has(above, "facing")) {
                    if (axis(getdir(above)) == axis(dir)) middle = false;
                } else if (has(above, "rotation")) {
                    int ad = segment_dir(geti(above, "rotation"));
                    if (ad >= 0 && axis(ad) == axis(dir)) middle = false;
                }
            }
            int seg = !middle ? rot_segment(OPP[dir]) : rot_segment(yaw + 180.0f);
            out = with(with(with(s, "attached", middle), "rotation", seg), "waterlogged", water_source(cur));
            return true;
        }
        case P_WALL_HANGING_SIGN:
            out = -1;
            for (int d : c.nearest())
                if (axis(d) != 1 && axis(d) != axis(c.face)) {
                    int t = withdir(s, "facing", OPP[d]);
                    if (can_survive(t, p) && hanging_wall_can_place(t, p)) { out = with(t, "waterlogged", water_source(cur)); break; }
                }
            return true;
        case P_BELL: {
            out = -1;
            int f = c.face;
            if (axis(f) == 1) {
                int t = withdir(with(s, "attachment", f == DOWN ? "ceiling" : "floor"), "facing", hdir);
                if (can_survive(t, p)) out = t;
            } else {
                bool dbl = (axis(f) == 0 && face_sturdy(p.rel(WEST), EAST) && face_sturdy(p.rel(EAST), WEST)) ||
                           (axis(f) == 2 && face_sturdy(p.rel(NORTH), SOUTH) && face_sturdy(p.rel(SOUTH), NORTH));
                int t = with(withdir(s, "facing", OPP[f]), "attachment", dbl ? "double_wall" : "single_wall");
                if (can_survive(t, p)) out = t;
                else {
                    t = with(t, "attachment", face_sturdy(p.below(), UP) ? "floor" : "ceiling");
                    if (can_survive(t, p)) out = t;
                }
            }
            return true;
        }
        case P_COCOA:
            out = -1;
            for (int d : c.nearest())
                if (axis(d) != 1) {
                    int t = withdir(s, "facing", d);
                    if (can_survive(t, p)) { out = t; break; }
                }
            return true;
        case P_SHELF_MUSHROOM: out = nearest_horizontal([&](int t, int) { return can_survive(t, p); }); return true;
        case P_CANDLE:
            out = block(cur) == b ? with(cur, "candles", geti(cur, "candles") % 4 + 1) : with(s, "waterlogged", water_source(cur));
            return true;
        case P_SEA_PICKLE:
            out = block(cur) == b ? with(cur, "pickles", std::min(4, geti(cur, "pickles") + 1)) : with(s, "waterlogged", water_source(cur));
            return true;
        case P_TURTLE_EGG: out = block(cur) == b ? with(cur, "eggs", std::min(4, geti(cur, "eggs") + 1)) : s; return true;
        case P_SEGMENTED:
        case P_LEAF_LITTER: {
            const char* prop = pkind(s) == P_SEGMENTED ? "flower_amount" : "segment_amount";
            out = block(cur) == b ? with(cur, prop, std::min(4, geti(cur, prop) + 1)) : withdir(s, "facing", OPP[hdir]);
            return true;
        }
        case P_SCAFFOLDING: {
            int dist = scaffolding_distance(p);
            out = with(with(with(s, "waterlogged", water_source(cur)), "distance", dist), "bottom", dist > 0 && block(get(p.below())) != b);
            return true;
        }
        case P_ROD: {
            int behind = get(p.rel(OPP[c.face]));
            out = withdir(s, "facing", block(behind) == b && getdir(behind) == c.face ? OPP[c.face] : c.face);
            return true;
        }
        case P_LIGHTNING_ROD: out = with(withdir(s, "facing", c.face), "waterlogged", water_source(cur)); return true;
        case P_AMETHYST: out = withdir(with(s, "waterlogged", water_source(cur)), "facing", c.face); return true;
        case P_SHULKER: out = withdir(s, "facing", c.face); return true;
        case P_HOPPER: {
            int d = OPP[c.face];
            out = with(withdir(s, "facing", axis(d) == 1 ? DOWN : d), "enabled", true);
            return true;
        }
        case P_OBSERVER: out = withdir(s, "facing", ordered_by_nearest(c.player)[0]); return true;
        case P_NEAREST_OPP:
            s = withdir(s, "facing", OPP[ordered_by_nearest(c.player)[0]]);
            if (has(s, "extended")) s = with(s, "extended", false);
            out = s;
            return true;
        case P_HORIZONTAL:
            s = withdir(s, "facing", hdir);
            if (has(s, "cracked")) s = with(s, "cracked", false);
            out = has(s, "waterlogged") ? with(s, "waterlogged", water_source(cur)) : s;
            return true;
        case P_HORIZONTAL_OPP:
            s = withdir(s, "facing", OPP[hdir]);
            if (has(s, "waterlogged")) s = with(s, "waterlogged", water_source(cur));
            if (has(s, "has_eye")) s = with(s, "has_eye", false);
            if (has(s, "has_book")) s = with(s, "has_book", false);  // ponytail: creative copies with a Book (block entity data)
            if (has(s, "powered")) s = with(s, "powered", false);    // ponytail: no redstone signal
            if (has(s, "locked")) s = with(s, "locked", false);
            out = s;
            return true;
        case P_CAMPFIRE: {
            bool w = water_source(cur);
            out = withdir(with(with(with(s, "waterlogged", w), "signal_fire", block(get(p.below())) == block_id("hay_block")), "lit", !w), "facing", hdir);
            return true;
        }
        case P_PILLAR: {
            s = with(s, "axis", axis(c.face) == 0 ? "x" : axis(c.face) == 1 ? "y" : "z");
            out = has(s, "waterlogged") ? with(s, "waterlogged", water_source(cur)) : s;  // ChainBlock
            return true;
        }
        case P_CREAKING_HEART: {
            s = with(s, "axis", axis(c.face) == 0 ? "x" : axis(c.face) == 1 ? "y" : "z");
            int ax = axis(c.face);
            bool logs = true;
            for (int d = 0; d < 6; d++)
                if (axis(d) == ax) {
                    int n = get(p.rel(d));
                    logs = logs && is(n, tag("pale_oak_logs")) && get(n, "axis") == get(s, "axis");
                }
            // ponytail: EnvironmentAttributes.CREAKING_ACTIVE (night) not ported: placed hearts start dormant.
            out = logs && get(s, "creaking_heart_state") == "uprooted" ? with(s, "creaking_heart_state", "dormant") : s;
            return true;
        }
        case P_CRAFTER: {
            int front = OPP[ordered_by_nearest(c.player)[0]];
            int top = front == DOWN ? OPP[hdir] : front == UP ? hdir : UP;
            out = with(with(s, "orientation", std::string(DIR_NAME[front]) + "_" + DIR_NAME[top]), "triggered", false);
            return true;
        }
        case P_JIGSAW: {
            int top = axis(c.face) == 1 ? OPP[hdir] : UP;
            out = with(s, "orientation", std::string(DIR_NAME[c.face]) + "_" + DIR_NAME[top]);
            return true;
        }
        case P_BIG_DRIPLEAF: {
            int below = get(p.below());
            bool part = block_of(below).name == "minecraft:big_dripleaf" || block_of(below).name == "minecraft:big_dripleaf_stem";
            out = withdir(with(s, "waterlogged", water_source(cur)), "facing", part ? getdir(below) : OPP[hdir]);
            return true;
        }
        case P_SMALL_DRIPLEAF:  // DoublePlantBlock.getStateForPlacement + facing + copyWaterloggedFrom
            if (p.y >= world.max_y() - 1 || !can_be_replaced_ctx(get(p.above()), c)) out = -1;
            else out = with(withdir(s, "facing", OPP[hdir]), "waterlogged", water_source(cur));
            return true;
        case P_MANGROVE_PROPAGULE: out = with(with(s, "waterlogged", water_source(cur)), "age", 4); return true;
        case P_PITCHER_CROP: out = s; return true;
        case P_POTENT_SULFUR: out = potent_sulfur_state(s, p); return true;
        case P_WATERLOGGED_ONLY: out = with(s, "waterlogged", water_source(cur)); return true;
        default: return false;
    }
}

// StandingAndWallBlockItem / HangingSignItem: the wall block and the attachment direction (DOWN, or UP for hanging signs).
inline int wall_variant_of(int standing, int& attach) {
    const std::string& n = block_of(standing).name;
    auto ends = [&](const char* suf) { size_t k = strlen(suf); return n.size() > k && n.compare(n.size() - k, k, suf) == 0; };
    auto base = [&](const char* suf) { return n.substr(10, n.size() - 10 - strlen(suf)); };
    std::string w;
    attach = DOWN;
    if (n == "minecraft:torch") w = "wall_torch";
    else if (ends("_torch")) w = base("_torch") + "_wall_torch";
    else if (ends("_hanging_sign")) w = base("_hanging_sign") + "_wall_hanging_sign", attach = UP;
    else if (ends("_sign") && n.find("_wall_") == std::string::npos) w = base("_sign") + "_wall_sign";
    else if (ends("_banner") && n.find("_wall_") == std::string::npos) w = base("_banner") + "_wall_banner";
    else if (ends("_skull") && n.find("_wall_") == std::string::npos) w = base("_skull") + "_wall_skull";
    else if (ends("_head") && n.find("_wall_") == std::string::npos && n != "minecraft:piston_head") w = base("_head") + "_wall_head";
    else if (ends("_coral_fan")) w = base("_coral_fan") + "_coral_wall_fan";
    else return -1;
    int id = block_id(w.c_str());
    return id < 0 ? -1 : def(id);
}

}  // namespace lvl
