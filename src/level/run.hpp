#pragma once
// The level tick (ServerLevel.tick order): scheduled block ticks, fluid ticks, chunk ticks (post-processing of
// newly ticking chunks, precipitation, random ticks), falling blocks; client sync at the end of the tick.
// Ticking chunks: loaded, within simulation distance of a player (Chebyshev, like the ticket tracker) and with
// all 8 neighbours loaded. Cost is proportional to scheduled work plus random_tick_speed samples per section
// that holds randomly ticking states.
#include "falling.hpp"

#include <unordered_set>

namespace lvl {

inline std::unordered_set<int64_t> ticking;  // this tick's ticking chunks
inline int rand_value = int(std::random_device{}());

inline bool neighbours_loaded(int cx, int cz) {
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++)
            if (!chunks.count(chunk_key(cx + dx, cz + dz)) && !lchunk(cx + dx, cz + dz)) return false;
    return true;
}
inline void compute_ticking() {
    ticking.clear();
    int r = sim_distance();
    for (auto& p : players)
        for (int x = p->cx - r; x <= p->cx + r; x++)
            for (int z = p->cz - r; z <= p->cz + r; z++) {
                int64_t k = chunk_key(x, z);
                if (ticking.count(k) || !chunks.count(k)) continue;
                if (neighbours_loaded(x, z)) ticking.insert(k);
            }
}

// Level.getBlockRandomPos
inline Pos block_random_pos(int x0, int y0, int z0, int ymask) {
    rand_value = int(uint32_t(rand_value) * 3u + 1013904223u);
    int v = rand_value >> 2;
    return {x0 + (v & 15), y0 + (v >> 16 & ymask), z0 + (v >> 8 & 15)};
}

inline void tick_precipitation(const Pos& p);  // behaviours (ice / snow)

inline uint32_t ticking_sections(LChunk& l) {
    if (l.seen_edits == l.c->edits) return l.ticking_sections;
    std::lock_guard lock(l.c->m);
    l.seen_edits = l.c->edits;
    uint32_t bits = 0;
    for (int s = 0; s < SECTIONS; s++) {
        auto& b = l.c->sections[size_t(s)].blocks;
        bool any = false;
        if (b.bits <= 8) {
            for (uint16_t v : b.palette) if (random_ticks(v)) { any = true; break; }
        } else {
            for (int i = 0; i < 4096 && !any; i++) any = random_ticks(b.get(i));
        }
        if (any) bits |= 1u << s;
    }
    return l.ticking_sections = bits;
}

// ServerLevel.tickChunk
inline void tick_chunk(LChunk& l, int speed) {
    int x0 = l.c->cx * 16, z0 = l.c->cz * 16;
    for (int i = 0; i < speed; i++)
        if (rng().nextInt(48) == 0) tick_precipitation(block_random_pos(x0, 0, z0, 15));
    if (speed <= 0) return;
    uint32_t bits = ticking_sections(l);
    for (int s = 0; s < SECTIONS; s++) {
        if (!(bits >> s & 1)) continue;
        int y0 = MIN_Y + s * 16;
        for (int i = 0; i < speed; i++) {
            Pos p = block_random_pos(x0, y0, z0, 15);
            int st;
            {
                std::lock_guard lock(l.c->m);
                st = l.c->get(p.x & 15, p.y, p.z & 15);
            }
            if (random_ticks(st)) random_tick(st, p);
            if (is_lava(st)) fluid_random_tick(p, st);  // FluidState.isRandomlyTicking: lava only
        }
    }
}

// LevelChunk.postProcessGeneration
inline void post_process(LChunk& l) {
    std::vector<uint32_t> post;
    {
        std::lock_guard lock(l.c->m);
        post.swap(l.c->level.post);
        bool was_on_disk = l.c->on_disk && !l.c->level.post_processed;
        l.c->level.post_processed = true;
        if (was_on_disk) l.c->dirty = true;
    }
    std::stable_sort(post.begin(), post.end(), [](uint32_t a, uint32_t b) { return (a >> 12) < (b >> 12); });  // by section
    for (uint32_t v : post) {
        Pos p{l.c->cx * 16 + int(v & 15), MIN_Y + int(v >> 8), l.c->cz * 16 + int(v >> 4 & 15)};
        int s = get(p);
        if (has_fluid(s)) fluid_tick(p, s);
        if (kind(s) == K_LIQUID) {
            block_tick(s, p);
        } else {
            int ns = update_from_neighbour_shapes(s, p);
            if (ns != s) set(p, ns, UPDATE_INVISIBLE | UPDATE_KNOWN_SHAPE | UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS);
        }
    }
}

inline std::thread::id tick_thread;
inline int64_t last_release = 0;

inline void level_tick() {
    tick_thread = std::this_thread::get_id();
    {
        std::vector<ChunkPtr> q;
        {
            std::lock_guard lock(loaded_m);
            q.swap(loaded_q);
        }
        for (auto& c : q) adopt(c);
    }
    compute_ticking();
    int64_t t = now();
    auto check = [](int64_t k) { return ticking.count(k) != 0; };
    block_ticks.tick(t, 65536, check, [](const Pos& p, int type) {  // ServerLevel.tickBlock
        int s = get(p);
        if (block(s) == type) block_tick(s, p);
    });
    fluid_ticks.tick(t, 65536, check, [](const Pos& p, int type) {  // ServerLevel.tickFluid
        int s = get(p);
        if (getenv("CRAFTPP_LEVEL_DEBUG")) log_info("fluid tick %d %d %d type %d state %d", p.x, p.y, p.z, type, s);
        if (fluid_type(s) == type) fluid_tick(p, s);
    });
    // chunk source: newly ticking chunks get post-processed, then ticking chunks tick
    for (size_t i = 0; i < post_pending.size();) {
        int64_t k = post_pending[i];
        auto it = chunks.find(k);
        if (it == chunks.end()) { post_pending[i] = post_pending.back(); post_pending.pop_back(); continue; }
        if (!neighbours_loaded(it->second.c->cx, it->second.c->cz)) { i++; continue; }
        post_pending[i] = post_pending.back();
        post_pending.pop_back();
        it->second.post_queued = false;
        post_process(it->second);
    }
    int speed = rule("random_tick_speed");
    for (int64_t k : ticking) {
        auto it = chunks.find(k);
        if (it != chunks.end()) tick_chunk(it->second, speed);
    }
    tick_falling_blocks();
    // Every 5 s: save tick lists into the chunks; let go of chunks no player views (World unloads them).
    if (server_tick - last_release >= 100) {
        last_release = server_tick;
        std::vector<int64_t> gone;
        for (auto& [k, l] : chunks)
            if (!world.is_viewed(l.c->cx, l.c->cz)) gone.push_back(k);
        for (int64_t k : gone) release(k);
        pack_all();
        if (getenv("CRAFTPP_LEVEL_DEBUG"))
            log_info("level: %zu chunks, %zu ticking, %zu block ticks, %zu fluid ticks pending containers", chunks.size(), ticking.size(),
                     block_ticks.next_for.size(), fluid_ticks.next_for.size());
    }
}

inline void init() {
    if (!load()) {
        log_error("data/level.json missing or stale (run: python3 tools/gen_level_data.py)");
        _exit(1);
    }
    init_kinds();
    try {
        load_climates();
    } catch (const std::exception& e) {
        log_warn("biome climates: %s", e.what());
    }
    world.on_load = [](const ChunkPtr& c) {
        std::lock_guard lock(loaded_m);
        loaded_q.push_back(c);
    };
}

}  // namespace lvl
