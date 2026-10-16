#pragma once
// Entity storage, ticking, tracking and persistence (ServerLevel entity lists + ChunkMap.TrackedEntity +
// ServerEntity + EntityStorage). Included from mobs/entity.hpp. Tick thread only.
//
//  * Storage: entities are owned by an id map and filed per chunk. Players are represented by PlayerEntity
//    proxies (registered with add_player_entity) that are found by queries and ids but never ticked or tracked here.
//  * Ticking: only entities in chunks within the simulation distance of a player tick, so a tick costs
//    O(active entities). The ticking chunk set is recomputed only when a player changes chunk.
//  * Tracking: a player sees an entity within min(type tracking range, view distance) blocks horizontally whose chunk
//    is in its view. Watchers update when the entity changes section or the player changes chunk. Movement goes out
//    as move_entity_pos/_pos_rot/_rot deltas (VecDeltaCodec, 1/4096 block) at the type's update interval, with
//    entity_position_sync every 400 updates or when on_ground flips, plus rotate_head, set_entity_motion and dirty data.
//  * Persistence: <world>/entities/e.<cx>.<cz>.nbt (binary NBT {Entities:[...]}). A chunk's entities load when a
//    player's view reaches it and are saved and unloaded when no player is within view distance + 1.
#include <unordered_set>

using EntityFactory = std::unique_ptr<Entity> (*)();

namespace ent_detail {
struct ChunkEnts { std::vector<Entity*> list; bool ticking = false; };
struct PlayerView { int cx = INT32_MIN, cz = INT32_MIN; std::vector<Entity*> seen; };
inline std::unordered_map<int, std::unique_ptr<Entity>> by_id;
inline std::unordered_map<int64_t, ChunkEnts> by_chunk;
inline std::vector<int64_t> ticking_keys;
inline bool ticking_dirty = true;
inline std::vector<Entity*> player_ents;
inline std::vector<Entity*> to_tick, extra_sync, free_queue;
inline std::unordered_set<int64_t> loaded;  // chunks whose entity storage is in memory
inline std::vector<EntityFactory> factories;
inline std::unordered_map<Player*, PlayerView> views;
inline int64_t section_key(const Entity& e) {
    return mth::block_key(mth::floor(e.pos.x) >> 4, mth::floor(e.pos.y) >> 4, mth::floor(e.pos.z) >> 4);
}
}  // namespace ent_detail

inline int simulation_distance() {
    static int s = props.count("simulation-distance") ? std::stoi(props["simulation-distance"]) : view_distance();
    return std::min(s, view_distance());
}
inline bool near_any_player_chunk(int cx, int cz, int r) {
    for (auto& p : players)
        if (std::abs(cx - p->cx) <= r && std::abs(cz - p->cz) <= r) return true;
    return false;
}

// ---------- registry / factories ----------
inline void register_entity_factory(int type, EntityFactory f) {
    if (ent_detail::factories.size() <= size_t(type)) ent_detail::factories.resize(entity_type_info.size() + 1);
    ent_detail::factories[size_t(type)] = f;
}
inline std::unique_ptr<Entity> create_entity(int type) {  // EntityType.create; null when the type isn't implemented
    if (type < 0 || size_t(type) >= ent_detail::factories.size() || !ent_detail::factories[size_t(type)]) return nullptr;
    auto e = ent_detail::factories[size_t(type)]();
    if (e) e->type = type;
    return e;
}
inline std::unique_ptr<Entity> load_entity_nbt(const Nbt& tag) {  // EntityType.loadEntityRecursive (no passengers)
    const Nbt* id = tag.get("id");
    if (!id || id->t != Nbt::String) return nullptr;
    auto e = create_entity(entity_type_id(id->s));
    if (e) e->load(tag);
    return e;
}

