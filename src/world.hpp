#pragma once
#include "gen/ids.hpp"
#include "net.hpp"
#include "light/light.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <condition_variable>
#include <deque>
#include <future>
#include <thread>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

#include <zlib.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// Overworld dimension: y -64..319, 24 sections of 16x16x16. Other dimensions: Chunk::min_y / sections.size().
inline constexpr int MIN_Y = -64, HEIGHT = 384, SECTIONS = HEIGHT / 16;

// Vanilla PalettedContainer: one value (bits 0), a palette of up to 2^MAX_BITS ids packed MIN_BITS..MAX_BITS
// per entry, or raw global ids (GLOBAL_BITS). Entries never span longs, so the in-memory longs ARE the
// network/disk encoding. N = 4096 (blocks) or 64 (biomes); index = (y*16 + z)*16 + x (or 4x4x4 cells).
template <int N, int MIN_BITS, int MAX_BITS, int GLOBAL_BITS>
struct Paletted {
    std::vector<uint16_t> palette{0};  // empty in global mode
    std::vector<uint64_t> data;
    uint8_t bits = 0;

    uint16_t get(int i) const {
        if (!bits) return palette[0];
        int per = 64 / bits;
        uint16_t v = uint16_t((data[size_t(i / per)] >> ((i % per) * bits)) & ((1u << bits) - 1));
        return bits > MAX_BITS ? v : palette[v];
    }
    void set(int i, uint16_t v) {
        if (!bits && palette[0] == v) return;
        uint16_t idx = v;
        if (bits <= MAX_BITS) {
            auto it = std::find(palette.begin(), palette.end(), v);
            idx = uint16_t(it - palette.begin());
            if (it == palette.end()) {
                palette.push_back(v);
                if (palette.size() > (bits ? 1u << bits : 1u)) {
                    resize(palette.size());
                    if (bits > MAX_BITS) idx = v;
                }
            }
        }
        put(i, idx);
    }
    void fill(uint16_t v) { palette.assign(1, v), data.clear(), data.shrink_to_fit(), bits = 0; }
    // Rebuilds with only the values in use (generation leaves stale palette entries behind).
    void compact() {
        if (!bits) return;
        std::array<uint16_t, N> all;
        for (int i = 0; i < N; i++) all[i] = get(i);
        std::vector<uint16_t> used;
        for (uint16_t v : all)
            if (std::find(used.begin(), used.end(), v) == used.end() && used.size() <= (1u << MAX_BITS)) used.push_back(v);
        if (used.size() == 1) return fill(used[0]);
        fill(used[0]);
        for (int i = 0; i < N; i++) set(i, all[i]);
    }
    void write(Writer& w) const {
        w.u8(bits);
        if (!bits) { w.varint(palette[0]); return; }
        if (bits <= MAX_BITS) {
            w.varint(int32_t(palette.size()));
            for (uint16_t v : palette) w.varint(v);
        }
        size_t at = w.b.size();
        w.b.resize(at + data.size() * 8);
        for (uint64_t l : data)
            for (int k = 7; k >= 0; k--) w.b[at++] = uint8_t(l >> (k * 8));
    }
    void read(Reader& r) {
        int b = r.u8();
        if (b == 0) return fill(uint16_t(r.varint()));
        if (b < MIN_BITS || (b > MAX_BITS && b != GLOBAL_BITS)) throw std::runtime_error("bad paletted bits");
        bits = uint8_t(b);
        palette.clear();
        if (bits <= MAX_BITS)
            for (int n = r.varint(); n > 0; n--) palette.push_back(uint16_t(r.varint()));
        data.assign(size_t((N + 64 / bits - 1) / (64 / bits)), 0);
        for (auto& l : data) l = uint64_t(r.i64());
    }

private:
    void put(int i, uint16_t idx) {
        int per = 64 / bits, sh = (i % per) * bits;
        uint64_t& l = data[size_t(i / per)];
        l = (l & ~(uint64_t((1u << bits) - 1) << sh)) | uint64_t(idx) << sh;
    }
    void resize(size_t entries) {  // palette already holds the new value; re-pack existing entries
        std::array<uint16_t, N> all;
        for (int i = 0; i < N; i++) all[i] = get(i);
        int nb = std::max(MIN_BITS, int(std::bit_width(entries - 1)));
        bool global = nb > MAX_BITS;
        bits = uint8_t(global ? GLOBAL_BITS : nb);
        data.assign(size_t((N + 64 / bits - 1) / (64 / bits)), 0);
        if (global) {
            palette.clear();
            for (int i = 0; i < N; i++) put(i, all[i]);
        } else {
            for (int i = 0; i < N; i++) put(i, uint16_t(std::find(palette.begin(), palette.end(), all[i]) - palette.begin()));
        }
    }
};

