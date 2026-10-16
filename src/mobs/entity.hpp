#pragma once
// ============================================================================================================
// Entity core (net.minecraft.world.entity.Entity + SynchedEntityData + DamageSource + Entity.move physics).
// Everything here is tick-thread only (see game/tick.hpp). Overview of the mob framework: src/mobs/mobs.hpp.
//
//  * Entity: base class of every non-player entity (items, xp orbs, arrows, mobs) plus the per-player proxy
//    (PlayerEntity, mobs/living.hpp) so AI, combat and selectors see players as entities too.
//    Override tick() / base_tick() like vanilla; call move(delta) for collision-aware movement.
//  * SynchedData (entity metadata): sparse list of the values that differ from the client's defaults, keyed by
//    the vanilla slot id (src/gen/entity_data.hpp, e.g. edata::Sheep::WOOL). set_*() marks it dirty; the tracker
//    sends dirty values each update and every stored value on spawn.
//  * DamageSource: damage_type registry id + causing/direct entity + tag bits (DT_* from damage_type tags).
//  * Blocks: block_at() reads loaded chunks only (unloaded = air, never generates); collision boxes come from
//    the generated per-state shapes (tools/gen_mob_data.py).
// ============================================================================================================
#include "data.hpp"
#include "../game/tick.hpp"
#include "../commands/nbt.hpp"
#include "../gen/entity_data.hpp"

#include <memory>
#include <optional>
#include <random>

inline std::mt19937_64 game_rng{std::random_device{}()};
inline float rand_float() { return float(game_rng() >> 40) * 0x1.0p-24f; }  // [0, 1)
inline double rand_double() { return double(game_rng() >> 11) * 0x1.0p-53; }
inline int rand_int(int bound) { return bound <= 0 ? 0 : int(game_rng() % uint64_t(bound)); }
inline bool rand_bool() { return game_rng() & 1; }
inline int rand_between(int lo, int hi) { return lo + rand_int(hi - lo + 1); }  // Mth.randomBetweenInclusive
inline double rand_gaussian() {
    static std::normal_distribution<double> n(0.0, 1.0);
    return n(game_rng);
}
inline double rand_triangle(double mode, double dev) { return mode + dev * (rand_double() - rand_double()); }  // RandomSource.triangle

// ---------- packet helpers ----------
inline Writer& write_lpvec3(Writer& w, double x, double y, double z) {  // LpVec3.write
    auto clean = [](double v) { return std::isnan(v) ? 0.0 : std::clamp(v, -1.7179869183E10, 1.7179869183E10); };
    x = clean(x), y = clean(y), z = clean(z);
    double m = std::max({std::abs(x), std::abs(y), std::abs(z)});
    if (m < 3.051944088384301E-5) return w.u8(0);
    int64_t scale = int64_t(std::ceil(m));
    bool partial = (scale & 3) != scale;
    auto pack = [](double v) { return int64_t(std::llround((v * 0.5 + 0.5) * 32766.0)); };
    uint64_t buf = uint64_t(partial ? (scale & 3) | 4 : scale) | uint64_t(pack(x / double(scale))) << 3 |
                   uint64_t(pack(y / double(scale))) << 18 | uint64_t(pack(z / double(scale))) << 33;
    w.u8(uint8_t(buf)).u8(uint8_t(buf >> 8)).i32(int32_t(uint32_t(buf >> 16)));
    if (partial) w.varint(int32_t(scale >> 2));
    return w;
}
inline Vec3 read_lpvec3(Reader& r) {  // LpVec3.read
    int a = r.u8();
    if (a == 0) return {0, 0, 0};
    int b = r.u8();
    uint32_t c = uint32_t(r.i32());
    uint64_t buf = uint64_t(c) << 16 | uint64_t(b) << 8 | uint64_t(a);
    int64_t scale = a & 3;
    if (a & 4) scale |= int64_t(uint32_t(r.varint())) << 2;
    auto unpack = [](uint64_t v) { return std::min(double(v & 32767), 32766.0) * 2.0 / 32766.0 - 1.0; };
    return {unpack(buf >> 3) * double(scale), unpack(buf >> 18) * double(scale), unpack(buf >> 33) * double(scale)};
}

// ItemStack.OPTIONAL_STREAM_CODEC: count, item, component patch (damage + ItemStack::comp).
inline Writer& write_slot(Writer& w, const ItemStack& s) {
    if (s.empty()) return w.varint(0);
    w.varint(s.count).varint(s.item);
    static const int damage_id = component_ids["damage"];
    w.varint((s.damage > 0) + (s.comp ? int(s.comp->set.size()) : 0)).varint(s.comp ? int(s.comp->removed.size()) : 0);
    if (s.damage > 0) w.varint(damage_id).varint(s.damage);
    if (s.comp) {
        for (auto& [k, v] : s.comp->set) w.varint(k).bytes(v);
        for (int k : s.comp->removed) w.varint(k);
    }
    return w;
}

inline Writer add_entity_packet(int eid, int64_t uh, int64_t ul, int type, double x, double y, double z, double vx,
                                double vy, double vz, float yaw = 0, float pitch = 0, int data = 0, float head = NAN) {
    Writer w;
    w.varint(play_cb::add_entity).varint(eid).i64(uh).i64(ul).varint(type).f64(x).f64(y).f64(z);
    write_lpvec3(w, vx, vy, vz);
    w.u8(angle(pitch)).u8(angle(yaw)).u8(angle(std::isnan(head) ? yaw : head)).varint(data);
    return w;
}
inline Writer remove_entities_packet(std::initializer_list<int> ids) {
    Writer w;
    w.varint(play_cb::remove_entities).varint(int32_t(ids.size()));
    for (int id : ids) w.varint(id);
    return w;
}
inline Writer teleport_entity_packet(int eid, double x, double y, double z, double vx, double vy, double vz,
                                     float yaw, float pitch, bool on_ground) {
    return std::move(Writer().varint(play_cb::teleport_entity).varint(eid).f64(x).f64(y).f64(z).f64(vx).f64(vy).f64(vz)
                         .f32(yaw).f32(pitch).i32(0).boolean(on_ground));
}
inline Writer set_entity_motion_packet(int eid, double vx, double vy, double vz) {
    Writer w;
    w.varint(play_cb::set_entity_motion).varint(eid);
    write_lpvec3(w, vx, vy, vz);
    return w;
}
inline Writer entity_event_packet(int eid, int event) {
    return std::move(Writer().varint(play_cb::entity_event).i32(eid).u8(uint8_t(event)));
}
// sound packet with a registry id (sound_event registry protocol id + 1) or an inline resource location.
inline Writer sound_packet(const std::string& sound, int source, double x, double y, double z, float vol, float pitch) {
    return std::move(Writer().varint(play_cb::sound).varint(0).str(sound).boolean(false).varint(source)
                         .i32(int32_t(std::floor(x * 8))).i32(int32_t(std::floor(y * 8))).i32(int32_t(std::floor(z * 8)))
                         .f32(vol).f32(pitch).i64(int64_t(game_rng())));
}