// ---------- lookup / queries ----------
inline Entity* entity_by_id(int id) {
    auto it = ent_detail::by_id.find(id);
    return it == ent_detail::by_id.end() ? nullptr : it->second.get();
}
inline Entity* entity_by_uuid(int64_t hi, int64_t lo) {
    for (auto& [id, e] : ent_detail::by_id)
        if (e->uuid_hi == hi && e->uuid_lo == lo) return e.get();
    return nullptr;
}
inline size_t entity_count() { return ent_detail::by_id.size() - ent_detail::player_ents.size(); }
// Every loaded entity incl. player proxies (commands; O(all)).
template <class F> inline void for_each_entity(F&& f) {
    for (auto& [id, e] : ent_detail::by_id)
        if (!e->removed) f(*e);
}
// Level.getEntities(box): entities whose bounding box intersects `box`, players included when `with_players`.
template <class F> inline void for_each_entity_in(const AABB& box, F&& f, bool with_players = true) {
    int cx0 = mth::floor((box.x0 - 2.0) / 16.0), cx1 = mth::floor((box.x1 + 2.0) / 16.0);
    int cz0 = mth::floor((box.z0 - 2.0) / 16.0), cz1 = mth::floor((box.z1 + 2.0) / 16.0);
    for (int cx = cx0; cx <= cx1; cx++)
        for (int cz = cz0; cz <= cz1; cz++) {
            auto it = ent_detail::by_chunk.find(mth::chunk_key(cx, cz));
            if (it == ent_detail::by_chunk.end()) continue;
            for (size_t i = 0; i < it->second.list.size(); i++) {
                Entity* e = it->second.list[i];
                if (!e->removed && e->bb.intersects(box)) f(*e);
            }
        }
    if (with_players)
        for (Entity* e : ent_detail::player_ents)
            if (!e->removed && e->bb.intersects(box)) f(*e);
}
inline const std::vector<Entity*>& player_entities() { return ent_detail::player_ents; }

// ---------- tracking (ServerEntity / TrackedEntity) ----------
inline void send_dirty_data(Entity& e) {  // ServerEntity.sendDirtyEntityData
    Writer w;
    if (e.data.write_dirty(w, e.id)) e.broadcast(w);
    e.send_extra_changes();
}
inline void track_add(Entity& e, Player& p) {
    e.trk.watchers.push_back(&p);
    ent_detail::views[&p].seen.push_back(&e);
    e.send_pairing(p);
}
inline void track_remove(Entity& e, Player& p, bool send = true) {
    std::erase(e.trk.watchers, &p);
    std::erase(ent_detail::views[&p].seen, &e);
    if (send) p.send(remove_entities_packet({e.id}));
}
inline void update_player(Entity& e, Player& p) {  // TrackedEntity.updatePlayer
    if (e.as_player() == &p) return;
    double dx = p.x - e.pos.x, dz = p.z - e.pos.z;
    double range = std::min(e.info().range * 16, view_dist_cached() * 16);
    int cx = mth::floor(e.pos.x) >> 4, cz = mth::floor(e.pos.z) >> 4;
    bool vis = !e.removed && dx * dx + dz * dz <= range * range && sees_chunk(p, cx, cz);
    bool has = std::find(e.trk.watchers.begin(), e.trk.watchers.end(), &p) != e.trk.watchers.end();
    if (vis && !has) track_add(e, p);
    else if (!vis && has) track_remove(e, p);
}
inline void update_players(Entity& e) {
    for (auto& p : players) update_player(e, *p);
}

inline Writer position_sync_packet(const Entity& e) {  // ClientboundEntityPositionSyncPacket (linear path)
    return std::move(Writer().varint(play_cb::entity_position_sync).varint(e.id).varint(0).f64(e.pos.x).f64(e.pos.y)
                         .f64(e.pos.z).f32(e.yaw).f32(e.pitch).boolean(e.on_ground));
}
inline int64_t enc4096(double v) { return int64_t(std::llround(v * 4096.0)); }  // VecDeltaCodec.encode (Math.round)