inline constexpr int BLOCK_GLOBAL_BITS = std::bit_width(unsigned(BLOCK_STATE_COUNT - 1));
inline constexpr int BIOME_GLOBAL_BITS = std::bit_width(unsigned(biomes::COUNT - 1));

struct Section {
    Paletted<4096, 4, 8, BLOCK_GLOBAL_BITS> blocks;  // index = (y*16 + z)*16 + x
    Paletted<64, 1, 3, BIOME_GLOBAL_BITS> biomes;    // 4x4x4 cells, same order
};

// Block entity (chest, spawner, beehive, ...): chunk-local x, z, world y, block_entity_types:: id and its
// network NBT (nameless compound incl. the 0x0a tag byte). Chunk::set drops the one at a replaced position.
struct BlockEntity {
    uint8_t x, z;
    int16_t y;
    int type;
    std::vector<uint8_t> nbt{0x0a, 0x00};
};

struct Chunk {
    int cx = 0, cz = 0;
    int min_y;                // dimension type min_y
    bool skylight = true;     // dimension type has_skylight
    bool fullbright = false;  // not lit by the light engine (nether, end): full light sent, none saved
    std::vector<Section> sections;
    std::vector<BlockEntity> block_entities;
    std::mutex m;        // guards sections + body against concurrent edit + encode
    bool dirty = false;  // modified since last save
    bool on_disk = false;
    std::shared_ptr<const std::vector<uint8_t>> body;  // cached network frame (chunk_frame), reset by set()
    uint32_t light_version = 0;  // bumped (with body.reset) by the light engine under light::mutex() + m
    std::vector<uint8_t> saved_light;  // read by load_chunk, consumed by light::produced

    explicit Chunk(int min_y = MIN_Y, int height = HEIGHT) : min_y(min_y), sections(size_t(height / 16)) {}
    int height() const { return int(sections.size()) * 16; }
    uint16_t get(int x, int y, int z) const {
        return sections[(y - min_y) >> 4].blocks.get((((y - min_y) & 15) * 16 + z) * 16 + x);
    }
    void set(int x, int y, int z, uint16_t s) {
        sections[(y - min_y) >> 4].blocks.set((((y - min_y) & 15) * 16 + z) * 16 + x, s);
        body.reset();
        if (!block_entities.empty())
            std::erase_if(block_entities, [&](const BlockEntity& e) { return e.x == x && e.y == y && e.z == z; });
    }
    // Biome cell: qx, qz 0..3, qy 0..height/4-1 (quart rows from min_y).
    void set_biome(int qx, int qy, int qz, uint16_t b) { sections[qy >> 2].biomes.set(((qy & 3) * 4 + qz) * 4 + qx, b); }
    uint16_t get_biome(int qx, int qy, int qz) const { return sections[qy >> 2].biomes.get(((qy & 3) * 4 + qz) * 4 + qx); }
};
using ChunkPtr = std::shared_ptr<Chunk>;
struct ChunkCancelled : std::runtime_error {  // World::request of a chunk nobody viewed any more
    ChunkCancelled() : std::runtime_error("chunk request cancelled") {}
};

enum : uint8_t { STATE_AIR = 1, STATE_FLUID = 2 };

// Per-state flags from the generated AIR_STATES / FLUID_STATES tables (35 KB, built once).
inline const std::array<uint8_t, BLOCK_STATE_COUNT>& state_flags() {
    static const auto flags = [] {
        std::array<uint8_t, BLOCK_STATE_COUNT> f{};
        for (int s : AIR_STATES) f[size_t(s)] |= STATE_AIR;
        for (int s : FLUID_STATES) f[size_t(s)] |= STATE_FLUID;
        return f;
    }();
    return flags;
}

