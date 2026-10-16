#pragma once
// Block behaviours (BlockBehaviour overrides of vanilla 26.3 block classes), dispatched by a per-block kind
// derived from the class chain in data/level.json. Tick thread only.
#include "fluids.hpp"

namespace lvl {

enum Kind : uint8_t {
    K_NONE, K_LIQUID, K_BUBBLE_COLUMN, K_FALLING, K_CONCRETE_POWDER, K_ANVIL,
    K_COUNT_
};
inline std::vector<uint8_t> kinds;  // per block
inline int kind(int s) { return kinds[size_t(block(s))]; }

inline void init_kinds() {
    kinds.assign(block_info.size(), K_NONE);
    for (size_t b = 0; b < block_info.size(); b++) {
        int s = def(int(b));
        uint8_t k = K_NONE;
        if (has_class(s, "LiquidBlock")) k = K_LIQUID;
        else if (has_class(s, "BubbleColumnBlock")) k = K_BUBBLE_COLUMN;
        else if (has_class(s, "ConcretePowderBlock")) k = K_CONCRETE_POWDER;
        else if (has_class(s, "AnvilBlock")) k = K_ANVIL;
        else if (has_class(s, "FallingBlock")) k = K_FALLING;
        kinds[b] = k;
    }
}

// ---------- shared helpers ----------
inline bool can_support_center(const Pos& p, int dir) { return sturdy(get(p), dir, CENTER); }  // Block.canSupportCenter
inline bool can_support_rigid(const Pos& p) { return sturdy(get(p), UP, RIGID); }
inline bool face_sturdy(const Pos& p, int dir) { return sturdy(get(p), dir, FULL); }

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

// ==================================================================================================
// dispatch
// ==================================================================================================
inline int update_shape(int state, Pos pos, int dir, Pos npos, int nstate) {
    (void)npos;
    switch (kind(state)) {
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
        default:
            schedule_waterlogged(state, pos);
            return state;
    }
}

inline void neighbor_changed(int state, Pos pos, int src_block, bool piston) {
    (void)src_block, (void)piston;
    switch (kind(state)) {
        case K_LIQUID: liquid_neighbor_changed(state, pos); break;
        default: break;
    }
}

inline void on_place(int state, Pos pos, int old, bool piston) {
    (void)old, (void)piston;
    switch (kind(state)) {
        case K_LIQUID: liquid_on_place(state, pos); break;
        case K_FALLING:
        case K_ANVIL:
        case K_CONCRETE_POWDER: schedule_block(pos, block(state), 2); break;
        default: break;
    }
}

inline void after_removal(int state, Pos pos, bool piston) { (void)state, (void)pos, (void)piston; }

inline void block_tick(int state, Pos pos) {
    switch (kind(state)) {
        case K_LIQUID:
            if (bubble_column_occupies(state)) bubble_update_column(pos, get(pos), get(pos.below()));
            break;
        case K_BUBBLE_COLUMN: bubble_update_column(pos, state, get(pos.below())); break;
        case K_FALLING:
        case K_ANVIL:
        case K_CONCRETE_POWDER:
            if (falling_is_free(get(pos.below())) && pos.y >= MIN_Y) start_falling(pos, state);
            break;
        default: break;
    }
}

inline void random_tick(int state, Pos pos) {
    switch (kind(state)) {
        case K_LIQUID: fluid_random_tick(pos, state); break;
        default: break;
    }
}

inline bool can_survive(int state, Pos pos) {
    switch (kind(state)) {
        case K_BUBBLE_COLUMN: return bubble_can_survive(pos);
        default: return true;
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
inline int fire_state_for(const Pos& p) {  // BaseFireBlock.getState
    return is(get(p.below()), tag("soul_fire_base_blocks")) ? def(ID.soul_fire) : def(ID.fire);
}

inline void player_will_destroy(Player& p, const Pos& pos, int state) { (void)p, (void)pos, (void)state; }
inline void tick_precipitation(const Pos& p) { (void)p; }

}  // namespace lvl