inline void send_changes(Entity& e) {  // ServerEntity.sendChanges
    TrackState& t = e.trk;
    int interval = std::max(1, e.info().interval);
    if (e.sync_position) {
        t.tick_count = t.tick_count / interval * interval + interval;
        e.sync_position = false;
    }
    if (e.needs_sync || t.tick_count % interval == 0 || e.data.dirty) {
        uint8_t yr = mth::pack_degrees(e.yaw), xr = mth::pack_degrees(e.pitch);
        bool rot = yr != t.yaw || xr != t.pitch;
        t.teleport_delay++;
        int64_t dx = enc4096(e.pos.x) - t.base_x, dy = enc4096(e.pos.y) - t.base_y, dz = enc4096(e.pos.z) - t.base_z;
        bool arrow = e.in_tag("arrows");
        Vec3 base{double(t.base_x) / 4096.0, double(t.base_y) / 4096.0, double(t.base_z) / 4096.0};
        bool send_pos = arrow || (e.pos - base).length_sqr() >= 7.6293945E-6f || t.tick_count % 60 == 0;
        if (arrow) rot = true;
        Writer pkt;
        bool has_pos = false, has_rot = false;
        if (t.teleport_delay > 400 || t.was_on_ground != e.on_ground) {
            t.was_on_ground = e.on_ground;
            t.teleport_delay = 0;
            pkt = position_sync_packet(e);
            has_pos = has_rot = true;
        } else if (send_pos) {
            if (dx < -32768 || dx > 32767 || dy < -32768 || dy > 32767 || dz < -32768 || dz > 32767) {
                pkt = position_sync_packet(e);
                has_pos = has_rot = true;
            } else if (rot) {
                pkt.varint(play_cb::move_entity_pos_rot).varint(e.id).varint(e.on_ground ? 1 : 0)
                    .i16(int16_t(dx)).i16(int16_t(dy)).i16(int16_t(dz)).u8(yr).u8(xr);
                has_pos = has_rot = true;
            } else {
                pkt.varint(play_cb::move_entity_pos).varint(e.id).varint(e.on_ground ? 1 : 0)
                    .i16(int16_t(dx)).i16(int16_t(dy)).i16(int16_t(dz));
                has_pos = true;
            }
        } else if (rot) {
            pkt.varint(play_cb::move_entity_rot).varint(e.id).boolean(e.on_ground).u8(yr).u8(xr);
            has_rot = true;
        }
        if (e.needs_sync || e.info().deltas) {
            double diff = e.vel.dist_sqr(t.sent_vel);
            if (diff > 1.0E-7 || (diff > 0.0 && e.vel.length_sqr() == 0.0)) {
                t.sent_vel = e.vel;
                e.broadcast(set_entity_motion_packet(e.id, e.vel.x, e.vel.y, e.vel.z));
            }
        }
        if (!pkt.b.empty()) e.broadcast(pkt);
        send_dirty_data(e);
        if (has_pos) t.base_x = enc4096(e.pos.x), t.base_y = enc4096(e.pos.y), t.base_z = enc4096(e.pos.z);
        if (has_rot) t.yaw = yr, t.pitch = xr;
        uint8_t hr = mth::pack_degrees(e.head_yaw());
        if (hr != t.head) {
            e.broadcast(std::move(Writer().varint(play_cb::rotate_head).varint(e.id).u8(hr)));
            t.head = hr;
        }
        e.needs_sync = false;
    }
    t.tick_count++;
    if (e.sync_velocity) {
        e.sync_velocity = false;
        Writer w = set_entity_motion_packet(e.id, e.vel.x, e.vel.y, e.vel.z);
        e.broadcast(w);
        if (Player* self = e.as_player()) self->send(w);
    }
}

// ---------- chunk index ----------
inline void ensure_entity_chunk_loaded(int cx, int cz);
inline ent_detail::ChunkEnts& chunk_ents(int cx, int cz) {
    auto [it, fresh] = ent_detail::by_chunk.try_emplace(mth::chunk_key(cx, cz));
    if (fresh && near_any_player_chunk(cx, cz, simulation_distance())) {
        it->second.ticking = true;
        ent_detail::ticking_keys.push_back(it->first);
    }
    return it->second;
}
inline void entity_moved_chunk(Entity& e, int ocx, int ocz) {
    if (e.as_player()) return;
    auto it = ent_detail::by_chunk.find(mth::chunk_key(ocx, ocz));
    if (it != ent_detail::by_chunk.end()) std::erase(it->second.list, &e);
    ensure_entity_chunk_loaded(e.chunk_x, e.chunk_z);
    chunk_ents(e.chunk_x, e.chunk_z).list.push_back(&e);
}