// level_chunk_with_light (packet id + body + light), after `reserve` bytes left free at the front. Caller holds c.m;
// `light` is the light tail from light::write (taken before c.m: lock order is light, then chunk).
inline std::vector<uint8_t> chunk_packet(const Chunk& c, size_t reserve, const Writer& light) {
    Writer w;
    w.b.reserve(80 << 10);
    w.b.resize(reserve);
    w.varint(play_cb::level_chunk_with_light).i32(c.cx).i32(c.cz);

    // Heightmaps WORLD_SURFACE(1), MOTION_BLOCKING(4), MOTION_BLOCKING_NO_LEAVES(5): 9 bits, 7 per long.
    // ponytail: all three = highest non-air block; split once leaves/fluids/glass matter.
    std::array<uint64_t, 37> hm{};
    int top = int(c.sections.size()) - 1;  // skip all-air sections from the top
    while (top >= 0 && !c.sections[size_t(top)].blocks.bits && c.sections[size_t(top)].blocks.palette[0] == blocks::air) top--;
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int h = 0;
            for (int y = c.min_y + top * 16 + 15; y >= c.min_y; y--)
                if (c.get(x, y, z) != blocks::air) { h = y - c.min_y + 1; break; }
            int i = z * 16 + x;
            hm[size_t(i / 7)] |= uint64_t(h) << ((i % 7) * 9);
        }
    w.varint(3);
    for (int type : {1, 4, 5}) {
        w.varint(type).varint(int32_t(hm.size()));
        for (uint64_t l : hm) w.i64(int64_t(l));
    }

    // Section data, written in place behind a 3-byte padded VarInt length (valid for the vanilla reader).
    size_t len_at = w.b.size();
    w.b.resize(len_at + 3);
    std::vector<std::pair<int, int>> portals;  // (section index, block index): end portal block entities
    auto is_portal = [](uint16_t b) { return b == blocks::end_portal || b == blocks::end_gateway; };
    for (auto& s : c.sections) {
        int16_t non_air = 0, fluids = 0;
        const auto& f = state_flags();
        if (!s.blocks.bits) {
            uint8_t fl = f[s.blocks.palette[0]];
            non_air = (fl & STATE_AIR) ? 0 : 4096, fluids = (fl & STATE_FLUID) ? 4096 : 0;
            if (is_portal(s.blocks.palette[0]))
                for (int i = 0; i < 4096; i++) portals.push_back({int(&s - &c.sections[0]), i});
        } else {
            bool portal = s.blocks.bits <= 8 && std::any_of(s.blocks.palette.begin(), s.blocks.palette.end(), is_portal);
            for (int i = 0; i < 4096; i++) {
                uint16_t b = s.blocks.get(i);
                uint8_t fl = f[b];
                non_air += !(fl & STATE_AIR);
                fluids += (fl & STATE_FLUID) != 0;
                if ((portal || s.blocks.bits > 8) && is_portal(b)) portals.push_back({int(&s - &c.sections[0]), i});
            }
        }
        // The client skips all fluid checks (swimming, buoyancy) in sections whose fluid count is 0.
        w.i16(non_air).i16(fluids);
        s.blocks.write(w);
        s.biomes.write(w);
    }
    size_t n = w.b.size() - len_at - 3;
    w.b[len_at] = uint8_t(n & 0x7F) | 0x80, w.b[len_at + 1] = uint8_t((n >> 7) & 0x7F) | 0x80, w.b[len_at + 2] = uint8_t(n >> 14);
    // Block entities, plus end portals / gateways (the client needs one to render them; nothing stores them yet).
    w.varint(int32_t(c.block_entities.size() + portals.size()));
    for (auto& e : c.block_entities) {
        w.u8(uint8_t(e.x << 4 | e.z)).i16(e.y).varint(e.type);
        w.bytes(e.nbt.data(), e.nbt.size());
    }
    for (auto [si, i] : portals) {
        uint16_t state = c.sections[size_t(si)].blocks.get(i);
        w.u8(uint8_t((i & 15) << 4 | ((i >> 4) & 15))).i16(int16_t(c.min_y + si * 16 + (i >> 8)));
        w.varint(state == blocks::end_portal ? block_entity_types::end_portal : block_entity_types::end_gateway);
        w.u8(0x0A).u8(0x00);
    }
    w.bytes(light.b);
    return std::move(w.b);
}

// Light tail for chunks the engine doesn't light (fullbright): full sky light (no-skylight dimensions: full
// block light instead) in every section + 1 below/above. ponytail: nether/end need a per-dimension light engine.
inline void fullbright_light(Writer& w, const Chunk& c) {
    int nl = int(c.sections.size()) + 2;
    std::vector<uint8_t> mask;  // BitSet.toByteArray: bits 0..nl-1, little-endian bytes
    for (int i = 0; i < nl; i += 8) mask.push_back(uint8_t(nl - i >= 8 ? 0xFF : (1 << (nl - i)) - 1));
    auto write_mask = [&](bool set) {
        w.varint(set ? int32_t(mask.size()) : 0);
        if (set)
            for (uint8_t b : mask) w.u8(b);
    };
    write_mask(c.skylight);   // sky mask
    write_mask(!c.skylight);  // block mask
    w.varint(0);              // empty sky mask
    w.varint(0);              // empty block mask
    for (int light = 0; light < 2; light++) {  // sky arrays, then block arrays
        bool set = (light == 0) == c.skylight;
        w.varint(set ? nl : 0);
        for (int i = 0; set && i < nl; i++) {
            w.varint(2048);
            w.b.insert(w.b.end(), 2048, 0xFF);
        }
    }
}

