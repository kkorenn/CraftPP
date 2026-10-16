#pragma once
// Scheduled ticks: port of net.minecraft.world.ticks (ScheduledTick, LevelChunkTicks, LevelTicks, SavedTick).
// One LevelTicks for blocks (type = block index) and one for fluids (type = Fluids id). Tick thread only.
#include "data.hpp"

#include <deque>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace lvl {

enum Priority : int8_t { EXTREMELY_HIGH = -3, VERY_HIGH = -2, HIGH = -1, NORMAL = 0, LOW = 1, VERY_LOW = 2, EXTREMELY_LOW = 3 };

inline int64_t chunk_key(int cx, int cz) { return int64_t(uint32_t(cx)) | int64_t(uint64_t(uint32_t(cz)) << 32); }  // ChunkPos.pack
inline int64_t pos_key(const Pos& p) {  // BlockPos.asLong
    return int64_t((uint64_t(int64_t(p.x)) & 0x3FFFFFF) << 38 | (uint64_t(int64_t(p.z)) & 0x3FFFFFF) << 12 | (uint64_t(int64_t(p.y)) & 0xFFF));
}

struct ScheduledTick {
    int type;
    Pos pos;
    int64_t trigger;
    int8_t priority;
    int64_t sub;
};
inline bool drain_less(const ScheduledTick& a, const ScheduledTick& b) {  // DRAIN_ORDER
    if (a.trigger != b.trigger) return a.trigger < b.trigger;
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.sub < b.sub;
}
inline bool intra_less(const ScheduledTick& a, const ScheduledTick& b) {  // INTRA_TICK_DRAIN_ORDER
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.sub < b.sub;
}
struct TickKey {  // UNIQUE_TICK_HASH: (type, pos)
    int64_t pos;
    int type;
    bool operator==(const TickKey&) const = default;
};
struct TickKeyHash {
    size_t operator()(const TickKey& k) const { return std::hash<int64_t>()(k.pos * 31 + k.type); }
};

struct ChunkTicks {  // LevelChunkTicks
    struct Later {
        bool operator()(const ScheduledTick& a, const ScheduledTick& b) const { return drain_less(b, a); }
    };
    std::priority_queue<ScheduledTick, std::vector<ScheduledTick>, Later> q;
    std::unordered_set<TickKey, TickKeyHash> keys;

    const ScheduledTick* peek() const { return q.empty() ? nullptr : &q.top(); }
    ScheduledTick poll() {
        ScheduledTick t = q.top();
        q.pop();
        keys.erase({pos_key(t.pos), t.type});
        return t;
    }
    bool schedule(const ScheduledTick& t) {  // false when (type, pos) is already scheduled
        if (!keys.insert({pos_key(t.pos), t.type}).second) return false;
        q.push(t);
        return true;
    }
    bool has(const Pos& p, int type) const { return keys.count({pos_key(p), type}) != 0; }
    size_t count() const { return q.size(); }
};

struct LevelTicks {
    std::unordered_map<int64_t, ChunkTicks*> containers;
    std::unordered_map<int64_t, int64_t> next_for;  // nextTickForContainer
    std::vector<ScheduledTick> to_run;
    size_t run_at = 0;
    std::unordered_set<TickKey, TickKeyHash> to_run_set;

    void add_container(int cx, int cz, ChunkTicks* c) {
        int64_t k = chunk_key(cx, cz);
        containers[k] = c;
        if (auto* t = c->peek()) next_for[k] = t->trigger;
    }
    void remove_container(int cx, int cz) {
        containers.erase(chunk_key(cx, cz));
        next_for.erase(chunk_key(cx, cz));
    }
    bool schedule(const ScheduledTick& t) {
        int64_t k = chunk_key(t.pos.x >> 4, t.pos.z >> 4);
        auto it = containers.find(k);
        if (it == containers.end()) return false;  // vanilla: "Trying to schedule tick in not loaded position"
        if (!it->second->schedule(t)) return false;
        if (it->second->peek() && it->second->peek()->sub == t.sub && it->second->peek()->type == t.type &&
            it->second->peek()->pos == t.pos)
            next_for[k] = t.trigger;  // chunkScheduleUpdater: the new tick is the container's head
        return true;
    }
    bool has(const Pos& p, int type) const {
        auto it = containers.find(chunk_key(p.x >> 4, p.z >> 4));
        return it != containers.end() && it->second->has(p, type);
    }
    bool will_tick_this_tick(const Pos& p, int type) {
        if (to_run_set.empty())
            for (size_t i = run_at; i < to_run.size(); i++) to_run_set.insert({pos_key(to_run[i].pos), to_run[i].type});
        return to_run_set.count({pos_key(p), type}) != 0;
    }