inline int view_dist_cached() {
    static int v = view_distance();
    return v;
}
inline bool sees_chunk(const Player& p, int cx, int cz) {
    int v = view_dist_cached();
    return std::abs(cx - p.cx) <= v && std::abs(cz - p.cz) <= v;
}
// Sends to every player whose view contains (x, z).
inline void broadcast_near(double x, double z, const Writer& w, const Player* except = nullptr) {
    int cx = int(std::floor(x)) >> 4, cz = int(std::floor(z)) >> 4;
    for (auto& p : players)
        if (p.get() != except && sees_chunk(*p, cx, cz)) p->send(w);
}
// Level.playSound(null, x, y, z, ...): to players within the sound's range (16 * volume, at least 16 blocks).
enum SoundSource { SRC_MASTER, SRC_MUSIC, SRC_RECORDS, SRC_WEATHER, SRC_BLOCKS, SRC_HOSTILE, SRC_NEUTRAL, SRC_PLAYERS, SRC_AMBIENT, SRC_VOICE, SRC_UI };
inline void play_sound(const std::string& sound, int source, double x, double y, double z, float vol = 1, float pitch = 1,
                       const Player* except = nullptr) {
    double r = vol > 1 ? 16.0 * vol : 16.0;
    Writer w = sound_packet(sound, source, x, y, z, vol, pitch);
    for (auto& p : players)
        if (p.get() != except && (p->x - x) * (p->x - x) + (p->y - y) * (p->y - y) + (p->z - z) * (p->z - z) < r * r) p->send(w);
}

// ---------- blocks (tick thread) ----------
// Loaded chunks only (World::get_if_loaded): entity code never loads or generates terrain. An unloaded chunk reads
// as barrier (solid, path-blocked) like vanilla's "can't enter unloaded chunks". The per-tick cache holds ChunkPtrs,
// so a chunk can't unload (World::save_dirty on the autosave thread) while entity code is using it; it is cleared
// at the start of every entity tick.
inline constexpr int UNLOADED_BLOCK = blocks::barrier;
namespace block_cache_detail {
struct Slot { int64_t key = INT64_MIN; ChunkPtr c; };
inline std::array<Slot, 256> slots;
}
inline void block_cache_clear() { for (auto& s : block_cache_detail::slots) s = {}; }
inline Chunk* loaded_chunk(int cx, int cz) {
    int64_t key = mth::chunk_key(cx, cz);
    auto& s = block_cache_detail::slots[size_t((cx * 31 + cz) & 255)];
    if (s.key == key) return s.c.get();
    ChunkPtr c = world.get_if_loaded(cx, cz);
    if (c) s = {key, c};
    return c.get();
}
inline int block_at(int x, int y, int z) {
    if (y < MIN_Y || y >= MIN_Y + HEIGHT) return blocks::air;
    Chunk* c = loaded_chunk(x >> 4, z >> 4);
    return c ? c->get(x & 15, y, z & 15) : UNLOADED_BLOCK;
}
inline bool chunk_loaded_at(int x, int z) { return loaded_chunk(x >> 4, z >> 4) != nullptr; }
// Large shapes (fences, walls: taller than 1 block) are seen from the neighbouring cell too (BlockCollisions).
inline std::vector<uint8_t> shape_large;  // by shape index, filled lazily
inline bool shape_is_large(int state) {
    if (shape_large.empty()) {
        shape_large.resize(shape_start.size() - 1);
        for (size_t i = 0; i + 1 < shape_start.size(); i++)
            for (uint32_t k = shape_start[i]; k < shape_start[i + 1]; k++) {
                const AABB& b = shape_boxes[k];
                if (b.x0 < 0 || b.y0 < 0 || b.z0 < 0 || b.x1 > 1 || b.y1 > 1 || b.z1 > 1) shape_large[i] = 1;
            }
    }
    return shape_large[state_shape_idx[size_t(state)]];
}
// Level.getBlockCollisions: every block collision box intersecting `box`, in world coordinates.
inline void collect_block_boxes(const AABB& box, std::vector<AABB>& out) {
    int x0 = mth::floor(box.x0 - 1.0E-7) - 1, x1 = mth::floor(box.x1 + 1.0E-7) + 1;
    int y0 = mth::floor(box.y0 - 1.0E-7) - 1, y1 = mth::floor(box.y1 + 1.0E-7) + 1;
    int z0 = mth::floor(box.z0 - 1.0E-7) - 1, z1 = mth::floor(box.z1 + 1.0E-7) + 1;
    for (int x = x0; x <= x1; x++)
        for (int z = z0; z <= z1; z++) {
            Chunk* c = loaded_chunk(x >> 4, z >> 4);
            for (int y = std::max(y0, MIN_Y); y <= std::min(y1, MIN_Y + HEIGHT - 1); y++) {
                int edges = (x == x0 || x == x1) + (y == y0 || y == y1) + (z == z0 || z == z1);
                if (edges == 3) continue;
                int s = c ? c->get(x & 15, y, z & 15) : UNLOADED_BLOCK;
                if (!has_collision(s)) continue;
                if (edges > 0 && !shape_is_large(s)) continue;
                for (const AABB* b = shape_begin(s); b != shape_end(s); b++) {
                    AABB w = b->move(x, y, z);
                    if (w.intersects(box)) out.push_back(w);
                }
            }
        }
}
inline bool no_block_collision(const AABB& box) {  // Level.noBlockCollision
    static std::vector<AABB> tmp;
    tmp.clear();
    collect_block_boxes(box, tmp);
    return tmp.empty();
}
inline bool contains_any_liquid(const AABB& box) {  // Level.containsAnyLiquid
    for (int x = mth::floor(box.x0); x < int(std::ceil(box.x1)); x++)
        for (int y = mth::floor(box.y0); y < int(std::ceil(box.y1)); y++)
            for (int z = mth::floor(box.z0); z < int(std::ceil(box.z1)); z++)
                if (fluid_type(block_at(x, y, z))) return true;
    return false;
}

// VoxelShape.collide for a list of world boxes (Shapes.collide): clips movement `d` along `axis`.
inline double collide_axis(int axis, const AABB& m, const std::vector<AABB>& boxes, double d) {
    int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    for (auto& b : boxes) {
        if (std::abs(d) < 1.0E-7) return 0.0;
        if (b.max(a1) <= m.min(a1) + 1.0E-7 || b.min(a1) >= m.max(a1) - 1.0E-7) continue;
        if (b.max(a2) <= m.min(a2) + 1.0E-7 || b.min(a2) >= m.max(a2) - 1.0E-7) continue;
        if (d > 0 && b.min(axis) >= m.max(axis) - 1.0E-7) {
            double nd = b.min(axis) - m.max(axis);
            if (nd >= -1.0E-7) d = std::min(d, nd);
        } else if (d < 0 && b.max(axis) <= m.min(axis) + 1.0E-7) {
            double nd = b.max(axis) - m.min(axis);
            if (nd <= 1.0E-7) d = std::max(d, nd);
        }
    }
    return std::abs(d) < 1.0E-7 ? 0.0 : d;
}
// Entity.collideWithShapes: Y first, then the larger horizontal axis last (Direction.axisStepOrder).
inline Vec3 collide_with_shapes(const Vec3& mv, const AABB& box, const std::vector<AABB>& shapes) {
    if (shapes.empty()) return mv;
    Vec3 r{0, 0, 0};
    double* c[3] = {&r.x, &r.y, &r.z};
    const double in[3] = {mv.x, mv.y, mv.z};
    int order[3] = {1, 0, 2};
    if (std::abs(mv.x) < std::abs(mv.z)) order[1] = 2, order[2] = 0;
    for (int axis : order) {
        if (in[axis] == 0) continue;
        *c[axis] = collide_axis(axis, box.move(r), shapes, in[axis]);
    }
    return r;
}