// The chunk's packet as a ready frame for the current network settings (net.hpp), cached in the chunk until
// it changes. Compressed (~3 KB) frames stay cached; uncompressed ones (~75 KB) are dropped at autosave.
inline std::shared_ptr<const std::vector<uint8_t>> chunk_frame(Chunk& c) {
    Writer light;
    if (c.fullbright) fullbright_light(light, c);
    std::unique_lock lock(c.m);
    while (!c.body && !c.fullbright) {  // light first (its lock comes before c.m); retry if it changed meanwhile
        lock.unlock();
        light.b.clear();
        uint32_t v = light::write(light, c);
        lock.lock();
        if (v == c.light_version) break;
    }
    if (c.body) return c.body;
    std::vector<uint8_t> f;
    if (compression_threshold >= 0) {
        auto pk = chunk_packet(c, 0, light);
        frame_packet(f, pk.data(), pk.size(), true);
        f.shrink_to_fit();
    } else {
        f = chunk_packet(c, 3, light);  // frame length in place: no copy of the packet
        size_t n = f.size() - 3;
        f[0] = uint8_t(n & 0x7F) | 0x80, f[1] = uint8_t((n >> 7) & 0x7F) | 0x80, f[2] = uint8_t(n >> 14);
    }
    return c.body = std::make_shared<const std::vector<uint8_t>>(std::move(f));
}

// Body of clientbound level_chunk_with_light (without packet id).
inline void write_chunk(Writer& w, Chunk& c) {
    Writer light;
    if (c.fullbright) fullbright_light(light, c);
    else light::write(light, c);
    std::lock_guard lock(c.m);
    auto pk = chunk_packet(c, 0, light);
    w.bytes(pk.data() + 1, pk.size() - 1);  // packet id is one byte
}

// Worker pool for chunk load/generate/encode, one thread per core of the CPU quota (cgroup cpu.max) or core
// count, max 4. Workers run at nice +5 (Linux) so the tick thread preempts them. `worker-threads` in
// server.properties overrides (World::start).
struct ChunkPool {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    std::vector<std::thread> threads;

    static int auto_size() {
        double cpus = std::thread::hardware_concurrency();
        std::ifstream f("/sys/fs/cgroup/cpu.max");  // "150000 100000" in a --cpus=1.5 container
        std::string quota;
        double period;
        if (f >> quota >> period && quota != "max" && period > 0) cpus = std::min(cpus, std::stod(quota) / period);
        return std::clamp(int(std::ceil(cpus - 0.01)), 1, 4);
    }
    void start(int n) {
        for (int i = 0; i < n; i++)
            threads.emplace_back([this] {
#ifdef __linux__
                setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 5);
#endif
                for (;;) {
                    std::function<void()> f;
                    {
                        std::unique_lock lock(m);
                        cv.wait(lock, [&] { return !q.empty(); });
                        f = std::move(q.front());
                        q.pop_front();
                    }
                    f();
                }
            });
        for (auto& t : threads) t.detach();
    }
    void submit(std::function<void()> f) {
        {
            std::lock_guard lock(m);
            q.push_back(std::move(f));
        }
        cv.notify_one();
    }
};

inline void generate_flat(Chunk& c);

// On-disk chunk: c.<x>.<z>.bin = "CPP2", raw size (u32 LE), then zlib of the 24 sections in network form
// (blocks then biomes paletted containers). "CPP1" (uncompressed flat arrays) is still read.
// ponytail: one file per chunk; switch to Anvil region files (NBT, block names) for vanilla compatibility.
inline std::string world_dir = "world";  // server.properties level-name

