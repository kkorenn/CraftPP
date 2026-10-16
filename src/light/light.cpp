// Light engine: a close port of vanilla 26.3 world/level/lighting.
//
// Storage (LayerLightSectionStorage): per chunk column, 26 light sections (y -5..20) for each layer. A section
// stores light while it or any of its 26 neighbours has blocks (SectionState: hasData bit + neighbour count),
// exactly as vanilla, so the same sections are present in packets. A DataLayer is a 2048-byte nibble array or
// null + a uniform value. After lighting, uniform layers are collapsed back to null (vanilla keeps the array
// once written; only the packet's "empty" mask can differ: an all-zero section is sent as empty, not zeros).
//
// One engine, one mutex: vanilla runs light on its own thread; here every call happens under mutex(), and
// block reads go straight to the chunks (World::set_block writes under mutex() too). Lock order: light, then
// Chunk::m. World chunks are held by ChunkPtr and released when only the World holds them (unloading).
//
// Persisted: light is saved with the chunk (light::save, ~5-10 KB before zlib) and restored on load like
// vanilla's "lighted" chunks. Recomputing instead would force the pipeline to regenerate every cached chunk's
// proto version through LIGHT, which defeats the chunk cache. Lighting costs ~0.25 ms/chunk (M4 Pro);
// memory ~5 KB per column (1.4 KB header + ~2 nibble arrays after compaction).
#include "light.hpp"

#include "../gen/light_data.hpp"
#include "../world.hpp"
#include "../worldgen/pipeline.hpp"

#include <chrono>
#include <climits>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace light {
namespace {

constexpr int MIN_SEC = (MIN_Y >> 4) - 1, NSEC = SECTIONS + 2;  // light sections -5..20
constexpr int SRC_MIN_Y = MIN_Y - 1;                              // ChunkSkyLightSources.minY

// ---------- per-state properties (generated from the vanilla BlockState cache) ----------
inline int emission(uint16_t s) { return light_data::PROPS[s] & 15; }
inline int dampening(uint16_t s) { return light_data::PROPS[s] >> 4 & 15; }
inline bool empty_shape(uint16_t s) { return (light_data::PROPS[s] >> 8) == 0; }  // LightEngine.isEmptyShape
inline int face(uint16_t s, int dir) { return light_data::FACES[light_data::PROPS[s] >> 8][dir]; }
inline int opacity(uint16_t s) { return std::max(1, dampening(s)); }

// Direction.values(): DOWN UP NORTH SOUTH WEST EAST; opposite = d ^ 1.
constexpr int DX[6] = {0, 0, 0, 0, -1, 1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};
enum { DOWN, UP, NORTH, SOUTH, WEST, EAST };

inline bool shape_occludes(uint16_t from, uint16_t to, int dir) {
    return light_data::FACE_OCCLUDES[face(from, dir)][face(to, dir ^ 1)];
}
// ChunkSkyLightSources.isEdgeOccluded
inline bool edge_occluded(uint16_t top, uint16_t bottom) {
    return dampening(bottom) != 0 || light_data::FACE_OCCLUDES[face(top, DOWN)][face(bottom, UP)];
}

// ---------- LightEngine.QueueEntry ----------
namespace qe {
constexpr uint32_t DIRS = 1008, FROM_EMPTY = 1024, FROM_EMISSION = 2048;
inline uint32_t with_level(uint32_t e, int l) { return (e & ~15u) | uint32_t(l & 15); }
inline uint32_t bit(int d) { return 1u << (d + 4); }
inline uint32_t decrease_skip(int level, int skip) { return with_level(DIRS & ~bit(skip), level); }
inline uint32_t decrease_all(int level) { return with_level(DIRS, level); }
inline uint32_t increase_emission(int level, bool empty) { return with_level(DIRS | FROM_EMISSION | (empty ? FROM_EMPTY : 0), level); }
inline uint32_t increase_skip(int level, bool empty, int skip) { return with_level((DIRS & ~bit(skip)) | (empty ? FROM_EMPTY : 0), level); }
inline uint32_t increase_only(int level, bool empty, int d) { return with_level(bit(d) | (empty ? FROM_EMPTY : 0), level); }
inline uint32_t sky_source(bool down, bool n, bool s, bool w, bool e) {
    return with_level((down ? bit(DOWN) : 0) | (n ? bit(NORTH) : 0) | (s ? bit(SOUTH) : 0) | (w ? bit(WEST) : 0) | (e ? bit(EAST) : 0), 15);
}
inline int level(uint32_t e) { return int(e & 15); }
inline bool dir(uint32_t e, int d) { return e & bit(d); }
}  // namespace qe
const uint32_t PULL_LIGHT_IN = qe::decrease_all(1);
const uint32_t REMOVE_TOP_SKY_SOURCE = qe::decrease_all(15), REMOVE_SKY_SOURCE = qe::decrease_skip(15, UP),
               ADD_SKY_SOURCE = qe::increase_skip(15, false, UP);

// ---------- DataLayer ----------
struct Layer {
    std::unique_ptr<uint8_t[]> data;
    uint8_t def = 0;
    int get(int i) const { return data ? data[i >> 1] >> ((i & 1) * 4) & 15 : def; }
    void set(int i, int v) {
        if (!data) {
            data.reset(new uint8_t[2048]);
            memset(data.get(), def * 0x11, 2048);
        }
        uint8_t& b = data[i >> 1];
        b = (i & 1) ? uint8_t((b & 0x0F) | v << 4) : uint8_t((b & 0xF0) | v);
    }
    void fill(int v) { data.reset(), def = uint8_t(v); }
    bool empty() const { return !data && def == 0; }
    void compact() {  // uniform array -> null (memory only; values unchanged)
        if (!data) return;
        uint8_t first = data[0];
        if ((first >> 4) != (first & 15)) return;
        for (int i = 1; i < 2048; i++)
            if (data[i] != first) return;
        fill(first & 15);
    }
    Layer copy() const {
        Layer l;
        l.def = def;
        if (data) l.data.reset(new uint8_t[2048]), memcpy(l.data.get(), data.get(), 2048);
        return l;
    }
};
inline int idx(int x, int y, int z) { return (y & 15) << 8 | (z & 15) << 4 | (x & 15); }

enum Status : uint8_t { NEW, INITIALIZED, LIT };

struct Column {
    Layer sky[NSEC], block[NSEC];
    uint8_t state[NSEC] = {};  // SectionState: bit 5 hasData, bits 0-4 neighbour count
    bool present[NSEC] = {};   // layer exists (both engines see identical section status updates)
    bool light_on = false;     // columnsWithSources
    bool has_top = false;      // SkyDataLayerStorageMap.topSections entry
    int top = 0;
    Status status = NEW;
    uint32_t affected_sky = 0, affected_block = 0;
    bool resend = false;
    uint32_t queued = 0;
    bool from_disk = false;  // lit from saved light: neighbours lit later pull it in (pull_edge)  // sections whose saved layers wait in sky/block until the section is created  // light changed under chunks already sent (a loaded chunk's edits): tell every viewer
    Chunk* chunk = nullptr;  // blocks read by the engine: the proto chunk until taken, then the World's
    ChunkPtr hold;           // keeps a World chunk alive; released when only the World holds it (unloading)
    bool adopted = false;    // chunk is (or will be) the World's; null = released, look it up again
    bool resolve = false;    // chunk is produce()'s copy: switch to the World's map entry once it is there
    int16_t sources[256];  // ChunkSkyLightSources (raw value, SRC_MIN_Y = none)
};

struct Node { int x, y, z; uint32_t data; };

struct Engine {
    World* world = nullptr;
    worldgen::Pipeline* pipeline = nullptr;  // set: light is computed by the pipeline's light steps
    std::unordered_map<int64_t, std::unique_ptr<Column>> columns;
    int lowest = INT_MAX;  // SkyDataLayerStorageMap.currentLowestY
    std::vector<std::tuple<int, int, int>> to_check;  // blockNodesToCheck (both engines)
    std::unordered_set<int64_t> to_remove;            // sections whose state went to 0 (removed after a run)
    std::unordered_set<int64_t> dirty;                // columns with affected_* bits set
    std::unordered_set<int64_t> touched;              // columns changed since the last frame invalidation
    std::vector<Node> decq[2], incq[2];  // per engine (0 block, 1 sky): FIFO, drained by run()
    bool sky = false;  // engine being run (selects the layer and queues)
    double ms = 0;
    size_t lit = 0;
    int64_t cache_key[2] = {INT64_MIN, INT64_MIN};
    Column* cache_col[2] = {};