// ---------- damage ----------
enum DamageTag : uint32_t {
    DT_BYPASSES_ARMOR = 1u << 0, DT_BYPASSES_COOLDOWN = 1u << 1, DT_BYPASSES_EFFECTS = 1u << 2,
    DT_BYPASSES_ENCHANTMENTS = 1u << 3, DT_BYPASSES_INVULNERABILITY = 1u << 4, DT_BYPASSES_RESISTANCE = 1u << 5,
    DT_BYPASSES_SHIELD = 1u << 6, DT_DAMAGES_HELMET = 1u << 7, DT_IS_DROWNING = 1u << 8, DT_IS_EXPLOSION = 1u << 9,
    DT_IS_FALL = 1u << 10, DT_IS_FIRE = 1u << 11, DT_IS_FREEZING = 1u << 12, DT_IS_LIGHTNING = 1u << 13,
    DT_IS_PLAYER_ATTACK = 1u << 14, DT_IS_PROJECTILE = 1u << 15, DT_NO_IMPACT = 1u << 16, DT_NO_KNOCKBACK = 1u << 17,
    DT_NO_ANGER = 1u << 18, DT_PANIC_CAUSES = 1u << 19, DT_PANIC_ENVIRONMENTAL_CAUSES = 1u << 20, DT_WITCH_RESISTANT_TO = 1u << 21,
    DT_ALWAYS_MOST_SIGNIFICANT_FALL = 1u << 22, DT_BURN_FROM_STEPPING = 1u << 23, DT_IS_MACE_SMASH = 1u << 24,
};
inline std::vector<std::string> damage_type_names;  // registry id -> name ("mob_attack")
inline std::vector<uint32_t> damage_type_tag_bits;
inline std::vector<uint8_t> damage_type_scaling;     // 0 never, 1 when_caused_by_living_non_player, 2 always
inline void init_damage_tags(const Json& scaling) {
    damage_type_names.resize(damage_types.size());
    for (auto& [n, d] : damage_types) damage_type_names[size_t(d.id)] = n;
    damage_type_tag_bits.assign(damage_type_names.size(), 0);
    static const std::pair<const char*, uint32_t> tags[] = {
        {"bypasses_armor", DT_BYPASSES_ARMOR}, {"bypasses_cooldown", DT_BYPASSES_COOLDOWN}, {"bypasses_effects", DT_BYPASSES_EFFECTS},
        {"bypasses_enchantments", DT_BYPASSES_ENCHANTMENTS}, {"bypasses_invulnerability", DT_BYPASSES_INVULNERABILITY},
        {"bypasses_resistance", DT_BYPASSES_RESISTANCE}, {"bypasses_shield", DT_BYPASSES_SHIELD}, {"damages_helmet", DT_DAMAGES_HELMET},
        {"is_drowning", DT_IS_DROWNING}, {"is_explosion", DT_IS_EXPLOSION}, {"is_fall", DT_IS_FALL}, {"is_fire", DT_IS_FIRE},
        {"is_freezing", DT_IS_FREEZING}, {"is_lightning", DT_IS_LIGHTNING}, {"is_player_attack", DT_IS_PLAYER_ATTACK},
        {"is_projectile", DT_IS_PROJECTILE}, {"no_impact", DT_NO_IMPACT}, {"no_knockback", DT_NO_KNOCKBACK}, {"no_anger", DT_NO_ANGER},
        {"panic_causes", DT_PANIC_CAUSES}, {"panic_environmental_causes", DT_PANIC_ENVIRONMENTAL_CAUSES},
        {"witch_resistant_to", DT_WITCH_RESISTANT_TO}, {"always_most_significant_fall", DT_ALWAYS_MOST_SIGNIFICANT_FALL},
        {"burn_from_stepping", DT_BURN_FROM_STEPPING}, {"mace_smash", DT_IS_MACE_SMASH}};
    for (auto& [name, bit] : tags)
        if (auto it = damage_tags.find(name); it != damage_tags.end())
            for (uint16_t id : it->second) damage_type_tag_bits[id] |= bit;
    for (auto& s : scaling.arr)
        damage_type_scaling.push_back(s.asString() == "always" ? 2 : s.asString() == "never" ? 0 : 1);
}

struct Entity;
// DamageSource: type (damage_type registry id), causing entity (getEntity) and direct entity (getDirectEntity).
// Pointers are valid for the current tick only (entities are freed at the end of the tick they are removed in).
struct DamageSource {
    int type = 0;
    Entity* causing = nullptr;
    Entity* direct = nullptr;
    std::optional<Vec3> source_pos;
    bool is(uint32_t tag) const { return type >= 0 && size_t(type) < damage_type_tag_bits.size() && (damage_type_tag_bits[size_t(type)] & tag); }
    const std::string& name() const { static std::string g = "generic"; return size_t(type) < damage_type_names.size() ? damage_type_names[size_t(type)] : g; }
    bool is_type(const char* n) const { return name() == n; }
    bool is_creative_player() const;   // isCreativePlayer
    std::optional<Vec3> position() const;  // getSourcePosition: explicit position, else the direct entity's
    bool scales_with_difficulty() const;
    static DamageSource of(const std::string& type_name, Entity* causing = nullptr, Entity* direct = nullptr) {
        DamageSource d;
        d.type = damage_type_id(type_name);
        d.causing = causing;
        d.direct = direct ? direct : causing;
        return d;
    }
};

// ---------- synched entity data ----------
struct DataItem {
    uint8_t id, ser;
    bool dirty;
    int64_t v;  // value, or an index into SynchedData::objs for item stacks / components / raw values
};
struct SynchedData {
    std::vector<DataItem> items;  // sorted by id; only values that were set (the rest are client defaults)
    std::vector<std::vector<uint8_t>> objs;  // pre-encoded non-scalar values
    bool dirty = false;