// <world_dir>/level.dat: per-world settings as key=value text lines (vanilla keeps these in level.dat NBT).
// Shared by all subsystems: read the map, change your keys, write it back (other keys are preserved).
// Keys so far: level-seed, level-type.
inline std::string level_data_path() { return world_dir + "/level.dat"; }
inline std::map<std::string, std::string> read_level_data() {
    std::map<std::string, std::string> kv;
    std::ifstream f(level_data_path());
    for (std::string line; std::getline(f, line);)
        if (auto eq = line.find('='); eq != std::string::npos && line[0] != '#') kv[line.substr(0, eq)] = line.substr(eq + 1);
    return kv;
}
inline void write_level_data(const std::map<std::string, std::string>& changes) {
    auto kv = read_level_data();
    auto old = kv;
    for (auto& [k, v] : changes) kv[k] = v;
    if (kv == old && std::filesystem::exists(level_data_path())) return;  // unchanged: spare the SD card
    std::filesystem::create_directories(world_dir);
    std::string path = level_data_path();
    {
        std::ofstream f(path + ".tmp");
        for (auto& [k, v] : kv) f << k << '=' << v << '\n';
    }
    std::filesystem::rename(path + ".tmp", path);
}

inline std::string chunk_path(const std::string& dir, int cx, int cz) {
    return dir + "/chunks/c." + std::to_string(cx) + "." + std::to_string(cz) + ".bin";
}
inline constexpr uint32_t CHUNK_MAGIC_V1 = 0x43505031, CHUNK_MAGIC = 0x43505032;  // "CPP1", "CPP2"

// Optional chunk-file sections owned by other subsystems (structures: starts + references), after block entities.
inline std::function<void(const Chunk&, Writer&)> chunk_save_extra;
inline std::function<void(Chunk&, Reader&)> chunk_load_extra;

inline bool load_chunk(const std::string& dir, Chunk& c) {
    std::ifstream f(chunk_path(dir, c.cx, c.cz), std::ios::binary);
    uint32_t magic = 0, raw = 0;
    if (!f.read(reinterpret_cast<char*>(&magic), 4)) return false;
    if (magic == CHUNK_MAGIC_V1) {
        std::vector<uint16_t> b(4096 + 64);
        for (auto& s : c.sections) {
            if (!f.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size() * 2))) return false;
            for (int i = 0; i < 4096; i++) s.blocks.set(i, b[size_t(i)]);
            for (int i = 0; i < 64; i++) s.biomes.set(i, b[size_t(4096 + i)]);
            s.blocks.compact(), s.biomes.compact();
        }
        return c.on_disk = true;
    }
    if (magic != CHUNK_MAGIC || !f.read(reinterpret_cast<char*>(&raw), 4) || raw > (4u << 20)) return false;
    std::vector<uint8_t> z(std::istreambuf_iterator<char>(f), {}), out(raw);
    uLongf n = raw;
    if (uncompress(out.data(), &n, z.data(), uLong(z.size())) != Z_OK || n != raw) return false;
    Reader r(out);
    for (auto& s : c.sections) s.blocks.read(r), s.biomes.read(r);
    if (r.i < out.size())  // block entities: count, then x z y type nbt-length nbt
        for (int n = r.varint(); n > 0; n--) {
            BlockEntity e;
            e.x = r.u8(), e.z = r.u8(), e.y = int16_t(r.varint()), e.type = r.varint();
            int len = r.varint();
            e.nbt.assign(out.begin() + std::ptrdiff_t(r.i), out.begin() + std::ptrdiff_t(r.i + size_t(len)));
            r.i += size_t(len);
            c.block_entities.push_back(std::move(e));
        }
    if (r.i < out.size()) {  // light (light::save)
        size_t len = size_t(r.varint());
        if (r.i + len <= out.size()) c.saved_light.assign(out.begin() + std::ptrdiff_t(r.i), out.begin() + std::ptrdiff_t(r.i + len));
        r.i += len;
    }
    if (r.i < out.size() && chunk_load_extra) chunk_load_extra(c, r);  // structures (starts + references)
    return c.on_disk = true;
}