    template <class Check, class Fn>
    void tick(int64_t now, size_t max, Check&& ticking, Fn&& run) {
        // sortContainersToTick
        struct Cmp {
            bool operator()(ChunkTicks* a, ChunkTicks* b) const { return intra_less(*b->peek(), *a->peek()); }
        };
        std::priority_queue<ChunkTicks*, std::vector<ChunkTicks*>, Cmp> pq;
        for (auto it = next_for.begin(); it != next_for.end();) {
            if (it->second > now) { ++it; continue; }
            auto c = containers.find(it->first);
            const ScheduledTick* t = c == containers.end() ? nullptr : c->second->peek();
            if (!t) { it = next_for.erase(it); continue; }
            if (t->trigger > now) { it->second = t->trigger; ++it; continue; }
            if (ticking(it->first)) {
                pq.push(c->second);
                it = next_for.erase(it);
            } else {
                ++it;
            }
        }
        // drainContainers
        to_run.clear();
        run_at = 0;
        auto reschedule = [&](const ScheduledTick& t) { next_for[chunk_key(t.pos.x >> 4, t.pos.z >> 4)] = t.trigger; };
        while (to_run.size() < max && !pq.empty()) {
            ChunkTicks* top = pq.top();
            pq.pop();
            to_run.push_back(top->poll());
            // drainFromCurrentContainer
            const ScheduledTick* other = pq.empty() ? nullptr : pq.top()->peek();
            while (to_run.size() < max) {
                const ScheduledTick* cur = top->peek();
                if (!cur || cur->trigger > now || (other && intra_less(*other, *cur))) break;
                to_run.push_back(top->poll());
            }
            if (const ScheduledTick* nx = top->peek()) {
                if (nx->trigger <= now && to_run.size() < max) pq.push(top);
                else reschedule(*nx);
            }
        }
        while (!pq.empty()) {  // rescheduleLeftoverContainers
            reschedule(*pq.top()->peek());
            pq.pop();
        }
        // runCollectedTicks
        for (; run_at < to_run.size(); run_at++) {
            ScheduledTick t = to_run[run_at];
            if (!to_run_set.empty()) to_run_set.erase({pos_key(t.pos), t.type});
            run(t.pos, t.type);
        }
        to_run.clear();
        run_at = 0;
        to_run_set.clear();
    }
};

// SavedTick list <-> bytes (ChunkLevelData::ticks): per list varint n, then (type, local xz, y, delay, priority).
inline void pack_ticks(Writer& w, const ChunkTicks& c, int64_t now) {
    std::vector<ScheduledTick> all;
    auto q = c.q;
    while (!q.empty()) all.push_back(q.top()), q.pop();
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.sub < b.sub; });  // SUB_TICK_ORDERING
    w.varint(int32_t(all.size()));
    for (auto& t : all)
        w.varint(t.type).u8(uint8_t((t.pos.x & 15) | (t.pos.z & 15) << 4)).varint(t.pos.y - MIN_Y)
            .i32(int32_t(t.trigger - now)).u8(uint8_t(t.priority));
}
// LevelChunkTicks.unpack: saved ticks get sub-tick orders -n..-1, before anything scheduled after load.
inline void unpack_ticks(Reader& r, ChunkTicks& c, int cx, int cz, int64_t now) {
    int n = r.varint();
    int64_t sub = -n;
    for (int i = 0; i < n; i++) {
        int type = r.varint();
        int xz = r.u8(), y = r.varint() + MIN_Y, delay = r.i32();
        int8_t pr = int8_t(r.u8());
        c.schedule({type, {cx * 16 + (xz & 15), y, cz * 16 + (xz >> 4)}, now + delay, pr, sub++});
    }
}

}  // namespace lvl
