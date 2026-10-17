#pragma once
// Natural spawning (NaturalSpawner + LocalMobCapCalculator + PotentialCalculator + SpawnPlacementTypes) and
// chunk-generation creature spawning (NaturalSpawner.spawnMobsForChunkGeneration). Tick thread.
#include "types/monsters.hpp"

namespace spawn_detail {
inline int64_t spawned_total = 0;
struct Charge { int x, y, z; double c; };
struct State {
    int spawnable_chunks = 0;
    std::array<int, CAT_COUNT> counts{};
    std::vector<Charge> charges;
    std::unordered_map<const Player*, std::array<int, CAT_COUNT>> local;  // LocalMobCapCalculator.MobCounts
    int last_x = INT32_MIN, last_y = 0, last_z = 0, last_type = -1;
    double last_charge = 0;
};
inline bool player_close_for_spawning(const Player& p, int cx, int cz) {  // ChunkMap.playerIsCloseEnoughForSpawning
    if (p.gamemode == 3 || p.level != cur_level) return false;  // ServerLevel players only
    double dx = cx * 16 + 8 - p.x, dz = cz * 16 + 8 - p.z;
    return dx * dx + dz * dz < 16384.0;
}
inline const BiomeSpawnInfo& spawns_at(int x, int y, int z) {
    static BiomeSpawnInfo none;
    int b = biome_at_pos(x, y, z);
    return size_t(b) < biome_spawns.size() ? biome_spawns[size_t(b)] : none;
}
inline double potential_change(const State& s, int x, int y, int z, double charge) {
    if (charge == 0.0) return 0.0;
    double t = 0;
    for (auto& c : s.charges) {
        double d = double(c.x - x) * (c.x - x) + double(c.y - y) * (c.y - y) + double(c.z - z) * (c.z - z);
        t += d == 0.0 ? INFINITY : c.c / std::sqrt(d);
    }
    return t * charge;
}
// SpawnPlacementTypes.*.isSpawnPositionOk
inline bool valid_empty_spawn_block(int s, int type) {  // NaturalSpawner.isValidEmptySpawnBlock
    if (sflag(s, SF_FULL_BLOCK) || sflag(s, SF_SIGNAL) || fluid_type(s)) return false;
    if (block_in_tag(s, "prevent_mob_spawning_inside")) return false;
    const auto& t = entity_type_info[size_t(type)];  // EntityType.isBlockDangerous
    if (in_sorted(t.immune, block_index(s))) return true;
    uint8_t pt = state_path_type[size_t(s)];
    if (!t.fire_immune && (pt == PT_FIRE || pt == PT_LAVA)) return false;
    int b = block_index(s);
    return b != block_index(blocks::wither_rose) && b != block_index(blocks::sweet_berry_bush) && b != block_index(blocks::cactus) &&
           b != block_index(blocks::powder_snow);
}
inline bool spawn_position_ok(int type, int x, int y, int z) {
    const auto& t = entity_type_info[size_t(type)];
    switch (t.place) {
        case SpawnPlacement::NONE: case SpawnPlacement::NO_RESTRICTIONS: return true;
        case SpawnPlacement::IN_WATER: return fluid_type(block_at(x, y, z)) == FLUID_WATER && !sflag(block_at(x, y + 1, z), SF_CONDUCTOR);
        case SpawnPlacement::IN_LAVA: return fluid_type(block_at(x, y, z)) == FLUID_LAVA;
        case SpawnPlacement::ON_GROUND:
            if (!valid_spawn_on(block_at(x, y - 1, z), type)) return false;
            return valid_empty_spawn_block(block_at(x, y, z), type) && valid_empty_spawn_block(block_at(x, y + 1, z), type);
    }
    return false;
}
inline bool no_collision_spawn_box(int type, double x, double y, double z) {  // level.noCollision(type.getSpawnAABB)
    const auto& t = entity_type_info[size_t(type)];
    return no_block_collision(AABB::at_feet(x, y, z, t.w, t.h));
}
inline const SpawnerData* pick_weighted(const std::vector<SpawnerData>& l) {  // WeightedList.getRandom
    int total = 0;
    for (auto& s : l) total += s.weight;
    if (total <= 0) return nullptr;
    int r = rand_int(total);
    for (auto& s : l) if ((r -= s.weight) < 0) return &s;
    return nullptr;
}
inline bool contains(const std::vector<SpawnerData>& l, const SpawnerData* d) {
    for (auto& s : l) if (&s == d) return true;
    return false;
}
inline Entity* nearest_spawn_player(double x, double y, double z, double& dsq) {  // getNearestPlayer(x, y, z, -1, false)
    Entity* best = nullptr;
    dsq = 1e300;
    for (Entity* p : player_entities()) {
        if (p->is_spectator() || p->level() != cur_level) continue;
        double d = p->dist_sqr(x, y, z);
        if (d < dsq) dsq = d, best = p;
    }
    return best;
}

// NaturalSpawner.spawnCategoryForPosition
inline void spawn_category_at(State& st, int cat, int cx, int cz, int sx, int sy, int sz) {
    if (sflag(block_at(sx, sy, sz), SF_CONDUCTOR)) return;
    int cluster = 0;
    for (int group = 0; group < 3; group++) {
        int x = sx, z = sz;
        const SpawnerData* data = nullptr;
        SpawnGroupData* gd = nullptr;
        int max = int(std::ceil(rand_float() * 4.0f)), group_size = 0;
        for (int ll = 0; ll < max; ll++) {
            x += rand_int(6) - rand_int(6);
            z += rand_int(6) - rand_int(6);
            double xx = x + 0.5, zz = z + 0.5, dsq;
            if (!nearest_spawn_player(xx, sy, zz, dsq)) continue;
            // isRightDistanceToPlayerAndSpawnPoint
            if (dsq <= 576.0) continue;
            double wx = world_spawn.x + 0.5 - xx, wy = world_spawn.y - sy, wz = world_spawn.z + 0.5 - zz;
            if (wx * wx + wy * wy + wz * wz < 576.0) continue;
            if (!((x >> 4) == cx && (z >> 4) == cz) && !is_entity_ticking_chunk(x >> 4, z >> 4)) continue;
            const BiomeSpawnInfo& bs = spawns_at(x, sy, z);
            const auto& list = bs.spawners[size_t(cat)];
            if (!data) {
                if (cat == CAT_WATER_AMBIENT && biome_in_tag(biome_at_pos(x, sy, z), "reduced_water_ambient_spawns") && rand_float() < 0.98f) break;
                data = pick_weighted(list);
                if (!data) break;
                max = rand_between(data->min, data->max);
            }
            // isValidSpawnPostitionForType
            const auto& t = entity_type_info[size_t(data->type)];
            const auto& c = mob_categories[size_t(t.cat)];
            if (t.cat == CAT_MISC) continue;
            if (!t.far && dsq > double(c.despawn) * c.despawn) continue;
            if (!t.summon || !contains(list, data)) continue;
            if (!spawn_position_ok(data->type, x, sy, z)) continue;
            if (difficulty() == 0 && !t.peaceful) continue;
            if (!check_spawn_rules_for(data->type, SpawnReason::NATURAL, x, sy, z)) continue;
            if (!no_collision_spawn_box(data->type, xx, sy, zz)) continue;
            // SpawnState.canSpawn (spawn costs)
            if (const double* cost = bs.cost(data->type)) {
                st.last_x = x, st.last_y = sy, st.last_z = z, st.last_type = data->type, st.last_charge = cost[1];
                if (potential_change(st, x, sy, z, cost[1]) > cost[0]) continue;
            } else st.last_charge = 0, st.last_type = data->type, st.last_x = x, st.last_y = sy, st.last_z = z;
            auto e = create_entity(data->type);
            Mob* mob = e ? e->as_mob() : nullptr;
            if (!mob) return;
            mob->snap_to(xx, sy, zz, rand_float() * 360.0f, 0.0f);
            // isValidPositionForMob
            if (dsq > double(c.despawn) * c.despawn && mob->remove_when_far_away(dsq)) continue;
            if (!mob->check_spawn_rules(SpawnReason::NATURAL) || !mob->check_spawn_obstruction()) continue;
            gd = mob->finalize_spawn(SpawnReason::NATURAL, gd);
            cluster++, group_size++;
            spawned_total++;
            // SpawnState.afterSpawn
            st.charges.push_back({x, sy, z, st.last_charge});
            st.counts[size_t(t.cat)]++;
            for (auto& pp : players)
                if (player_close_for_spawning(*pp, x >> 4, z >> 4)) st.local[pp.get()][size_t(t.cat)]++;
            int msc = mob->max_spawn_cluster_size();
            bool max_group = mob->is_max_group_size_reached(group_size);
            add_entity(std::move(e));
            if (cluster >= msc) return;
            if (max_group) break;
        }
    }
}

inline State create_state() {  // NaturalSpawner.createState (over loaded entities near players)
    State s;
    std::unordered_set<int64_t> chunks;  // getNaturalSpawnChunkCount: chunks within 8 of a player
    for (auto& p : players)
        if (p->level == cur_level)
        for (int x = p->cx - 8; x <= p->cx + 8; x++)
            for (int z = p->cz - 8; z <= p->cz + 8; z++) chunks.insert(mth::chunk_key(x, z));
    s.spawnable_chunks = int(chunks.size());
    for (auto& pp : players) s.local[pp.get()] = {};
    for (Entity* e : ent_detail::to_tick) {
        if (e->removed) continue;
        Mob* m = e->as_mob();
        if (m && (m->is_persistence_required() || m->requires_custom_persistence())) continue;
        int cat = e->info().cat;
        if (cat == CAT_MISC) continue;
        if (const double* c = spawns_at(e->block_x(), e->block_y(), e->block_z()).cost(e->type))
            s.charges.push_back({e->block_x(), e->block_y(), e->block_z(), c[1]});
        if (m)
            for (auto& pp : players)
                if (player_close_for_spawning(*pp, e->chunk_x, e->chunk_z)) s.local[pp.get()][size_t(cat)]++;
        s.counts[size_t(cat)]++;
    }
    return s;
}
}  // namespace spawn_detail