inline void save_chunk(const std::string& dir, Chunk& c) {
    std::filesystem::create_directories(dir + "/chunks");
    std::string path = chunk_path(dir, c.cx, c.cz), tmp = path + ".tmp";
    Writer w;
    uint32_t light_version;
    std::vector<uint8_t> light;
    if (!c.fullbright) light = light::save(c, light_version);  // before c.m: lock order is light, then chunk
    else light_version = c.light_version;
    {
        std::lock_guard lock(c.m);
        for (auto& s : c.sections) s.blocks.write(w), s.biomes.write(w);
        w.varint(int32_t(c.block_entities.size()));
        for (auto& e : c.block_entities)
            w.u8(e.x).u8(e.z).varint(e.y).varint(e.type).varint(int32_t(e.nbt.size())).bytes(e.nbt);
        w.varint(int32_t(light.size())).bytes(light);
        if (chunk_save_extra) chunk_save_extra(c, w);
        c.dirty = c.light_version != light_version;  // light changed since it was captured: save again
    }
    std::vector<uint8_t> z(compressBound(uLong(w.b.size())));
    uLongf n = uLongf(z.size());
    compress2(z.data(), &n, w.b.data(), uLong(w.b.size()), 1);  // level 1: ~3x faster than 6, barely bigger
    {
        std::ofstream f(tmp, std::ios::binary);
        uint32_t raw = uint32_t(w.b.size());
        f.write(reinterpret_cast<const char*>(&CHUNK_MAGIC), 4).write(reinterpret_cast<const char*>(&raw), 4);
        f.write(reinterpret_cast<const char*>(z.data()), std::streamsize(n));
    }
    std::filesystem::rename(tmp, path);  // atomic replace: a crash never leaves a half-written chunk
    c.on_disk = true;
}

inline ChunkPool chunk_pool;  // shared by every World (dimension)
inline bool is_nether_portal(uint16_t s) { return s == blocks::nether_portal || s == blocks::nether_portal + 1; }

// One dimension (vanilla ServerLevel): layout, save folder, chunk cache.
// Chunk cache with in-flight futures: every chunk is loaded/generated exactly once, on the worker pool
// (request) or inline (get_ptr) by whoever asks first; later askers share the future. Chunks no player views
// are saved (if dirty or never stored) and dropped by save_dirty(), but only when nothing else holds them.
struct World {
    using Key = std::pair<int, int>;
    std::mutex m;
    std::map<Key, ChunkPtr> chunks;
    std::map<Key, std::shared_future<ChunkPtr>> inflight;
    std::map<Key, int> viewers;  // players whose view square contains the chunk (ViewTicket)
    ChunkPool& pool = chunk_pool;
    bool cache_generated = true;  // write generated chunks to disk on unload (docs/performance.md)

    std::function<void(Chunk&)> generator = generate_flat;  // set before serving; called without the map lock
    std::string id = "minecraft:overworld";  // level key
    int type = 0;                            // dimension_type registry id (dimension_types::)
    int min_y = MIN_Y, height = HEIGHT, sea_level = 63, logical_height = HEIGHT;
    double coordinate_scale = 1;
    bool skylight = true, ceiling = false;
    bool fullbright = false;                 // no light engine for this level (Chunk::fullbright)
    std::string dir = "world";               // save folder (chunks/, portals.txt)
    // Nether portal blocks (vanilla PoiManager NETHER_PORTAL records), kept by set_block (under m), saved with chunks.
    std::set<std::tuple<int, int, int>> portals;
    bool portals_dirty = false;

    void start(int threads) { pool.start(threads > 0 ? threads : ChunkPool::auto_size()); }  // once, for all Worlds
    int max_y() const { return min_y + height; }  // exclusive
    void open(std::string level_id, int dim_type, int miny, int h, std::string folder) {
        id = std::move(level_id), type = dim_type, min_y = miny, height = h, dir = std::move(folder);
        std::ifstream f(dir + "/portals.txt");
        for (int x, y, z; f >> x >> y >> z;) portals.insert({x, y, z});
    }
    // Block if the chunk is loaded, else air (vanilla neighbour updates never load chunks).
    uint16_t peek_block(int x, int y, int z) {
        if (y < min_y || y >= max_y()) return blocks::air;
        ChunkPtr c;
        {
            std::lock_guard lock(m);
            auto it = chunks.find({x >> 4, z >> 4});
            if (it == chunks.end()) return blocks::air;
            c = it->second;
        }
        std::lock_guard lock(c->m);
        return c->get(x & 15, y, z & 15);
    }

    // Async: the chunk, loaded/generated/encoded on the pool. Ready immediately if loaded.
    std::shared_future<ChunkPtr> request(int cx, int cz) {
        std::shared_ptr<std::promise<ChunkPtr>> job;
        auto f = find_or_claim(cx, cz, job);
        if (job)
            pool.submit([this, cx, cz, job] {
                if (cancel_unviewed(cx, cz, *job)) return;  // player moved on before a worker got to it
                produce(cx, cz, *job, true);
            });
        return f;
    }
    // Sync: loads/generates on the calling thread unless another thread already is.
    ChunkPtr get_ptr(int cx, int cz) {
        {
            std::lock_guard lock(m);  // fast path (every get_block/set_block): no promise/future allocation
            if (auto it = chunks.find({cx, cz}); it != chunks.end()) return it->second;
        }
        std::shared_ptr<std::promise<ChunkPtr>> job;
        auto f = find_or_claim(cx, cz, job);
        if (job) produce(cx, cz, *job, false);
        return f.get();
    }
    // Reference stays valid while a player views the chunk (or the caller holds a ChunkPtr): unloading only
    // happens in save_dirty(). Prefer get_ptr() off the tick thread.
    Chunk& get(int cx, int cz) { return *get_ptr(cx, cz); }

