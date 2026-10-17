#pragma once
// FlowingFluid / WaterFluid / LavaFluid (net.minecraft.world.level.material) and the fluid side of LiquidBlock,
// LiquidBlockContainer / SimpleWaterloggedBlock. Tick thread only.
#include "level.hpp"

namespace lvl {

// FluidState: type (Fluids id), amount 1..8, falling. Empty = {0, 0, false}.
struct FS {
    int type = EMPTY, amount = 0;
    bool falling = false;
    bool empty() const { return type == EMPTY; }
    bool source() const { return type == WATER || type == LAVA; }
    bool water() const { return type == WATER || type == FLOWING_WATER; }
    bool lava() const { return type == LAVA || type == FLOWING_LAVA; }
    float own_height() const { return float(amount) / 9.0f; }
    bool operator==(const FS&) const = default;
};
inline FS fluid_of(int state) { return {fluid_type(state), fluid_amount(state), fluid_falling(state)}; }
inline bool same_kind(int a, int b) {  // Fluid.isSame
    return a != EMPTY && b != EMPTY && ((a <= WATER) == (b <= WATER));
}

// Dimension attribute minecraft:gameplay/fast_lava (true only in the nether), switched with cur_level.
inline bool fast_lava = false;

inline int tick_delay(int type) { return type <= WATER ? 5 : fast_lava ? 10 : 30; }        // getTickDelay
inline int drop_off(int type) { return type <= WATER ? 1 : fast_lava ? 1 : 2; }           // getDropOff
inline int slope_find_distance(int type) { return type <= WATER ? 4 : fast_lava ? 4 : 2; }  // getSlopeFindDistance
inline bool can_convert_to_source(int type) { return rule(type <= WATER ? "water_source_conversion" : "lava_source_conversion"); }

inline int legacy_block(const FS& f) {  // createLegacyBlock
    if (f.empty()) return blocks::air;
    int level = f.source() ? 0 : 8 - std::min(f.amount, 8) + (f.falling ? 8 : 0);
    return (f.water() ? blocks::water : blocks::lava) + level;
}
inline int legacy_fluid_block(int state) { return legacy_block(fluid_of(state)); }
inline FS flowing(int type, int amount, bool falling) { return {type <= WATER ? FLOWING_WATER : FLOWING_LAVA, amount, falling}; }
inline FS source_of(int type) { return {type <= WATER ? WATER : LAVA, 8, false}; }

// FlowingFluid.getHeight: 1 when the same fluid is above, else amount / 9.
inline float fluid_height(const FS& f, const Pos& p) {
    if (!f.empty() && same_kind(f.type, fluid_type(get(p.above())))) return 1.0f;
    return f.own_height();
}

// ---------- containers ----------
inline bool is_container(int s) {  // instanceof LiquidBlockContainer
    int b = block(s);
    return has_waterlogged(s) || b == ID.kelp || b == ID.kelp_plant || b == ID.seagrass || b == ID.tall_seagrass;
}
inline bool can_place_liquid(int s, int fluid, bool creative_player = false) {  // LiquidBlockContainer.canPlaceLiquid
    int b = block(s);
    if (b == ID.kelp || b == ID.kelp_plant || b == ID.seagrass || b == ID.tall_seagrass) return false;
    if (has(s, "type") && get(s, "type") == "double" && has_class(s, "SlabBlock")) return false;
    if (block(s) == block_id("barrier") && !creative_player) return false;
    return fluid == WATER;
}
// SimpleWaterloggedBlock.placeLiquid
inline bool place_liquid(const Pos& p, int s, const FS& f) {
    if (!can_place_liquid(s, WATER, true)) return false;
    if (waterlogged(s) || f.type != WATER) return false;
    set(p, with(s, "waterlogged", true));
    schedule_fluid(p, f.type, tick_delay(f.type));
    return true;
}

// ---------- shapes (Shapes.mergedFaceOccludes over collision boxes) ----------
inline bool is_full_block_shape(const std::vector<Box>& v) {
    return v.size() == 1 && v[0].x0 == 0 && v[0].y0 == 0 && v[0].z0 == 0 && v[0].x1 == 1 && v[0].y1 == 1 && v[0].z1 == 1;
}
inline bool merged_face_occludes(const std::vector<Box>& a, const std::vector<Box>& b, int dir) {
    if (is_full_block_shape(a) || is_full_block_shape(b)) return true;
    int ax = axis(dir);
    bool positive = dir == UP || dir == SOUTH || dir == EAST;
    const auto& first = positive ? a : b;
    const auto& second = positive ? b : a;
    auto lo = [&](const Box& x, int k) { return k == 0 ? x.x0 : k == 1 ? x.y0 : x.z0; };
    auto hi = [&](const Box& x, int k) { return k == 0 ? x.x1 : k == 1 ? x.y1 : x.z1; };
    double fmax = -INFINITY, smin = INFINITY;
    for (auto& x : first) fmax = std::max(fmax, hi(x, ax));
    for (auto& x : second) smin = std::min(smin, lo(x, ax));
    std::vector<std::array<double, 4>> rects;  // projected (u0, v0, u1, v1) on the two other axes
    int u = (ax + 1) % 3, v = (ax + 2) % 3;
    if (std::abs(fmax - 1.0) <= 1e-7)
        for (auto& x : first)
            if (std::abs(hi(x, ax) - fmax) <= 1e-7 || hi(x, ax) >= fmax) rects.push_back({lo(x, u), lo(x, v), hi(x, u), hi(x, v)});
    if (std::abs(smin) <= 1e-7)
        for (auto& x : second)
            if (lo(x, ax) <= smin + 1e-7) rects.push_back({lo(x, u), lo(x, v), hi(x, u), hi(x, v)});
    std::vector<double> us{0, 1}, vs{0, 1};
    for (auto& r : rects) us.push_back(r[0]), us.push_back(r[2]), vs.push_back(r[1]), vs.push_back(r[3]);
    std::sort(us.begin(), us.end()), std::sort(vs.begin(), vs.end());
    for (size_t i = 0; i + 1 < us.size(); i++)
        for (size_t j = 0; j + 1 < vs.size(); j++) {
            double cu = (us[i] + us[i + 1]) / 2, cv = (vs[j] + vs[j + 1]) / 2;
            if (cu <= 0 || cu >= 1 || cv <= 0 || cv >= 1 || us[i + 1] - us[i] < 1e-9 || vs[j + 1] - vs[j] < 1e-9) continue;
            bool cov = false;
            for (auto& r : rects)
                if (cu > r[0] && cu < r[2] && cv > r[1] && cv < r[3]) { cov = true; break; }
            if (!cov) return false;
        }
    return true;
}
// FlowingFluid.canPassThroughWall (pure in the two collision shapes: cached per shape pair + direction).
inline bool can_pass_through_wall(int dir, int src_state, int tgt_state) {
    int sa = T.shape[size_t(src_state)], sb = T.shape[size_t(tgt_state)];
    if (is_full_block_shape(T.shapes[size_t(sb)]) || is_full_block_shape(T.shapes[size_t(sa)])) return false;
    if (T.shapes[size_t(sa)].empty() && T.shapes[size_t(sb)].empty()) return true;
    static std::vector<int8_t> cache(T.shapes.size() * T.shapes.size() * 6, -1);  // (source, target, dir)
    int8_t& c = cache[(size_t(sa) * T.shapes.size() + size_t(sb)) * 6 + size_t(dir)];
    if (c < 0) c = !merged_face_occludes(T.shapes[size_t(sa)], T.shapes[size_t(sb)], dir);
    return c;
}

inline bool can_hold_any_fluid(int s) {
    static const auto& washed = tag("washed_away_by_fluids");
    return is_container(s) || is(s, washed);
}
inline bool can_hold_specific_fluid(int s, int fluid) { return is_container(s) ? can_place_liquid(s, fluid) : true; }
inline bool can_hold_fluid(int s, int fluid) { return can_hold_any_fluid(s) && can_hold_specific_fluid(s, fluid); }

// FluidState.canBeReplacedWith
inline bool can_be_replaced_with(const FS& f, const Pos& p, int other, int dir) {
    if (f.empty()) return true;
    if (f.water()) return dir == DOWN && !(other == WATER || other == FLOWING_WATER);
    return fluid_height(f, p) >= 0.44444445f && (other == WATER || other == FLOWING_WATER);
}

struct Flowing {  // one FlowingFluid (water or lava), evaluated at a tick
    int type;  // the ticking fluid's kind (any of its two ids)
    bool same(int t) const { return same_kind(type, t); }
    bool source_of_this(const FS& f) const { return same(f.type) && f.source(); }