    static int64_t key(int cx, int cz) { return int64_t(uint64_t(uint32_t(cx)) << 32 | uint32_t(cz)); }
    static int64_t sec_key(int cx, int sy, int cz) { return int64_t(uint64_t(uint32_t(cx)) << 38 | uint64_t(uint32_t(cz) & 0x3FFFFF) << 16 | uint16_t(sy)); }

    Column* col(int cx, int cz) {
        int64_t k = key(cx, cz);
        if (cache_key[0] == k) return cache_col[0];
        if (cache_key[1] == k) return cache_col[1];
        auto it = columns.find(k);
        Column* c = it == columns.end() ? nullptr : it->second.get();
        cache_key[1] = cache_key[0], cache_col[1] = cache_col[0];
        cache_key[0] = k, cache_col[0] = c;
        return c;
    }
    Column& col_create(int cx, int cz) {
        if (Column* c = col(cx, cz)) return *c;
        auto& p = columns[key(cx, cz)];
        p = std::make_unique<Column>();
        std::fill(std::begin(p->sources), std::end(p->sources), int16_t(SRC_MIN_Y));
        cache_key[0] = cache_key[1] = INT64_MIN;
        return *p;
    }

    // ---------- storage ----------
    Layer* layer(Column* c, int sy) {
        int si = sy - MIN_SEC;
        if (!c || si < 0 || si >= NSEC || !c->present[si]) return nullptr;
        return sky ? &c->sky[si] : &c->block[si];
    }
    bool storing(int cx, int sy, int cz) { return layer(col(cx, cz), sy) != nullptr; }
    int stored(int x, int y, int z) { return layer(col(x >> 4, z >> 4), y >> 4)->get(idx(x, y, z)); }
    void set_stored(int x, int y, int z, int v) {
        layer(col(x >> 4, z >> 4), y >> 4)->set(idx(x, y, z), v);
        // SectionPos.aroundAndAtBlockPos
        for (int sx = (x - 1) >> 4; sx <= (x + 1) >> 4; sx++)
            for (int sz = (z - 1) >> 4; sz <= (z + 1) >> 4; sz++)
                for (int sy = (y - 1) >> 4; sy <= (y + 1) >> 4; sy++) mark_affected(sx, sy, sz);
    }
    void mark_affected(int cx, int sy, int cz) {
        int si = sy - MIN_SEC;
        Column* c = col(cx, cz);
        if (!c || si < 0 || si >= 32) return;
        (sky ? c->affected_sky : c->affected_block) |= 1u << si;
        dirty.insert(key(cx, cz));
        touched.insert(key(cx, cz));
    }
    bool light_on(int cx, int cz) { Column* c = col(cx, cz); return c && c->light_on; }
    int top_section(Column* c) { return c && c->has_top ? c->top : lowest; }
    bool data_at_or_below(int sy) { return sy >= lowest; }
    bool above_data(int cx, int sy, int cz) {
        int t = top_section(col(cx, cz));
        return t == lowest || sy >= t;
    }

    uint16_t state(int x, int y, int z) {
        Column* c = col(x >> 4, z >> 4);
        if (c && c->adopted && (c->resolve || !c->chunk)) acquire(*c, x >> 4, z >> 4);
        Chunk* ch = c ? c->chunk : nullptr;
        if (!ch && !pipeline) {
            Column& n = col_create(x >> 4, z >> 4);
            n.adopted = true;
            ch = acquire(n, x >> 4, z >> 4);
        }
        if (!ch) return blocks::bedrock;  // LightEngine.getState: chunk not loaded
        if (y < MIN_Y || y >= MIN_Y + HEIGHT) return blocks::air;
        return ch->get(x & 15, y, z & 15);
    }
    Chunk* acquire(Column& c, int cx, int cz) {  // the World's loaded chunk (never generates)
        std::lock_guard lock(world->m);
        auto it = world->chunks.find({cx, cz});
        if (it != world->chunks.end()) c.hold = it->second, c.chunk = c.hold.get(), c.resolve = false;
        return c.resolve ? nullptr : c.chunk;
    }