    DataItem* find(int id) {
        for (auto& d : items) if (d.id == id) return &d;
        return nullptr;
    }
    const DataItem* find(int id) const {
        for (auto& d : items) if (d.id == id) return &d;
        return nullptr;
    }
    void set(int id, int ser, int64_t v) {
        if (DataItem* d = find(id)) {
            if (d->v == v && d->ser == ser) return;
            d->v = v, d->ser = uint8_t(ser), d->dirty = true;
        } else {
            DataItem n{uint8_t(id), uint8_t(ser), true, v};
            items.insert(std::upper_bound(items.begin(), items.end(), n, [](auto& a, auto& b) { return a.id < b.id; }), n);
        }
        dirty = true;
    }
    int64_t get(int id, int64_t def) const { const DataItem* d = find(id); return d ? d->v : def; }
    void set_byte(int id, int v) { set(id, edser::BYTE, uint8_t(v)); }
    void set_int(int id, int v) { set(id, edser::INT, v); }
    void set_bool(int id, bool v) { set(id, edser::BOOLEAN, v); }
    void set_float(int id, float f) { uint32_t u; memcpy(&u, &f, 4); set(id, edser::FLOAT, u); }
    float get_float(int id, float def) const {
        const DataItem* d = find(id);
        if (!d) return def;
        float f; uint32_t u = uint32_t(d->v); memcpy(&f, &u, 4); return f;
    }
    // Any other serializer: the value already encoded for the wire (without index/serializer prefix).
    void set_raw(int id, int ser, const std::vector<uint8_t>& bytes) {
        DataItem* d = find(id);
        if (d && d->ser == ser && objs[size_t(d->v)] == bytes) return;
        if (d) { objs[size_t(d->v)] = bytes; d->dirty = true; dirty = true; return; }
        objs.push_back(bytes);
        set(id, ser, int64_t(objs.size() - 1));
    }
    bool is_scalar(int ser) const {
        return ser == edser::BYTE || ser == edser::INT || ser == edser::FLOAT || ser == edser::BOOLEAN || ser == edser::LONG ||
               ser == edser::POSE || ser == edser::DIRECTION || ser == edser::BLOCK_STATE || ser == edser::OPTIONAL_BLOCK_STATE ||
               ser == edser::OPTIONAL_UNSIGNED_INT || ser == edser::HUMANOID_ARM || ser == edser::DYE_COLOR ||
               (ser >= edser::CAT_VARIANT && ser <= edser::ZOMBIE_NAUTILUS_VARIANT) || ser == edser::PAINTING_VARIANT ||
               ser == edser::SNIFFER_STATE || ser == edser::ARMADILLO_STATE || ser == edser::COPPER_GOLEM_STATE ||
               ser == edser::WEATHERING_COPPER_STATE;
    }
    void write_item(Writer& w, const DataItem& d) const {
        w.u8(d.id).varint(d.ser);
        if (!is_scalar(d.ser)) { w.bytes(objs[size_t(d.v)]); return; }
        if (d.ser == edser::BYTE || d.ser == edser::BOOLEAN) w.u8(uint8_t(d.v));
        else if (d.ser == edser::FLOAT) w.i32(int32_t(uint32_t(d.v)));
        else if (d.ser == edser::LONG) { uint64_t u = uint64_t(d.v); do { uint8_t b = u & 0x7F; u >>= 7; w.u8(u ? b | 0x80 : b); } while (u); }
        else w.varint(int32_t(d.v));
    }
    // set_entity_data body for dirty values; returns false when nothing is dirty.
    bool write_dirty(Writer& w, int eid) {
        if (!dirty) return false;
        w.varint(play_cb::set_entity_data).varint(eid);
        for (auto& d : items)
            if (d.dirty) { write_item(w, d); d.dirty = false; }
        w.u8(0xFF);
        dirty = false;
        return true;
    }
    bool write_all(Writer& w, int eid) const {
        if (items.empty()) return false;
        w.varint(play_cb::set_entity_data).varint(eid);
        for (auto& d : items) write_item(w, d);
        w.u8(0xFF);
        return true;
    }
};
inline std::vector<uint8_t> encode_item_stack(const ItemStack& s) { Writer w; write_slot(w, s); return w.b; }
inline std::vector<uint8_t> encode_opt_text(const std::string* text) {
    Writer w;
    w.boolean(text != nullptr);
    if (text) text_component(w, *text);
    return w.b;
}

enum class Pose : uint8_t { STANDING, FALL_FLYING, SLEEPING, SWIMMING, SPIN_ATTACK, CROUCHING, LONG_JUMPING, DYING, CROAKING,
                            USING_TONGUE, SITTING, ROARING, SNIFFING, EMERGING, DIGGING, SLIDING, SHOOTING, INHALING };
enum class Removal : uint8_t { NONE, KILLED, DISCARDED, UNLOADED_TO_CHUNK, CHANGED_DIMENSION };
enum InteractionResult { IR_PASS, IR_SUCCESS, IR_CONSUME, IR_FAIL };

struct LivingEntity;
struct Mob;
struct Player;
inline void entity_moved_chunk(Entity& e, int ocx, int ocz);  // manager.hpp
inline void entity_queue_free(Entity* e);                      // manager.hpp

// Per-entity tracking state (ServerEntity): what each watcher was last told.
struct TrackState {
    int64_t base_x = 0, base_y = 0, base_z = 0;  // VecDeltaCodec base, encoded (x * 4096)
    Vec3 sent_vel{0, 0, 0};
    uint8_t yaw = 0, pitch = 0, head = 0;
    int tick_count = 0, teleport_delay = 0;
    bool was_on_ground = false;
    int64_t section = INT64_MIN;  // last SectionPos (packed), to update watchers when it changes
    std::vector<Player*> watchers;
};

struct Entity {
    // ---- identity ----
    int id = 0;
    int64_t uuid_hi = 0, uuid_lo = 0;
    int type = 0;  // entity_types::*
    const EntityTypeInfo& info() const { return entity_type_info[size_t(type)]; }

    // ---- position / movement (Entity fields) ----
    Vec3 pos{0, 0, 0}, old{0, 0, 0}, vel{0, 0, 0};  // position, xo/yo/zo, deltaMovement
    float yaw = 0, pitch = 0, yaw_o = 0, pitch_o = 0;
    float width = 0, height = 0, eye_height = 0;  // current EntityDimensions
    AABB bb{};
    int chunk_x = 0, chunk_z = 0;                  // chunk index the manager files us under
    bool on_ground = false, horizontal_collision = false, vertical_collision = false, vertical_collision_below = false,
         minor_horizontal_collision = false;
    bool no_physics = false, first_tick = true, needs_sync = false, sync_velocity = false, sync_position = false;
    bool was_touching_water = false, was_eye_in_water = false, in_lava_ = false, eye_in_lava = false;
    double water_height = 0, lava_height = 0;
    double fall_distance = 0;
    Vec3 stuck_speed{0, 0, 0};
    float move_dist = 0, next_step = 1;
    int tick_count = 0, invulnerable_time = 0;
    int fire_ticks = -1;  // remainingFireTicks (-fireImmuneTicks when not burning)
    bool invulnerable = false, removed = false, added = false;
    Removal removal = Removal::NONE;
    std::vector<std::string> tags;
    SynchedData data;
    TrackState trk;

    Entity() = default;
    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;
    virtual ~Entity() = default;

    // ---- type queries ----
    virtual LivingEntity* as_living() { return nullptr; }
    virtual Mob* as_mob() { return nullptr; }
    virtual Player* as_player() { return nullptr; }  // PlayerEntity proxies only
    bool is_type(int t) const { return type == t; }
    bool in_tag(const std::string& tag) const { return entity_in_tag(type, tag); }

    // ---- lifecycle ----
    // Runs once right after construction (create_entity): vanilla constructor work that needs virtual dispatch
    // (dimensions, health, controls, registerGoals). Subclasses call their parent's init() first.
    virtual void init() { set_size(info().w, info().h, info().eye); }
    virtual void tick() { base_tick(); }       // Entity.tick
    virtual void base_tick();                  // Entity.baseTick: fire, fluids, below-world
    virtual void on_added() {}                 // added to the world (after spawn packets)
    virtual void on_removed() {}               // removed (before freeing)
    virtual void check_despawn() {}            // Mob.checkDespawn
    void discard() { remove(Removal::DISCARDED); }
    virtual void remove(Removal r) {  // Entity.remove: freed (and despawned for watchers) at the end of the tick
        if (removed) return;
        removed = true, removal = r;
        if (added) entity_queue_free(this);
    }
    virtual bool is_alive() const { return !removed; }