    FS new_liquid(const Pos& p, int state) {  // getNewLiquid
        int highest = 0, sources = 0;
        for (int d : HORIZONTAL) {
            Pos q = p.rel(d);
            int bs = get(q);
            FS f = fluid_of(bs);
            if (same(f.type) && can_pass_through_wall(d, state, bs)) {
                if (f.source()) sources++;
                highest = std::max(highest, f.amount);
            }
        }
        if (sources >= 2 && can_convert_to_source(type)) {
            int below = get(p.below());
            if (is_solid(below) || source_of_this(fluid_of(below))) return source_of(type);
        }
        int above = get(p.above());
        FS af = fluid_of(above);
        if (!af.empty() && same(af.type) && can_pass_through_wall(UP, state, above)) return flowing(type, 8, true);
        int amount = highest - drop_off(type);
        return amount <= 0 ? FS{} : flowing(type, amount, false);
    }
    bool can_maybe_pass_through(int src_state, int dir, int test_state) {
        return !source_of_this(fluid_of(test_state)) && can_hold_any_fluid(test_state) && can_pass_through_wall(dir, src_state, test_state);
    }
    bool can_pass_through(int fluid, int src_state, int dir, int test_state) {
        return can_maybe_pass_through(src_state, dir, test_state) && can_hold_specific_fluid(test_state, fluid);
    }
    bool is_water_hole(int top_state, int bottom_state) {  // isWaterHole
        if (!can_pass_through_wall(DOWN, top_state, bottom_state)) return false;
        return same(fluid_type(bottom_state)) ? true : can_hold_fluid(bottom_state, type <= WATER ? FLOWING_WATER : FLOWING_LAVA);
    }
    struct SpreadContext {
        Pos origin;
        // Short2ObjectMap caches keyed by the xz offset; the slope search stays within 8 blocks: 17x17 arrays.
        int states[17 * 17];
        int8_t holes[17 * 17];
        explicit SpreadContext(const Pos& o) : origin(o) {
            std::fill(std::begin(states), std::end(states), -1);
            std::fill(std::begin(holes), std::end(holes), int8_t(-1));
        }
        int key(const Pos& p) const { return (p.x - origin.x + 8) * 17 + (p.z - origin.z + 8); }
        int state(const Pos& p) {
            int& s = states[key(p)];
            if (s < 0) s = get(p);
            return s;
        }
    };
    bool is_hole(SpreadContext& ctx, const Pos& p) {
        int8_t& h = ctx.holes[ctx.key(p)];
        if (h < 0) h = is_water_hole(ctx.state(p), get(p.below()));
        return h;
    }
    int slope_distance(const Pos& p, int pass, int from, int state, SpreadContext& ctx) {  // getSlopeDistance
        int lowest = 1000;
        for (int d : HORIZONTAL) {
            if (d == from) continue;
            Pos t = p.rel(d);
            int ts = ctx.state(t);
            if (can_pass_through(type <= WATER ? FLOWING_WATER : FLOWING_LAVA, state, d, ts)) {
                if (is_hole(ctx, t)) return pass;
                if (pass < slope_find_distance(type)) {
                    int v = slope_distance(t, pass + 1, OPP[d], ts, ctx);
                    if (v < lowest) lowest = v;
                }
            }
        }
        return lowest;
    }
    // getSpread: EnumMap<Direction, FluidState> iterated in Direction order (N, S, W, E).
    int get_spread(const Pos& p, int state, std::array<FS, 6>& out) {
        int lowest = 1000, mask = 0;
        std::unique_ptr<SpreadContext> ctx;
        for (int d : HORIZONTAL) {
            Pos t = p.rel(d);
            int ts = get(t);
            FS tf = fluid_of(ts);
            if (!can_maybe_pass_through(state, d, ts)) continue;
            FS nf = new_liquid(t, ts);
            if (!can_hold_specific_fluid(ts, nf.type)) continue;
            if (!ctx) ctx = std::make_unique<SpreadContext>(p);
            int dist = is_hole(*ctx, t) ? 0 : slope_distance(t, 1, OPP[d], ts, *ctx);
            if (dist < lowest) mask = 0;
            if (dist <= lowest) {
                if (can_be_replaced_with(tf, t, nf.type, d)) mask |= 1 << d, out[size_t(d)] = nf;
                lowest = dist;
            }
        }
        return mask;
    }
    void spread_to(const Pos& p, int state, int dir, const FS& target) {
        if (lava_kind() && dir == DOWN && is_water(get(p))) {  // LavaFluid.spreadTo
            if (block(state) == ID.water || block(state) == ID.lava) set(p, blocks::stone);
            level_event(1501, p, 0);
            return;
        }
        if (is_container(state)) {
            place_liquid(p, state, target);
            return;
        }
        if (!is_air(state)) {  // beforeDestroyingBlock
            if (lava_kind()) level_event(1501, p, 0);
            else drop_resources(state, p);
        }
        set(p, legacy_block(target));
    }
    bool lava_kind() const { return type >= FLOWING_LAVA; }
    int source_neighbor_count(const Pos& p) {
        int n = 0;
        for (int d : HORIZONTAL)
            if (source_of_this(fluid_of(get(p.rel(d))))) n++;
        return n;
    }
    void spread_to_sides(const Pos& p, const FS& f, int state) {
        int neighbor = f.falling ? 7 : f.amount - drop_off(type);
        if (neighbor <= 0) return;
        std::array<FS, 6> nf;
        int mask = get_spread(p, state, nf);
        for (int d = 0; d < 6; d++)
            if (mask >> d & 1) spread_to(p.rel(d), get(p.rel(d)), d, nf[size_t(d)]);
    }
    void spread(const Pos& p, int state, const FS& f) {
        if (f.empty()) return;
        Pos bp = p.below();
        int bs = get(bp);
        FS bf = fluid_of(bs);
        if (can_maybe_pass_through(state, DOWN, bs)) {
            FS nb = new_liquid(bp, bs);
            if (can_be_replaced_with(bf, bp, nb.type, DOWN) && can_hold_specific_fluid(bs, nb.type)) {
                spread_to(bp, bs, DOWN, nb);
                if (source_neighbor_count(p) >= 3) spread_to_sides(p, f, state);
                return;
            }
        }
        if (f.source() || !is_water_hole(state, bs)) spread_to_sides(p, f, state);
    }
    int spread_delay(const Pos& p, const FS& old, const FS& nw) {  // getSpreadDelay
        int r = tick_delay(type);
        if (lava_kind() && !old.empty() && !nw.empty() && !old.falling && !nw.falling &&
            fluid_height(nw, p) > fluid_height(old, p) && rng().nextInt(4) != 0)
            r *= 4;
        return r;
    }
    void tick(const Pos& p, int state, FS f) {  // FlowingFluid.tick
        if (!f.source()) {
            FS nf = new_liquid(p, get(p));
            int delay = spread_delay(p, f, nf);
            if (nf.empty()) {
                f = nf;
                state = blocks::air;
                set(p, state);
            } else if (!(nf == f)) {
                f = nf;
                state = legacy_block(nf);
                set(p, state);
                schedule_fluid(p, nf.type, delay);
            }
        }
        spread(p, state, f);
    }
};

inline void fluid_tick(Pos p, int state) {
    FS f = fluid_of(state);
    if (f.empty()) return;
    Flowing{f.type}.tick(p, state, f);
}

// ---------- LavaFluid.randomTick (fire) ----------
inline bool can_spread_fire_around(const Pos& p);  // behaviours.hpp
inline int fire_state_for(const Pos& p);           // BaseFireBlock.getState
inline bool lava_flammable(const Pos& p) {
    if (in_height(p.y) && !loaded(p)) return false;
    return ignited_by_lava(get(p));
}
inline void fluid_random_tick(Pos p, int state) {
    if (!is_lava(state) || !can_spread_fire_around(p)) return;
    auto& r = rng();
    int passes = r.nextInt(3);
    if (passes > 0) {
        Pos t = p;
        for (int i = 0; i < passes; i++) {
            t = t.off(r.nextInt(3) - 1, 1, r.nextInt(3) - 1);
            if (!loaded(t)) return;
            int bs = get(t);
            if (is_air(bs)) {
                bool flammable = false;
                for (int d = 0; d < 6 && !flammable; d++) flammable = lava_flammable(t.rel(d));
                if (flammable) {
                    set(t, fire_state_for(t));
                    return;
                }
            } else if (is(bs, tag("blocks_lava_fire_spread"))) {
                return;
            }
        }
    } else {
        for (int i = 0; i < 3; i++) {
            Pos t = p.off(r.nextInt(3) - 1, 0, r.nextInt(3) - 1);
            if (!loaded(t)) return;
            if (is_air(get(t.above())) && lava_flammable(t)) set(t.above(), fire_state_for(t));
        }
    }
}

// ---------- LiquidBlock ----------
inline bool bubble_column_occupies(int state) {  // shouldBubbleColumnOccupy: water source (tag bubble_column_can_occupy)
    return fluid_type(state) == WATER && fluid_amount(state) == 8;
}
inline void try_schedule_bubble_column(const Pos& p, int below, int self_block) {
    if (is(below, tag("enables_bubble_column_drag_down")) || is(below, tag("enables_bubble_column_push_up")))
        schedule_block(p, self_block, 20);
}
// LiquidBlock.shouldSpreadLiquid: lava next to water turns to obsidian / cobblestone, over soul soil by blue ice basalt.
inline bool should_spread_liquid(const Pos& p, int state) {
    if (!is_lava(state)) return true;
    bool soul = block(get(p.below())) == ID.soul_soil;
    static constexpr int FLOW_DIRS[5] = {DOWN, SOUTH, NORTH, EAST, WEST};  // POSSIBLE_FLOW_DIRECTIONS
    for (int d : FLOW_DIRS) {
        Pos n = p.rel(OPP[d]);
        if (is_water(get(n))) {
            set(p, fluid_source(get(p)) ? blocks::obsidian : blocks::cobblestone);
            level_event(1501, p, 0);
            return false;
        }
        if (soul && block(get(n)) == ID.blue_ice) {
            set(p, def(ID.basalt));
            level_event(1501, p, 0);
            return false;
        }
    }
    return true;
}
inline void liquid_on_place(int state, const Pos& p) {
    if (should_spread_liquid(p, state)) schedule_fluid(p, fluid_type(state), tick_delay(fluid_type(state)));
    if (bubble_column_occupies(state)) try_schedule_bubble_column(p, get(p.below()), block(state));
}
inline int liquid_update_shape(int state, const Pos& p, int dir, int nstate) {
    if (fluid_source(state) || fluid_source(nstate)) schedule_fluid(p, fluid_type(state), tick_delay(fluid_type(state)));
    if (dir == DOWN && bubble_column_occupies(state)) try_schedule_bubble_column(p, nstate, block(state));
    return state;
}
inline void liquid_neighbor_changed(int state, const Pos& p) {
    if (should_spread_liquid(p, state)) schedule_fluid(p, fluid_type(state), tick_delay(fluid_type(state)));
    if (bubble_column_occupies(state)) try_schedule_bubble_column(p, get(p.below()), block(state));
}
// Waterlogged blocks' updateShape: `if (state.getValue(WATERLOGGED)) ticks.scheduleTick(pos, Fluids.WATER, 5)`.
inline void schedule_waterlogged(int state, const Pos& p) {
    if (waterlogged(state)) schedule_fluid(p, WATER, tick_delay(WATER));
}

}  // namespace lvl