    // LayerLightSectionStorage.updateSectionStatus (applied to both layers)
    void update_section_status(int cx, int sy, int cz, bool empty) {
        Column& c = col_create(cx, cz);
        int si = sy - MIN_SEC;
        uint8_t st = c.state[si], ns = empty ? uint8_t(st & ~32) : uint8_t(st | 32);
        if (st == ns) return;
        put_state(cx, sy, cz, ns);
        int inc_n = empty ? -1 : 1;
        for (int ox = -1; ox <= 1; ox++)
            for (int oy = -1; oy <= 1; oy++)
                for (int oz = -1; oz <= 1; oz++) {
                    if (!ox && !oy && !oz) continue;
                    int nsy = sy + oy, nsi = nsy - MIN_SEC;
                    if (nsi < 0 || nsi >= NSEC) continue;
                    Column& n = col_create(cx + ox, cz + oz);
                    uint8_t s = n.state[nsi];
                    put_state(cx + ox, nsy, cz + oz, uint8_t((s & ~31) | ((s & 31) + inc_n)));
                }
    }
    void put_state(int cx, int sy, int cz, uint8_t s) {
        Column& c = col_create(cx, cz);
        int si = sy - MIN_SEC;
        uint8_t old = c.state[si];
        c.state[si] = s;
        if (s != 0 && old == 0) initialize_section(c, cx, sy, cz);
        else if (s == 0 && old != 0) to_remove.insert(sec_key(cx, sy, cz));
    }
    void initialize_section(Column& c, int cx, int sy, int cz) {
        if (to_remove.erase(sec_key(cx, sy, cz))) return;
        int si = sy - MIN_SEC;
        if (c.queued >> si & 1) {  // LayerLightSectionStorage.createDataLayer: queued (saved) data first
            c.queued &= ~(1u << si);
        } else {
            c.block[si] = Layer();
            c.sky[si] = create_sky_layer(c, sy);
        }
        c.present[si] = true;
        // SkyLightSectionStorage.onNodeAdded
        if (lowest > sy) lowest = sy;
        if (top_section(&c) < sy + 1) c.has_top = true, c.top = sy + 1;
        for (int ox = -1; ox <= 1; ox++)  // markSectionAndNeighborsAsAffected
            for (int oz = -1; oz <= 1; oz++)
                for (int oy = -1; oy <= 1; oy++)
                    for (bool s : {false, true}) sky = s, mark_affected(cx + ox, sy + oy, cz + oz);
    }
    // SkyLightSectionStorage.createDataLayer
    Layer create_sky_layer(Column& c, int sy) {
        int t = top_section(&c);
        if (t != lowest && sy < t) {
            int ay = sy + 1;
            while (!c.present[ay - MIN_SEC]) ay++;
            const Layer& above = c.sky[ay - MIN_SEC];
            if (!above.data) return above.copy();
            Layer l;
            l.data.reset(new uint8_t[2048]);
            for (int i = 0; i < 16; i++) memcpy(l.data.get() + i * 128, above.data.get(), 128);
            return l;
        }
        Layer l;
        l.def = c.light_on ? 15 : 0;
        return l;
    }
    // markNewInconsistencies: drop sections whose state went to 0
    void remove_sections() {
        if (to_remove.empty()) return;
        std::vector<int64_t> list(to_remove.begin(), to_remove.end());
        to_remove.clear();
        auto unpack = [](int64_t k, int& cx, int& sy, int& cz) {
            cx = int(k >> 38), cz = int(int64_t(uint64_t(k) << 26) >> 42), sy = int16_t(k & 0xFFFF);
        };
        for (int64_t k : list) {
            int cx, sy, cz;
            unpack(k, cx, sy, cz);
            Column* c = col(cx, cz);
            int si = sy - MIN_SEC;
            c->present[si] = false;
            c->sky[si] = Layer();
            c->block[si] = Layer();
        }
        for (int64_t k : list) {  // SkyLightSectionStorage.onNodeRemoved
            int cx, sy, cz;
            unpack(k, cx, sy, cz);
            Column* c = col(cx, cz);
            if (top_section(c) != sy + 1) continue;
            int y = sy;
            sky = true;
            while (!storing(cx, y, cz) && data_at_or_below(y)) y--;
            if (storing(cx, y, cz)) c->top = y + 1;
            else c->has_top = false;
        }
    }

    // ---------- ChunkSkyLightSources ----------
    int lowest_source(int x, int z, int def) {  // getLowestSourceY of a chunk's sources, or def without one
        Column* c = col(x >> 4, z >> 4);
        if (!c || c->status == NEW) return def;
        int v = c->sources[(z & 15) * 16 + (x & 15)];
        return v == SRC_MIN_Y ? INT_MIN : v;
    }
    void fill_sources(Column& c, int cx, int cz) {
        int top_si = -1;
        for (int si = SECTIONS - 1; si >= 0 && top_si < 0; si--)
            if (!section_empty(*c.chunk, si)) top_si = si;
        for (int i = 0; i < 256; i++) {
            c.sources[i] = int16_t(SRC_MIN_Y);
            if (top_si < 0) continue;
            int x = cx * 16 + (i & 15), z = cz * 16 + (i >> 4);
            uint16_t prev = blocks::air;
            for (int y = MIN_Y + top_si * 16 + 15; y >= MIN_Y; y--) {
                uint16_t cur = c.chunk->get(x & 15, y, z & 15);
                if (edge_occluded(prev, cur)) { c.sources[i] = int16_t(y + 1); break; }
                prev = cur;
            }
        }
    }
    // ChunkSkyLightSources.update
    void update_sources(int x, int y, int z) {
        Column* c = col(x >> 4, z >> 4);
        if (!c || c->status == NEW) return;
        int i = (z & 15) * 16 + (x & 15), cur = c->sources[i];
        if (y + 1 < cur) return;
        auto edge = [&](int top_y, uint16_t top, uint16_t bottom) {
            if (edge_occluded(top, bottom)) {
                if (top_y > cur) return c->sources[i] = int16_t(top_y), true;
            } else if (top_y == cur) {
                int res = SRC_MIN_Y;  // findLowestSourceBelow(bottomPos = top_y - 1)
                uint16_t t = bottom;
                for (int by = top_y - 2; by >= SRC_MIN_Y; by--) {
                    uint16_t b = state(x, by, z);
                    if (edge_occluded(t, b)) { res = by + 1; break; }
                    t = b;
                }
                c->sources[i] = int16_t(res);
                return true;
            }
            return false;
        };
        uint16_t top = state(x, y + 1, z), mid = state(x, y, z);
        if (!edge(y + 1, top, mid)) edge(y, mid, state(x, y - 1, z));
    }
    static bool section_empty(const Chunk& ch, int si) {
        const auto& b = ch.sections[size_t(si)].blocks;
        if (!b.bits) return state_flags()[b.palette[0]] & STATE_AIR;
        for (int i = 0; i < 4096; i++)
            if (!(state_flags()[b.get(i)] & STATE_AIR)) return false;
        return true;
    }

    // ---------- LightEngine ----------
    void enqueue_dec(int x, int y, int z, uint32_t d) { decq[sky].push_back({x, y, z, d}); }
    void enqueue_inc(int x, int y, int z, uint32_t d) { incq[sky].push_back({x, y, z, d}); }