    // ---- physics properties ----
    virtual double default_gravity() const { return 0.0; }  // getDefaultGravity
    double gravity() const { return no_gravity() ? 0.0 : default_gravity(); }
    virtual float max_up_step() const { return 0.0f; }
    virtual float block_speed_factor() const;
    virtual bool is_pushable() const { return false; }
    virtual bool is_pushed_by_fluid() const { return true; }
    virtual bool can_be_collided_with() const { return false; }
    virtual bool is_pickable() const { return false; }
    virtual bool is_attackable() const { return true; }
    virtual bool fire_immune() const { return info().fire_immune; }
    virtual bool is_spectator() const { return false; }
    virtual bool is_creative() const { return false; }
    virtual float head_yaw() const { return yaw; }
    virtual float body_yaw() const { return yaw; }
    virtual void push(double x, double y, double z) {  // Entity.push(double...)
        if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) { vel = vel + Vec3{x, y, z}; needs_sync = true; }
    }
    virtual void push_entity(Entity& other);  // Entity.push(Entity)
    virtual void check_fall_damage(double dy, bool ground, int state_below, int bx, int by, int bz);
    virtual bool cause_fall_damage(double, float, const DamageSource&) { return false; }

    // ---- combat ----
    virtual bool hurt_server(const DamageSource&, float) { return false; }
    virtual void kill() { remove(Removal::KILLED); }  // /kill (LivingEntity: generic_kill damage)
    virtual bool is_invulnerable_to(const DamageSource& s) const {
        return removed || (invulnerable && !s.is(DT_BYPASSES_INVULNERABILITY) && !s.is_creative_player()) ||
               (s.is(DT_IS_FIRE) && fire_immune());
    }
    virtual void lava_hurt();
    virtual void ignite_for_ticks(int t) { if (fire_ticks < t) fire_ticks = t; }
    bool is_on_fire() const { return !fire_immune() && (fire_ticks > 0 || shared_flag(0)); }
    void clear_fire() { fire_ticks = std::min(0, fire_ticks); }

    // ---- interaction ----
    virtual int interact(Player&, int /*hand*/) { return IR_PASS; }
    virtual void player_touch(Player&) {}  // per tick while a player touches us (items, orbs, arrows)

    // ---- persistence (Entity.saveWithoutId / load) ----
    virtual void save_extra(Nbt&) const {}    // addAdditionalSaveData
    virtual void load_extra(const Nbt&) {}    // readAdditionalSaveData
    virtual bool should_save() const { return info().serialize && !removed && !as_player_const(); }
    void save(Nbt& out) const;
    void load(const Nbt& in);
    virtual const Player* as_player_const() const { return nullptr; }

    // ---- network ----
    virtual int add_entity_data() const { return 0; }    // the int of add_entity (projectile owner, item frame facing)
    virtual void send_pairing(Player& to);              // ServerEntity.sendPairingData
    virtual void send_extra_changes() {}                // per tracker update after data (attributes, equipment)
    virtual Writer spawn_packet() const {
        return add_entity_packet(id, uuid_hi, uuid_lo, type, pos.x, pos.y, pos.z, vel.x, vel.y, vel.z, yaw, pitch,
                                 add_entity_data(), head_yaw());
    }
    void broadcast(const Writer& w) const { for (Player* p : trk.watchers) p->send(w); }  // sendToTrackingPlayers
    void broadcast_event(int ev) const { broadcast(entity_event_packet(id, ev)); }      // Level.broadcastEntityEvent
    void play_sound(const std::string& s, float vol = 1, float pitch = 1) const;          // Entity.playSound
    virtual int sound_source() const { return SRC_NEUTRAL; }

    // ---- loot / predicates (entity_properties) ----
    virtual bool loot_flag(const std::string& flag) const {
        if (flag == "is_on_fire") return is_on_fire();
        return false;
    }
    virtual std::string loot_prop(const std::string&) const { return ""; }

    // ---- shared flags / data (Entity) ----
    bool shared_flag(int bit) const { return (data.get(edata::Entity::SHARED_FLAGS, 0) >> bit) & 1; }
    void set_shared_flag(int bit, bool v) {
        int f = int(data.get(edata::Entity::SHARED_FLAGS, 0));
        data.set_byte(edata::Entity::SHARED_FLAGS, v ? f | (1 << bit) : f & ~(1 << bit));
    }
    bool no_gravity() const { return data.get(edata::Entity::NO_GRAVITY, 0); }
    void set_no_gravity(bool v) { data.set_bool(edata::Entity::NO_GRAVITY, v); }
    bool silent() const { return data.get(edata::Entity::SILENT, 0); }
    void set_silent(bool v) { data.set_bool(edata::Entity::SILENT, v); }
    Pose pose() const { return Pose(data.get(edata::Entity::POSE, 0)); }
    void set_pose(Pose p) { data.set(edata::Entity::POSE, edser::POSE, int(p)); }
    int air_supply() const { return int(data.get(edata::Entity::AIR_SUPPLY, 300)); }
    void set_air_supply(int a) { data.set_int(edata::Entity::AIR_SUPPLY, a); }
    std::string custom_name;  // plain text (CustomName as literal text)
    bool has_custom_name() const { return !custom_name.empty(); }
    void set_custom_name(const std::string& n) {
        custom_name = n;
        data.set_raw(edata::Entity::CUSTOM_NAME, edser::OPTIONAL_COMPONENT, encode_opt_text(n.empty() ? nullptr : &n));
    }
    void set_custom_name_visible(bool v) { data.set_bool(edata::Entity::CUSTOM_NAME_VISIBLE, v); }
    bool glowing_tag = false;

    // ---- geometry ----
    void set_size(float w, float h, float eye) { width = w, height = h, eye_height = eye; bb = AABB::at_feet(pos.x, pos.y, pos.z, w, h); }
    void set_pos(double x, double y, double z) {  // Entity.setPos: box + chunk index
        pos = {x, y, z};
        bb = AABB::at_feet(x, y, z, width, height);
        int cx = mth::floor(x) >> 4, cz = mth::floor(z) >> 4;
        if (added && (cx != chunk_x || cz != chunk_z)) {
            int ox = chunk_x, oz = chunk_z;
            chunk_x = cx, chunk_z = cz;
            entity_moved_chunk(*this, ox, oz);
        } else if (!added) chunk_x = cx, chunk_z = cz;
    }
    void set_pos(const Vec3& p) { set_pos(p.x, p.y, p.z); }
    void snap_to(double x, double y, double z, float yr, float xr) {  // Entity.snapTo / moveTo
        set_pos(x, y, z);
        old = pos;
        yaw = yaw_o = yr, pitch = pitch_o = xr;
    }
    void teleport_to(double x, double y, double z) { set_pos(x, y, z); old = pos; needs_sync = true; sync_position = true; }
    double eye_y() const { return pos.y + eye_height; }
    Vec3 eye_pos() const { return {pos.x, pos.y + eye_height, pos.z}; }
    int block_x() const { return mth::floor(pos.x); }
    int block_y() const { return mth::floor(pos.y); }
    int block_z() const { return mth::floor(pos.z); }
    double dist_sqr(const Entity& o) const { return pos.dist_sqr(o.pos); }
    double dist_sqr(double x, double y, double z) const { return pos.dist_sqr({x, y, z}); }
    double distance_to(const Entity& o) const { return std::sqrt(dist_sqr(o)); }
    double y_at(double frac) const { return pos.y + height * frac; }  // getY(double)
    AABB fluid_box() const { return bb.inflate(-0.001); }

    // ---- movement (Entity.move) ----
    enum MoverType { SELF, PLAYER, PISTON, SHULKER_BOX, SHULKER };
    void move(MoverType type, Vec3 delta);
    Vec3 collide(const Vec3& movement);
    virtual Vec3 maybe_back_off_from_edge(const Vec3& d, MoverType) { return d; }
    int on_pos_y(float offset) const { return mth::floor(pos.y - offset); }  // getOnPos(offset), no supporting block cache
    int block_below_affecting_movement() const { return block_at(block_x(), on_pos_y(0.500001f), block_z()); }
    void move_relative(float speed, const Vec3& input);  // Entity.moveRelative
    void apply_gravity() { double g = gravity(); if (g != 0) vel.y -= g; }
    bool is_in_water() const { return was_touching_water; }
    bool is_in_lava() const { return !first_tick && lava_height > 0; }
    bool is_in_liquid() const { return is_in_water() || is_in_lava(); }
    bool is_under_water() const { return was_eye_in_water && was_touching_water; }
    bool is_eye_in_water() const { return was_eye_in_water; }
    bool update_fluid_interaction();  // EntityFluidInteraction.update + pushing
    double fluid_jump_threshold() const { return eye_height < 0.4 ? 0.0 : 0.4; }
    bool is_in_wall() const;
    bool is_free(double dx, double dy, double dz) const {
        AABB b = bb.move(dx, dy, dz);
        return no_block_collision(b) && !contains_any_liquid(b);
    }
    bool is_in_rain() const { return false; }  // ponytail: no weather precipitation check yet (commands/system has weather)
    bool is_in_water_or_rain() const { return is_in_water() || is_in_rain(); }
    void reset_fall_distance() { fall_distance = 0; }
    float rand_pitch() const { return (rand_float() - rand_float()) * 0.2f + 1.0f; }
};