// Adds an entity to the world (Level.addFreshEntity): assigns id/uuid if unset, files it, sends it to watchers.
inline Entity* add_entity(std::unique_ptr<Entity> up) {
    if (!up) return nullptr;
    Entity& e = *up;
    if (!e.id) e.id = next_entity_id++;
    if (!e.uuid_hi && !e.uuid_lo) {
        e.uuid_hi = int64_t((game_rng() & ~0xF000ull) | 0x4000);
        e.uuid_lo = int64_t((game_rng() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull);
    }
    if (e.width == 0 && e.height == 0) e.set_size(e.info().w, e.info().h, e.info().eye);
    e.set_pos(e.pos);
    e.trk.base_x = enc4096(e.pos.x), e.trk.base_y = enc4096(e.pos.y), e.trk.base_z = enc4096(e.pos.z);
    e.trk.yaw = mth::pack_degrees(e.yaw), e.trk.pitch = mth::pack_degrees(e.pitch), e.trk.head = mth::pack_degrees(e.head_yaw());
    e.trk.sent_vel = e.vel;
    e.trk.was_on_ground = e.on_ground;
    e.trk.section = ent_detail::section_key(e);
    e.added = true;
    ent_detail::by_id[e.id] = std::move(up);
    if (e.as_player()) {
        ent_detail::player_ents.push_back(&e);
    } else {
        ensure_entity_chunk_loaded(e.chunk_x, e.chunk_z);
        chunk_ents(e.chunk_x, e.chunk_z).list.push_back(&e);
        update_players(e);
        e.data.dirty = false;  // pairing already carried every value
        for (auto& d : e.data.items) d.dirty = false;
    }
    e.on_added();
    return &e;
}
template <class T> inline T* add_entity_t(std::unique_ptr<T> up) { return static_cast<T*>(add_entity(std::move(up))); }

// Frees one removed entity: remove packets, unindex.
inline void free_entity(Entity& e) {
    e.on_removed();
    if (!e.trk.watchers.empty()) {
        Writer w = remove_entities_packet({e.id});
        for (Player* p : e.trk.watchers) {
            p->send(w);
            std::erase(ent_detail::views[p].seen, &e);
        }
        e.trk.watchers.clear();
    }
    if (e.as_player()) std::erase(ent_detail::player_ents, &e);
    else if (auto it = ent_detail::by_chunk.find(mth::chunk_key(e.chunk_x, e.chunk_z)); it != ent_detail::by_chunk.end())
        std::erase(it->second.list, &e);
    ent_detail::by_id.erase(e.id);
}
inline void add_player_entity(std::unique_ptr<Entity> e) { add_entity(std::move(e)); }
inline void entity_queue_free(Entity* e) { ent_detail::free_queue.push_back(e); }
inline void flush_free_queue() {
    auto& q = ent_detail::free_queue;
    for (size_t i = 0; i < q.size(); i++) free_entity(*q[i]);  // index: on_removed may remove more
    q.clear();
}

// ---------- persistence ----------
inline Nbt read_nbt_payload(Reader& r, int t, int depth = 0) {
    if (depth > 512) throw std::runtime_error("nbt too deep");
    Nbt n;
    n.t = Nbt::Type(t);
    auto str = [&] { int len = r.u16(); std::string s; for (int i = 0; i < len; i++) s += char(r.u8()); return s; };
    switch (t) {
        case Nbt::Byte: n.i = int8_t(r.u8()); break;
        case Nbt::Short: n.i = int16_t(r.u16()); break;
        case Nbt::Int: n.i = r.i32(); break;
        case Nbt::Long: n.i = r.i64(); break;
        case Nbt::Float: n.d = r.f32(); break;
        case Nbt::Double: n.d = r.f64(); break;
        case Nbt::String: n.s = str(); break;
        case Nbt::List: {
            int et = r.u8(), len = r.i32();
            for (int i = 0; i < len; i++) n.list.push_back(read_nbt_payload(r, et, depth + 1));
            break;
        }
        case Nbt::Compound:
            for (int et; (et = r.u8()) != 0;) {
                std::string k = str();
                n.comp.emplace_back(k, read_nbt_payload(r, et, depth + 1));
            }
            break;
        case Nbt::ByteArray: { int len = r.i32(); for (int i = 0; i < len; i++) n.arr.push_back(int8_t(r.u8())); break; }
        case Nbt::IntArray: { int len = r.i32(); for (int i = 0; i < len; i++) n.arr.push_back(r.i32()); break; }
        case Nbt::LongArray: { int len = r.i32(); for (int i = 0; i < len; i++) n.arr.push_back(r.i64()); break; }
        default: throw std::runtime_error("bad nbt tag");
    }
    return n;
}
inline std::string entity_chunk_path(int cx, int cz) {
    return world_dir + "/entities/e." + std::to_string(cx) + "." + std::to_string(cz) + ".nbt";
}
inline void save_entity_chunk(int cx, int cz) {
    Nbt list = Nbt::make_list();
    if (auto it = ent_detail::by_chunk.find(mth::chunk_key(cx, cz)); it != ent_detail::by_chunk.end())
        for (Entity* e : it->second.list)
            if (e->should_save()) {
                Nbt c = Nbt::compound();
                e->save(c);
                list.list.push_back(std::move(c));
            }
    std::string path = entity_chunk_path(cx, cz);
    std::error_code ec;
    if (list.list.empty()) { std::filesystem::remove(path, ec); return; }
    Nbt root = Nbt::compound();
    root.set("Entities", std::move(list));
    Writer w;
    write_nbt(w, root);
    std::filesystem::create_directories(world_dir + "/entities", ec);
    {
        std::ofstream f(path + ".tmp", std::ios::binary);
        f.write(reinterpret_cast<const char*>(w.b.data()), std::streamsize(w.b.size()));
    }
    std::filesystem::rename(path + ".tmp", path, ec);
}
inline void ensure_entity_chunk_loaded(int cx, int cz) {
    if (!ent_detail::loaded.insert(mth::chunk_key(cx, cz)).second) return;
    std::ifstream f(entity_chunk_path(cx, cz), std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), {});
    try {
        Reader r(buf);
        int t = r.u8();
        Nbt root = read_nbt_payload(r, t);
        if (const Nbt* l = root.get("Entities"))
            for (auto& c : l->list)
                if (auto e = load_entity_nbt(c)) {
                    if (entity_by_uuid(e->uuid_hi, e->uuid_lo)) continue;  // already present (moved in meanwhile)
                    add_entity(std::move(e));
                }
    } catch (const std::exception& ex) {
        fprintf(stderr, "entities: bad file %s (%s)\n", entity_chunk_path(cx, cz).c_str(), ex.what());
    }
}
// Saves and frees the entities of chunks no player is near (EntityStorage + unloading).
inline void unload_far_entity_chunks(bool all = false) {
    int r = view_dist_cached() + 1;
    std::vector<int64_t> keys;
    for (int64_t k : ent_detail::loaded) {
        int cx = int32_t(uint32_t(k)), cz = int32_t(uint32_t(uint64_t(k) >> 32));
        if (all || !near_any_player_chunk(cx, cz, r)) keys.push_back(k);
    }
    for (int64_t k : keys) {
        int cx = int32_t(uint32_t(k)), cz = int32_t(uint32_t(uint64_t(k) >> 32));
        save_entity_chunk(cx, cz);
        if (all) continue;
        ent_detail::loaded.erase(k);
        auto it = ent_detail::by_chunk.find(k);
        if (it == ent_detail::by_chunk.end()) continue;
        std::vector<Entity*> v = it->second.list;
        for (Entity* e : v) e->remove(Removal::UNLOADED_TO_CHUNK);
        flush_free_queue();
        ent_detail::by_chunk.erase(k);
        std::erase(ent_detail::ticking_keys, k);
    }
}
inline void save_all_entities() { unload_far_entity_chunks(true); }