    void run() {  // LevelLightEngine.runLightUpdates: block engine, then sky engine
        std::sort(to_check.begin(), to_check.end());
        to_check.erase(std::unique(to_check.begin(), to_check.end()), to_check.end());
        for (bool s : {false, true}) {
            sky = s;
            for (auto [x, y, z] : to_check) s ? sky_check(x, y, z) : block_check(x, y, z);
            auto& dec = decq[s];
            auto& inc = incq[s];
            for (size_t i = 0; i < dec.size(); i++) {
                Node n = dec[i];
                s ? sky_dec(n) : block_dec(n);
            }
            dec.clear();
            for (size_t i = 0; i < inc.size(); i++) {
                Node n = inc[i];
                int from = stored(n.x, n.y, n.z), target = qe::level(n.data);
                if ((n.data & qe::FROM_EMISSION) && from < target) set_stored(n.x, n.y, n.z, target), from = target;
                if (from == target) s ? sky_inc(n, from) : block_inc(n, from);
            }
            inc.clear();
        }
        to_check.clear();
        remove_sections();
        for (int64_t k : touched)  // cached chunk frames carry light: drop them (lock order light, chunk)
            if (auto it = columns.find(k); it != columns.end() && it->second->adopted) {
                Column& c = *it->second;
                if ((c.resolve || !c.chunk) && !acquire(c, int(k >> 32), int(int32_t(uint32_t(k)))) && !c.chunk) continue;
                Chunk& ch = *c.chunk;
                std::lock_guard lock(ch.m);
                ch.body.reset();
                ch.light_version++;
                ch.dirty = true;  // ChunkHolder.sectionLightChanged: markUnsaved (light is saved with the chunk)
            }
        touched.clear();
    }

    // ---------- BlockLightEngine ----------
    int block_emission(int x, int z, uint16_t s) {
        int e = emission(s);
        return e > 0 && light_on(x >> 4, z >> 4) ? e : 0;
    }
    void block_check(int x, int y, int z) {
        if (!storing(x >> 4, y >> 4, z >> 4)) return;
        uint16_t s = state(x, y, z);
        int e = block_emission(x, z, s), old = stored(x, y, z);
        if (e < old) set_stored(x, y, z, 0), enqueue_dec(x, y, z, qe::decrease_all(old));
        else enqueue_dec(x, y, z, PULL_LIGHT_IN);
        if (e > 0) enqueue_inc(x, y, z, qe::increase_emission(e, empty_shape(s)));
    }
    void block_inc(const Node& n, int from) {
        bool have_from = false;
        uint16_t from_state = 0;
        for (int d = 0; d < 6; d++) {
            if (!qe::dir(n.data, d)) continue;
            int tx = n.x + DX[d], ty = n.y + DY[d], tz = n.z + DZ[d];
            if (!storing(tx >> 4, ty >> 4, tz >> 4)) continue;
            int to = stored(tx, ty, tz);
            if (from - 1 <= to) continue;
            uint16_t to_state = state(tx, ty, tz);
            int nl = from - opacity(to_state);
            if (nl <= to) continue;
            if (!have_from) from_state = (n.data & qe::FROM_EMPTY) ? uint16_t(blocks::air) : state(n.x, n.y, n.z), have_from = true;
            if (shape_occludes(from_state, to_state, d)) continue;
            set_stored(tx, ty, tz, nl);
            if (nl > 1) enqueue_inc(tx, ty, tz, qe::increase_skip(nl, empty_shape(to_state), d ^ 1));
        }
    }
    void block_dec(const Node& n) {
        int old = qe::level(n.data);
        for (int d = 0; d < 6; d++) {
            if (!qe::dir(n.data, d)) continue;
            int tx = n.x + DX[d], ty = n.y + DY[d], tz = n.z + DZ[d];
            if (!storing(tx >> 4, ty >> 4, tz >> 4)) continue;
            int to = stored(tx, ty, tz);
            if (to == 0) continue;
            if (to <= old - 1) {
                uint16_t ts = state(tx, ty, tz);
                int te = block_emission(tx, tz, ts);
                set_stored(tx, ty, tz, 0);
                if (te < to) enqueue_dec(tx, ty, tz, qe::decrease_skip(to, d ^ 1));
                if (te > 0) enqueue_inc(tx, ty, tz, qe::increase_emission(te, empty_shape(ts)));
            } else {
                enqueue_inc(tx, ty, tz, qe::increase_only(to, false, d ^ 1));
            }
        }
    }
    void block_propagate_sources(int cx, int cz) {
        Column* c = col(cx, cz);
        c->light_on = true;
        for (int si = 0; si < SECTIONS; si++) {
            const auto& blocks = c->chunk->sections[size_t(si)].blocks;
            if (!blocks.bits && !emission(blocks.palette[0])) continue;
            for (int i = 0; i < 4096; i++) {
                uint16_t s = blocks.get(i);
                if (int e = emission(s))
                    enqueue_inc(cx * 16 + (i & 15), MIN_Y + si * 16 + (i >> 8), cz * 16 + (i >> 4 & 15), qe::increase_emission(e, empty_shape(s)));
            }
        }
    }

