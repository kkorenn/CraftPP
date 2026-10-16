#pragma once
// Pathfinding (net.minecraft.world.level.pathfinder): PathFinder (A*), BinaryHeap, Node/Target/Path,
// NodeEvaluator + WalkNodeEvaluator, and the per-tick node budget.
// Other evaluators (swim, fly, amphibious) subclass NodeEvaluator and override start/target/neighbours/path types.
#include "../living.hpp"

#include <unordered_map>

struct Mob;

// Every pathfind in a tick draws from this node budget; when it runs out createPath() returns null and goals
// retry on a later tick (vanilla has no such cap; it keeps worst-case ticks bounded on a Pi).
inline int path_budget_per_tick = 6000;
inline int path_budget_left = 6000;
inline int64_t path_nodes_total = 0, path_searches_total = 0, path_budget_misses = 0;

struct PNode {
    int x, y, z;
    int heap_idx = -1;
    float g = 0, h = 0, f = 0;
    int came_from = -1;
    bool closed = false;
    float walked = 0, cost_malus = 0;
    uint8_t type = PT_BLOCKED;
    static int hash(int x, int y, int z) {  // Node.createHash
        return int((uint32_t(y) & 0xFF) | (uint32_t(x) & 32767) << 8 | (uint32_t(z) & 32767) << 24 | (x < 0 ? 0x80000000u : 0) |
                   (z < 0 ? 32768u : 0));
    }
    float dist(const PNode& o) const {
        float xd = float(o.x - x), yd = float(o.y - y), zd = float(o.z - z);
        return std::sqrt(xd * xd + yd * yd + zd * zd);
    }
    float manhattan(const PNode& o) const { return float(std::abs(o.x - x) + std::abs(o.y - y) + std::abs(o.z - z)); }
};
struct PathPoint { int x, y, z; uint8_t type; };

struct Path {
    std::vector<PathPoint> nodes;
    int next = 0;
    int tx = 0, ty = 0, tz = 0;  // target block
    bool reached = false;
    float dist_to_target = 3.4e38f;
    bool done() const { return next >= int(nodes.size()); }
    void advance() { next++; }
    bool not_started() const { return next <= 0; }
    int count() const { return int(nodes.size()); }
    const PathPoint* end() const { return nodes.empty() ? nullptr : &nodes.back(); }
    Vec3 entity_pos_at(const Entity& e, int i) const {  // Path.getEntityPosAtNode
        const PathPoint& n = nodes[size_t(i)];
        double off = int(e.width + 1.0f) * 0.5;
        return {n.x + off, double(n.y), n.z + off};
    }
    Vec3 next_entity_pos(const Entity& e) const { return entity_pos_at(e, next); }
    const PathPoint& next_node() const { return nodes[size_t(next)]; }
    bool same_as(const Path* o) const {
        if (!o || o->nodes.size() != nodes.size()) return false;
        for (size_t i = 0; i < nodes.size(); i++)
            if (nodes[i].x != o->nodes[i].x || nodes[i].y != o->nodes[i].y || nodes[i].z != o->nodes[i].z) return false;
        return true;
    }
    void truncate(int i) { if (int(nodes.size()) > i) nodes.resize(size_t(i)); }
};

// Search scratch shared by every evaluator: one search at a time on the tick thread, so mobs don't each keep
// node pools/hash tables alive between searches (memory per mob stays small).
struct PathScratch {
    std::vector<PNode> pool;
    std::unordered_map<int, int> by_hash;
    std::unordered_map<int64_t, uint8_t> type_cache;
    std::vector<int> heap;
};
inline PathScratch& path_scratch() { static PathScratch s; return s; }
struct NodeEvaluator {
    Mob* mob = nullptr;
    std::vector<PNode>& pool = path_scratch().pool;
    std::unordered_map<int, int>& by_hash = path_scratch().by_hash;
    std::unordered_map<int64_t, uint8_t>& type_cache = path_scratch().type_cache;
    int entity_width = 1, entity_height = 1, entity_depth = 1;
    bool can_pass_doors = true, can_open_doors = false, can_float = false, can_walk_over_fences = false;
    virtual ~NodeEvaluator() = default;
    virtual void prepare(Mob& m);
    virtual void done() { mob = nullptr; pool.clear(); by_hash.clear(); type_cache.clear(); }
    int node(int x, int y, int z) {
        auto [it, fresh] = by_hash.try_emplace(PNode::hash(x, y, z), int(pool.size()));
        if (fresh) {
            PNode n;
            n.x = x, n.y = y, n.z = z;
            pool.push_back(n);
        }
        return it->second;
    }
    PNode& at(int i) { return pool[size_t(i)]; }
    virtual int start() = 0;                                    // -1 = none
    virtual int target(double x, double y, double z) { return node(mth::floor(x), mth::floor(y), mth::floor(z)); }
    virtual int neighbors(int* out, int cur) = 0;
    virtual uint8_t path_type(int x, int y, int z) = 0;         // getPathType(context, x, y, z)
    virtual uint8_t path_type_of_mob(int x, int y, int z) = 0;
};