inline bool DamageSource::is_creative_player() const { return causing && causing->as_player() && causing->is_creative(); }
inline std::optional<Vec3> DamageSource::position() const {
    if (source_pos) return source_pos;
    if (direct) return direct->pos;
    return std::nullopt;
}
inline bool DamageSource::scales_with_difficulty() const {  // DamageSource.scalesWithDifficulty
    int s = size_t(type) < damage_type_scaling.size() ? damage_type_scaling[size_t(type)] : 0;
    if (s == 2) return true;
    if (s == 0) return false;
    return causing && causing->as_living() && !causing->as_player();
}

// ---------- Entity method bodies ----------
inline void Entity::play_sound(const std::string& s, float vol, float p) const {
    if (!silent()) ::play_sound(s, sound_source(), pos.x, pos.y, pos.z, vol, p);
}

inline float Entity::block_speed_factor() const {  // Entity.getBlockSpeedFactor
    int here = block_at(block_x(), block_y(), block_z());
    float f = phys_of(here).speed;
    if (block_index(here) == block_index(blocks::water)) return f;
    return f == 1.0f ? phys_of(block_below_affecting_movement()).speed : f;
}

inline Vec3 input_vector(const Vec3& input, float speed, float yrot) {  // Entity.getInputVector
    double len = input.length_sqr();
    if (len < 1.0E-7) return {0, 0, 0};
    Vec3 mv = (len > 1.0 ? input.normalize() : input) * double(speed);
    float s = mth::sin(yrot * mth::DEG), c = mth::cos(yrot * mth::DEG);
    return {mv.x * c - mv.z * s, mv.y, mv.z * c + mv.x * s};
}
inline void Entity::move_relative(float speed, const Vec3& input) { vel = vel + input_vector(input, speed, yaw); }

inline Vec3 Entity::collide(const Vec3& movement) {  // Entity.collide (block colliders; entity colliders: none yet)
    static std::vector<AABB> shapes;
    shapes.clear();
    AABB box = bb;
    Vec3 step = movement;
    if (movement.length_sqr() != 0) {
        collect_block_boxes(box.expand_towards(movement), shapes);
        step = collide_with_shapes(movement, box, shapes);
    }
    bool xc = movement.x != step.x, yc = movement.y != step.y, zc = movement.z != step.z;
    bool ground_after = yc && movement.y < 0.0;
    float up = max_up_step();
    if (up > 0.0f && (ground_after || on_ground) && (xc || zc)) {
        AABB grounded = ground_after ? box.move(0, step.y, 0) : box;
        AABB step_box = grounded.expand_towards(movement.x, up, movement.z);
        if (!ground_after) step_box = step_box.expand_towards(0, -1.0E-5f, 0);
        shapes.clear();
        collect_block_boxes(step_box, shapes);
        float skip = float(step.y);
        float cands[16];
        int n = 0;
        for (auto& s : shapes)
            for (double coord : {s.y0, s.y1}) {
                float rel = float(coord - grounded.y0);
                if (rel < 0.0f || rel == skip || rel > up) continue;
                bool dup = false;
                for (int k = 0; k < n; k++) dup |= cands[k] == rel;
                if (!dup && n < 16) cands[n++] = rel;
            }
        std::sort(cands, cands + n);
        for (int k = 0; k < n; k++) {
            Vec3 from_ground = collide_with_shapes({movement.x, double(cands[k]), movement.z}, grounded, shapes);
            if (from_ground.horizontal_sqr() > step.horizontal_sqr()) {
                double to_ground = box.y0 - grounded.y0;
                return {from_ground.x, from_ground.y - to_ground, from_ground.z};
            }
        }
    }
    return step;
}

inline void Entity::move(MoverType type, Vec3 delta) {
    if (no_physics) {
        set_pos(pos.x + delta.x, pos.y + delta.y, pos.z + delta.z);
        horizontal_collision = vertical_collision = vertical_collision_below = minor_horizontal_collision = false;
        return;
    }
    if (stuck_speed.length_sqr() > 1.0E-7) {
        if (type != PISTON) delta = delta.multiply(stuck_speed.x, stuck_speed.y, stuck_speed.z);
        stuck_speed = {0, 0, 0};
        vel = {0, 0, 0};
    }
    delta = maybe_back_off_from_edge(delta, type);
    Vec3 mv = collide(delta);
    double len = mv.length_sqr();
    if (len > 1.0E-7 || delta.length_sqr() - len < 1.0E-7) {
        // ponytail: the FALLDAMAGE_RESETTING clip for >= 1 block/tick falls (water, cobweb) is skipped
        set_pos(pos.x + mv.x, pos.y + mv.y, pos.z + mv.z);
    }
    bool xc = !mth::equal(delta.x, mv.x), zc = !mth::equal(delta.z, mv.z);
    horizontal_collision = xc || zc;
    if (std::abs(delta.y) > 0.0) {
        vertical_collision = delta.y != mv.y;
        vertical_collision_below = vertical_collision && delta.y < 0.0;
        on_ground = vertical_collision_below;
    }
    minor_horizontal_collision = false;
    int ex = block_x(), ey = on_pos_y(0.2f), ez = block_z();
    int effect_state = block_at(ex, ey, ez);
    check_fall_damage(mv.y, on_ground, effect_state, ex, ey, ez);
    if (removed) return;
    if (horizontal_collision || (std::abs(delta.y) > 0 && vertical_collision)) {  // restituteMovementAfterCollisions
        if (xc) vel.x = 0;  // bounciness 0 for everything but slime blocks / bouncy entities
        if (zc) vel.z = 0;
        if (vertical_collision) {
            // ponytail: slime block / bed bounce (getBlockBounciness) not ported; restitution 0
            vel.y = 0;
        }
    }
    float f = block_speed_factor();
    vel.x *= f, vel.z *= f;
}