    // ---------- SkyLightEngine ----------
    void sky_check(int x, int y, int z) {
        int lowest_src = light_on(x >> 4, z >> 4) ? lowest_source(x, z, INT_MAX) : INT_MAX;
        if (lowest_src != INT_MAX) update_sources_in_column(x, z, lowest_src);
        if (!storing(x >> 4, y >> 4, z >> 4)) return;
        if (y >= lowest_src) {
            enqueue_dec(x, y, z, REMOVE_SKY_SOURCE);
            enqueue_inc(x, y, z, ADD_SKY_SOURCE);
        } else if (int old = stored(x, y, z); old > 0) {
            set_stored(x, y, z, 0);
            enqueue_dec(x, y, z, qe::decrease_all(old));
        } else {
            enqueue_dec(x, y, z, PULL_LIGHT_IN);
        }
    }
    void update_sources_in_column(int x, int z, int lowest_src) {
        int bottom = lowest * 16;  // worldBottomY
        // removeSourcesBelow
        if (lowest_src > bottom) {
            int start = lowest_src - 1;
            for (int sy = start >> 4; data_at_or_below(sy); sy--) {
                if (!storing(x >> 4, sy, z >> 4)) continue;
                bool stop = false;
                for (int y = std::min(sy * 16 + 15, start); y >= sy * 16; y--) {
                    if (stored(x, y, z) != 15) { stop = true; break; }
                    set_stored(x, y, z, 0);
                    enqueue_dec(x, y, z, y == lowest_src - 1 ? REMOVE_TOP_SKY_SOURCE : REMOVE_SKY_SOURCE);
                }
                if (stop) break;
            }
        }
        // addSourcesAbove
        int nl = std::max(std::max(lowest_source(x - 1, z, INT_MIN), lowest_source(x + 1, z, INT_MIN)),
                          std::max(lowest_source(x, z - 1, INT_MIN), lowest_source(x, z + 1, INT_MIN)));
        int start = std::max(lowest_src, bottom);
        for (int sy = start >> 4; !above_data(x >> 4, sy, z >> 4); sy++) {
            if (!storing(x >> 4, sy, z >> 4)) continue;
            for (int y = std::max(sy * 16, start); y <= sy * 16 + 15; y++) {
                if (stored(x, y, z) == 15) return;
                set_stored(x, y, z, 15);
                if (y < nl || y == lowest_src) enqueue_inc(x, y, z, ADD_SKY_SOURCE);
            }
        }
    }
    void sky_inc(const Node& n, int from) {
        bool have_from = false;
        uint16_t from_state = 0;
        int empty_below = empty_sections_below(n.x, n.y, n.z);
        for (int d = 0; d < 6; d++) {
            if (!qe::dir(n.data, d)) continue;
            int tx = n.x + DX[d], ty = n.y + DY[d], tz = n.z + DZ[d];
            if (!storing(tx >> 4, ty >> 4, tz >> 4)) continue;
            int to = stored(tx, ty, tz);
            if (from - 1 <= to) continue;
            uint16_t to_state = state(tx, ty, tz);
            int nl = from - opacity(to_state);
            if (nl <= to) continue;
            if (!have_from) from_state = (n.data & qe::FROM_EMPTY) ? uint16_t(blocks::air) : state(n.x, n.y, n.z), have_from = true;
            if (shape_occludes(from_state, to_state, d)) continue;
            set_stored(tx, ty, tz, nl);
            if (nl > 1) enqueue_inc(tx, ty, tz, qe::increase_skip(nl, empty_shape(to_state), d ^ 1));
            from_empty_sections(tx, ty, tz, d, nl, true, empty_below);
        }
    }
    void sky_dec(const Node& n) {
        int empty_below = empty_sections_below(n.x, n.y, n.z), old = qe::level(n.data);
        for (int d = 0; d < 6; d++) {
            if (!qe::dir(n.data, d)) continue;
            int tx = n.x + DX[d], ty = n.y + DY[d], tz = n.z + DZ[d];
            if (!storing(tx >> 4, ty >> 4, tz >> 4)) continue;
            int to = stored(tx, ty, tz);
            if (to == 0) continue;
            if (to <= old - 1) {
                set_stored(tx, ty, tz, 0);
                enqueue_dec(tx, ty, tz, qe::decrease_skip(to, d ^ 1));
                from_empty_sections(tx, ty, tz, d, to, false, empty_below);
            } else {
                enqueue_inc(tx, ty, tz, qe::increase_only(to, false, d ^ 1));
            }
        }
    }
    // countEmptySectionsBelowIfAtBorder
    int empty_sections_below(int x, int y, int z) {
        if ((y & 15) != 0) return 0;
        int lx = x & 15, lz = z & 15;
        if (lx != 0 && lx != 15 && lz != 0 && lz != 15) return 0;
        int sy = y >> 4, n = 0;
        while (!storing(x >> 4, sy - n - 1, z >> 4) && data_at_or_below(sy - n - 1)) n++;
        return n;
    }
    // propagateFromEmptySections
    void from_empty_sections(int x, int y, int z, int d, int level, bool increase, int empty_below) {
        if (empty_below == 0) return;
        int lx = x & 15, lz = z & 15;
        bool crossed = (d == NORTH && lz == 15) || (d == SOUTH && lz == 0) || (d == WEST && lx == 15) || (d == EAST && lx == 0);
        if (!crossed) return;
        int sy = (y >> 4) - 1, bottom = sy - empty_below + 1;
        for (; sy >= bottom; sy--) {
            if (!storing(x >> 4, sy, z >> 4)) continue;
            for (int ly = 15; ly >= 0; ly--) {
                int by = sy * 16 + ly;
                if (increase) {
                    set_stored(x, by, z, level);
                    if (level > 1) enqueue_inc(x, by, z, qe::increase_skip(level, true, d ^ 1));
                } else {
                    set_stored(x, by, z, 0);
                    enqueue_dec(x, by, z, qe::decrease_skip(level, d ^ 1));
                }
            }
        }
    }
    // SkyLightEngine.propagateLightSources
    void sky_propagate_sources(int cx, int cz) {
        Column* c = col(cx, cz);
        c->light_on = true;
        auto src = [&](int x, int z) { return lowest_source(x, z, INT_MIN); };
        int top = top_section(c), mx = cx * 16, mz = cz * 16;
        for (int sy = top - 1; sy >= lowest; sy--) {
            Layer* l = layer(c, sy);
            if (!l) continue;
            int min_y = sy * 16, max_y = min_y + 15;
            bool below = false;
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) {
                    int ls = src(mx + x, mz + z);
                    if (ls > max_y) continue;
                    int n = src(mx + x, mz + z - 1), s = src(mx + x, mz + z + 1), w = src(mx + x - 1, mz + z), e = src(mx + x + 1, mz + z);
                    int nl = std::max(std::max(n, s), std::max(w, e));
                    for (int y = max_y; y >= std::max(min_y, ls); y--) {
                        l->set(idx(x, y, z), 15);
                        if (y == ls || y < nl)
                            enqueue_inc(mx + x, y, mz + z, qe::sky_source(y == ls, y < n, y < s, y < w, y < e));
                    }
                    if (ls < min_y) below = true;
                }
            if (!below) break;
        }
    }

    // ---------- chunk lifecycle ----------
    void initialize(int cx, int cz, Chunk* blocks = nullptr) {  // ChunkStatusTasks.initializeLight
        Column& c = col_create(cx, cz);
        if (c.status != NEW) return;
        if (blocks) c.chunk = blocks;
        else c.adopted = true, acquire(c, cx, cz);
        fill_sources(c, cx, cz);
        c.status = INITIALIZED;
        for (int si = 0; si < SECTIONS; si++)
            if (!section_empty(*c.chunk, si)) update_section_status(cx, si + (MIN_Y >> 4), cz, false);
    }
};

Engine& engine() {
    static Engine e;
    return e;
}