    uint16_t get_block(int x, int y, int z) {
        if (y < min_y || y >= max_y()) return blocks::air;
        ChunkPtr c = get_ptr(x >> 4, z >> 4);
        std::lock_guard lock(c->m);
        return c->get(x & 15, y, z & 15);
    }

    bool set_block(int x, int y, int z, uint16_t state) {
        if (y < min_y || y >= max_y()) return false;
        ChunkPtr c = get_ptr(x >> 4, z >> 4);
        uint16_t old;
        {
            std::unique_lock light_lock(light::mutex(), std::defer_lock);  // the light engine reads chunks under it
            if (!fullbright) light_lock.lock();
            {
                std::lock_guard lock(c->m);
                old = c->get(x & 15, y, z & 15);
                c->set(x & 15, y, z & 15, state);
                c->dirty = true;
            }
            if (!fullbright) light::block_changed(x, y, z, old, state);
        }
        if (is_nether_portal(old) != is_nether_portal(state)) {
            std::lock_guard pl(m);
            if (is_nether_portal(state)) portals.insert({x, y, z});
            else portals.erase({x, y, z});
            portals_dirty = true;
        }
        return true;
    }

    // Saves modified chunks, then unloads (saving first if needed) chunks nobody views or holds.
    std::function<void()> on_saved;  // after the dirty chunks are written (structure piece state)
    int save_dirty() {
        std::vector<ChunkPtr> dirty, idle;
        {
            std::lock_guard lock(m);
            for (auto& [k, c] : chunks) {
                if (compression_threshold < 0) {
                    std::lock_guard cl(c->m);  // uncompressed frames are ~75 KB: only for the sends right after generation
                    c->body.reset();
                }
                if (c->dirty) dirty.push_back(c);
                else if (!viewed(k) && c.use_count() == 1) idle.push_back(c);
            }
        }
        int saved = int(dirty.size());
        for (auto& c : dirty) save_chunk(dir, *c);
        if (on_saved) on_saved();
        for (auto& c : idle)
            if (cache_generated && !c->on_disk) save_chunk(dir, *c);
        std::lock_guard lock(m);
        if (portals_dirty) {  // ponytail: whole set rewritten; fine for hundreds of portal blocks
            portals_dirty = false;
            std::filesystem::create_directories(dir);
            {
                std::ofstream f(dir + "/portals.txt.tmp");
                for (auto& [x, y, z] : portals) f << x << ' ' << y << ' ' << z << '\n';
            }
            std::filesystem::rename(dir + "/portals.txt.tmp", dir + "/portals.txt");
        }
        for (auto& c : dirty) idle.push_back(std::move(c));
        for (auto& c : idle) {
            Key k{c->cx, c->cz};
            auto it = chunks.find(k);  // use_count 2 = map + `idle`: nobody else picked it up meanwhile
            if (it != chunks.end() && !viewed(k) && it->second.use_count() == 2 && !c->dirty && (c->on_disk || !cache_generated))
                chunks.erase(it);
        }
#ifdef __GLIBC__
        malloc_trim(0);  // hand freed chunk memory (and the join burst buffers) back to the OS
#endif
        return saved;
    }
    size_t loaded() {
        std::lock_guard lock(m);
        return chunks.size();
    }

    // View square bookkeeping (ViewTicket): +1/-1 for every chunk of the square.
    void view(int cx, int cz, int r, int delta) {
        std::lock_guard lock(m);
        for (int x = cx - r; x <= cx + r; x++)
            for (int z = cz - r; z <= cz + r; z++)
                if ((viewers[{x, z}] += delta) <= 0) viewers.erase({x, z});
    }

private:
    bool viewed(const Key& k) const { return viewers.count(k) != 0; }
    bool cancel_unviewed(int cx, int cz, std::promise<ChunkPtr>& job) {
        {
            std::lock_guard lock(m);
            if (viewed({cx, cz})) return false;
            inflight.erase({cx, cz});
        }
        job.set_exception(std::make_exception_ptr(ChunkCancelled()));
        return true;
    }