// ServerChunkCache.tickChunks spawning part. Runs once per tick after entity ticking.
inline void tick_natural_spawning() {
    using namespace spawn_detail;
    if (!level_has_players(cur_level) || !rule("spawn_mobs")) return;
    State st = create_state();
    bool persistent = game_time % 400 == 0, enemies = difficulty() != 0;
    std::vector<int> cats;
    for (int c = 0; c < CAT_COUNT; c++) {  // getFilteredSpawningCategories
        if (c == CAT_MISC) continue;
        const auto& mc = mob_categories[size_t(c)];
        if (!(enemies || mc.friendly) || (!persistent && mc.persistent)) continue;
        if (c == CAT_MONSTER && !rule("spawn_monsters")) continue;
        if (st.counts[size_t(c)] >= mc.max * st.spawnable_chunks / 289) continue;
        cats.push_back(c);
    }
    if (cats.empty()) return;
    // collectSpawningChunks: entity-ticking chunks with a non-spectator within 128 blocks, shuffled
    std::vector<std::pair<int, int>> chunks;
    std::unordered_set<int64_t> seen;
    int sim = simulation_distance();
    for (auto& p : players)
        if (p->level == cur_level)
        for (int x = p->cx - sim; x <= p->cx + sim; x++)
            for (int z = p->cz - sim; z <= p->cz + sim; z++) {
                if (!seen.insert(mth::chunk_key(x, z)).second || !loaded_chunk(x, z)) continue;
                bool close = false;
                for (auto& q : players) close |= player_close_for_spawning(*q, x, z);
                if (close) chunks.emplace_back(x, z);
            }
    std::shuffle(chunks.begin(), chunks.end(), game_rng);
    for (auto [cx, cz] : chunks)
        for (int c : cats) {
            bool local_ok = false;  // LocalMobCapCalculator.canSpawn
            for (auto& pp : players)
                if (player_close_for_spawning(*pp, cx, cz) && st.local[pp.get()][size_t(c)] < mob_categories[size_t(c)].max) local_ok = true;
            if (!local_ok) continue;
            int x = cx * 16 + rand_int(16), z = cz * 16 + rand_int(16);  // getRandomPosWithin
            int top = surface_y_cached(x, z) + 1;  // ponytail: MOTION_BLOCKING stands in for WORLD_SURFACE (same without leaves/glass panes)
            int y = rand_between(MIN_Y, top);
            if (y >= MIN_Y + 1) spawn_category_at(st, c, cx, cz, x, y, z);
        }
}