// ponytail: runs here (pipeline light steps, tick) read proto blocks that another thread's FEATURES step may be
// writing outside this lock (vanilla's light thread races the same way); the write reports itself through
// on_block_changed, so the light converges. Take the light lock in ProtoChunk::set if storage gets resizable.
// PULL_LIGHT_IN on the column's blocks along one side (DX/DZ direction d: NORTH SOUTH WEST EAST): their
// neighbours across that side push light in. Vanilla never needs it (it saves proto chunks with their light);
// here a chunk lit from saved light can meet a neighbour that was regenerated.
void pull_edge(Engine& e, int cx, int cz, int d) {
    for (int y = MIN_Y; y < MIN_Y + HEIGHT; y++)
        for (int i = 0; i < 16; i++) {
            int x = d == WEST ? 0 : d == EAST ? 15 : i, z = d == NORTH ? 0 : d == SOUTH ? 15 : i;
            for (int k = 0; k < 2; k++) e.decq[k].push_back({cx * 16 + x, y, cz * 16 + z, PULL_LIGHT_IN});
        }
}

void light_column(Engine& e, int cx, int cz) {  // LevelLightEngine.propagateLightSources: block, then sky
    e.sky = false, e.block_propagate_sources(cx, cz);
    e.sky = true, e.sky_propagate_sources(cx, cz);
    e.col(cx, cz)->status = LIT;
    for (int d = NORTH; d <= EAST; d++)
        if (Column* n = e.col(cx + DX[d], cz + DZ[d]); n && n->from_disk) pull_edge(e, cx, cz, d);
}

void print_stats(Engine& e, size_t just_lit) {  // CRAFTPP_LIGHT_STATS=1: every 64 chunks lit
    static const bool on = getenv("CRAFTPP_LIGHT_STATS");
    if (!on || e.lit % 64 >= just_lit) return;
    size_t layers = 0, arrays = 0;
    for (auto& [k, c] : e.columns)
        for (int si = 0; si < NSEC; si++)
            if (c->present[si]) layers += 2, arrays += !!c->sky[si].data + !!c->block[si].data;
    printf("light: %zu chunks lit, %.3f ms/chunk, %zu columns, %zu layers, %zu arrays (%.1f MB)\n", e.lit,
           e.ms / double(e.lit), e.columns.size(), layers, arrays,
           double(arrays * 2048 + e.columns.size() * sizeof(Column)) / 1048576.0);
    fflush(stdout);
}

void compact(Column& c) {
    for (auto& l : c.sky) l.compact();
    for (auto& l : c.block) l.compact();
}

using Saved = std::vector<std::tuple<int64_t, uint32_t, uint32_t>>;
Saved save_affected(Engine& e) {
    Saved out;
    for (int64_t k : e.dirty)
        if (auto it = e.columns.find(k); it != e.columns.end()) {
            out.push_back({k, it->second->affected_sky, it->second->affected_block});
            it->second->affected_sky = it->second->affected_block = 0;
        }
    e.dirty.clear();
    return out;
}

void write_masks(Writer& w, const Column* c, uint32_t sky_filter, uint32_t block_filter) {
    uint32_t m[4] = {};  // sky, block, empty sky, empty block
    for (int si = 0; c && si < NSEC; si++) {
        if (!c->present[si]) continue;
        if (sky_filter >> si & 1) m[c->sky[si].empty() ? 2 : 0] |= 1u << si;
        if (block_filter >> si & 1) m[c->block[si].empty() ? 3 : 1] |= 1u << si;
    }
    for (uint32_t v : m) {  // BitSet.toByteArray: little-endian, trailing zero bytes dropped
        int n = (std::bit_width(v) + 7) / 8;
        w.varint(n);
        for (int i = 0; i < n; i++) w.u8(uint8_t(v >> (8 * i)));
    }
    for (int k = 0; k < 2; k++) {
        w.varint(std::popcount(m[k]));
        for (int si = 0; si < NSEC; si++) {
            if (!(m[k] >> si & 1)) continue;
            const Layer& l = k == 0 ? c->sky[si] : c->block[si];
            w.varint(2048);
            if (l.data) w.bytes(l.data.get(), 2048);
            else w.b.insert(w.b.end(), 2048, uint8_t(l.def * 0x11));
        }
    }
}

}  // namespace

std::mutex& mutex() {
    static std::mutex m;
    return m;
}

void attach(World& world, worldgen::Pipeline& p) {
    engine().world = &world;
    engine().pipeline = &p;
    using worldgen::Status;
    p.set_step(Status::INITIALIZE_LIGHT, [](worldgen::Region& r) {
        std::lock_guard lock(mutex());
        engine().initialize(r.cx, r.cz, r.center().chunk.get());
    });
    p.set_step(Status::LIGHT, [](worldgen::Region& r) {  // reads the 3x3: lock it like vanilla's radius 1
        std::lock_guard lock(mutex());
        Engine& e = engine();
        if (e.col(r.cx, r.cz)->status == LIT) return;  // loaded with its saved light
        auto t0 = std::chrono::steady_clock::now();
        light_column(e, r.cx, r.cz);
        e.run();
        for (int dx = -1; dx <= 1; dx++)  // the 3x3 is what this step wrote
            for (int dz = -1; dz <= 1; dz++)
                if (Column* c = e.col(r.cx + dx, r.cz + dz)) compact(*c);
        e.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        e.lit++;
        print_stats(e, 1);
    }, 1);
    p.on_block_changed = [](worldgen::ProtoChunk& c, int x, int y, int z, uint16_t old_state, uint16_t new_state) {
        std::lock_guard lock(mutex());
        block_changed(c.cx * 16 + x, y, c.cz * 16 + z, old_state, new_state);
    };
    p.on_take = [](worldgen::ProtoChunk& c, Chunk& out) {
        std::lock_guard lock(mutex());
        if (Column* col = engine().col(c.cx, c.cz)) col->chunk = &out, col->adopted = col->resolve = true;
    };
}

// A chunk the World loaded from disk while the pipeline lit its generated version: read the World's blocks
// from now on and relight what differs, as if the saved edits had just been made.
void sync_sections(Engine& e, Column& c, Chunk& w);
void adopt_loaded(Engine& e, Column& c, Chunk& w) {
    Chunk& proto = *c.chunk;
    c.chunk = &w, c.adopted = c.resolve = true;
    sync_sections(e, c, w);
    for (int si = 0; si < SECTIONS; si++) {
        const auto &a = proto.sections[size_t(si)].blocks, &b = w.sections[size_t(si)].blocks;
        if (!a.bits && !b.bits && a.palette[0] == b.palette[0]) continue;
        for (int i = 0; i < 4096; i++) {
            uint16_t o = a.get(i), n = b.get(i);
            if (o != n && (dampening(o) != dampening(n) || emission(o) != emission(n) || !empty_shape(o) || !empty_shape(n)))
                e.to_check.push_back({w.cx * 16 + (i & 15), MIN_Y + si * 16 + (i >> 8), w.cz * 16 + (i >> 4 & 15)});
        }
    }
    e.run();
    for (int64_t k : e.dirty) e.columns[k]->resend = true;
}