// WalkNodeEvaluator.getPathTypeStatic: base type with the block below and neighbours considered.
inline uint8_t path_from_state(int x, int y, int z) { return state_path_type[size_t(block_at(x, y, z))]; }
inline uint8_t check_neighbour_blocks(int x, int y, int z, uint8_t t) {
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                if (dx == 0 && dz == 0) continue;
                uint8_t p = path_from_state(x + dx, y + dy, z + dz);
                if (p == PT_DAMAGING) return PT_DAMAGING_IN_NEIGHBOR;
                if (p == PT_FIRE || p == PT_LAVA) return PT_FIRE_IN_NEIGHBOR;
                if (p == PT_WATER) return PT_WATER_BORDER;
                if (p == PT_DAMAGE_CAUTIOUS) return PT_DAMAGE_CAUTIOUS;
            }
    return t;
}
inline uint8_t path_type_static(int x, int y, int z) {
    uint8_t t = path_from_state(x, y, z);
    if (t == PT_OPEN && y >= MIN_Y + 1) {
        switch (path_from_state(x, y - 1, z)) {
            case PT_OPEN: case PT_WATER: case PT_LAVA: case PT_WALKABLE: return PT_OPEN;
            case PT_FIRE: return PT_FIRE;
            case PT_DAMAGING: return PT_DAMAGING;
            case PT_STICKY_HONEY: return PT_STICKY_HONEY;
            case PT_POWDER_SNOW: return PT_ON_TOP_OF_POWDER_SNOW;
            case PT_DAMAGE_CAUTIOUS: return PT_DAMAGE_CAUTIOUS;
            case PT_TRAPDOOR: return PT_ON_TOP_OF_TRAPDOOR;
            default: return check_neighbour_blocks(x, y, z, PT_WALKABLE);
        }
    }
    return t;
}
inline double floor_level(int x, int y, int z) {  // WalkNodeEvaluator.getFloorLevel(level, pos)
    int s = block_at(x, y - 1, z);
    double top = 0;
    for (const AABB* b = shape_begin(s); b != shape_end(s); b++) top = std::max(top, b->y1);
    return y - 1 + (has_collision(s) ? top : 0.0);
}

struct WalkNodeEvaluator : NodeEvaluator {
    int reuse[4] = {-1, -1, -1, -1};
    int start() override;
    int neighbors(int* out, int cur) override;
    uint8_t path_type(int x, int y, int z) override { return path_type_static(x, y, z); }
    uint8_t path_type_of_mob(int x, int y, int z) override;
    uint8_t cached_type(int x, int y, int z) {
        auto [it, fresh] = type_cache.try_emplace(mth::block_key(x, y, z), 0);
        if (fresh) it->second = path_type_of_mob(x, y, z);
        return it->second;
    }
    virtual bool is_amphibious() const { return false; }
    double node_floor(int x, int y, int z);
    int find_accepted(int x, int y, int z, int jump, double node_h, int dx, int dz, uint8_t cur_type);
    int try_jump_on(int x, int y, int z, int jump, double node_h, int dx, int dz, uint8_t cur_type);
    bool has_collisions(const AABB& b) { return !no_block_collision(b); }
    bool can_reach_without_collision(const PNode& to);
    bool can_start_at(int x, int y, int z);
};

struct PathFinder {
    NodeEvaluator* eval;
    int max_visited;
    std::vector<int>& heap = path_scratch().heap;
    PathFinder(NodeEvaluator* e, int mv) : eval(e), max_visited(mv) {}
    // BinaryHeap
    void up(int idx) {
        int n = heap[size_t(idx)];
        float cost = eval->at(n).f;
        while (idx > 0) {
            int pi = (idx - 1) >> 1;
            int p = heap[size_t(pi)];
            if (!(cost < eval->at(p).f)) break;
            heap[size_t(idx)] = p, eval->at(p).heap_idx = idx;
            idx = pi;
        }
        heap[size_t(idx)] = n, eval->at(n).heap_idx = idx;
    }
    void down(int idx) {
        int n = heap[size_t(idx)];
        float cost = eval->at(n).f;
        int size = int(heap.size());
        for (;;) {
            int li = 1 + (idx << 1), ri = li + 1;
            if (li >= size) break;
            int l = heap[size_t(li)];
            float lc = eval->at(l).f, rc = ri >= size ? INFINITY : eval->at(heap[size_t(ri)]).f;
            if (lc < rc) {
                if (!(lc < cost)) break;
                heap[size_t(idx)] = l, eval->at(l).heap_idx = idx, idx = li;
            } else {
                if (!(rc < cost)) break;
                int r = heap[size_t(ri)];
                heap[size_t(idx)] = r, eval->at(r).heap_idx = idx, idx = ri;
            }
        }
        heap[size_t(idx)] = n, eval->at(n).heap_idx = idx;
    }
    void insert(int n) { heap.push_back(n); eval->at(n).heap_idx = int(heap.size()) - 1; up(int(heap.size()) - 1); }
    int pop() {
        int top = heap[0];
        heap[0] = heap.back();
        heap.pop_back();
        if (!heap.empty()) { eval->at(heap[0]).heap_idx = 0; down(0); }
        eval->at(top).heap_idx = -1;
        return top;
    }
    void change_cost(int n, float c) {
        float o = eval->at(n).f;
        eval->at(n).f = c;
        if (c < o) up(eval->at(n).heap_idx);
        else down(eval->at(n).heap_idx);
    }
    // PathFinder.findPath for one or more target blocks.
    std::unique_ptr<Path> find(Mob& m, const std::vector<std::array<int, 3>>& targets, float max_len, int reach, float mult);
};