// Chunk-generation creature spawning (NaturalSpawner.spawnMobsForChunkGeneration), run on the tick thread for
// freshly generated chunks once they are loaded. ponytail: uses the shared RNG, not the worldgen decoration seed.
inline std::mutex fresh_chunks_mtx;
inline std::vector<std::pair<int, int>> fresh_chunks;
inline void spawn_for_chunk_generation(int cx, int cz) {
    if (!rule("spawn_mobs")) return;
    int xo = cx * 16, zo = cz * 16;
    const BiomeSpawnInfo& bs = spawn_detail::spawns_at(xo + 8, 64, zo + 8);
    const auto& mobs = bs.spawners[CAT_CREATURE];
    if (mobs.empty()) return;
    while (rand_float() < bs.creature_probability) {
        const SpawnerData* d = spawn_detail::pick_weighted(mobs);
        if (!d) continue;
        int count = rand_between(d->min, d->max);
        SpawnGroupData* gd = nullptr;
        int x = xo + rand_int(16), z = zo + rand_int(16), sx = x, sz = z;
        const auto& t = entity_type_info[size_t(d->type)];
        for (int i = 0; i < count; i++) {
            bool ok = false;
            for (int a = 0; !ok && a < 4; a++) {
                int y = surface_y_cached(x, z);  // getTopNonCollidingPos (MOTION_BLOCKING_NO_LEAVES ~ MOTION_BLOCKING)
                if (t.place == SpawnPlacement::ON_GROUND && state_path_type[size_t(block_at(x, y - 1, z))] != PT_BLOCKED && !has_collision(block_at(x, y - 1, z))) y--;
                if (t.summon && spawn_detail::spawn_position_ok(d->type, x, y, z)) {
                    double fx = std::clamp(double(x), double(xo) + t.w, xo + 16.0 - t.w), fz = std::clamp(double(z), double(zo) + t.w, zo + 16.0 - t.w);
                    if (spawn_detail::no_collision_spawn_box(d->type, fx, y, fz) &&
                        check_spawn_rules_for(d->type, SpawnReason::CHUNK_GENERATION, mth::floor(fx), y, mth::floor(fz))) {
                        auto e = create_entity(d->type);
                        if (Mob* m = e ? e->as_mob() : nullptr) {
                            m->snap_to(fx, y, fz, rand_float() * 360.0f, 0.0f);
                            if (m->check_spawn_rules(SpawnReason::CHUNK_GENERATION) && m->check_spawn_obstruction()) {
                                gd = m->finalize_spawn(SpawnReason::CHUNK_GENERATION, gd);
                                add_entity(std::move(e));
                                ok = true;
                            }
                        }
                    }
                }
                x += rand_int(5) - rand_int(5);
                for (z += rand_int(5) - rand_int(5); x < xo || x >= xo + 16 || z < zo || z >= zo + 16; z = sz + rand_int(5) - rand_int(5))
                    x = sx + rand_int(5) - rand_int(5);
            }
        }
    }
}
// Mobs that structure pieces placed while generating the chunk (structure::Level::addMob): witch hut witch + cat,
// monument elder guardians, mansion illagers + allays, ocean ruin drowned, end city shulkers.
inline void spawn_structure_mobs(Chunk& c) {
    std::vector<Chunk::StructureMob> list;
    list.swap(c.structure_mobs);
    for (auto& m : list) {
        auto e = create_entity(m.type);
        if (!e) continue;  // type not implemented yet
        e->snap_to(m.x, m.y, m.z, 0.0f, 0.0f);
        if (Mob* mob = e->as_mob()) {
            if (m.persistent) mob->set_persistence_required();
            if (m.heal) mob->heal(mob->max_health());
            if (m.finalize) mob->finalize_spawn(SpawnReason::STRUCTURE, nullptr);
        }
        add_entity(std::move(e));
    }
}
inline void tick_fresh_chunks() {
    std::vector<std::pair<int, int>> list;
    {
        std::lock_guard lock(fresh_chunks_mtx);
        list.swap(fresh_chunks);
    }
    std::vector<std::pair<int, int>> later;
    for (auto [cx, cz] : list) {
        if (!loaded_chunk(cx, cz)) continue;  // unloaded again before we got to it
        // vanilla spawns in the SPAWN status, once neighbours exist: wait until all 8 neighbours are loaded
        bool ready = true;
        for (int dx = -1; dx <= 1; dx++)
            for (int dz = -1; dz <= 1; dz++) ready &= loaded_chunk(cx + dx, cz + dz) != nullptr;
        if (!ready) { later.emplace_back(cx, cz); continue; }
        ensure_entity_chunk_loaded(cx, cz);
        spawn_for_chunk_generation(cx, cz);
        if (Chunk* c = loaded_chunk(cx, cz)) spawn_structure_mobs(*c);
    }
    if (!later.empty()) {
        std::lock_guard lock(fresh_chunks_mtx);
        fresh_chunks.insert(fresh_chunks.end(), later.begin(), later.end());
    }
}
