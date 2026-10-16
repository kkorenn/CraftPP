#pragma once
#include "gen/ids.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

// Overworld dimension: y -64..319, 24 sections of 16x16x16.
inline constexpr int MIN_Y = -64, HEIGHT = 384, SECTIONS = HEIGHT / 16;

struct Section {
    std::array<uint16_t, 4096> blocks{};  // index = (y*16 + z)*16 + x
    std::array<uint16_t, 64> biomes{};    // 4x4x4 cells, same order
};

struct Chunk {
    int cx, cz;
    std::array<Section, SECTIONS> sections;
    std::mutex m;        // guards sections against concurrent edit + encode
    bool dirty = false;  // modified since last save

    uint16_t get(int x, int y, int z) const {
        return sections[(y - MIN_Y) >> 4].blocks[(((y - MIN_Y) & 15) * 16 + z) * 16 + x];
    }
    void set(int x, int y, int z, uint16_t s) {
        sections[(y - MIN_Y) >> 4].blocks[(((y - MIN_Y) & 15) * 16 + z) * 16 + x] = s;
    }
    // Biome cell: qx, qz 0..3, qy 0..HEIGHT/4-1 (quart rows from MIN_Y).
    void set_biome(int qx, int qy, int qz, uint16_t b) { sections[qy >> 2].biomes[((qy & 3) * 4 + qz) * 4 + qx] = b; }
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

// Paletted container (single / linear-or-hashmap / global), matches vanilla Strategy.
template <size_t N>
inline void write_paletted(Writer& w, const std::array<uint16_t, N>& vals, int min_bits, int max_bits, int global_bits) {
    std::vector<uint16_t> palette;
    for (uint16_t v : vals)
        if (std::find(palette.begin(), palette.end(), v) == palette.end()) {
            palette.push_back(v);
            if (int(palette.size()) > (1 << max_bits)) break;
        }
    if (palette.size() == 1) {
        w.u8(0).varint(palette[0]);
        return;
    }
    int bits = std::bit_width(palette.size() - 1);
    bool global = bits > max_bits;
    bits = global ? global_bits : std::max(bits, min_bits);
    w.u8(uint8_t(bits));
    if (!global) {
        w.varint(int32_t(palette.size()));
        for (uint16_t v : palette) w.varint(v);
    }
    int per_long = 64 / bits;
    std::vector<uint64_t> longs((N + per_long - 1) / per_long);
    for (size_t i = 0; i < N; i++) {
        uint64_t idx = global ? vals[i] : uint64_t(std::find(palette.begin(), palette.end(), vals[i]) - palette.begin());
        longs[i / per_long] |= idx << ((i % per_long) * bits);
    }
    for (uint64_t l : longs) w.i64(int64_t(l));  // fixed-size: no length prefix
}

// Body of clientbound level_chunk_with_light (without packet id).
inline void write_chunk(Writer& w, Chunk& c) {
    std::lock_guard lock(c.m);
    w.i32(c.cx).i32(c.cz);

    // Heightmaps WORLD_SURFACE(1), MOTION_BLOCKING(4), MOTION_BLOCKING_NO_LEAVES(5): 9 bits, 7 per long.
    // ponytail: all three = highest non-air block; split once leaves/fluids/glass matter.
    std::array<uint64_t, 37> hm{};
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int h = 0;
            for (int y = MIN_Y + HEIGHT - 1; y >= MIN_Y; y--)
                if (c.get(x, y, z) != blocks::air) { h = y - MIN_Y + 1; break; }
            int i = z * 16 + x;
            hm[i / 7] |= uint64_t(h) << ((i % 7) * 9);
        }
    w.varint(3);
    for (int type : {1, 4, 5}) {
        w.varint(type).varint(int32_t(hm.size()));
        for (uint64_t l : hm) w.i64(int64_t(l));
    }

    Writer data;
    const int block_bits = std::bit_width(unsigned(BLOCK_STATE_COUNT - 1));
    const int biome_bits = std::bit_width(unsigned(biomes::COUNT - 1));
    for (auto& s : c.sections) {
        int16_t non_air = 0, fluids = 0;
        for (uint16_t b : s.blocks) {
            uint8_t f = state_flags()[b];
            non_air += !(f & STATE_AIR);
            fluids += (f & STATE_FLUID) != 0;
        }
        // The client skips all fluid checks (swimming, buoyancy) in sections whose fluid count is 0.
        data.i16(non_air).i16(fluids);
        write_paletted(data, s.blocks, 4, 8, block_bits);
        write_paletted(data, s.biomes, 1, 3, biome_bits);
    }
    w.byte_array(data.b);
    w.varint(0);  // block entities

