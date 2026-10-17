#pragma once
// Redstone (vanilla 26.3, default feature set so RedstoneWireBlock uses DefaultRedstoneWireEvaluator):
//  * SignalGetter: getSignal / getDirectSignal / hasNeighborSignal / getBestNeighborSignal / control inputs,
//  * wire power (HashSet<BlockPos> neighbour-update order reproduced), torches with burnout, repeaters (delay,
//    locking), comparators (compare / subtract, analog inputs), observers, buttons, levers, pressure plates
//    (players), lamps, redstone blocks, daylight detectors, target blocks, copper bulbs,
//  * powered doors / trapdoors / fence gates / note blocks / TNT / dispensers (triggered) / hoppers (enabled),
//  * pistons and sticky pistons: PistonStructureResolver (push limit 12, slime/honey), block events,
//    moving_piston block entities ticked after entities like Level.tickBlockEntities,
//  * rails: RailState on power (RailBlock), powered / activator rail power propagation.
// Included at the end of placement.hpp (after place_rules.hpp); behaviours.hpp / level.hpp / run.hpp call in
// through forward declarations. Tick thread only.
#include <list>

Entity* summon_entity(int type, double x, double y, double z, const Nbt* tag, std::string& err);  // mobs/api.hpp

namespace lvl {

enum RKind : uint8_t {
    R_NONE, R_WIRE, R_TORCH, R_WALL_TORCH, R_REPEATER, R_COMPARATOR, R_OBSERVER, R_BUTTON, R_LEVER, R_PLATE, R_WPLATE,
    R_LAMP, R_POWERED, R_TARGET, R_DAYLIGHT, R_LECTERN, R_HOOK, R_ROD, R_TRAPPED_CHEST, R_DOOR, R_TRAPDOOR, R_GATE,
    R_NOTE, R_TNT, R_PISTON, R_HEAD, R_MOVING, R_DISPENSER, R_HOPPER, R_RAIL, R_POWERED_RAIL, R_DETECTOR_RAIL, R_BULB,
};
struct RsTables {
    std::vector<uint8_t> kind;                     // per block
    std::vector<int8_t> power, facing, powered, lit;  // per state, -1 = no such property
    int wire, piston, sticky_piston, head, moving, comparator, hopper, slime, honey, air;
};
inline RsTables RS;
inline void rs_init() {
    static const std::pair<const char*, RKind> by_class[] = {  // most specific first
        {"RedstoneWireBlock", R_WIRE}, {"RedstoneWallTorchBlock", R_WALL_TORCH}, {"RedstoneTorchBlock", R_TORCH},
        {"RepeaterBlock", R_REPEATER}, {"ComparatorBlock", R_COMPARATOR}, {"ObserverBlock", R_OBSERVER},
        {"ButtonBlock", R_BUTTON}, {"LeverBlock", R_LEVER}, {"WeightedPressurePlateBlock", R_WPLATE},
        {"PressurePlateBlock", R_PLATE}, {"RedstoneLampBlock", R_LAMP}, {"PoweredBlock", R_POWERED},
        {"TargetBlock", R_TARGET}, {"DaylightDetectorBlock", R_DAYLIGHT}, {"LecternBlock", R_LECTERN},
        {"TripWireHookBlock", R_HOOK}, {"LightningRodBlock", R_ROD}, {"TrappedChestBlock", R_TRAPPED_CHEST},
        {"DoorBlock", R_DOOR}, {"TrapDoorBlock", R_TRAPDOOR}, {"FenceGateBlock", R_GATE}, {"NoteBlock", R_NOTE},
        {"TntBlock", R_TNT}, {"PistonBaseBlock", R_PISTON}, {"PistonHeadBlock", R_HEAD}, {"MovingPistonBlock", R_MOVING},
        {"DispenserBlock", R_DISPENSER}, {"HopperBlock", R_HOPPER}, {"DetectorRailBlock", R_DETECTOR_RAIL},
        {"PoweredRailBlock", R_POWERED_RAIL}, {"RailBlock", R_RAIL}, {"CopperBulbBlock", R_BULB},
    };
    RS.kind.assign(block_info.size(), R_NONE);
    for (size_t b = 0; b < block_info.size(); b++)
        for (auto& [c, k] : by_class)
            if (has_class(def(int(b)), c)) { RS.kind[b] = k; break; }
    size_t n = T.flags.size();
    RS.power.assign(n, -1), RS.facing.assign(n, -1), RS.powered.assign(n, -1), RS.lit.assign(n, -1);
    for (size_t s = 0; s < n; s++) {
        if (!RS.kind[size_t(block(int(s)))]) continue;
        int st = int(s);
        if (has(st, "power")) RS.power[s] = int8_t(geti(st, "power"));
        if (has(st, "facing")) RS.facing[s] = int8_t(getdir(st));
        if (has(st, "powered")) RS.powered[s] = int8_t(getb(st, "powered"));
        if (has(st, "lit")) RS.lit[s] = int8_t(getb(st, "lit"));
    }
    RS.wire = block_id("redstone_wire"), RS.piston = block_id("piston"), RS.sticky_piston = block_id("sticky_piston");
    RS.head = block_id("piston_head"), RS.moving = block_id("moving_piston"), RS.comparator = block_id("comparator");
    RS.hopper = block_id("hopper"), RS.slime = block_id("slime_block"), RS.honey = block_id("honey_block");
    RS.air = blocks::air;
}
inline int rkind(int s) {
    if (RS.kind.empty()) rs_init();
    return RS.kind[size_t(block(s))];
}
inline int rpower(int s) { return RS.power[size_t(s)]; }
inline int rfacing(int s) { return RS.facing[size_t(s)]; }
inline bool rpowered(int s) { return RS.powered[size_t(s)] == 1; }
inline bool rlit(int s) { return RS.lit[size_t(s)] == 1; }

// ---------- sounds / block events ----------
inline void rs_sound(const char* name, const Pos& p, float vol, float pitch) {  // Level.playSound(null, pos, ..., BLOCKS)
    Writer w;
    w.varint(play_cb::sound).varint(0).str(std::string("minecraft:") + name).boolean(false).varint(4)
        .i32(int32_t(std::floor((p.x + 0.5) * 8))).i32(int32_t(std::floor((p.y + 0.5) * 8))).i32(int32_t(std::floor((p.z + 0.5) * 8)))
        .f32(vol).f32(pitch).i64(rng().nextLong());
    double r = vol > 1 ? 16.0 * vol : 16.0;
    for (auto& pl : players)
        if ((pl->x - p.x - 0.5) * (pl->x - p.x - 0.5) + (pl->y - p.y - 0.5) * (pl->y - p.y - 0.5) + (pl->z - p.z - 0.5) * (pl->z - p.z - 0.5) < r * r) pl->send(w);
}

// ==================================================================================================
// SignalGetter
// ==================================================================================================
inline bool should_signal = true;  // RedstoneWireBlock.shouldSignal (one wire block, tick thread only)
inline int comparator_output(const Pos& p);
inline bool is_signal_source(int s) { return rkind(s) == R_WIRE ? should_signal : (T.flags[size_t(s)] & F_SIGNAL_SOURCE) != 0; }
inline int attached_dir(int s) {  // FaceAttachedHorizontalDirectionalBlock.getConnectedDirection
    auto f = get(s, "face");
    return f == "ceiling" ? DOWN : f == "floor" ? UP : rfacing(s);
}
inline int own_signal(int s, const Pos& p) {  // BlockState.getOwnSignal
    switch (rkind(s)) {
        case R_WIRE: case R_WPLATE: case R_TARGET: case R_DAYLIGHT: return rpower(s);
        case R_TORCH: case R_WALL_TORCH: return rlit(s) ? 15 : 0;
        case R_COMPARATOR: return rpowered(s) ? comparator_output(p) : 0;
        case R_REPEATER: case R_OBSERVER: case R_BUTTON: case R_LEVER: case R_PLATE: case R_LECTERN: case R_HOOK:
        case R_ROD: case R_DETECTOR_RAIL: return rpowered(s) ? 15 : 0;
        case R_POWERED: return 15;
        default: return 0;  // ponytail: trapped chests / jukeboxes / sculk sensors need their block entities
    }
}
inline int wire_connection_state(int s, const Pos& p);  // place_rules.hpp
inline int signal_of(int s, const Pos& p, int dir) {  // BlockState.getSignal(level, pos, direction)
    switch (rkind(s)) {
        case R_WIRE: {
            if (!should_signal || dir == DOWN) return 0;
            int pw = rpower(s);
            if (pw == 0) return 0;
            return dir != UP && !wire_conn(wire_connection_state(s, p), OPP[dir]) ? 0 : pw;
        }
        case R_TORCH: return dir != UP ? own_signal(s, p) : 0;
        case R_WALL_TORCH: return rfacing(s) != dir ? own_signal(s, p) : 0;
        case R_REPEATER: case R_COMPARATOR: case R_OBSERVER: return rfacing(s) == dir ? own_signal(s, p) : 0;
        case R_NONE: return 0;
        default: return own_signal(s, p);
    }
}
inline int direct_signal_of(int s, const Pos& p, int dir) {  // BlockState.getDirectSignal
    switch (rkind(s)) {
        case R_WIRE: return should_signal ? signal_of(s, p, dir) : 0;
        case R_TORCH: case R_WALL_TORCH: return dir == DOWN ? signal_of(s, p, dir) : 0;
        case R_REPEATER: case R_COMPARATOR: case R_OBSERVER: return signal_of(s, p, dir);
        case R_BUTTON: case R_LEVER: return rpowered(s) && attached_dir(s) == dir ? 15 : 0;
        case R_PLATE: case R_WPLATE: case R_LECTERN: case R_DETECTOR_RAIL: case R_TRAPPED_CHEST:
            return dir == UP ? own_signal(s, p) : 0;
        case R_HOOK: case R_ROD: return rpowered(s) && rfacing(s) == dir ? 15 : 0;
        default: return 0;
    }
}
inline int direct_signal_to(const Pos& p) {  // getDirectSignalTo
    static constexpr int ORDER[6] = {DOWN, UP, NORTH, SOUTH, WEST, EAST};
    int r = 0;
    for (int d : ORDER) {
        Pos q = p.rel(d);
        r = std::max(r, direct_signal_of(get(q), q, d));
        if (r >= 15) return r;
    }
    return r;
}
inline int get_signal(const Pos& p, int dir) {  // SignalGetter.getSignal
    int s = get(p);
    int v = signal_of(s, p, dir);
    return conductor(s) ? std::max(v, direct_signal_to(p)) : v;
}
inline bool has_signal(const Pos& p, int dir) { return get_signal(p, dir) > 0; }
inline bool has_neighbor_signal(const Pos& p) {
    for (int d : {DOWN, UP, NORTH, SOUTH, WEST, EAST})
        if (get_signal(p.rel(d), d) > 0) return true;
    return false;
}
inline int best_neighbor_signal(const Pos& p) {
    int best = 0;
    for (int d = 0; d < 6; d++) {
        int v = get_signal(p.rel(d), d);
        if (v >= 15) return 15;
        best = std::max(best, v);
    }
    return best;
}
inline bool is_diode(int s) { int k = rkind(s); return k == R_REPEATER || k == R_COMPARATOR; }
inline int control_input_signal(const Pos& p, int dir, bool only_diodes) {
    int s = get(p);
    if (only_diodes) return is_diode(s) ? direct_signal_of(s, p, dir) : 0;
    if (rkind(s) == R_POWERED) return 15;
    if (rkind(s) == R_WIRE) return rpower(s);
    return is_signal_source(s) ? direct_signal_of(s, p, dir) : 0;
}

// ---------- analog output (hasAnalogOutputSignal / getAnalogOutputSignal) ----------
// Containers: set by the container block entities (AbstractContainerMenu.getRedstoneSignalFromBlockEntity).
inline int (*container_signal)(int state, const Pos& p) = nullptr;
inline int analog_output(int s, const Pos& p) {
    const std::string& n = block_of(s).name;
    if (has(s, "bites")) return (7 - geti(s, "bites")) * 2;  // cake
    if (n == "minecraft:composter" || n == "minecraft:water_cauldron" || n == "minecraft:powder_snow_cauldron") return geti(s, "level");
    if (n == "minecraft:lava_cauldron") return 3;
    if (n == "minecraft:end_portal_frame") return getb(s, "eye") ? 15 : 0;
    if (n == "minecraft:respawn_anchor") return int(std::floor(geti(s, "charges") / 4.0f * 15));
    if (has(s, "honey_level")) return geti(s, "honey_level");
    if (rkind(s) == R_BULB) return rlit(get(p)) ? 15 : 0;
    if (has_class(s, "CandleCakeBlock")) return 14;
    if (has_class(s, "AbstractCauldronBlock")) return 0;
    return container_signal ? container_signal(s, p) : 0;  // ponytail: chests/hoppers/... until containers lands
}
inline bool has_analog(int s) { return T.flags[size_t(s)] & F_ANALOG; }

// Level.updateNeighbourForOutputSignal
inline void update_neighbour_for_output_signal(const Pos& p, int src_block) {
    for (int d : HORIZONTAL) {
        Pos q = p.rel(d);
        if (!loaded(q)) continue;
        int s = get(q);
        if (block(s) == RS.comparator) neighbor_changed_full(s, q, src_block, false);
        else if (conductor(s)) {
            q = q.rel(d);
            s = get(q);
            if (block(s) == RS.comparator) neighbor_changed_full(s, q, src_block, false);
        }
    }
}

// Java HashMap / HashSet iteration order of up to 12 BlockPos keys (default capacity 16, no resize):
// bucket (h ^ h >>> 16) & 15 with Vec3i.hashCode, insertion order within a bucket.
inline int pos_bucket(const Pos& p) {
    uint32_t h = uint32_t((p.y + p.z * 31) * 31 + p.x);
    return int((h ^ (h >> 16)) & 15);
}
template <class T, class F> inline void java_hash_order(std::vector<T>& v, F&& key) {
    std::stable_sort(v.begin(), v.end(), [&](const T& a, const T& b) { return pos_bucket(key(a)) < pos_bucket(key(b)); });
}

// ==================================================================================================
// redstone wire (DefaultRedstoneWireEvaluator)
// ==================================================================================================
inline int wire_power_of(int s) { return rkind(s) == R_WIRE ? rpower(s) : 0; }
inline int wire_target_strength(const Pos& p) {
    should_signal = false;
    int bs = best_neighbor_signal(p);
    should_signal = true;
    if (bs == 15) return 15;
    int w = 0;
    bool above_cond = conductor(get(p.above()));
    for (int d : HORIZONTAL) {
        Pos np = p.rel(d);
        int ns = get(np);
        w = std::max(w, wire_power_of(ns));
        if (conductor(ns) && !above_cond) w = std::max(w, wire_power_of(get(np.above())));
        else if (!conductor(ns)) w = std::max(w, wire_power_of(get(np.below())));
    }
    return std::max(bs, std::max(0, w - 1));
}
inline void wire_update_power(const Pos& p, int s) {
    int t = wire_target_strength(p);
    if (rpower(s) == t) return;
    if (get(p) == s) set(p, with(s, "power", t), UPDATE_CLIENTS);
    std::vector<Pos> to{p};
    for (int d = 0; d < 6; d++) to.push_back(p.rel(d));
    java_hash_order(to, [](const Pos& q) { return q; });
    for (auto& q : to) update_neighbors(q, RS.wire);
}
inline void wire_check_corner(const Pos& p) {
    if (block(get(p)) != RS.wire) return;
    update_neighbors(p, RS.wire);
    for (int d = 0; d < 6; d++) update_neighbors(p.rel(d), RS.wire);
}
inline void wire_update_neighbors_of_wires(const Pos& p) {
    for (int d : HORIZONTAL) wire_check_corner(p.rel(d));
    for (int d : HORIZONTAL) {
        Pos t = p.rel(d);
        wire_check_corner(conductor(get(t)) ? t.above() : t.below());
    }
}

// ==================================================================================================
// torches
// ==================================================================================================
struct Toggle { Pos pos; int64_t when; };
inline std::vector<Toggle> recent_toggles;
inline bool toggled_too_frequently(const Pos& p, bool add) {
    if (add) recent_toggles.push_back({p, now()});
    int n = 0;
    for (auto& t : recent_toggles)
        if (t.pos == p && ++n >= 8) return true;
    return false;
}
inline bool torch_has_signal(int s, const Pos& p) {
    if (rkind(s) == R_WALL_TORCH) {
        int o = OPP[rfacing(s)];
        return has_signal(p.rel(o), o);
    }
    return has_signal(p.below(), DOWN);
}
inline void torch_notify(const Pos& p, int b) {
    for (int d = 0; d < 6; d++) update_neighbors(p.rel(d), b);
}

// ==================================================================================================
// diodes (DiodeBlock, RepeaterBlock, ComparatorBlock)
// ==================================================================================================
inline std::unordered_map<int64_t, int> comparator_out;  // ComparatorBlockEntity.output (ponytail: not saved)
inline int comparator_output(const Pos& p) {
    auto it = comparator_out.find(pos_key(p));
    return it == comparator_out.end() ? 0 : it->second;
}
inline int diode_delay(int s) { return rkind(s) == R_REPEATER ? geti(s, "delay") * 2 : 2; }
inline int alternate_signal(const Pos& p, int s) {
    int f = rfacing(s), cw = clockwise(f), cc = ccw(f);
    bool only = rkind(s) == R_REPEATER;
    return std::max(control_input_signal(p.rel(cw), cw, only), control_input_signal(p.rel(cc), cc, only));
}
inline bool diode_locked(const Pos& p, int s) { return rkind(s) == R_REPEATER && alternate_signal(p, s) > 0; }
inline int diode_input(const Pos& p, int s) {
    int d = rfacing(s);
    Pos tp = p.rel(d);
    int in = get_signal(tp, d);
    int ts = get(tp);
    if (in < 15) in = std::max(in, rkind(ts) == R_WIRE ? rpower(ts) : 0);
    if (rkind(s) != R_COMPARATOR) return in;
    if (has_analog(ts)) return analog_output(ts, tp);
    if (in < 15 && conductor(ts)) {
        tp = tp.rel(d);
        ts = get(tp);
        if (has_analog(ts)) return analog_output(ts, tp);  // ponytail: item frames not counted
    }
    return in;
}
inline bool diode_should_turn_on(const Pos& p, int s) {
    int in = diode_input(p, s);
    if (rkind(s) != R_COMPARATOR) return in > 0;
    if (in == 0) return false;
    int side = alternate_signal(p, s);
    return in > side || (in == side && get(s, "mode") == "compare");
}
inline int comparator_calculate(const Pos& p, int s) {
    int in = diode_input(p, s);
    if (in == 0) return 0;
    int alt = alternate_signal(p, s);
    if (alt > in) return 0;
    return get(s, "mode") == "subtract" ? in - alt : in;
}
inline bool diode_prioritize(const Pos& p, int s) {
    int d = OPP[rfacing(s)];
    int o = get(p.rel(d));
    return is_diode(o) && rfacing(o) != d;
}
inline void update_neighbors_in_front(const Pos& p, int s) {  // DiodeBlock / ObserverBlock
    int f = rfacing(s);
    Pos op = p.rel(OPP[f]);
    neighbor_changed_at(op, block(s));
    update_neighbors_except(op, block(s), f);
}
inline void diode_check_tick(const Pos& p, int s) {
    int b = block(s);
    if (rkind(s) == R_COMPARATOR) {
        if (block_ticks.will_tick_this_tick(p, b)) return;
        if (comparator_calculate(p, s) != comparator_output(p) || rpowered(s) != diode_should_turn_on(p, s))
            schedule_block(p, b, 2, diode_prioritize(p, s) ? HIGH : NORMAL);
        return;
    }
    if (diode_locked(p, s)) return;
    bool on = rpowered(s);
    if (on != diode_should_turn_on(p, s) && !block_ticks.will_tick_this_tick(p, b))
        schedule_block(p, b, diode_delay(s), diode_prioritize(p, s) ? EXTREMELY_HIGH : on ? VERY_HIGH : HIGH);
}
inline void comparator_refresh(const Pos& p, int s) {
    int out = comparator_calculate(p, s);
    int old = comparator_output(p);
    comparator_out[pos_key(p)] = out;
    if (old != out || get(s, "mode") == "compare") {
        bool src = diode_should_turn_on(p, s), on = rpowered(s);
        if (on && !src) set(p, with(s, "powered", false), UPDATE_CLIENTS);
        else if (!on && src) set(p, with(s, "powered", true), UPDATE_CLIENTS);
        update_neighbors_in_front(p, s);
    }
}

// ==================================================================================================
// pistons
// ==================================================================================================
enum Push { PUSH_PULL, PUSH_ONLY, POPPED, IMMOVEABLE, IGNORE_ENTITY };
inline int push_reaction(int s) { return int(T.flags[size_t(s)] >> 50 & 7); }
inline bool is_piston_base(int s) { int b = block(s); return b == RS.piston || b == RS.sticky_piston; }
inline int max_y() { return MIN_Y + HEIGHT - 1; }
inline bool is_pushable(int s, const Pos& p, int dir, bool allow_destroy, int conn) {  // PistonBaseBlock.isPushable
    if (p.y < MIN_Y || p.y > max_y()) return false;
    if (is_air(s)) return true;
    if ((dir == DOWN && p.y == MIN_Y) || (dir == UP && p.y == max_y())) return false;
    if (!is_piston_base(s)) {
        if (block_of(s).hardness == -1.0f) return false;
        switch (push_reaction(s)) {
            case IMMOVEABLE: return false;
            case POPPED: return allow_destroy;
            case PUSH_ONLY: return dir == conn;
            default: break;
        }
    } else if (getb(s, "extended")) {
        return false;
    }
    return !(T.flags[size_t(s)] & F_BLOCK_ENTITY);
}
struct Resolver {  // PistonStructureResolver
    Pos piston, start;
    int piston_dir, push_dir;
    bool extending;
    std::vector<Pos> to_push, to_destroy;
    Resolver(const Pos& p, int dir, bool ext) : piston(p), piston_dir(dir), extending(ext) {
        push_dir = ext ? dir : OPP[dir];
        start = ext ? p.rel(dir) : p.rel(dir, 2);
    }
    static bool sticky(int s) { return block(s) == RS.slime || block(s) == RS.honey; }
    static bool stick(int a, int b) {
        if (block(a) == RS.honey && block(b) == RS.slime) return false;
        if (block(a) == RS.slime && block(b) == RS.honey) return false;
        return sticky(a) || sticky(b);
    }
    int index_of(const Pos& p) const {
        for (size_t i = 0; i < to_push.size(); i++) if (to_push[i] == p) return int(i);
        return -1;
    }
    bool resolve() {
        to_push.clear(), to_destroy.clear();
        int ns = get(start);
        if (!is_pushable(ns, start, push_dir, false, piston_dir)) {
            if (extending && push_reaction(ns) == POPPED) {
                to_destroy.push_back(start);
                return true;
            }
            return false;
        }
        if (!add_line(start, push_dir)) return false;
        for (size_t i = 0; i < to_push.size(); i++)
            if (sticky(get(to_push[i])) && !add_branching(to_push[i])) return false;
        return true;
    }
    bool add_line(const Pos& st, int dir) {
        int ns = get(st);
        if (is_air(ns) || !is_pushable(ns, st, push_dir, false, dir) || st == piston || index_of(st) >= 0) return true;
        int count = 1;
        if (count + int(to_push.size()) > 12) return false;
        while (sticky(ns)) {
            Pos p = st.rel(OPP[push_dir], count);
            int prev = ns;
            ns = get(p);
            if (is_air(ns) || !stick(prev, ns) || !is_pushable(ns, p, push_dir, false, OPP[push_dir]) || p == piston) break;
            if (++count + int(to_push.size()) > 12) return false;
        }
        int added = 0;
        for (int i = count - 1; i >= 0; i--) to_push.push_back(st.rel(OPP[push_dir], i)), added++;
        for (int i = 1;; i++) {
            Pos p = st.rel(push_dir, i);
            int col = index_of(p);
            if (col > -1) {
                reorder(added, col);
                for (int j = 0; j <= col + added; j++)
                    if (sticky(get(to_push[size_t(j)])) && !add_branching(to_push[size_t(j)])) return false;
                return true;
            }
            ns = get(p);
            if (is_air(ns)) return true;
            if (!is_pushable(ns, p, push_dir, true, push_dir) || p == piston) return false;
            if (push_reaction(ns) == POPPED) {
                to_destroy.push_back(p);
                return true;
            }
            if (to_push.size() >= 12) return false;
            to_push.push_back(p);
            added++;
        }
    }
    void reorder(int added, int col) {
        std::vector<Pos> r(to_push.begin(), to_push.begin() + col);
        r.insert(r.end(), to_push.end() - added, to_push.end());
        r.insert(r.end(), to_push.begin() + col, to_push.end() - added);
        to_push = std::move(r);
    }
    bool add_branching(const Pos& from) {
        int fs = get(from);
        for (int d = 0; d < 6; d++) {
            if (axis(d) == axis(push_dir)) continue;
            Pos n = from.rel(d);
            if (stick(get(n), fs) && !add_line(n, d)) return false;
        }
        return true;
    }
};

// PistonMovingBlockEntity, ticked in creation order like Level.blockEntityTickers (a block entity replaced
// at the same position keeps its ticker slot: LevelChunk.RebindableTickingBlockEntityWrapper).
struct Moving {
    Pos pos;
    int moved, dir;
    bool extending, source;
    float progress = 0, progress_o = 0;
    int64_t last_ticked = 0;
    bool removed = false;
};
inline std::list<Moving> moving_list;
inline std::unordered_map<int64_t, Moving*> moving_at;
inline bool ticking_block_entities = false;
inline size_t moving_pending = 0;  // added while ticking: not ticked until the next tick
inline Moving* moving(const Pos& p) {
    auto it = moving_at.find(pos_key(p));
    return it == moving_at.end() ? nullptr : it->second;
}
inline void moving_set(const Moving& m) {
    if (Moving* e = moving(m.pos)) { *e = m; return; }
    moving_list.push_back(m);
    moving_at[pos_key(m.pos)] = &moving_list.back();
    if (ticking_block_entities) moving_pending++;
}
inline void moving_remove(const Pos& p) {
    auto it = moving_at.find(pos_key(p));
    if (it == moving_at.end()) return;
    it->second->removed = true;
    moving_at.erase(it);
}
inline int moving_state(int dir, bool sticky) {
    return with(withdir(def(RS.moving), "facing", dir), "type", sticky ? "sticky" : "normal");
}
inline void moving_final_tick(Moving& m) {  // PistonMovingBlockEntity.finalTick
    if (m.progress_o >= 1.0f) return;
    m.progress = m.progress_o = 1.0f;
    Pos p = m.pos;
    int moved = m.moved;
    bool source = m.source;
    moving_remove(p);
    if (block(get(p)) == RS.moving) {
        int ns = source ? RS.air : update_from_neighbour_shapes(moved, p);
        set(p, ns, UPDATE_ALL);
        neighbor_changed_at(p, block(ns));
    }
}
inline void moving_tick(Moving& m) {  // PistonMovingBlockEntity.tick
    m.last_ticked = now();
    m.progress_o = m.progress;
    if (m.progress_o >= 1.0f) {
        Pos p = m.pos;
        int moved = m.moved;
        moving_remove(p);
        if (block(get(p)) == RS.moving) {
            int ns = update_from_neighbour_shapes(moved, p);
            if (is_air(ns)) {
                set(p, moved, UPDATE_KNOWN_SHAPE | UPDATE_INVISIBLE | UPDATE_MOVE_BY_PISTON | UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS);
                update_or_destroy(moved, ns, p, UPDATE_ALL, UPDATE_LIMIT);
            } else {
                if (waterlogged(ns)) ns = with(ns, "waterlogged", false);
                set(p, ns, UPDATE_ALL | UPDATE_MOVE_BY_PISTON);
                neighbor_changed_at(p, block(ns));
            }
        }
    } else {
        m.progress = std::min(1.0f, m.progress + 0.5f);  // ponytail: entities are not pushed (moveCollidedEntities)
    }
}
inline void tick_block_entities() {  // Level.tickBlockEntities (moving pistons)
    ticking_block_entities = true;
    moving_pending = 0;
    size_t n = moving_list.size();
    auto it = moving_list.begin();
    for (size_t i = 0; i < n && it != moving_list.end(); i++) {
        if (it->removed) { it = moving_list.erase(it); continue; }
        if (ticking.count(chunk_key(it->pos.x >> 4, it->pos.z >> 4))) moving_tick(*it);
        ++it;
    }
    ticking_block_entities = false;
}

// ServerLevel.blockEvent / runBlockEvents (an ObjectLinkedOpenHashSet: duplicates are dropped).
struct BlockEvent {
    Pos pos;
    int block, a, b;
    bool operator==(const BlockEvent&) const = default;
};
inline std::deque<BlockEvent> block_events;
inline void block_event(const Pos& p, int b, int a, int bb) {
    BlockEvent e{p, b, a, bb};
    if (std::find(block_events.begin(), block_events.end(), e) == block_events.end()) block_events.push_back(e);
}
inline bool trigger_event(int s, const Pos& p, int a, int b);
inline void run_block_events() {
    std::deque<BlockEvent> later;
    while (!block_events.empty()) {
        BlockEvent e = block_events.front();
        block_events.pop_front();
        if (!ticking.count(chunk_key(e.pos.x >> 4, e.pos.z >> 4))) { later.push_back(e); continue; }
        int s = get(e.pos);
        if (block(s) != e.block || !trigger_event(s, e.pos, e.a, e.b)) continue;
        Writer w;
        w.varint(play_cb::block_event).i64(pos_key(e.pos)).u8(uint8_t(e.a)).u8(uint8_t(e.b)).varint(e.block);
        for (auto& pl : players)
            if (std::abs(pl->x - e.pos.x) < 64 && std::abs(pl->y - e.pos.y) < 64 && std::abs(pl->z - e.pos.z) < 64 &&
                (pl->x - e.pos.x) * (pl->x - e.pos.x) + (pl->y - e.pos.y) * (pl->y - e.pos.y) + (pl->z - e.pos.z) * (pl->z - e.pos.z) < 64 * 64)
                pl->send(w);
    }
    block_events = std::move(later);
}

inline bool piston_neighbor_signal(const Pos& p, int push) {  // PistonBaseBlock.getNeighborSignal
    for (int d = 0; d < 6; d++)
        if (d != push && has_signal(p.rel(d), d)) return true;
    if (has_signal(p, DOWN)) return true;
    Pos a = p.above();
    for (int d = 0; d < 6; d++)
        if (d != DOWN && has_signal(a.rel(d), d)) return true;
    return false;
}
inline void piston_check_extend(const Pos& p, int s) {
    int dir = rfacing(s);
    bool ext = piston_neighbor_signal(p, dir);
    bool extended = getb(s, "extended");
    if (ext && !extended) {
        if (Resolver(p, dir, true).resolve()) block_event(p, block(s), 0, dir);
    } else if (!ext && extended) {
        Pos pp = p.rel(dir, 2);
        int ps = get(pp);
        int ev = 1;
        Moving* m = moving(pp);
        if (block(ps) == RS.moving && rfacing(ps) == dir && m && m->extending &&
            (m->progress_o < 0.5f || now() == m->last_ticked || handling_tick))
            ev = 2;
        block_event(p, block(s), ev, dir);
    }
}
inline bool piston_move_blocks(const Pos& pp, int dir, bool extending, bool sticky_piston) {
    Pos arm = pp.rel(dir);
    if (!extending && block(get(arm)) == RS.head) set(arm, RS.air, UPDATE_NONE | UPDATE_KNOWN_SHAPE);
    Resolver r(pp, dir, extending);
    if (!r.resolve()) return false;
    struct Del { Pos pos; int state; bool gone; };
    std::vector<Del> del;  // deleteAfterMove (HashMap<BlockPos, BlockState>)
    std::vector<int> shapes;
    for (auto& p : r.to_push) {
        int s = get(p);
        shapes.push_back(s);
        del.push_back({p, s, false});
    }
    auto del_remove = [&](const Pos& p) {
        for (auto& d : del) if (!d.gone && d.pos == p) d.gone = true;
    };
    std::vector<int> upd;
    int push_dir = extending ? dir : OPP[dir];
    for (int i = int(r.to_destroy.size()) - 1; i >= 0; i--) {
        Pos p = r.to_destroy[size_t(i)];
        int s = get(p);
        drop_resources(s, p);
        set(p, RS.air, UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE);
        upd.push_back(s);
    }
    for (int i = int(r.to_push.size()) - 1; i >= 0; i--) {
        Pos p = r.to_push[size_t(i)];
        int bs = get(p);
        p = p.rel(push_dir);
        del_remove(p);
        int actual = withdir(def(RS.moving), "facing", dir);
        set(p, actual, UPDATE_INVISIBLE | UPDATE_MOVE_BY_PISTON | UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS);
        moving_set({p, shapes[size_t(i)], dir, extending, false});
        upd.push_back(bs);
    }
    if (extending) {
        int head = with(withdir(def(RS.head), "facing", dir), "type", sticky_piston ? "sticky" : "normal");
        del_remove(arm);
        set(arm, moving_state(dir, sticky_piston), UPDATE_INVISIBLE | UPDATE_MOVE_BY_PISTON | UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS);
        moving_set({arm, head, dir, true, true});
    }
    std::vector<Del> rest;
    for (auto& d : del) if (!d.gone) rest.push_back(d);
    java_hash_order(rest, [](const Del& d) { return d.pos; });
    for (auto& d : rest) set(d.pos, RS.air, UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE | UPDATE_MOVE_BY_PISTON);
    for (auto& d : rest) {
        wire_indirect_shapes(d.state, d.pos, UPDATE_CLIENTS, UPDATE_LIMIT);
        update_neighbour_shapes(RS.air, d.pos, UPDATE_CLIENTS, UPDATE_LIMIT);
    }
    size_t k = 0;
    for (int i = int(r.to_destroy.size()) - 1; i >= 0; i--) {
        int s = upd[k++];
        Pos p = r.to_destroy[size_t(i)];
        after_removal(s, p, false);
        wire_indirect_shapes(s, p, UPDATE_CLIENTS, UPDATE_LIMIT);
        update_neighbors(p, block(s));
    }
    for (int i = int(r.to_push.size()) - 1; i >= 0; i--) update_neighbors(r.to_push[size_t(i)], block(upd[k++]));
    if (extending) update_neighbors(arm, RS.head);
    return true;
}
inline bool piston_trigger(int s, const Pos& p, int a, int b) {  // PistonBaseBlock.triggerEvent
    int dir = rfacing(s);
    bool sticky = block(s) == RS.sticky_piston;
    int ext_state = with(s, "extended", true);
    bool ext = piston_neighbor_signal(p, dir);
    if (ext && (a == 1 || a == 2)) {
        set(p, ext_state, UPDATE_CLIENTS);
        return false;
    }
    if (!ext && a == 0) return false;
    if (a == 0) {
        if (!piston_move_blocks(p, dir, true, sticky)) return false;
        set(p, ext_state, UPDATE_ALL | UPDATE_MOVE_BY_PISTON);
        rs_sound("block.piston.extend", p, 0.5f, rng().nextFloat() * 0.25f + 0.6f);
    } else {
        if (Moving* m = moving(p.rel(dir))) moving_final_tick(*m);
        int mps = moving_state(dir, sticky);
        set(p, mps, UPDATE_NONE | UPDATE_KNOWN_SHAPE);
        moving_set({p, withdir(def(block(s)), "facing", b & 7), dir, false, true});
        update_neighbors(p, RS.moving);
        update_neighbour_shapes(mps, p, UPDATE_CLIENTS, UPDATE_LIMIT);
        if (sticky) {
            Pos two = p.rel(dir, 2);
            int ms = get(two);
            bool piece = false;
            Moving* m = moving(two);
            if (block(ms) == RS.moving && m && m->dir == dir && m->extending) {
                moving_final_tick(*m);
                piece = true;
            }
            if (!piece) {
                if (a != 1 || is_air(ms) || !is_pushable(ms, two, OPP[dir], false, dir) ||
                    (push_reaction(ms) != PUSH_PULL && !is_piston_base(ms)))
                    remove(p.rel(dir), false);
                else
                    piston_move_blocks(p, dir, false, sticky);
            }
        } else {
            remove(p.rel(dir), false);
        }
        rs_sound("block.piston.contract", p, 0.5f, rng().nextFloat() * 0.15f + 0.6f);
    }
    return true;
}
inline bool head_fitting_base(int head, int base) {
    int want = get(head, "type") == "normal" ? RS.piston : RS.sticky_piston;
    return block(base) == want && getb(base, "extended") && rfacing(base) == rfacing(head);
}
inline bool head_can_survive(int s, const Pos& p) {
    int base = get(p.rel(OPP[rfacing(s)]));
    return head_fitting_base(s, base) || (block(base) == RS.moving && rfacing(base) == rfacing(s));
}

inline bool trigger_event(int s, const Pos& p, int a, int b) {  // BlockState.triggerEvent
    switch (rkind(s)) {
        case R_PISTON: return piston_trigger(s, p, a, b);
        case R_NOTE: return true;  // ponytail: the note sound (NoteBlock.triggerEvent) is not sent; clients show the particle
        default: return false;
    }
}

// ==================================================================================================
// rails
// ==================================================================================================
inline bool rail_should_remove(int s, const Pos& p) {
    if (!can_support_rigid(p.below())) return true;
    auto shape = get(s, "shape");
    if (shape.rfind("ascending_", 0) == 0) return !can_support_rigid(p.rel(dir_by_name(shape.substr(10))));
    return false;
}
inline bool powered_rail_same(const Pos& p, bool fwd, int depth, std::string_view dir, int b);
inline bool powered_rail_find(const Pos& p0, int s, bool fwd, int depth) {  // findPoweredRailSignal
    if (depth >= 8) return false;
    int x = p0.x, y = p0.y, z = p0.z;
    bool below = true;
    std::string_view shape = get(s, "shape");
    if (shape == "north_south") fwd ? z++ : z--;
    else if (shape == "east_west") fwd ? x-- : x++;
    else if (shape == "ascending_east") { if (fwd) x--; else x++, y++, below = false; shape = "east_west"; }
    else if (shape == "ascending_west") { if (fwd) x--, y++, below = false; else x++; shape = "east_west"; }
    else if (shape == "ascending_north") { if (fwd) z++; else z--, y++, below = false; shape = "north_south"; }
    else if (shape == "ascending_south") { if (fwd) z++, y++, below = false; else z--; shape = "north_south"; }
    if (powered_rail_same({x, y, z}, fwd, depth, shape, block(s))) return true;
    return below && powered_rail_same({x, y - 1, z}, fwd, depth, shape, block(s));
}
inline bool powered_rail_same(const Pos& p, bool fwd, int depth, std::string_view dir, int b) {  // isSameRailWithPower
    int s = get(p);
    if (block(s) != b) return false;
    auto my = get(s, "shape");
    if (dir == "east_west" && (my == "north_south" || my == "ascending_north" || my == "ascending_south")) return false;
    if (dir == "north_south" && (my == "east_west" || my == "ascending_east" || my == "ascending_west")) return false;
    if (!rpowered(s)) return false;
    return has_neighbor_signal(p) || powered_rail_find(p, s, fwd, depth + 1);
}
inline int rail_potential_connections(const Pos& p) {
    int n = 0;
    for (int d : HORIZONTAL) {
        Pos q = p.rel(d);
        if (is_rail(get(q)) || is_rail(get(q.above())) || is_rail(get(q.below()))) n++;
    }
    return n;
}
inline void rail_update_state(int s, const Pos& p, int src) {  // BaseRailBlock.updateState(state, level, pos, block)
    switch (rkind(s)) {
        case R_RAIL:
            if ((T.flags[size_t(def(src))] & F_SIGNAL_SOURCE) && rail_potential_connections(p) == 3) {
                Rail r(p, s);
                r.place(has_neighbor_signal(p), false, std::string(get(s, "shape")));
            }
            break;
        case R_POWERED_RAIL: {
            bool on = rpowered(s);
            bool should = has_neighbor_signal(p) || powered_rail_find(p, s, true, 0) || powered_rail_find(p, s, false, 0);
            if (should != on) {
                set(p, with(s, "powered", should));
                update_neighbors(p.below(), block(s));
                if (get(s, "shape").rfind("ascending_", 0) == 0) update_neighbors(p.above(), block(s));
            }
            break;
        }
        default: break;  // ponytail: detector rails need minecarts
    }
}

// ==================================================================================================
// pressure plates (players only), daylight detectors
// ==================================================================================================
inline int plate_strength(int s, const Pos& p) {  // getSignalStrength over TOUCH_AABB (1/16 .. 15/16, height 4/16)
    int n = 0;
    for (auto& pl : players) {
        if (pl->gamemode == 3 || pl->g.dead) continue;
        if (pl->x + 0.3 > p.x + 0.0625 && pl->x - 0.3 < p.x + 0.9375 && pl->z + 0.3 > p.z + 0.0625 && pl->z - 0.3 < p.z + 0.9375 &&
            pl->y < p.y + 0.25 && pl->y + 1.8 > p.y)
            n++;
    }  // ponytail: mobs and items don't press plates yet
    if (rkind(s) == R_PLATE) return n > 0 ? 15 : 0;
    int maxw = block_of(s).name == "minecraft:light_weighted_pressure_plate" ? 15 : 150;
    n = std::min(n, maxw);
    return n > 0 ? int(std::ceil(float(std::min(maxw, n)) / float(maxw) * 15.0f)) : 0;
}
inline int plate_signal(int s) { return rkind(s) == R_PLATE ? (rpowered(s) ? 15 : 0) : rpower(s); }
inline void plate_update_neighbours(const Pos& p, int b) {
    update_neighbors(p, b);
    update_neighbors(p.below(), b);
}
inline void plate_check(const Pos& p, int s, int old) {  // BasePressurePlateBlock.checkPressed
    int sig = plate_strength(s, p);
    if (old != sig) {
        int ns = rkind(s) == R_PLATE ? with(s, "powered", sig > 0) : with(s, "power", sig);
        set(p, ns, UPDATE_CLIENTS);
        plate_update_neighbours(p, block(s));
    }
    if (sig > 0) schedule_block(p, block(s), rkind(s) == R_PLATE ? 20 : 10);
}
inline void plates_player_tick(Player& pl) {  // entityInside for players
    int x0 = int(std::floor(pl.x - 0.3)), x1 = int(std::floor(pl.x + 0.3));
    int z0 = int(std::floor(pl.z - 0.3)), z1 = int(std::floor(pl.z + 0.3)), y = int(std::floor(pl.y));
    for (int x = x0; x <= x1; x++)
        for (int z = z0; z <= z1; z++) {
            Pos p{x, y, z};
            int s = get(p);
            int k = rkind(s);
            if ((k == R_PLATE || k == R_WPLATE) && plate_signal(s) == 0) plate_check(p, s, 0);
        }
}
inline std::unordered_set<int64_t> daylight_detectors;  // ponytail: registered on place/update, not found on chunk load
inline void daylight_update(const Pos& p, int s) {  // DaylightDetectorBlock.updateSignalStrength
    int target = std::max(0, sky_light(p) - sky_darken());
    if (getb(s, "inverted")) target = 15 - target;
    else if (target > 0) {
        double f = double(clocks[0].total % 24000) / 24000.0 - 0.25;  // ponytail: legacy sun angle
        f -= std::floor(f);
        double orig = f;
        f = (orig * 2.0 + (0.5 - std::cos(f * M_PI) / 2.0)) / 3.0;
        float angle = float(f) * float(M_PI * 2);
        float offset = angle < float(M_PI) ? 0.0f : float(M_PI * 2);
        angle += (offset - angle) * 0.2f;
        target = int(std::lround(target * std::cos(angle)));
    }
    target = std::clamp(target, 0, 15);
    if (rpower(s) != target) set(p, with(s, "power", target));
}
inline void daylight_tick() {
    if (now() % 20 != 0) return;
    for (auto it = daylight_detectors.begin(); it != daylight_detectors.end();) {
        int64_t k = *it;
        Pos p{int(k >> 38), int(k << 52 >> 52), int(k << 26 >> 38)};
        int s = get(p);
        if (rkind(s) != R_DAYLIGHT) { it = daylight_detectors.erase(it); continue; }
        if (ticking.count(chunk_key(p.x >> 4, p.z >> 4))) daylight_update(p, s);
        ++it;
    }
}

// ==================================================================================================
// behaviour hooks (behaviours.hpp)
// ==================================================================================================
inline bool rs_can_survive(int s, const Pos& p, bool& r) {
    switch (rkind(s)) {
        case R_REPEATER: case R_COMPARATOR: r = sturdy(get(p.below()), UP, RIGID); return true;
        case R_HEAD: r = head_can_survive(s, p); return true;
        default: return false;
    }
}
inline bool rs_update_shape(int s, const Pos& p, int dir, int ns, int& r) {
    switch (rkind(s)) {
        case R_REPEATER:
            if (dir == DOWN && !sturdy(ns, UP, RIGID)) r = air();
            else if (axis(dir) != axis(rfacing(s))) r = with(s, "locked", diode_locked(p, s));
            else r = s;
            return true;
        case R_COMPARATOR: r = dir == DOWN && !sturdy(ns, UP, RIGID) ? air() : s; return true;
        case R_OBSERVER:
            if (rfacing(s) == dir && !rpowered(s) && !has_block_tick(p, block(s))) schedule_block(p, block(s), 2);
            r = s;
            return true;
        case R_HEAD: r = OPP[dir] == rfacing(s) && !head_can_survive(s, p) ? air() : s; return true;
        default: return false;
    }
}
inline bool tnt_prime(const Pos& p) {  // TntBlock.prime (TNT_EXPLODES game rule)
    if (!rule("tnt_explodes")) return false;
    std::string err;
    return summon_entity(entity_types::tnt, p.x + 0.5, p.y, p.z + 0.5, nullptr, err) != nullptr;
}
inline void diode_drop(int s, const Pos& p) {
    drop_resources(s, p);
    remove(p, false);
    for (int d = 0; d < 6; d++) update_neighbors(p.rel(d), block(s));
}
// true: handled (behaviours.hpp's own switch is skipped)
inline bool rs_neighbor_changed(int s, const Pos& p, int src, bool piston) {
    int b = block(s);
    switch (rkind(s)) {
        case R_WIRE:
            if (can_survive(s, p)) wire_update_power(p, s);
            else {
                drop_resources(s, p);
                remove(p, false);
            }
            return true;
        case R_TORCH: case R_WALL_TORCH:
            if (rlit(s) == torch_has_signal(s, p) && !block_ticks.will_tick_this_tick(p, b)) schedule_block(p, b, 2);
            return true;
        case R_REPEATER: case R_COMPARATOR:
            if (block(get(p)) != b) return true;
            if (can_survive(s, p)) diode_check_tick(p, s);
            else diode_drop(s, p);
            return true;
        case R_LAMP: {
            bool l = rlit(s);
            if (l != has_neighbor_signal(p)) {
                if (l) schedule_block(p, b, 4);
                else set(p, with(s, "lit", true), UPDATE_CLIENTS);
            }
            return true;
        }
        case R_DOOR: {
            bool sig = has_neighbor_signal(p) || has_neighbor_signal(p.rel(get(s, "half") == "lower" ? UP : DOWN));
            if (b != src && sig != rpowered(s)) {
                if (sig != getb(s, "open")) level_event(sig ? (block_of(s).name == "minecraft:iron_door" ? 1005 : 1006) : (block_of(s).name == "minecraft:iron_door" ? 1011 : 1012), p, 0);
                set(p, with(with(s, "powered", sig), "open", sig), UPDATE_CLIENTS);
            }
            return true;
        }
        case R_TRAPDOOR: {
            bool sig = has_neighbor_signal(p);
            if (sig != rpowered(s)) {
                if (getb(s, "open") != sig) s = with(s, "open", sig);
                set(p, with(s, "powered", sig), UPDATE_CLIENTS);
                if (waterlogged(s)) schedule_fluid(p, WATER, tick_delay(WATER));
            }
            return true;
        }
        case R_GATE: {
            bool sig = has_neighbor_signal(p);
            if (rpowered(s) != sig) set(p, with(with(s, "powered", sig), "open", sig), UPDATE_CLIENTS);
            return true;
        }
        case R_NOTE: {
            bool sig = has_neighbor_signal(p);
            if (sig != rpowered(s)) {
                if (sig && is_air(get(p.above()))) block_event(p, b, 0, 0);  // ponytail: mob-head instruments
                set(p, with(s, "powered", sig));
            }
            return true;
        }
        case R_TNT:
            if (has_neighbor_signal(p) && tnt_prime(p)) remove(p, false);
            return true;
        case R_DISPENSER: {
            bool should = has_neighbor_signal(p) || has_neighbor_signal(p.above());
            bool trig = getb(s, "triggered");
            if (should && !trig) {
                schedule_block(p, b, 4);
                set(p, with(s, "triggered", true), UPDATE_CLIENTS);
            } else if (!should && trig) {
                set(p, with(s, "triggered", false), UPDATE_CLIENTS);
            }
            return true;
        }
        case R_HOPPER: {
            bool on = !has_neighbor_signal(p);
            if (on != getb(s, "enabled")) set(p, with(s, "enabled", on), UPDATE_CLIENTS);
            return true;
        }
        case R_BULB: {
            bool sig = has_neighbor_signal(p);
            if (sig != rpowered(s)) {
                int ns = s;
                if (!rpowered(s)) ns = with(s, "lit", !rlit(s));
                set(p, with(ns, "powered", sig));
            }
            return true;
        }
        case R_PISTON: piston_check_extend(p, s); return true;
        case R_HEAD:
            if (head_can_survive(s, p)) neighbor_changed_at(p.rel(OPP[rfacing(s)]), src);
            return true;
        case R_RAIL: case R_POWERED_RAIL: case R_DETECTOR_RAIL:
            if (block(get(p)) != b) return true;
            if (rail_should_remove(s, p)) {
                drop_resources(s, p);
                remove(p, piston);
            } else {
                rail_update_state(s, p, src);
            }
            return true;
        default: return false;
    }
}
inline bool rs_on_place(int s, const Pos& p, int old, bool piston) {
    (void)piston;
    int b = block(s);
    switch (rkind(s)) {
        case R_WIRE:
            if (block(old) == b) return true;
            wire_update_power(p, s);
            update_neighbors(p.above(), b);
            update_neighbors(p.below(), b);
            wire_update_neighbors_of_wires(p);
            return true;
        case R_TORCH: case R_WALL_TORCH: torch_notify(p, b); return true;
        case R_REPEATER: case R_COMPARATOR: update_neighbors_in_front(p, s); return true;
        case R_OBSERVER:
            if (block(old) != b && rpowered(s) && !has_block_tick(p, b)) {
                int ns = with(s, "powered", false);
                set(p, ns, UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE);
                update_neighbors_in_front(p, ns);
            }
            return true;
        case R_TARGET:
            if (block(old) != b && rpower(s) > 0 && !has_block_tick(p, b)) set(p, with(s, "power", 0), UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE);
            return true;
        case R_TNT:
            if (block(old) != b && has_neighbor_signal(p) && tnt_prime(p)) remove(p, false);
            return true;
        case R_HOPPER:
            if (block(old) != b) {
                bool on = !has_neighbor_signal(p);
                if (on != getb(s, "enabled")) set(p, with(s, "enabled", on), UPDATE_CLIENTS);
            }
            return true;
        case R_BULB:
            if (block(old) != b) rs_neighbor_changed(s, p, b, false);
            return true;
        case R_PISTON:
            if (block(old) != b && !moving(p)) piston_check_extend(p, s);
            return true;
        case R_DAYLIGHT: daylight_detectors.insert(pos_key(p)); return true;
        default: return false;
    }
}
inline void rs_after_removal(int s, const Pos& p, bool piston) {
    int b = block(s);
    switch (rkind(s)) {
        case R_WIRE:
            if (piston) return;
            for (int d = 0; d < 6; d++) update_neighbors(p.rel(d), b);
            wire_update_power(p, s);
            wire_update_neighbors_of_wires(p);
            return;
        case R_TORCH: case R_WALL_TORCH: if (!piston) torch_notify(p, b); return;
        case R_REPEATER: case R_COMPARATOR: if (!piston) update_neighbors_in_front(p, s); return;
        case R_OBSERVER:
            if (rpowered(s) && has_block_tick(p, b)) update_neighbors_in_front(p, with(s, "powered", false));
            return;
        case R_BUTTON: case R_LEVER:
            if (!piston && rpowered(s)) {
                update_neighbors(p, b);
                update_neighbors(p.rel(OPP[attached_dir(s)]), b);
            }
            return;
        case R_PLATE: case R_WPLATE: if (!piston && plate_signal(s) > 0) plate_update_neighbours(p, b); return;
        case R_HEAD: {
            Pos bp = p.rel(OPP[rfacing(s)]);
            if (head_fitting_base(s, get(bp))) destroy(bp, true);
            return;
        }
        case R_RAIL: case R_POWERED_RAIL: case R_DETECTOR_RAIL:
            if (piston) return;
            if (get(s, "shape").rfind("ascending_", 0) == 0) update_neighbors(p.above(), b);
            if (rkind(s) != R_RAIL) {
                update_neighbors(p, b);
                update_neighbors(p.below(), b);
            }
            return;
        default: return;
    }
}
inline bool rs_tick(int s, const Pos& p) {
    int b = block(s);
    switch (rkind(s)) {
        case R_TORCH: case R_WALL_TORCH: {
            bool sig = torch_has_signal(s, p);
            while (!recent_toggles.empty() && now() - recent_toggles.front().when > 60) recent_toggles.erase(recent_toggles.begin());
            if (rlit(s)) {
                if (sig) {
                    set(p, with(s, "lit", false));
                    if (toggled_too_frequently(p, true)) {
                        level_event(1502, p, 0);
                        schedule_block(p, block(get(p)), 160);
                    }
                }
            } else if (!sig && !toggled_too_frequently(p, false)) {
                set(p, with(s, "lit", true));
            }
            return true;
        }
        case R_REPEATER: {
            if (diode_locked(p, s)) return true;
            bool on = rpowered(s), should = diode_should_turn_on(p, s);
            if (on && !should) set(p, with(s, "powered", false), UPDATE_CLIENTS);
            else if (!on) {
                set(p, with(s, "powered", true), UPDATE_CLIENTS);
                if (!should) schedule_block(p, b, diode_delay(s), VERY_HIGH);
            }
            return true;
        }
        case R_COMPARATOR: comparator_refresh(p, s); return true;
        case R_OBSERVER:
            if (rpowered(s)) set(p, with(s, "powered", false), UPDATE_CLIENTS);
            else {
                set(p, with(s, "powered", true), UPDATE_CLIENTS);
                schedule_block(p, b, 2);
            }
            update_neighbors_in_front(p, s);
            return true;
        case R_BUTTON:
            if (rpowered(s)) {  // checkPressed without arrows
                set(p, with(s, "powered", false));
                update_neighbors(p, b);
                update_neighbors(p.rel(OPP[attached_dir(s)]), b);
                rs_sound(block_of(s).name.find("stone") != std::string::npos ? "block.stone_button.click_off" : "block.wooden_button.click_off", p, 1.0f, 1.0f);
            }
            return true;
        case R_PLATE: case R_WPLATE:
            if (int sig = plate_signal(s); sig > 0) plate_check(p, s, sig);
            return true;
        case R_LAMP:
            if (rlit(s) && !has_neighbor_signal(p)) set(p, with(s, "lit", false), UPDATE_CLIENTS);
            return true;
        case R_TARGET:
            if (rpower(s) != 0) set(p, with(s, "power", 0));
            return true;
        case R_DISPENSER: return true;  // ponytail: dispenseFrom needs the container block entity (DispenseItemBehavior)
        default: return false;
    }
}

// ==================================================================================================
// player interaction and placement (placement.hpp)
// ==================================================================================================
inline int button_ticks(int s) {
    const std::string& n = block_of(s).name;
    return n.find("stone") != std::string::npos ? 20 : 30;  // BlockSetType: stone / polished blackstone press 20 ticks
}
inline bool rs_use(Player& pl, const Pos& p, int s) {  // useWithoutItem
    (void)pl;
    int b = block(s);
    switch (rkind(s)) {
        case R_LEVER: {
            int ns = with(s, "powered", !rpowered(s));
            set(p, ns);
            update_neighbors(p, b);
            update_neighbors(p.rel(OPP[attached_dir(ns)]), b);
            rs_sound("block.lever.click", p, 0.3f, rpowered(ns) ? 0.6f : 0.5f);
            return true;
        }
        case R_BUTTON:
            if (rpowered(s)) return true;
            set(p, with(s, "powered", true));
            update_neighbors(p, b);
            update_neighbors(p.rel(OPP[attached_dir(s)]), b);
            schedule_block(p, b, button_ticks(s));
            rs_sound(block_of(s).name.find("stone") != std::string::npos ? "block.stone_button.click_on" : "block.wooden_button.click_on", p, 1.0f, 1.0f);
            return true;
        case R_REPEATER: set(p, with(s, "delay", geti(s, "delay") % 4 + 1)); return true;
        case R_COMPARATOR: {
            int ns = with(s, "mode", get(s, "mode") == "compare" ? "subtract" : "compare");
            set(p, ns, UPDATE_CLIENTS);
            if (block(get(p)) == b) comparator_refresh(p, ns);
            return true;
        }
        case R_DAYLIGHT: {
            int ns = with(s, "inverted", !getb(s, "inverted"));
            set(p, ns, UPDATE_CLIENTS);
            daylight_detectors.insert(pos_key(p));
            daylight_update(p, ns);
            return true;
        }
        case R_NOTE: {
            int ns = with(s, "note", (geti(s, "note") + 1) % 25);
            set(p, ns);
            if (is_air(get(p.above()))) block_event(p, b, 0, 0);
            return true;
        }
        default: return false;
    }
}
// The redstone inputs of getStateForPlacement (powered / open / lit / locked from the current signal).
inline int rs_place_inputs(int s, const Pos& p) {
    switch (rkind(s)) {
        case R_DOOR: {
            bool pw = has_neighbor_signal(p) || has_neighbor_signal(p.above());
            return with(with(s, "powered", pw), "open", pw);
        }
        case R_TRAPDOOR: return has_neighbor_signal(p) ? with(with(s, "open", true), "powered", true) : s;
        case R_GATE: { bool pw = has_neighbor_signal(p); return with(with(s, "open", pw), "powered", pw); }
        case R_LAMP: return with(s, "lit", has_neighbor_signal(p));
        case R_REPEATER: return with(s, "locked", diode_locked(p, s));
        default:
            if (has_class(s, "SkullBlock") || has_class(s, "WallSkullBlock")) return with(s, "powered", has_neighbor_signal(p));
            return s;
    }
}
inline void rs_placed_by(int s, const Pos& p) {  // setPlacedBy
    switch (rkind(s)) {
        case R_REPEATER: case R_COMPARATOR:
            if (diode_should_turn_on(p, s)) schedule_block(p, block(s), 1);
            break;
        case R_PISTON: piston_check_extend(p, s); break;
        default: break;
    }
}

// ==================================================================================================
// level hooks (level.hpp / run.hpp)
// ==================================================================================================
inline void be_removed(int old, const Pos& p) {  // LevelChunk.removeBlockEntity for the redstone block entities
    int k = rkind(old);
    if (k == R_MOVING) {
        if (Moving* m = moving(p)) {  // preRemoveSideEffects -> finalTick: the block is no longer moving_piston
            m->progress = m->progress_o = 1.0f;
            moving_remove(p);
        }
    } else if (k == R_COMPARATOR) {
        comparator_out.erase(pos_key(p));
    }
}
inline void output_signal_changed(int state, const Pos& p) {
    if (has_analog(state)) update_neighbour_for_output_signal(p, block(state));
}
inline void rs_level_tick_end() {  // after the chunk tick: block events, then entities, then block entities
    run_block_events();
    handling_tick = false;
}

}  // namespace lvl