    std::shared_future<ChunkPtr> find_or_claim(int cx, int cz, std::shared_ptr<std::promise<ChunkPtr>>& job) {
        std::lock_guard lock(m);
        if (auto it = chunks.find({cx, cz}); it != chunks.end()) {
            std::promise<ChunkPtr> p;
            p.set_value(it->second);
            return p.get_future().share();
        }
        if (auto it = inflight.find({cx, cz}); it != inflight.end()) return it->second;
        job = std::make_shared<std::promise<ChunkPtr>>();
        return inflight[{cx, cz}] = job->get_future().share();
    }
    void produce(int cx, int cz, std::promise<ChunkPtr>& job, bool encode);
};

// Keeps a player's view square counted in World::viewers; released when the player is destroyed.
struct ViewTicket {
    World* w = nullptr;
    int cx = 0, cz = 0, r = -1;
    void move(World& world, int ncx, int ncz, int nr) {
        if (w) w->view(cx, cz, r, -1);
        w = &world, cx = ncx, cz = ncz, r = nr;
        w->view(cx, cz, r, +1);
    }
    ~ViewTicket() { if (w) w->view(cx, cz, r, -1); }
};

// ---------- dimensions ----------
// One World per vanilla level. Game code reaches the level it acts in through `world` (below): the tick thread
// points cur_level at the acting player's / command source's / ticking entity's level (InLevel), other threads
// default to the overworld. Code that knows its level explicitly (chunk streaming, spawn search) uses the World.
enum Dim { DIM_OVERWORLD, DIM_NETHER, DIM_END, DIM_COUNT };
inline World worlds[DIM_COUNT];
inline thread_local World* cur_level = &worlds[DIM_OVERWORLD];
inline World* level_by_id(const std::string& id) {
    for (auto& w : worlds)
        if (w.id == id) return &w;
    return nullptr;
}
struct InLevel {  // RAII: run a block of game code in level `w`
    World* prev;
    explicit InLevel(World* w) : prev(cur_level) { cur_level = w; }
    ~InLevel() { cur_level = prev; }
};
inline struct LevelProxy {
    World* operator->() const { return cur_level; }
    World& operator*() const { return *cur_level; }
    Chunk& get(int cx, int cz) const { return cur_level->get(cx, cz); }
    ChunkPtr get_ptr(int cx, int cz) const { return cur_level->get_ptr(cx, cz); }
    uint16_t get_block(int x, int y, int z) const { return cur_level->get_block(x, y, z); }
    // Game edits: fire on_block_changed (portal ignition / collapse: game/portal.hpp) after a real change.
    bool set_block(int x, int y, int z, uint16_t s) const;
    int min_y() const { return cur_level->min_y; }
    int max_y() const { return cur_level->max_y(); }  // exclusive
} world;
inline void (*on_block_changed)(int x, int y, int z, uint16_t old_state, uint16_t new_state) = nullptr;
inline bool LevelProxy::set_block(int x, int y, int z, uint16_t s) const {
    uint16_t old = cur_level->get_block(x, y, z);
    if (!cur_level->set_block(x, y, z, s)) return false;
    if (old != s && on_block_changed) on_block_changed(x, y, z, old, s);
    return true;
}

// Superflat: bedrock, 2 dirt, grass, plains.
inline void generate_flat(Chunk& c) {
    for (auto& s : c.sections) s.biomes.fill(biomes::plains);
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            c.set(x, -64, z, blocks::bedrock);
            c.set(x, -63, z, blocks::dirt);
            c.set(x, -62, z, blocks::dirt);
            c.set(x, -61, z, blocks::grass_block);
        }
}

inline void World::produce(int cx, int cz, std::promise<ChunkPtr>& job, bool encode) {
    try {
        auto c = std::make_shared<Chunk>(min_y, height);
        c->cx = cx, c->cz = cz, c->skylight = skylight, c->fullbright = fullbright;
        bool loaded = load_chunk(dir, *c);
        if (!loaded) {
            generator(*c);
            for (auto& s : c->sections) s.blocks.compact(), s.biomes.compact();
        }
        if (!fullbright) light::produced(*this, *c, loaded);  // light the 3x3 (generated worlds) before the frame is encoded
        if (encode) chunk_frame(*c);
        {
            std::lock_guard lock(m);
            chunks[{cx, cz}] = c;
            inflight.erase({cx, cz});
        }
        job.set_value(c);
    } catch (...) {
        {
            std::lock_guard lock(m);
            inflight.erase({cx, cz});
        }
        job.set_exception(std::current_exception());
    }
}