// ---------- per player ----------
inline void entities_forget_player(Player& p) {  // left: drop as a watcher (client is gone, nothing to send)
    auto it = ent_detail::views.find(&p);
    if (it == ent_detail::views.end()) return;
    for (Entity* e : it->second.seen) std::erase(e->trk.watchers, &p);
    ent_detail::views.erase(it);
    ent_detail::ticking_dirty = true;
}
inline void entities_reset_view(Player& p) {  // respawn: the client dropped all entities; resend on the next tick
    entities_forget_player(p);
}
inline void update_player_view(Player& p) {
    auto& v = ent_detail::views[&p];
    if (v.cx == p.cx && v.cz == p.cz) return;
    v.cx = p.cx, v.cz = p.cz;
    ent_detail::ticking_dirty = true;
    int r = view_dist_cached();
    for (int cx = p.cx - r; cx <= p.cx + r; cx++)
        for (int cz = p.cz - r; cz <= p.cz + r; cz++) {
            ensure_entity_chunk_loaded(cx, cz);
            auto it = ent_detail::by_chunk.find(mth::chunk_key(cx, cz));
            if (it == ent_detail::by_chunk.end()) continue;
            std::vector<Entity*> list = it->second.list;
            for (Entity* e : list) update_player(*e, p);
        }
    std::vector<Entity*> seen = v.seen;
    for (Entity* e : seen) update_player(*e, p);
}

