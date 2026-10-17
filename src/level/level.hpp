#pragma once
// The level's block update machinery (tick thread only), ported from vanilla 26.3:
//  * Level.setBlock / LevelChunk.setBlockState with the Block.UPDATE_* flag semantics,
//  * CollectingNeighborUpdater (neighborChanged / updateShape, vanilla update orders, chain limit),
//  * scheduled block + fluid ticks (ticks.hpp, 65536 per tick each), random ticks (random_tick_speed per
//    section, ServerLevel.getBlockRandomPos) and precipitation in chunks within simulation distance,
//  * LevelChunk.postProcessGeneration for worldgen-marked fluids when a chunk becomes ticking,
//  * client sync like ChunkHolder.broadcastChanges: block_update / section_blocks_update once per tick,
//  * tick lists saved with the chunk (Chunk::level), so they survive unload/reload and restarts.
// Block behaviours (updateShape, neighborChanged, onPlace, tick, randomTick...) live in behaviours.hpp,
// fluids (FlowingFluid / WaterFluid / LavaFluid) in fluids.hpp.
//
// Redstone hooks: neighbor updates carry the source block and run through the same collecting updater;
// scheduled ticks take a priority; F_CONDUCTOR / F_SIGNAL_SOURCE / F_ANALOG flags are loaded per state.
#include "../commands/state.hpp"
#include "../game/entity.hpp"
#include "../game/loot.hpp"
#include "../worldgen/material.hpp"
#include "../light/light.hpp"
#include "ticks.hpp"

#include <unordered_map>