// Section status + sky sources of a column from the World's blocks (they may differ from the proto's).
void sync_sections(Engine& e, Column& c, Chunk& w) {
    for (int si = 0; si < SECTIONS; si++) {
        bool has = !Engine::section_empty(w, si);
        if (has != bool(c.state[si + 1] & 32)) e.update_section_status(w.cx, si + (MIN_Y >> 4), w.cz, !has);
    }
    e.fill_sources(c, w.cx, w.cz);
}

// Light saved with a chunk (light::save): per light section a flags byte (1 present, 2 sky array, 4 block
// array), then sky and block as 2048 bytes or one uniform value.
std::vector<uint8_t> save_column(const Column& c) {
    std::vector<uint8_t> out;
    for (int si = 0; si < NSEC; si++) {
        const Layer &s = c.sky[si], &b = c.block[si];
        out.push_back(uint8_t(c.present[si] | (s.data ? 2 : 0) | (b.data ? 4 : 0)));
        if (!c.present[si]) continue;
        for (const Layer* l : {&s, &b})
            if (l->data) out.insert(out.end(), l->data.get(), l->data.get() + 2048);
            else out.push_back(l->def);
    }
    return out;
}

// SkyLightEngine / BlockLightEngine for a chunk loaded with light (ChunkStatusTasks.isLighted): stored data,
// sources on, no propagation. False if the data is malformed (the caller lights it from scratch).
bool restore(Engine& e, Column& c, Chunk& w, const std::vector<uint8_t>& d) {
    std::vector<std::pair<Layer, Layer>> layers(NSEC);
    size_t at = 0;
    std::vector<bool> has(NSEC);
    for (int si = 0; si < NSEC; si++) {
        if (at >= d.size()) return false;
        uint8_t f = d[at++];
        has[size_t(si)] = f & 1;
        if (!(f & 1)) continue;
        for (int k = 0; k < 2; k++) {
            Layer& l = k ? layers[size_t(si)].second : layers[size_t(si)].first;
            if (f & (k ? 4 : 2)) {
                if (at + 2048 > d.size()) return false;
                l.data.reset(new uint8_t[2048]);
                memcpy(l.data.get(), d.data() + at, 2048), at += 2048;
            } else {
                if (at >= d.size()) return false;
                l.def = d[at++];
            }
        }
    }
    if (c.status == NEW) e.initialize(w.cx, w.cz, &w);
    else sync_sections(e, c, w);
    c.chunk = &w, c.adopted = c.resolve = true;
    for (int si = 0; si < NSEC; si++) {
        if (!has[size_t(si)]) continue;
        c.sky[si] = std::move(layers[size_t(si)].first), c.block[si] = std::move(layers[size_t(si)].second);
        if (!c.present[si]) c.queued |= 1u << si;  // a neighbour's blocks made it present when it was saved
    }
    c.light_on = true;
    c.status = LIT;
    c.from_disk = true;
    // Lit neighbours exchange light with it across the border (they may have been regenerated meanwhile).
    for (int d = NORTH; d <= EAST; d++) {
        pull_edge(e, w.cx, w.cz, d);
        if (Column* n = e.col(w.cx + DX[d], w.cz + DZ[d]); n && n->status == LIT) pull_edge(e, w.cx + DX[d], w.cz + DZ[d], d ^ 1);
    }
    e.run();
    return true;
}

// DEBUG: lower-bound fixed-point check (light that should have arrived from a neighbour but did not).
void verify(Engine& e, int cx, int cz) {
    for (bool s : {false, true}) {
        e.sky = s;
        int shown = 0;
        for (int y = MIN_Y; y < MIN_Y + HEIGHT; y++)
            for (int z = cz * 16; z < cz * 16 + 16; z++)
                for (int x = cx * 16; x < cx * 16 + 16; x++) {
                    if (!e.storing(x >> 4, y >> 4, z >> 4)) continue;
                    int l = e.stored(x, y, z);
                    uint16_t ps = e.state(x, y, z);
                    for (int d = 0; d < 6; d++) {
                        int nx = x - DX[d], ny = y - DY[d], nz = z - DZ[d];  // neighbour n, propagating in d to p
                        if (!e.storing(nx >> 4, ny >> 4, nz >> 4)) continue;
                        int nl = e.stored(nx, ny, nz);
                        if (nl <= 1 || nl - opacity(ps) <= l) continue;
                        uint16_t ns = e.state(nx, ny, nz);
                        if (shape_occludes(ns, ps, d)) continue;
                        if (shown++ < 3) {
                            auto st = [&](int a, int b) { Column* c = e.col(a, b); return c ? int(c->status) * 10 + c->adopted : -1; };
                            printf("VERIFY %s (%d,%d,%d)=%d state %d; from (%d,%d,%d)=%d state %d dir %d; col %d n-col %d\n",
                                   s ? "sky" : "block", x, y, z, l, ps, nx, ny, nz, nl, ns, d, st(x >> 4, z >> 4), st(nx >> 4, nz >> 4));
                        }
                    }
                }
        if (shown) printf("VERIFY %s chunk %d,%d: %d violations\n", s ? "sky" : "block", cx, cz, shown);
    }
    fflush(stdout);
}

bool lit_3x3(Engine& e, int cx, int cz) {
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++)
            if (Column* c = e.col(cx + dx, cz + dz); !c || c->status != LIT) return false;
    return true;
}

// ChunkMap.prepareTickingChunk: a chunk is sent once its 3x3 is FULL, so every light source that can reach it
// (< 16 blocks) has been propagated.
void produced(World& world, Chunk& w, bool loaded) {
    Engine& e = engine();
    {
        std::lock_guard lock(mutex());
        e.world = &world;
    }
    if (loaded) {
        std::vector<uint8_t> saved = std::move(w.saved_light);
        w.saved_light.clear(), w.saved_light.shrink_to_fit();
        {
            std::lock_guard lock(mutex());
            Column& c = e.col_create(w.cx, w.cz);
            if (c.adopted) {  // reloaded after unloading: same blocks, and the engine kept its light
                c.chunk = &w, c.resolve = true;
                return;
            }
            if (c.status == LIT && c.chunk) {  // its generated version was lit as a neighbour: apply the edits
                adopt_loaded(e, c, w);
                return;
            }
            if (!saved.empty() && restore(e, c, w, saved)) return;
            if (!e.pipeline) {  // flat world, no saved light: ensure_lit lights it
                if (c.status != NEW) sync_sections(e, c, w);
                else e.initialize(w.cx, w.cz, &w);
                c.chunk = &w, c.adopted = c.resolve = true;
                return;
            }
        }
        e.pipeline->ensure(w.cx, w.cz, worldgen::Status::LIGHT);  // no saved light: light the generated version
        std::lock_guard lock(mutex());
        Column& c = *e.col(w.cx, w.cz);
        if (!c.adopted) adopt_loaded(e, c, w);
        return;
    }
    if (!e.pipeline) return;
    // ChunkMap.prepareTickingChunk: sent once its 3x3 is lit (loaded neighbours keep their saved light).
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++) e.pipeline->ensure(w.cx + dx, w.cz + dz, worldgen::Status::LIGHT);
}