inline void Entity::check_fall_damage(double dy, bool ground, int state, int, int, int) {  // Entity.checkFallDamage
    if (!is_in_water() && dy < 0.0) fall_distance -= float(dy);
    if (ground) {
        if (fall_distance > 0.0) {
            // Block.fallOn: default damages; farmland trample / hay bale / slime reduce (ponytail: generic only)
            float mod = 1.0f;
            if (block_in_tag(state, "beds")) mod = 0.5f;
            if (block_index(state) == block_index(blocks::hay_block) || block_index(state) == block_index(blocks::honey_block)) mod = 0.2f;
            if (block_index(state) == block_index(blocks::slime_block)) mod = 0.0f;
            if (mod > 0) cause_fall_damage(fall_distance, mod, DamageSource::of("fall"));
        }
        reset_fall_distance();
    }
}

inline void Entity::push_entity(Entity& e) {  // Entity.push(Entity)
    if (e.no_physics || no_physics) return;
    double xa = e.pos.x - pos.x, za = e.pos.z - pos.z;
    double dd = std::max(std::abs(xa), std::abs(za));
    if (dd < 0.01f) return;
    dd = std::sqrt(dd);
    xa /= dd, za /= dd;
    double pw = std::min(1.0 / dd, 1.0);
    xa *= pw * 0.05f, za *= pw * 0.05f;
    if (is_pushable()) push(-xa, 0.0, -za);
    if (e.is_pushable()) e.push(xa, 0.0, za);
}

inline bool Entity::is_in_wall() const {  // Entity.isInWall
    if (no_physics) return false;
    float w = width * 0.8f;
    Vec3 e = eye_pos();
    AABB eb{e.x - w / 2, e.y - 0.5E-6, e.z - w / 2, e.x + w / 2, e.y + 0.5E-6, e.z + w / 2};
    for (int x = mth::floor(eb.x0); x <= mth::floor(eb.x1); x++)
        for (int y = mth::floor(eb.y0); y <= mth::floor(eb.y1); y++)
            for (int z = mth::floor(eb.z0); z <= mth::floor(eb.z1); z++) {
                int s = block_at(x, y, z);
                if (sflag(s, SF_AIR) || !sflag(s, SF_SUFFOCATING)) continue;
                for (const AABB* b = shape_begin(s); b != shape_end(s); b++)
                    if (b->move(x, y, z).intersects(eb)) return true;
            }
    return false;
}

// FlowingFluid.getFlow at (x, y, z) holding fluid state `s`.
inline Vec3 fluid_flow(int x, int y, int z, int s) {
    int ft = fluid_type(s);
    auto affects = [&](int o) { int t = fluid_type(o); return t == 0 || t == ft; };
    double fx = 0, fz = 0;
    static const int dirs[4][3] = {{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}};  // N E S W (Plane.HORIZONTAL order)
    for (auto& d : dirs) {
        int n = block_at(x + d[0], y, z + d[2]);
        if (!affects(n)) continue;
        float nh = fluid_type(n) ? fluid_own_height(n) : 0.0f, dist = 0.0f;
        if (nh == 0.0f) {
            if (!block_in_tag(n, "blocks_fluid_flow")) {
                int below = block_at(x + d[0], y - 1, z + d[2]);
                if (affects(below)) {
                    nh = fluid_type(below) ? fluid_own_height(below) : 0.0f;
                    if (nh > 0.0f) dist = fluid_own_height(s) - (nh - 0.8888889f);
                }
            }
        } else if (nh > 0.0f) {
            dist = fluid_own_height(s) - nh;
        }
        if (dist != 0.0f) fx += d[0] * dist, fz += d[2] * dist;
    }
    Vec3 flow{fx, 0, fz};
    if (fluid_falling(s)) {
        static const uint32_t face[4] = {SF_STURDY_NORTH, SF_STURDY_EAST, SF_STURDY_SOUTH, SF_STURDY_WEST};
        for (int i = 0; i < 4; i++) {
            auto solid_face = [&](int bx, int by, int bz) {
                int b = block_at(bx, by, bz);
                if (fluid_type(b) == ft) return false;
                return !block_in_tag(b, "ice") && sflag(b, face[i]);
            };
            if (solid_face(x + dirs[i][0], y, z + dirs[i][2]) || solid_face(x + dirs[i][0], y + 1, z + dirs[i][2])) {
                flow = flow.normalize() + Vec3{0, -6.0, 0};
                break;
            }
        }
    }
    return flow.normalize();
}

// Entity.updateFluidInteraction (EntityFluidInteraction.update + applyCurrentTo).
inline bool Entity::update_fluid_interaction() {
    AABB box = fluid_box();
    int x0 = mth::floor(box.x0), y0 = mth::floor(box.y0), z0 = mth::floor(box.z0);
    int x1 = int(std::ceil(box.x1)) - 1, y1 = int(std::ceil(box.y1)) - 1, z1 = int(std::ceil(box.z1)) - 1;
    bool pushed = is_pushed_by_fluid();
    double wh = 0, lh = 0;
    bool eye_w = false, eye_l = false;
    Vec3 cur[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    int cnt[3] = {0, 0, 0};
    double ch[3] = {0, 0, 0};
    double ey = eye_y();
    int ebx = block_x(), ebz = block_z();
    bool any = false;
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++) {
                int s = block_at(x, y, z);
                int ft = fluid_type(s);
                if (!ft) continue;
                double bottom = y;
                double top = bottom + (fluid_type(block_at(x, y + 1, z)) == ft ? 1.0 : double(fluid_own_height(s)));
                if (top < box.y0) continue;
                any = true;
                double& h = ft == FLUID_WATER ? wh : lh;
                if (x == ebx && z == ebz && ey >= bottom) {
                    double cam = fluid_source(s) && sflag(block_at(x, y + 1, z), SF_STURDY_DOWN) ? 1.0 : top - bottom;
                    if (ey <= bottom + cam) (ft == FLUID_WATER ? eye_w : eye_l) = true;
                }
                h = std::max(top - bb.y0, h);  // tracker.height: from the entity's real minY
                if (pushed) {
                    Vec3 flow = fluid_flow(x, y, z, s);
                    ch[ft] = std::max(h, ch[ft]);
                    if (ch[ft] < 0.4) flow = flow * ch[ft];
                    cur[ft] = cur[ft] + flow;
                    cnt[ft]++;
                }
            }
    water_height = wh, lava_height = lh;
    was_eye_in_water = eye_w, eye_in_lava = eye_l;
    bool in_water = wh > 0, in_lava = lh > 0;
    if (in_water) reset_fall_distance();
    was_touching_water = in_water;
    in_lava_ = in_lava;
    if (pushed) {
        auto apply = [&](int ft, double scale) {
            if (!cnt[ft] || cur[ft].length_sqr() < 1.0E-5f) return;
            Vec3 imp = as_player() ? cur[ft] * (1.0 / cnt[ft]) : cur[ft].normalize();
            imp = imp * scale;
            if (std::abs(vel.x) < 0.003 && std::abs(vel.z) < 0.003 && imp.length() < 0.0045000000000000005)
                imp = imp.normalize() * 0.0045000000000000005;
            vel = vel + imp;
        };
        if (in_water) apply(FLUID_WATER, 0.014);
        if (in_lava) apply(FLUID_LAVA, 0.0023333333333333335);
    }
    return any;
}