namespace lvl {

enum : int {
    UPDATE_NEIGHBORS = 1, UPDATE_CLIENTS = 2, UPDATE_INVISIBLE = 4, UPDATE_IMMEDIATE = 8, UPDATE_KNOWN_SHAPE = 16,
    UPDATE_SUPPRESS_DROPS = 32, UPDATE_MOVE_BY_PISTON = 64, UPDATE_SKIP_SHAPE_UPDATE_ON_WIRE = 128,
    UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS = 256, UPDATE_SKIP_ON_PLACE = 512, UPDATE_NONE = 260, UPDATE_ALL = 3,
    UPDATE_ALL_IMMEDIATE = 11, UPDATE_SKIP_ALL_SIDEEFFECTS = 816, UPDATE_LIMIT = 512,
};
inline constexpr int UPDATE_ORDER[6] = {WEST, EAST, DOWN, UP, NORTH, SOUTH};        // NeighborUpdater.UPDATE_ORDER
inline constexpr int SHAPE_ORDER[6] = {WEST, EAST, NORTH, SOUTH, DOWN, UP};         // BlockBehaviour.UPDATE_SHAPE_ORDER

// ---------- behaviours (behaviours.hpp / fluids.hpp) ----------
inline int update_shape(int state, Pos pos, int dir, Pos npos, int nstate);
inline void neighbor_changed(int state, Pos pos, int src_block, bool piston);
inline void on_place(int state, Pos pos, int old, bool piston);
inline void after_removal(int state, Pos pos, bool piston);
inline void block_tick(int state, Pos pos);
inline void random_tick(int state, Pos pos);
inline void fluid_tick(Pos pos, int state);
inline void fluid_random_tick(Pos pos, int state);
inline bool can_survive(int state, Pos pos);
inline void wire_indirect_shapes(int s, const Pos& p, int flags, int limit);  // place_rules.hpp
inline void be_removed(int old, const Pos& p);              // redstone.hpp: moving piston / comparator block entities
inline void output_signal_changed(int state, const Pos& p);  // redstone.hpp: Level.updateNeighbourForOutputSignal

// Level.random: RandomSource.create() (LegacyRandomSource with a random seed).
inline LegacyRandomSource& rng() {
    static LegacyRandomSource r(int64_t(std::random_device{}()) << 32 ^ std::random_device{}());
    return r;
}

// ---------- chunks the tick thread has picked up ----------
struct LChunk {
    ChunkPtr c;
    ChunkTicks bt, ft;  // block / fluid ticks
    uint32_t seen_edits = ~0u;
    uint32_t ticking_sections = 0;  // bit per section with random-ticking states
    bool post_queued = false;
};
inline std::unordered_map<int64_t, LChunk> chunks;
inline LevelTicks block_ticks, fluid_ticks;
inline int64_t sub_tick = 0;
inline int64_t now() { return game_time; }

inline std::mutex loaded_m;
inline std::vector<ChunkPtr> loaded_q[DIM_COUNT];  // per level: World::on_load (any thread) -> adopt() on the tick thread
inline std::vector<int64_t> post_pending;

inline int64_t last_key = INT64_MIN;
inline LChunk* last_chunk = nullptr;  // one-entry cache (reset by release)
inline void adopt(const ChunkPtr& c) {
    int64_t k = chunk_key(c->cx, c->cz);
    if (chunks.count(k)) return;
    LChunk& l = chunks[k];
    l.c = c;
    std::vector<uint8_t> saved;
    {
        std::lock_guard lock(c->m);
        saved = c->level.ticks;
        if (!c->level.post_processed) l.post_queued = true;
    }
    if (!saved.empty()) {
        Reader r(saved);
        try {
            unpack_ticks(r, l.bt, c->cx, c->cz, now());
            unpack_ticks(r, l.ft, c->cx, c->cz, now());
        } catch (const std::exception&) {
        }
    }
    block_ticks.add_container(c->cx, c->cz, &l.bt);
    fluid_ticks.add_container(c->cx, c->cz, &l.ft);
    if (l.post_queued) post_pending.push_back(k);
}
// Saves the tick lists into the chunk (marks it dirty when they changed).
inline void pack(LChunk& l) {
    Writer w;
    if (l.bt.count() || l.ft.count()) {
        pack_ticks(w, l.bt, now());
        pack_ticks(w, l.ft, now());
    }
    std::lock_guard lock(l.c->m);
    if (l.c->level.ticks != w.b) {
        l.c->level.ticks = std::move(w.b);
        l.c->dirty = true;
    }
}
inline void release(int64_t k) {
    auto it = chunks.find(k);
    if (it == chunks.end()) return;
    pack(it->second);
    block_ticks.remove_container(it->second.c->cx, it->second.c->cz);
    fluid_ticks.remove_container(it->second.c->cx, it->second.c->cz);
    chunks.erase(it);
    last_key = INT64_MIN, last_chunk = nullptr;
}
inline void pack_all() {
    for (auto& [k, l] : chunks) pack(l);
}

// The chunk at (cx, cz) if loaded (never loads or generates).
inline LChunk* lchunk(int cx, int cz) {
    int64_t k0 = chunk_key(cx, cz);
    if (k0 == last_key) return last_chunk;
    auto it = chunks.find(k0);
    if (it != chunks.end()) {
        last_key = k0, last_chunk = &it->second;
        return last_chunk;
    }
    ChunkPtr c = world.find(cx, cz);
    if (!c) return nullptr;
    adopt(c);
    return &chunks[chunk_key(cx, cz)];
}
inline bool loaded(int cx, int cz) { return lchunk(cx, cz) != nullptr; }
inline bool loaded(const Pos& p) { return loaded(p.x >> 4, p.z >> 4); }
inline bool in_height(int y) { return y >= world.min_y() && y < world.max_y(); }

inline int get(const Pos& p) {
    if (!in_height(p.y)) return blocks::void_air;
    LChunk* l = lchunk(p.x >> 4, p.z >> 4);
    if (!l) return blocks::void_air;
    std::lock_guard lock(l->c->m);
    return l->c->get(p.x & 15, p.y, p.z & 15);
}
inline int get(int x, int y, int z) { return get(Pos{x, y, z}); }

// ---------- client sync (ChunkHolder.blockChanged / broadcastChanges) ----------
inline std::unordered_map<int64_t, std::vector<uint16_t>> changed;  // section key -> local (x<<8 | z<<4 | y)
inline int64_t section_key(int sx, int sy, int sz) {  // SectionPos.asLong
    return int64_t((uint64_t(int64_t(sx)) & 0x3FFFFF) << 42 | (uint64_t(int64_t(sz)) & 0x3FFFFF) << 20 | (uint64_t(int64_t(sy)) & 0xFFFFF));
}
inline std::unordered_map<int64_t, int64_t> chunk_changed_at;  // chunk -> server_tick of its last client-visible change
inline void block_changed(const Pos& p) {
    chunk_changed_at[chunk_key(p.x >> 4, p.z >> 4)] = server_tick;
    changed[section_key(p.x >> 4, p.y >> 4, p.z >> 4)].push_back(uint16_t((p.x & 15) << 8 | (p.z & 15) << 4 | (p.y & 15)));
}
inline void broadcast_changes() {
    for (auto& [k, list] : changed) {
        int sx = int(k >> 42), sz = int(k << 22 >> 42), sy = int(k << 44 >> 44);
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
        LChunk* l = lchunk(sx, sz);
        if (!l) continue;
        Writer w;
        if (list.size() == 1) {
            int x = sx * 16 + (list[0] >> 8), y = sy * 16 + (list[0] & 15), z = sz * 16 + (list[0] >> 4 & 15);
            w = block_update(x, y, z, get(x, y, z));
        } else {
            w.varint(play_cb::section_blocks_update).i64(k).varint(int32_t(list.size()));
            std::lock_guard lock(l->c->m);
            for (uint16_t e : list) write_varlong(w, int64_t(l->c->get(e >> 8, sy * 16 + (e & 15), e >> 4 & 15)) << 12 | e);
        }
        for (auto& p : players)
            if (sees_chunk(*p, sx, sz)) p->send(w);
    }
    changed.clear();
}

// ---------- scheduled ticks (LevelAccessor.scheduleTick) ----------
inline void schedule_block(const Pos& p, int block, int delay, int8_t priority = NORMAL) {
    block_ticks.schedule({block, p, now() + delay, priority, sub_tick++});
}
inline void schedule_fluid(const Pos& p, int fluid, int delay) {
    fluid_ticks.schedule({fluid, p, now() + delay, NORMAL, sub_tick++});
}
inline bool has_block_tick(const Pos& p, int block) { return block_ticks.has(p, block); }

// ---------- neighbor updates (CollectingNeighborUpdater) ----------
inline int max_chained_updates() {
    static int v = props.count("max-chained-neighbor-updates") ? std::stoi(props["max-chained-neighbor-updates"]) : 1000000;
    return v;
}
struct NUpdate {
    enum Kind : uint8_t { SHAPE, SIMPLE, FULL, MULTI } kind;
    Pos pos, npos;  // SHAPE: pos updated, npos changed; MULTI: npos = source
    int dir = 0, state = 0, block = 0, flags = 0, limit = 0, idx = 0, skip = -1;
    bool piston = false;
};
inline std::deque<NUpdate> nu_stack;
inline std::vector<NUpdate> nu_added;
inline int nu_count = 0;

inline bool set(const Pos& p, int state, int flags = UPDATE_ALL, int limit = UPDATE_LIMIT);
inline bool destroy(const Pos& p, bool drop, int limit = UPDATE_LIMIT);

// Block.updateOrDestroy
inline void update_or_destroy(int old, int now_state, const Pos& p, int flags, int limit) {
    if (now_state == old) return;
    if (is_air(now_state)) destroy(p, (flags & UPDATE_SUPPRESS_DROPS) == 0, limit);
    else set(p, now_state, flags & ~UPDATE_SUPPRESS_DROPS, limit);
}
inline void execute_update(int state, const Pos& p, int src_block, bool piston) {
    neighbor_changed(state, p, src_block, piston);
}
inline bool nu_run_next(NUpdate& u) {
    switch (u.kind) {
        case NUpdate::SHAPE: {
            int cur = get(u.pos);
            if ((u.flags & UPDATE_SKIP_SHAPE_UPDATE_ON_WIRE) == 0 || block(cur) != block_id("redstone_wire"))
                update_or_destroy(cur, update_shape(cur, u.pos, u.dir, u.npos, u.state), u.pos, u.flags, u.limit);
            return false;
        }
        case NUpdate::SIMPLE: execute_update(get(u.pos), u.pos, u.block, false); return false;
        case NUpdate::FULL: execute_update(u.state, u.pos, u.block, u.piston); return false;
        case NUpdate::MULTI: {
            int d = UPDATE_ORDER[u.idx++];
            Pos np = u.npos.rel(d);
            execute_update(get(np), np, u.block, false);
            if (u.idx < 6 && UPDATE_ORDER[u.idx] == u.skip) u.idx++;
            return u.idx < 6;
        }
    }
    return false;
}
inline void nu_add_and_run(NUpdate u) {
    bool running = nu_count > 0;
    int maxc = max_chained_updates();
    bool too_many = maxc >= 0 && nu_count >= maxc;
    nu_count++;
    if (!too_many) {
        if (running) nu_added.push_back(u);
        else nu_stack.push_front(u);
    } else if (nu_count - 1 == maxc) {
        log_error("Too many chained neighbor updates. Skipping the rest. First skipped position: %d, %d, %d", u.pos.x, u.pos.y, u.pos.z);
    }
    if (running) return;
    while (!nu_stack.empty() || !nu_added.empty()) {
        for (int i = int(nu_added.size()) - 1; i >= 0; i--) nu_stack.push_front(nu_added[size_t(i)]);
        nu_added.clear();
        while (nu_added.empty()) {
            if (!nu_run_next(nu_stack.front())) {
                nu_stack.pop_front();
                break;
            }
        }
    }
    nu_stack.clear();
    nu_added.clear();
    nu_count = 0;
}
// Level.neighborShapeChanged: `p` learns that its neighbor in direction `dir` (at np) is now `nstate`.
inline void shape_update(int dir, int nstate, const Pos& p, const Pos& np, int flags, int limit) {
    NUpdate u{NUpdate::SHAPE, p, np};
    u.dir = dir, u.state = nstate, u.flags = flags, u.limit = limit;
    nu_add_and_run(u);
}
inline void neighbor_changed_at(const Pos& p, int src_block) {  // Level.neighborChanged(pos, block, orientation)
    NUpdate u{NUpdate::SIMPLE, p, p};
    u.block = src_block;
    nu_add_and_run(u);
}
inline void neighbor_changed_full(int state, const Pos& p, int src_block, bool piston) {
    NUpdate u{NUpdate::FULL, p, p};
    u.state = state, u.block = src_block, u.piston = piston;
    nu_add_and_run(u);
}
inline void update_neighbors_except(const Pos& p, int src_block, int skip) {  // updateNeighborsAtExceptFromFacing
    NUpdate u{NUpdate::MULTI, p, p};
    u.block = src_block, u.skip = skip;
    if (UPDATE_ORDER[0] == skip) u.idx = 1;
    nu_add_and_run(u);
}
inline void update_neighbors(const Pos& p, int src_block) { update_neighbors_except(p, src_block, -1); }
// BlockStateBase.updateNeighbourShapes
inline void update_neighbour_shapes(int state, const Pos& p, int flags, int limit) {
    for (int d : SHAPE_ORDER) shape_update(OPP[d], state, p.rel(d), p, flags, limit);
}
// Block.updateFromNeighbourShapes
inline int update_from_neighbour_shapes(int state, const Pos& p) {
    for (int d : SHAPE_ORDER) state = update_shape(state, p, d, p.rel(d), get(p.rel(d)));
    return state;
}

// ---------- Level.setBlock ----------
inline bool section_only_air(const Chunk& c, int y) {
    auto& b = c.sections[size_t((y - c.min_y) >> 4)].blocks;
    return !b.bits && is_air(b.palette[0]);
}
inline bool set(const Pos& p, int state, int flags, int limit) {
    if (!in_height(p.y)) return false;
    LChunk* l = lchunk(p.x >> 4, p.z >> 4);
    if (!l) return false;
    Chunk& c = *l->c;
    int old;
    {  // LevelChunk.setBlockState (lock order: light engine, then chunk; like World::set_block)
        std::lock_guard light_lock(light::mutex());
        {
            std::lock_guard lock(c.m);
            if (section_only_air(c, p.y) && is_air(state)) return false;
            old = c.get(p.x & 15, p.y, p.z & 15);
            if (old == state) return false;
            c.set(p.x & 15, p.y, p.z & 15, uint16_t(state));
            c.dirty = true;
        }
        if (!c.fullbright) light::block_changed(p.x, p.y, p.z, uint16_t(old), uint16_t(state));
    }
    cur_level->note_block(p.x, p.y, p.z, uint16_t(old), uint16_t(state));
    bool piston = flags & UPDATE_MOVE_BY_PISTON;
    bool changed_block = block(old) != block(state);
    if (changed_block && (T.flags[size_t(old)] & F_BLOCK_ENTITY)) be_removed(old, p);
    if ((changed_block || rail_table[size_t(block(state))]) && ((flags & UPDATE_NEIGHBORS) || piston)) after_removal(old, p, piston);
    if (block(get(p)) != block(state)) return false;
    if (!(flags & UPDATE_SKIP_ON_PLACE)) on_place(state, p, old, piston);
    // Level.setBlock
    if (get(p) != state) return true;
    if (flags & UPDATE_CLIENTS) block_changed(p);
    if (flags & UPDATE_NEIGHBORS) {
        update_neighbors(p, block(old));
        output_signal_changed(state, p);
    }
    if (!(flags & UPDATE_KNOWN_SHAPE) && limit > 0) {
        int nf = flags & ~(UPDATE_NEIGHBORS | UPDATE_SUPPRESS_DROPS);
        wire_indirect_shapes(old, p, nf, limit - 1);
        update_neighbour_shapes(state, p, nf, limit - 1);
        wire_indirect_shapes(state, p, nf, limit - 1);  // updateIndirectNeighbourShapes: redstone wire only
    }
    return true;
}
inline bool set(int x, int y, int z, int state, int flags = UPDATE_ALL) { return set(Pos{x, y, z}, state, flags); }

// FluidState.createLegacyBlock of the fluid at p (what's left when the block goes).
inline int legacy_fluid_block(int state);  // fluids.hpp
inline void level_event(int event, const Pos& p, int data) {  // Level.levelEvent (sounds / particles)
    Writer w;
    w.varint(play_cb::level_event).i32(event);
    uint64_t pos = (uint64_t(int64_t(p.x) & 0x3FFFFFF) << 38) | (uint64_t(int64_t(p.z) & 0x3FFFFFF) << 12) | uint64_t(int64_t(p.y) & 0xFFF);
    w.i64(int64_t(pos)).i32(data).boolean(false);
    broadcast_near(p.x, p.z, w);
}
// Block.dropResources (no tool).
inline void drop_resources(int state, const Pos& p) {
    if (!rule("block_drops")) return;
    static std::vector<ItemStack> drops;
    drops.clear();
    block_drops(state, p.x, p.y, p.z, ItemStack{}, drops);
    for (auto& d : drops) pop_resource(p.x, p.y, p.z, d);
}
// Level.destroyBlock
inline bool destroy(const Pos& p, bool drop, int limit) {
    int state = get(p);
    if (is_air(state)) return false;
    level_event(2001, p, state);  // spawnDestroyParticles
    if (drop) drop_resources(state, p);
    return set(p, legacy_fluid_block(state), UPDATE_ALL, limit);
}
inline bool remove(const Pos& p, bool piston = false) {  // Level.removeBlock
    return set(p, legacy_fluid_block(get(p)), UPDATE_ALL | (piston ? UPDATE_MOVE_BY_PISTON : 0));
}

// ---------- environment helpers ----------
inline int sim_distance() {
    static int v = std::min(view_distance(), props.count("simulation-distance") ? std::stoi(props["simulation-distance"]) : 10);
    return v;
}
inline bool raining() { return weather.rain_level > 0.2f; }
inline bool thundering() { return raining() && weather.thunder_level > 0.9f; }
// Heightmap MOTION_BLOCKING: first y above the top block that blocks motion or holds fluid.
inline int motion_blocking_height(int x, int z) {
    LChunk* l = lchunk(x >> 4, z >> 4);
    if (!l) return world.min_y();
    std::lock_guard lock(l->c->m);
    for (int y = world.max_y() - 1; y >= world.min_y(); y--) {
        int s = l->c->get(x & 15, y, z & 15);
        if (is_solid(s) || has_fluid(s)) return y + 1;
    }
    return world.min_y();
}
// Light from the light engine (src/light/): stored sky / block light, LevelReader.getRawBrightness.
inline int sky_light(const Pos& p) { if (!cur_level->skylight) return 0; return in_height(p.y) ? light::level(true, p.x, p.y, p.z) : 15; }
inline int block_light(const Pos& p) { return in_height(p.y) ? light::level(false, p.x, p.y, p.z) : 0; }
inline int raw_light(const Pos& p, int darken) { return std::max(sky_light(p) - darken, block_light(p)); }
// Level.skyDarken: 15 - SKY_LIGHT_LEVEL. ponytail: the overworld timeline curve approximated by the legacy
// celestial-angle formula (0 at noon .. 11 at night, more under rain/thunder).
inline int sky_darken() {
    int64_t t = clocks[0].total % 24000;
    double f = (double(t) / 24000.0 - 0.25);
    f = f - std::floor(f);
    double orig = f;
    f = 0.5 - std::cos(f * M_PI) / 2.0;
    f = (orig * 2.0 + f) / 3.0;
    double d = 1.0 - (std::cos(f * 2 * M_PI) * 2.0 + 0.5);
    d = std::clamp(d, 0.0, 1.0);
    d = 1.0 - d;
    d *= 1.0 - weather.rain_level * 5.0f / 16.0;
    d *= 1.0 - weather.thunder_level * 5.0f / 16.0;
    return int((1.0 - d) * 11.0);
}
inline bool can_see_sky(const Pos& p) { return p.y >= motion_blocking_height(p.x, p.z) || sky_light(p) >= 15; }

// ---------- biomes / weather (Biome climate, BiomeManager zoom) ----------
struct Climate { float temperature = 0.5f; bool frozen = false, precipitation = true; };
inline std::vector<Climate> climates;
inline int64_t biome_zoom_seed = 0;
inline void load_climates() {
    std::vector<std::string> names;
    for (auto& e : std::filesystem::recursive_directory_iterator("vanilla/src/data/minecraft/worldgen/biome"))
        if (e.path().extension() == ".json") {
            auto rel = std::filesystem::relative(e.path(), "vanilla/src/data/minecraft/worldgen/biome").string();
            names.push_back(rel.substr(0, rel.size() - 5));
        }
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        Json j = Json::parseFile("vanilla/src/data/minecraft/worldgen/biome/" + n + ".json");
        Climate c;
        c.temperature = j["temperature"].asFloat();
        c.frozen = j.find("temperature_modifier") && j["temperature_modifier"].asString() == "frozen";
        c.precipitation = j["has_precipitation"].asBool();
        climates.push_back(c);
    }
    auto kv = read_level_data();
    int64_t seed = kv.count("level-seed") ? parse_seed(kv["level-seed"]) : 0;
    // BiomeManager.obfuscateSeed: first 8 bytes of SHA-256(seed as little-endian long).
    biome_zoom_seed = material::MaterialSystem::obfuscateSeed(seed);
}
inline int64_t lcg(int64_t a, int64_t b) { return int64_t(uint64_t(a) * (uint64_t(a) * 6364136223846793005ull + 1442695040888963407ull) + uint64_t(b)); }
inline int noise_biome(int qx, int qy, int qz) {
    LChunk* l = lchunk(qx >> 2, qz >> 2);
    if (!l) return 0;
    int y = std::clamp(qy - (l->c->min_y >> 2), 0, l->c->height() / 4 - 1);
    std::lock_guard lock(l->c->m);
    return l->c->get_biome(qx & 3, y, qz & 3);
}
inline int biome_at(const Pos& p) {  // BiomeManager.getBiome
    int ax = p.x - 2, ay = p.y - 2, az = p.z - 2;
    int px = ax >> 2, py = ay >> 2, pz = az >> 2;
    double fx = (ax & 3) / 4.0, fy = (ay & 3) / 4.0, fz = (az & 3) / 4.0;
    int best = 0;
    double best_d = INFINITY;
    auto fiddle = [](int64_t s) {
        double d = double(int(uint64_t(s) >> 24 & 1023)) / 1024.0;
        return (d - 0.5) * 0.9;
    };
    for (int i = 0; i < 8; i++) {
        bool xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
        int cx = xe ? px : px + 1, cy = ye ? py : py + 1, cz = ze ? pz : pz + 1;
        double dx = xe ? fx : fx - 1.0, dy = ye ? fy : fy - 1.0, dz = ze ? fz : fz - 1.0;
        int64_t r = lcg(biome_zoom_seed, cx);
        r = lcg(r, cy), r = lcg(r, cz), r = lcg(r, cx), r = lcg(r, cy), r = lcg(r, cz);
        double ox = fiddle(r);
        r = lcg(r, biome_zoom_seed);
        double oy = fiddle(r);
        r = lcg(r, biome_zoom_seed);
        double oz = fiddle(r);
        double d = (dz + oz) * (dz + oz) + (dy + oy) * (dy + oy) + (dx + ox) * (dx + ox);
        if (best_d > d) best = i, best_d = d;
    }
    return noise_biome((best & 4) == 0 ? px : px + 1, (best & 2) == 0 ? py : py + 1, (best & 1) == 0 ? pz : pz + 1);
}
inline float temperature(int biome, const Pos& p) {  // Biome.getHeightAdjustedTemperature (sea level 63)
    static SimplexNoise temp_noise = [] { WorldgenRandom r(std::make_unique<LegacyRandomSource>(1234)); return SimplexNoise(r, true); }();
    static SimplexNoise info_noise = [] { WorldgenRandom r(std::make_unique<LegacyRandomSource>(2345)); return SimplexNoise(r, true); }();
    static auto frozen_noise = [] {
        WorldgenRandom r(std::make_unique<LegacyRandomSource>(3456));
        NoiseStack::Builder b;
        b.add(std::make_shared<SimplexNoise>(r, true), 1.0, 0.14285715f);
        b.add(std::make_shared<SimplexNoise>(r, true), 0.5, 0.2857143f);
        b.add(std::make_shared<SimplexNoise>(r, true), 0.25, 0.5714286f);
        return b.build();
    }();
    const Climate& c = climates.empty() ? Climate{} : climates[size_t(biome)];
    float t = c.temperature;
    if (c.frozen) {
        double large = double(frozen_noise->get(p.x * 0.05, p.z * 0.05) * 7.0f);
        double edge = info_noise.get(p.x * 0.2, p.z * 0.2);
        if (large + edge < 0.3 && info_noise.get(p.x * 0.09, p.z * 0.09) < 0.8) t = 0.2f;
    }
    int snow_level = 63 + 17;
    if (p.y > snow_level) {
        float v = temp_noise.get(double(float(p.x) / 8.0f), double(float(p.z) / 8.0f)) * 8.0f;
        return t - (v + float(p.y) - float(snow_level)) * 0.05f / 40.0f;
    }
    return t;
}
inline bool warm_enough_to_rain(const Pos& p) { return temperature(biome_at(p), p) >= 0.15f; }
inline bool cold_enough_to_snow(const Pos& p) { return !warm_enough_to_rain(p); }
enum Precip { NONE, RAIN, SNOW };
inline int precipitation_at(const Pos& p) {
    int b = biome_at(p);
    if (!climates.empty() && !climates[size_t(b)].precipitation) return NONE;
    return temperature(b, p) >= 0.15f ? RAIN : SNOW;
}
inline bool raining_at(const Pos& p) {  // Level.isRainingAt
    if (!raining() || !can_see_sky(p) || motion_blocking_height(p.x, p.z) > p.y) return false;
    return precipitation_at(p) == RAIN;
}
inline bool is_water_at(const Pos& p) { return is_water(get(p)); }

}  // namespace lvl