// ---------- the entity tick ----------
inline void recompute_ticking() {
    using namespace ent_detail;
    ticking_keys.clear();
    int sim = simulation_distance();
    std::erase_if(by_chunk, [](auto& kv) { return kv.second.list.empty() && !loaded.count(kv.first); });
    for (auto& [k, ce] : by_chunk) {
        int cx = int32_t(uint32_t(k)), cz = int32_t(uint32_t(uint64_t(k) >> 32));
        ce.ticking = near_any_player_chunk(cx, cz, sim);
        if (ce.ticking) ticking_keys.push_back(k);
    }
    ticking_dirty = false;
}
inline bool is_entity_ticking_chunk(int cx, int cz) {
    auto it = ent_detail::by_chunk.find(mth::chunk_key(cx, cz));
    return it != ent_detail::by_chunk.end() ? it->second.ticking : near_any_player_chunk(cx, cz, simulation_distance());
}
inline void entity_common_tick(Entity& e) {  // Entity.commonTick
    if (e.invulnerable_time > 0) e.invulnerable_time--;
    e.old = e.pos;
    e.yaw_o = e.yaw, e.pitch_o = e.pitch;
    e.tick_count++;
}
inline std::vector<void (*)()> entity_pre_tick_hooks, entity_post_tick_hooks;  // before/after the entity loop

// One server tick for all entities (ServerLevel.tick entity part + ChunkMap.tick tracker part).
inline void tick_world_entities() {
    using namespace ent_detail;
    block_cache_clear();
    for (auto& p : players) update_player_view(*p);
    if (ticking_dirty) recompute_ticking();
    for (auto f : entity_pre_tick_hooks) f();
    to_tick.clear();
    for (int64_t k : ticking_keys) {
        auto it = by_chunk.find(k);
        if (it == by_chunk.end() || it->second.list.empty()) continue;
        int cx = int32_t(uint32_t(k)), cz = int32_t(uint32_t(uint64_t(k) >> 32));
        if (!loaded_chunk(cx, cz)) continue;  // terrain not loaded yet: freeze (vanilla needs a full chunk)
        to_tick.insert(to_tick.end(), it->second.list.begin(), it->second.list.end());
    }
    for (Entity* e : to_tick) {
        if (e->removed) continue;
        e->check_despawn();
        if (e->removed) continue;
        entity_common_tick(*e);
        e->tick();
    }
    for (auto f : entity_post_tick_hooks) f();
    // tracker
    for (Entity* e : to_tick) {
        if (e->removed) continue;
        int64_t sec = section_key(*e);
        if (sec != e->trk.section) {
            e->trk.section = sec;
            update_players(*e);
        }
        send_changes(*e);
    }
    for (Entity* e : extra_sync)
        if (!e->removed) send_changes(*e);
    extra_sync.clear();
    flush_free_queue();  // entities removed this tick are freed now; pointers stay valid within the tick
    if (server_tick % 100 == 0) unload_far_entity_chunks();
    if (server_tick % 6000 == 0) save_all_entities();  // vanilla autosave interval
}
// Entities changed outside their own tick (summoned/teleported/hurt while not ticking) get a tracker update.
inline void mark_entity_sync(Entity& e) { e.needs_sync = true; ent_detail::extra_sync.push_back(&e); }
