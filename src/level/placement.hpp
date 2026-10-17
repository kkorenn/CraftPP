#pragma once
// Player interaction with blocks: ServerPlayerGameMode.useItemOn (block use: doors, trapdoors, fence gates),
// BlockItem.place with BlockPlaceContext and getStateForPlacement per block class, setPlacedBy (second halves),
// bone meal, hoe tilling, flint and steel, and buckets (BucketItem.use). Tick thread only.
#include "falling.hpp"
#include "cauldrons.hpp"

namespace lvl {

// ---------- Direction helpers on the player's view ----------
inline int horizontal_direction(const Player& p) {  // Direction.fromYRot
    static constexpr int BY_2D[4] = {SOUTH, WEST, NORTH, EAST};
    return BY_2D[int(std::floor(p.yaw / 90.0 + 0.5)) & 3];
}
inline std::array<int, 6> ordered_by_nearest(const Player& p) {  // Direction.orderedByNearest
    float pitch = p.pitch * float(M_PI / 180.0), yaw = -p.yaw * float(M_PI / 180.0);
    float ps = std::sin(pitch), pc = std::cos(pitch), ys = std::sin(yaw), yc = std::cos(yaw);
    bool xp = ys > 0, yp = ps < 0, zp = yc > 0;
    float xyaw = xp ? ys : -ys, ymag = yp ? -ps : ps, zyaw = zp ? yc : -yc;
    float xmag = xyaw * pc, zmag = zyaw * pc;
    int ax = xp ? EAST : WEST, ay = yp ? UP : DOWN, az = zp ? SOUTH : NORTH;
    auto mk = [](int a, int b, int c) { return std::array<int, 6>{a, b, c, OPP[c], OPP[b], OPP[a]}; };
    if (xyaw > zyaw) {
        if (ymag > xmag) return mk(ay, ax, az);
        return zmag > ymag ? mk(ax, az, ay) : mk(ax, ay, az);
    }
    if (ymag > zmag) return mk(ay, az, ax);
    return xmag > ymag ? mk(az, ax, ay) : mk(az, ay, ax);
}

struct PlaceCtx {  // BlockPlaceContext
    Player& player;
    Pos hit;        // clicked block
    int face;       // clicked face
    double cy;      // click location y within the hit block (0..1)
    double cx, cz;
    int item;
    bool replace_clicked = false;
    Pos pos{};      // getClickedPos
    int clicked_face() const { return face; }
    double click_y() const { return hit.y + cy - pos.y; }  // getClickLocation().y - pos.y
    std::array<int, 6> nearest() const {  // getNearestLookingDirections
        auto d = ordered_by_nearest(player);
        if (replace_clicked) return d;
        int want = OPP[face], i = 0;
        while (i < 6 && d[size_t(i)] != want) i++;
        for (int k = i; k > 0; k--) d[size_t(k)] = d[size_t(k - 1)];
        d[0] = want;
        return d;
    }
};

inline int item_block(int item) { return item >= 0 && item < ITEM_COUNT ? ITEM_TO_BLOCK[item] : -1; }  // default state
// place_rules.hpp: the block families this file doesn't dispatch.
inline int can_be_replaced_more(int s, const PlaceCtx& c);
inline bool place_more(int b, PlaceCtx& c, int& out);
inline int wall_variant_of(int standing, int& attach);
inline bool hanging_wall_can_place(int s, const Pos& p);
inline int rs_place_inputs(int s, const Pos& p);  // redstone.hpp
inline void rs_placed_by(int s, const Pos& p);
inline bool rs_use(Player& pl, const Pos& p, int s);
// BlockBehaviour.canBeReplaced(state, context)
inline bool can_be_replaced_ctx(int s, const PlaceCtx& c) {
    if (int r = can_be_replaced_more(s, c); r >= 0) return r;
    int ib = item_block(c.item);
    bool same_item = ib >= 0 && block(ib) == block(s);
    if (kind(s) == K_SNOW_LAYER) {
        int layers = geti(s, "layers");
        if (!same_item || layers >= 8) return layers == 1;
        return c.replace_clicked ? c.face == UP : true;
    }
    if (has_class(s, "SlabBlock")) {
        auto type = get(s, "type");
        if (type == "double" || !same_item) return false;
        if (!c.replace_clicked) return true;
        bool above = c.cy > 0.5;
        return type == "bottom" ? (c.face == UP || (above && axis(c.face) != 1)) : (c.face == DOWN || (!above && axis(c.face) != 1));
    }
    return can_be_replaced(s) && !same_item;
}
inline bool water_source_at(const Pos& p) { return fluid_type(get(p)) == WATER; }
inline int with_waterlogged(int s, const Pos& p) { return has(s, "waterlogged") ? with(s, "waterlogged", water_source_at(p)) : s; }

// Block.getStateForPlacement: -1 = null (can't place).
inline int state_for_placement(int b, PlaceCtx& c) {
    if (int r; place_more(b, c, r)) return r;
    int s = def(b);
    const Pos& p = c.pos;
    int hdir = horizontal_direction(c.player);
    switch (kind(s)) {
        case K_DOOR: {
            if (p.y >= world.max_y() - 1 || !can_be_replaced_ctx(get(p.above()), c)) return -1;
            // getHinge
            int left = ccw(hdir), right = clockwise(hdir);
            Pos lp = p.rel(left), rp = p.rel(right);
            int ls = get(lp), las = get(lp.above()), rs = get(rp), ras = get(rp.above());
            int bal = (full_collision(ls) ? -1 : 0) + (full_collision(las) ? -1 : 0) + (full_collision(rs) ? 1 : 0) + (full_collision(ras) ? 1 : 0);
            bool door_left = kind(ls) == K_DOOR && get(ls, "half") == "lower", door_right = kind(rs) == K_DOOR && get(rs, "half") == "lower";
            const char* hinge;
            if ((!door_left || door_right) && bal <= 0) {
                if ((!door_right || door_left) && bal >= 0) {
                    int sx = DX[hdir], sz = DZ[hdir];
                    double clx = c.hit.x + c.cx - p.x, clz = c.hit.z + c.cz - p.z;
                    hinge = (sx >= 0 || !(clz < 0.5)) && (sx <= 0 || !(clz > 0.5)) && (sz >= 0 || !(clx > 0.5)) && (sz <= 0 || !(clx < 0.5)) ? "left" : "right";
                } else hinge = "left";
            } else hinge = "right";
            return with(with(withdir(s, "facing", hdir), "hinge", hinge), "half", "lower");  // ponytail: powered = no redstone
        }
        case K_BED: {
            Pos rel = p.rel(hdir);
            return can_be_replaced_ctx(get(rel), c) ? withdir(s, "facing", hdir) : -1;
        }
        case K_DOUBLE_PLANT:
            if (p.y >= world.max_y() - 1 || !can_be_replaced_ctx(get(p.above()), c)) return -1;
            return with_waterlogged(s, p);
        case K_WALL_TORCH:
        case K_WALL_SIGN:
        case K_LADDER: {
            if (kind(s) == K_LADDER && !c.replace_clicked) {
                int st = get(p.rel(OPP[c.face]));
                if (kind(st) == K_LADDER && getdir(st) == c.face) return -1;
            }
            for (int d : c.nearest())
                if (axis(d) != 1) {
                    int t = withdir(s, "facing", OPP[d]);
                    if (can_survive(t, p)) return with_waterlogged(t, p);
                }
            return -1;
        }
        case K_LANTERN:
            for (int d : c.nearest())
                if (axis(d) == 1) {
                    int t = with(s, "hanging", d == UP);
                    if (can_survive(t, p)) return with_waterlogged(t, p);
                }
            return -1;
        case K_FACE_ATTACHED:
            for (int d : c.nearest()) {
                int t = axis(d) == 1 ? withdir(with(s, "face", d == UP ? "ceiling" : "floor"), "facing", hdir)
                                     : withdir(with(s, "face", "wall"), "facing", OPP[d]);
                if (can_survive(t, p)) return t;
            }
            return -1;
        case K_STAIRS: {
            int f = c.face;
            int t = with(withdir(s, "facing", hdir), "half", f != DOWN && (f == UP || !(c.click_y() > 0.5)) ? "bottom" : "top");
            t = with_waterlogged(t, p);
            return with(t, "shape", stairs_shape(t, p));
        }
        case K_TRAPDOOR: {
            int t = s;
            if (!c.replace_clicked && axis(c.face) != 1)
                t = with(withdir(t, "facing", c.face), "half", c.click_y() > 0.5 ? "top" : "bottom");
            else
                t = with(withdir(t, "facing", OPP[hdir]), "half", c.face == UP ? "bottom" : "top");
            return with_waterlogged(t, p);
        }
        case K_FENCE:
        case K_BARS: {
            int t = with_waterlogged(s, p);
            for (int d : HORIZONTAL) t = with(t, DIR_NAME[d], cross_connect(t, p, d));
            return t;
        }
        case K_FENCE_GATE: {
            int ax = axis(hdir);
            bool in_wall = (ax == 2 && (is(get(p.rel(WEST)), tag("walls")) || is(get(p.rel(EAST)), tag("walls")))) ||
                           (ax == 0 && (is(get(p.rel(NORTH)), tag("walls")) || is(get(p.rel(SOUTH)), tag("walls"))));
            return with(withdir(s, "facing", hdir), "in_wall", in_wall);
        }
        case K_SNOW_LAYER: {
            int cur = get(p);
            if (block(cur) == block(s)) return with(cur, "layers", std::min(8, geti(cur, "layers") + 1));
            return s;
        }
        case K_LEAVES: return leaves_update_distance(with(with(s, "persistent", true), "waterlogged", water_source_at(p)), p);
        case K_SNOWY:
        case K_SPREADING: return with(s, "snowy", snowy_setting(get(p.above())));
        case K_RAIL: {
            int t = has(s, "shape") ? with(s, "shape", axis(hdir) == 0 ? "east_west" : "north_south") : s;
            return with_waterlogged(t, p);
        }
        case K_CONCRETE_POWDER: {
            int cur = get(p);
            return concrete_can_solidify(cur) || concrete_touches_liquid(p) ? concrete_of(s) : s;
        }
        case K_FARMLAND: return can_survive(s, p) ? s : blocks::dirt;
        case K_FIRE: return fire_placement_state(p);
        case K_GROWING_HEAD:
        case K_GROWING_BODY: {
            const Growing* g = growing_of(s);
            if (g && growing_is_kelp(*g) && !(fluid_type(get(p)) == WATER && fluid_amount(get(p)) == 8)) return -1;
            if (g) {
                int gd = get(p.rel(g->dir));
                const std::string& n = block_of(gd).name;
                if (n == std::string("minecraft:") + g->head || n == std::string("minecraft:") + g->body) return def(g->body);
                int h = def(g->head);
                return has(h, "age") ? with(h, "age", rng().nextInt(25)) : h;
            }
            return s;
        }
        case K_BAMBOO: {
            if (has_fluid(get(p))) return -1;
            int below = get(p.below());
            if (!is(below, tag("supports_bamboo"))) return -1;
            if (block(below) == ID.bamboo_sapling) return with(s, "age", 0);
            if (block(below) == ID.bamboo) return with(s, "age", geti(below, "age") > 0 ? 1 : 0);
            int above = get(p.above());
            return block(above) == ID.bamboo ? with(s, "age", geti(above, "age")) : def(ID.bamboo_sapling);
        }
        default: break;
    }
    if (has_class(s, "SlabBlock")) {
        int cur = get(p);
        if (block(cur) == block(s)) return with(with(cur, "type", "double"), "waterlogged", false);
        int t = with(with_waterlogged(s, p), "type", "bottom");
        int f = c.face;
        return f != DOWN && (f == UP || !(c.click_y() > 0.5)) ? t : with(t, "type", "top");
    }
    if (has(s, "axis") && has_class(s, "RotatedPillarBlock")) return with(s, "axis", axis(c.face) == 0 ? "x" : axis(c.face) == 1 ? "y" : "z");
    if (has(s, "facing")) {
        const auto& vals = [&]() -> const std::vector<std::string>& {
            for (auto& pr : block_of(s).props) if (pr.name == "facing") return pr.values;
            static const std::vector<std::string> none;
            return none;
        }();
        if (vals.size() == 4) {  // HorizontalDirectionalBlock subclasses mostly face the player
            int f = has_class(s, "AnvilBlock") ? clockwise(hdir) : OPP[hdir];
            s = withdir(s, "facing", f);
        } else if (vals.size() == 6 && has_class(s, "DirectionalBlock")) {  // dispensers, observers, pistons...
            s = withdir(s, "facing", OPP[c.nearest()[0]]);
        }
        // ponytail: per-class facing rules (chests connecting, end rods, hoppers...) are not all ported.
    }
    return with_waterlogged(s, p);
}

// Level.isUnobstructed for players against the state's collision boxes.
inline bool obstructed_by_player(int s, const Pos& p) {
    for (auto& q : players) {
        if (q->gamemode == 3 || q->g.dead) continue;
        for (auto& b : collision(s))
            if (q->x + 0.3 > p.x + b.x0 && q->x - 0.3 < p.x + b.x1 && q->y + 1.8 > p.y + b.y0 && q->y < p.y + b.y1 &&
                q->z + 0.3 > p.z + b.z0 && q->z - 0.3 < p.z + b.z1)
                return true;
    }
    return false;
}

// Block.setPlacedBy
inline void placed_by(int s, const Pos& p) {
    switch (kind(s)) {
        case K_DOOR: set(p.above(), with(s, "half", "upper")); break;
        case K_BED: set(p.rel(getdir(s)), with(s, "part", "head")); break;
        case K_DOUBLE_PLANT: set(p.above(), with_waterlogged(with(def(block(s)), "half", "upper"), p.above())); break;
        default: rs_placed_by(s, p); break;
    }
}

// BlockItem.updateCustomBlockEntityTag: block entity data from the placed item (src/containers).
inline void (*block_item_placed_hook)(Player& p, int x, int y, int z, const ItemStack& item) = nullptr;
// BlockItem.place. Returns true when a block was placed.
inline bool place_block_item(PlaceCtx& c, int standing) {
    int hit_state = get(c.hit);
    c.replace_clicked = can_be_replaced_ctx(hit_state, c);
    c.pos = c.replace_clicked ? c.hit : c.hit.rel(c.face);
    if (!in_height(c.pos.y) || !loaded(c.pos)) return false;
    if (!c.replace_clicked && !can_be_replaced_ctx(get(c.pos), c)) return false;  // canPlace
    int state = -1;
    int attach;
    int wall = wall_variant_of(standing, attach);
    if (wall >= 0) {  // StandingAndWallBlockItem / HangingSignItem.getPlacementState
        int ws = state_for_placement(block(wall), c);
        for (int d : c.nearest()) {
            if (d == OPP[attach]) continue;
            int cand = d == attach ? state_for_placement(block(standing), c) : ws;
            if (cand >= 0 && can_survive(cand, c.pos) && (attach != UP || cand != ws || hanging_wall_can_place(cand, c.pos))) {
                state = cand;
                break;
            }
        }
    } else {
        state = state_for_placement(block(standing), c);
        if (state >= 0 && !can_survive(state, c.pos)) state = -1;  // BlockItem.canPlace: mustSurvive
    }
    if (state >= 0) state = rs_place_inputs(state, c.pos);
    if (state < 0 || obstructed_by_player(state, c.pos)) return false;
    if (!set(c.pos, state, UPDATE_ALL_IMMEDIATE)) return false;
    int placed = get(c.pos);
    if (block(placed) == block(state)) placed_by(placed, c.pos);
    return true;
}

// ---------- block use (useWithoutItem) ----------
inline bool opens_by_hand(int s) {
    const std::string& n = block_of(s).name;
    return n.find("iron_") == std::string::npos;  // BlockSetType.IRON: redstone only
}
inline bool use_without_item(Player& pl, const Pos& p, int s) {
    switch (kind(s)) {
        case K_DOOR:
            if (!opens_by_hand(s)) return false;
            set(p, with(s, "open", !getb(s, "open")), UPDATE_CLIENTS | UPDATE_IMMEDIATE);
            level_event(getb(s, "open") ? 1012 : 1006, p, 0);
            return true;
        case K_TRAPDOOR:
            if (!opens_by_hand(s)) return false;
            set(p, with(s, "open", !getb(s, "open")), UPDATE_CLIENTS);
            if (waterlogged(s)) schedule_fluid(p, WATER, tick_delay(WATER));
            level_event(getb(s, "open") ? 1013 : 1007, p, 0);
            return true;
        case K_FENCE_GATE: {
            int ns;
            if (getb(s, "open")) ns = with(s, "open", false);
            else {
                int d = horizontal_direction(pl);
                ns = getdir(s) == OPP[d] ? withdir(s, "facing", d) : s;
                ns = with(ns, "open", true);
            }
            set(p, ns, UPDATE_CLIENTS | UPDATE_IMMEDIATE);
            level_event(getb(ns, "open") ? 1008 : 1014, p, 0);
            return true;
        }
        default: return rs_use(pl, p, s);
    }
}

// ---------- items used on blocks ----------
inline bool bonemeal(const Pos& p) {  // BoneMealItem.growCrop
    int s = get(p);
    auto& r = rng();
    switch (kind(s)) {
        case K_CROP:
            if (age_of(s) >= max_age(s)) return false;
            set(p, with(s, "age", std::min(max_age(s), age_of(s) + r.nextIntBetweenInclusive(2, 5))), UPDATE_CLIENTS);
            break;
        case K_STEM: {
            if (age_of(s) == 7) return false;
            int age = std::min(7, age_of(s) + r.nextIntBetweenInclusive(2, 5));
            int ns = with(s, "age", age);
            set(p, ns, UPDATE_CLIENTS);
            if (age == 7) random_tick(ns, p);
            break;
        }
        case K_SAPLING:
            if (r.nextFloat() < 0.45f && geti(s, "stage") == 0) set(p, with(s, "stage", 1), UPDATE_NONE);
            break;  // ponytail: no tree growth (TreeGrower needs tree features)
        case K_BERRY_BUSH:
            if (age_of(s) >= 3) return false;
            set(p, with(s, "age", std::min(3, age_of(s) + 1)), UPDATE_CLIENTS);
            break;
        case K_GROWING_HEAD: case K_GROWING_BODY: {
            const Growing* g = growing_of(s);
            if (!g) return false;
            Pos h = p;
            while (block_of(get(h)).name == std::string("minecraft:") + g->body) h = h.rel(g->dir);
            int hs = get(h);
            if (block_of(hs).name != std::string("minecraft:") + g->head || !growing_can_grow_into(*g, get(h.rel(g->dir)))) return false;
            int n = growing_is_kelp(*g) ? 1 : r.nextInt(2) + 1;  // getBlocksToGrowWhenBonemealed (vines: NetherVines)
            Pos f = h.rel(g->dir);
            int age = std::min(age_of(hs) + 1, 25);
            for (int i = 0; i < n && growing_can_grow_into(*g, get(f)) && in_height(f.y); i++) {
                set(f, with(hs, "age", age));
                f = f.rel(g->dir);
                age = std::min(age + 1, 25);
            }
            break;
        }
        case K_BAMBOO_SAPLING:
            if (!is_air(get(p.above()))) return false;
            set(p.above(), with(def(ID.bamboo), "leaves", "small"));
            break;
        default: return false;  // ponytail: grass/flower spreading and bigger plants (BonemealableBlock) not ported
    }
    level_event(1505, p, 15);
    return true;
}

inline void consume(Player& pl, ItemStack& slot) {
    if (pl.gamemode != 1 && --slot.count <= 0) slot = {};
}

inline bool item_name_is(int item, const char* n) { return item >= 0 && item_info[size_t(item)].name == std::string("minecraft:") + n; }
inline bool item_ends_with(int item, const char* suffix) {
    if (item < 0) return false;
    const std::string& n = item_info[size_t(item)].name;
    size_t k = strlen(suffix);
    return n.size() >= k && n.compare(n.size() - k, k, suffix) == 0;
}

// ServerPlayerGameMode.useItemOn
inline int use_item_on(Player& pl, int hand, int x, int y, int z, int face, float cx, float cy, float cz) {
    Pos hit{x, y, z};
    ItemStack& slot = pl.inv[hand == 1 ? 40u : size_t(pl.selected)];
    bool hands = !pl.inv[size_t(pl.selected)].empty() || !pl.inv[40].empty();
    bool suppress = pl.g.sneaking && hands;
    int s = get(hit);
    if (!suppress) {
        if (int result = use_cauldron_bucket(pl, hit, s, slot)) return result;
        if (hand == 0 && use_without_item(pl, hit, s)) return 1;
    }
    if (slot.empty() || pl.gamemode == 2) return false;  // ponytail: adventure can_place_on not checked
    int item = slot.item;
    if (item_name_is(item, "bone_meal")) {
        if (!bonemeal(hit)) return false;
        consume(pl, slot);
        return true;
    }
    if (item_ends_with(item, "_hoe")) {  // block_transformer/hoe.json
        if (face == DOWN || !is_air(get(hit.above()))) return false;
        int ns = -1;
        if (is(s, tag("turns_into_farmland"))) ns = def(ID.farmland);
        else if (block(s) == ID.coarse_dirt) ns = blocks::dirt;
        if (ns < 0) return false;
        set(hit, ns, UPDATE_ALL_IMMEDIATE);
        if (pl.gamemode != 1) slot.damage++;
        return true;
    }
    if (item_name_is(item, "flint_and_steel")) {  // FlintAndSteelItem.useOn (campfires/candles not ported)
        Pos t = hit.rel(face);
        if (is_air(get(t))) {
            int fs = fire_state_for(t);
            if (can_survive(fs, t)) {
                set(t, fs, UPDATE_ALL_IMMEDIATE);
                if (pl.gamemode != 1) slot.damage++;
                return true;
            }
        }
        return false;
    }
    int b = item_block(item);
    if (b < 0) return false;
    PlaceCtx c{pl, hit, face, cy, cx, cz, item};
    if (!place_block_item(c, b)) return false;
    if (block_item_placed_hook) block_item_placed_hook(pl, c.pos.x, c.pos.y, c.pos.z, slot);
    consume(pl, slot);
    return true;
}

// ---------- buckets (BucketItem.use, raycast from the eye) ----------
struct Hit { bool ok; Pos pos; int face; };
inline Hit raycast(const Player& p, bool source_fluids) {  // ponytail: every non-air block is a full cube for the ray
    double ex = p.x, ey = p.y + 1.62, ez = p.z;
    double yaw = p.yaw * M_PI / 180, pitch = p.pitch * M_PI / 180;
    double dx = -std::sin(yaw) * std::cos(pitch), dy = -std::sin(pitch), dz = std::cos(yaw) * std::cos(pitch);
    double reach = p.gamemode == 1 ? 5.0 : 4.5;
    Pos last{int(std::floor(ex)), int(std::floor(ey)), int(std::floor(ez))};
    for (double t = 0; t <= reach; t += 0.02) {
        Pos q{int(std::floor(ex + dx * t)), int(std::floor(ey + dy * t)), int(std::floor(ez + dz * t))};
        if (q == last && t > 0) continue;
        int s = get(q);
        bool hit_block = !is_air(s) && kind(s) != K_LIQUID;
        bool hit_fluid = source_fluids && fluid_source(s);
        if (hit_block || hit_fluid) {
            int face = UP;
            for (int d = 0; d < 6; d++)
                if (q.rel(d) == last) face = d;
            return {true, q, face};
        }
        last = q;
    }
    return {false, {}, 0};
}
inline bool empty_bucket_contents(Player& pl, const Pos& p, int fluid, const Hit* hit) {  // BucketItem.emptyContents
    if (!in_height(p.y)) return false;
    int s = get(p);
    bool can_replace = can_be_replaced(s) || !is_solid(s);  // canBeReplaced(state, fluid)
    bool place = is_air(s) || can_replace || (is_container(s) && can_place_liquid(s, fluid, pl.gamemode == 1));
    if (!place) return hit && empty_bucket_contents(pl, hit->pos.rel(hit->face), fluid, nullptr);
    if (is_container(s) && fluid == WATER) {
        place_liquid(p, s, source_of(WATER));
        return true;
    }
    if (can_replace && !liquid(s)) destroy(p, true);
    return set(p, legacy_block(source_of(fluid)), UPDATE_ALL_IMMEDIATE) || fluid_source(s);
}
inline bool use_bucket(Player& pl, ItemStack& slot) {
    int item = slot.item;
    bool water = item_name_is(item, "water_bucket"), lava = item_name_is(item, "lava_bucket"), empty = item_name_is(item, "bucket");
    if (!water && !lava && !empty) return false;
    Hit h = raycast(pl, empty);
    if (!h.ok) return true;
    int s = get(h.pos);
    if (empty) {  // BucketPickup.pickupBlock
        int got = -1;
        if (kind(s) == K_LIQUID && fluid_source(s)) {
            got = item_by_name[is_water(s) ? "minecraft:water_bucket" : "minecraft:lava_bucket"];
            set(h.pos, blocks::air, UPDATE_ALL_IMMEDIATE);
        } else if (waterlogged(s)) {
            got = item_by_name["minecraft:water_bucket"];
            set(h.pos, with(s, "waterlogged", false), UPDATE_ALL);
        } else if (kind(s) == K_BUBBLE_COLUMN) {
            got = item_by_name["minecraft:water_bucket"];
            set(h.pos, blocks::air, UPDATE_ALL_IMMEDIATE);
        }
        if (got >= 0 && pl.gamemode != 1) {
            if (--slot.count <= 0) slot = {got, 1, 0};
            else give_item(pl, got, 1);
        }
        return true;
    }
    int fluid = water ? WATER : LAVA;
    Pos target = is_container(s) && water ? h.pos : h.pos.rel(h.face);
    if (empty_bucket_contents(pl, target, fluid, &h) && pl.gamemode != 1) slot = {item_by_name["minecraft:bucket"], 1, 0};
    return true;
}

}  // namespace lvl

#include "place_rules.hpp"
#include "redstone.hpp"