inline void Entity::lava_hurt() {
    if (!fire_immune() && hurt_server(DamageSource::of("lava"), 4.0f))
        play_sound("minecraft:entity.generic.burn", 0.4f, 2.0f + rand_float() * 0.4f);
}

inline void Entity::base_tick() {  // Entity.baseTick
    update_fluid_interaction();
    if (fire_ticks > 0) {
        if (fire_immune()) clear_fire();
        else {
            if (fire_ticks % 20 == 0 && !is_in_lava()) hurt_server(DamageSource::of("on_fire"), 1.0f);
            fire_ticks--;
        }
    }
    if (is_in_lava()) {  // Entity.applyEffectsFromBlocks -> LiquidBlock.entityInside for lava
        lava_hurt();
        if (!fire_immune()) ignite_for_ticks(15 * 20);
        fall_distance *= 0.5;
    }
    if (is_in_water_or_rain() && fire_ticks > 0) clear_fire();
    if (pos.y < MIN_Y - 64) {  // checkBelowWorld -> onBelowWorld
        if (as_living()) hurt_server(DamageSource::of("out_of_world"), 4.0f);
        else discard();
    }
    set_shared_flag(0, fire_ticks > 0);
    first_tick = false;
}

// ---------- persistence (Entity.saveWithoutId / load) ----------
inline Nbt nbt_list_d(std::initializer_list<double> v) {
    Nbt l = Nbt::make_list();
    for (double d : v) l.list.push_back(Nbt::real(Nbt::Double, d));
    return l;
}
inline double nbt_num(const Nbt& c, const char* k, double def) {
    const Nbt* v = c.get(k);
    return v && v->is_num() ? v->as_double() : def;
}
inline void Entity::save(Nbt& o) const {
    o.set("id", Nbt::str("minecraft:" + info().id));
    o.set("Pos", nbt_list_d({pos.x, pos.y, pos.z}));
    o.set("Motion", nbt_list_d({vel.x, vel.y, vel.z}));
    Nbt rot = Nbt::make_list();
    rot.list = {Nbt::real(Nbt::Float, yaw), Nbt::real(Nbt::Float, pitch)};
    o.set("Rotation", rot);
    o.set("fall_distance", Nbt::real(Nbt::Double, fall_distance));
    o.set("Fire", Nbt::num(Nbt::Short, fire_ticks));
    o.set("Air", Nbt::num(Nbt::Short, air_supply()));
    o.set("OnGround", Nbt::num(Nbt::Byte, on_ground));
    o.set("Invulnerable", Nbt::num(Nbt::Byte, invulnerable));
    Nbt u;
    u.t = Nbt::IntArray;
    u.arr = {int64_t(int32_t(uuid_hi >> 32)), int64_t(int32_t(uuid_hi)), int64_t(int32_t(uuid_lo >> 32)), int64_t(int32_t(uuid_lo))};
    o.set("UUID", u);
    if (has_custom_name()) o.set("CustomName", Nbt::str(custom_name));
    if (data.get(edata::Entity::CUSTOM_NAME_VISIBLE, 0)) o.set("CustomNameVisible", Nbt::num(Nbt::Byte, 1));
    if (silent()) o.set("Silent", Nbt::num(Nbt::Byte, 1));
    if (no_gravity()) o.set("NoGravity", Nbt::num(Nbt::Byte, 1));
    if (glowing_tag) o.set("Glowing", Nbt::num(Nbt::Byte, 1));
    if (!tags.empty()) {
        Nbt l = Nbt::make_list();
        for (auto& t : tags) l.list.push_back(Nbt::str(t));
        o.set("Tags", l);
    }
    save_extra(o);
}
inline void Entity::load(const Nbt& in) {
    auto vec = [&](const char* k, Vec3 def) {
        const Nbt* l = in.get(k);
        if (!l || l->t != Nbt::List || l->list.size() != 3) return def;
        return Vec3{l->list[0].as_double(), l->list[1].as_double(), l->list[2].as_double()};
    };
    Vec3 p = vec("Pos", pos);
    Vec3 m = vec("Motion", vel);
    auto clampm = [](double v) { return std::abs(v) > 10.0 ? 0.0 : v; };
    vel = {clampm(m.x), clampm(m.y), clampm(m.z)};
    if (const Nbt* r = in.get("Rotation"); r && r->t == Nbt::List && r->list.size() == 2) {
        yaw = float(r->list[0].as_double()), pitch = std::clamp(float(r->list[1].as_double()), -90.0f, 90.0f) ;
        yaw = std::isfinite(yaw) ? std::fmod(yaw, 360.0f) : 0;
    }
    snap_to(std::clamp(p.x, -3.0000512E7, 3.0000512E7), std::clamp(p.y, -2.0E7, 2.0E7), std::clamp(p.z, -3.0000512E7, 3.0000512E7), yaw, pitch);
    fall_distance = nbt_num(in, "fall_distance", 0);
    fire_ticks = int(nbt_num(in, "Fire", fire_ticks));
    if (const Nbt* a = in.get("Air")) set_air_supply(int(a->as_double()));
    on_ground = nbt_num(in, "OnGround", 0) != 0;
    invulnerable = nbt_num(in, "Invulnerable", 0) != 0;
    if (const Nbt* u = in.get("UUID"); u && u->t == Nbt::IntArray && u->arr.size() == 4) {
        uuid_hi = int64_t(uint64_t(uint32_t(u->arr[0])) << 32 | uint32_t(u->arr[1]));
        uuid_lo = int64_t(uint64_t(uint32_t(u->arr[2])) << 32 | uint32_t(u->arr[3]));
    }
    if (const Nbt* n = in.get("CustomName")) {
        if (n->t == Nbt::String) set_custom_name(n->s);
        else if (n->t == Nbt::Compound) if (const Nbt* t = n->get("text"); t && t->t == Nbt::String) set_custom_name(t->s);
    }
    if (nbt_num(in, "CustomNameVisible", 0)) set_custom_name_visible(true);
    if (nbt_num(in, "Silent", 0)) set_silent(true);
    if (nbt_num(in, "NoGravity", 0)) set_no_gravity(true);
    glowing_tag = nbt_num(in, "Glowing", 0) != 0;
    if (glowing_tag) set_shared_flag(6, true);
    if (const Nbt* t = in.get("Tags"); t && t->t == Nbt::List) {
        tags.clear();
        for (auto& s : t->list) if (s.t == Nbt::String) tags.push_back(s.s);
    }
    load_extra(in);
}

inline void Entity::send_pairing(Player& to) {  // ServerEntity.sendPairingData
    to.send(spawn_packet());
    Writer w;
    if (data.write_all(w, id)) to.send(w);
}

#include "manager.hpp"