std::vector<uint8_t> save(Chunk& c, uint32_t& version) {
    std::lock_guard lock(mutex());
    Column* col = engine().col(c.cx, c.cz);
    version = c.light_version;
    return col && col->status == LIT ? save_column(*col) : std::vector<uint8_t>{};
}

void ensure_lit(World& world, Chunk& center) {
    int cx = center.cx, cz = center.cz;
    if (Engine& e = engine(); e.pipeline) {
        {
            std::lock_guard lock(mutex());
            if (lit_3x3(e, cx, cz)) return;
        }
        for (int dx = -1; dx <= 1; dx++)  // normally done by produced() on the worker
            for (int dz = -1; dz <= 1; dz++) e.pipeline->ensure(cx + dx, cz + dz, worldgen::Status::LIGHT);
        return;
    }
    {
        std::lock_guard lock(mutex());
        engine().world = &world;
        if (lit_3x3(engine(), cx, cz)) return;
    }
    std::vector<ChunkPtr> keep;  // generation is slow and thread-safe: keep it outside the light lock
    for (int dx = -2; dx <= 2; dx++)
        for (int dz = -2; dz <= 2; dz++) keep.push_back(world.get_ptr(cx + dx, cz + dz));
    std::lock_guard lock(mutex());
    Engine& e = engine();
    if (lit_3x3(e, cx, cz)) return;
    auto t0 = std::chrono::steady_clock::now();
    e.run();  // queued block changes first: their affected sections still go out through take_changes
    Saved saved = save_affected(e);
    std::vector<std::pair<int, int>> todo;
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++)
            if (Column* c = e.col(cx + dx, cz + dz); !c || c->status != LIT) todo.push_back({cx + dx, cz + dz});
    for (auto [x, z] : todo)  // LIGHT needs INITIALIZE_LIGHT in a radius of 1
        for (int dx = -1; dx <= 1; dx++)
            for (int dz = -1; dz <= 1; dz++) e.initialize(x + dx, z + dz);
    for (auto [x, z] : todo) light_column(e, x, z);
    e.run();
    // Lighting never changes an already-sent chunk (its 3x3 was lit before it was sent; light travels < 16
    // blocks), so the sections it touched need no light_update.
    save_affected(e);
    for (auto& [k, sky, block] : saved) {
        Column& c = *e.columns[k];
        c.affected_sky |= sky, c.affected_block |= block;
        e.dirty.insert(k);
    }
    for (auto [x, z] : todo) compact(*e.col(x, z));
    e.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    e.lit += todo.size();
    print_stats(e, todo.size());
}

uint32_t write(Writer& w, Chunk& c) {
    std::lock_guard lock(mutex());
    Engine& e = engine();
    e.run();
    static const bool check = getenv("CRAFTPP_LIGHT_VERIFY");
    if (check) verify(e, c.cx, c.cz);
    write_masks(w, e.col(c.cx, c.cz), ~0u, ~0u);
    return c.light_version;
}

int level(bool sky, int x, int y, int z) {
    std::lock_guard lock(mutex());
    Engine& e = engine();
    Column* c = e.col(x >> 4, z >> 4);
    if (!c) return sky ? 15 : 0;
    bool was = e.sky;
    e.sky = sky;
    int v = sky ? 15 : 0;
    if (!sky) {
        if (Layer* l = e.layer(c, y >> 4)) v = l->get(idx(x, y, z));
    } else {
        for (int sy = y >> 4; sy < MIN_SEC + NSEC; sy++)  // SkyLightSectionStorage.getLightValue: first stored section up
            if (Layer* l = e.layer(c, sy)) { v = l->get(idx(x, sy == y >> 4 ? y : 0, z)); break; }
    }
    e.sky = was;
    return v;
}

void block_changed(int x, int y, int z, uint16_t old_state, uint16_t new_state) {
    Engine& e = engine();
    Column* c = e.col(x >> 4, z >> 4);
    if (!c || c->status == NEW || old_state == new_state) return;  // initialize() reads the final blocks
    bool old_air = state_flags()[old_state] & STATE_AIR, new_air = state_flags()[new_state] & STATE_AIR;
    if (old_air != new_air) {  // LevelChunk.setBlockState: section emptiness changed
        int si = (y - MIN_Y) >> 4, here = idx(x, y, z), others = 0;
        if (c->adopted && (c->resolve || !c->chunk)) e.acquire(*c, x >> 4, z >> 4);
        if (!c->chunk) return;
        const auto& blocks = c->chunk->sections[size_t(si)].blocks;
        for (int i = 0; i < 4096 && !others; i++) others = i != here && !(state_flags()[blocks.get(i)] & STATE_AIR);
        if (!others) e.update_section_status(x >> 4, y >> 4, z >> 4, new_air);
    }
    // LightEngine.hasDifferentLightProperties (useShapeForLightOcclusion ~ a face tuple: only those occlude)
    if (dampening(old_state) != dampening(new_state) || emission(old_state) != emission(new_state) ||
        !empty_shape(old_state) || !empty_shape(new_state)) {
        e.update_sources(x, y, z);
        e.to_check.push_back({x, y, z});
    }
}

std::vector<Change> take_changes() {
    std::lock_guard lock(mutex());
    Engine& e = engine();
    e.run();
    static int ticks = 0;
    if (++ticks % 100 == 0)  // let the World unload chunks only the light engine still holds
        for (auto& [k, c] : e.columns)
            if (c->hold && c->hold.use_count() <= 2) c->hold.reset(), c->chunk = nullptr;
    std::vector<Change> out;
    for (auto& [k, sky, block] : save_affected(e)) {
        Column& c = *e.columns[k];
        if (c.status == LIT) out.push_back({int(k >> 32), int(int32_t(uint32_t(k))), sky, block, c.resend});
        c.resend = false;
    }
    return out;
}

void write_update(Writer& w, const Change& ch) {
    std::lock_guard lock(mutex());
    w.varint(ch.cx).varint(ch.cz);
    write_masks(w, engine().col(ch.cx, ch.cz), ch.sky, ch.block);
}

}  // namespace light