    // Light: full sky light everywhere, no block light. 26 sections (one below + one above).
    // ponytail: no light engine; caves are lit. Add real sky/block light propagation with terrain gen.
    w.varint(4).u8(0xFF).u8(0xFF).u8(0xFF).u8(0x03);  // sky mask (BitSet as LE bytes)
    w.varint(0);                                       // block mask
    w.varint(0);                                       // empty sky mask
    w.varint(0);                                       // empty block mask
    static const std::vector<uint8_t> full(2048, 0xFF);
    w.varint(SECTIONS + 2);
    for (int i = 0; i < SECTIONS + 2; i++) w.byte_array(full);
    w.varint(0);
}

inline void generate_flat(Chunk& c);

// On-disk chunk: magic + raw little-endian sections (blocks then biomes).
// ponytail: uncompressed ~200 KB per modified chunk; switch to Anvil region files (zlib NBT) for vanilla compatibility.
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
    for (auto& [k, v] : changes) kv[k] = v;
    std::filesystem::create_directories(world_dir);
    std::string path = level_data_path();
    {
        std::ofstream f(path + ".tmp");
        for (auto& [k, v] : kv) f << k << '=' << v << '\n';
    }
    std::filesystem::rename(path + ".tmp", path);
}

inline std::string chunk_path(int cx, int cz) {
    return world_dir + "/chunks/c." + std::to_string(cx) + "." + std::to_string(cz) + ".bin";
}
inline constexpr uint32_t CHUNK_MAGIC = 0x43505031;  // "CPP1"

inline bool load_chunk(Chunk& c) {
    std::ifstream f(chunk_path(c.cx, c.cz), std::ios::binary);
    uint32_t magic = 0;
    if (!f.read(reinterpret_cast<char*>(&magic), 4) || magic != CHUNK_MAGIC) return false;
    for (auto& s : c.sections) {
        f.read(reinterpret_cast<char*>(s.blocks.data()), sizeof s.blocks);
        f.read(reinterpret_cast<char*>(s.biomes.data()), sizeof s.biomes);
    }
    return bool(f);
}

inline void save_chunk(Chunk& c) {
    std::filesystem::create_directories(world_dir + "/chunks");
    std::string path = chunk_path(c.cx, c.cz), tmp = path + ".tmp";
    {
        std::lock_guard lock(c.m);
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(&CHUNK_MAGIC), 4);
        for (auto& s : c.sections) {
            f.write(reinterpret_cast<const char*>(s.blocks.data()), sizeof s.blocks);
            f.write(reinterpret_cast<const char*>(s.biomes.data()), sizeof s.biomes);
        }
        c.dirty = false;
    }
    std::filesystem::rename(tmp, path);  // atomic replace: a crash never leaves a half-written chunk
}

// Chunk cache. Chunks are never unloaded, so references stay valid.
// ponytail: unbounded memory (~200 KB/chunk); add unloading once worlds get big.
struct World {
    std::mutex m;
    std::map<std::pair<int, int>, std::unique_ptr<Chunk>> chunks;

    std::function<void(Chunk&)> generator = generate_flat;  // set before serving; called without the map lock

    Chunk& get(int cx, int cz) {
        {
            std::lock_guard lock(m);
            auto it = chunks.find({cx, cz});
            if (it != chunks.end()) return *it->second;
        }
        // Load/generate outside the map lock; if another thread raced us, keep its chunk.
        // ponytail: racing threads may both generate the same chunk; add an in-flight set if that shows up.
        auto c = std::make_unique<Chunk>();
        c->cx = cx;
        c->cz = cz;
        if (!load_chunk(*c)) generator(*c);
        std::lock_guard lock(m);
        auto& slot = chunks[{cx, cz}];
        if (!slot) slot = std::move(c);
        return *slot;
    }

    uint16_t get_block(int x, int y, int z) {
        if (y < MIN_Y || y >= MIN_Y + HEIGHT) return blocks::air;
        Chunk& c = get(x >> 4, z >> 4);
        std::lock_guard lock(c.m);
        return c.get(x & 15, y, z & 15);
    }

    bool set_block(int x, int y, int z, uint16_t state) {
        if (y < MIN_Y || y >= MIN_Y + HEIGHT) return false;
        Chunk& c = get(x >> 4, z >> 4);
        std::lock_guard lock(c.m);
        c.set(x & 15, y, z & 15, state);
        c.dirty = true;
        return true;
    }

    int save_dirty() {
        std::vector<Chunk*> list;
        {
            std::lock_guard lock(m);
            for (auto& [k, c] : chunks)
                if (c->dirty) list.push_back(c.get());
        }
        for (Chunk* c : list) save_chunk(*c);
        return int(list.size());
    }
};

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
