// World chunks <-> region files (load_chunk / save_chunk of world.hpp), plus the old CPP1/CPP2 reader.
#include <fstream>
#include <iterator>

#include "../log.hpp"
#include "../world.hpp"
#include "chunk_nbt.hpp"
#include "nbt_io.hpp"
#include "region.hpp"

namespace {

// world/chunks/c.X.Z.bin from before region files: "CPP2" + raw size + zlib(sections, block entities, light,
// extra), or "CPP1" (flat arrays).
bool load_legacy(const std::string& dir, Chunk& c) {
    std::ifstream f(dir + "/chunks/c." + std::to_string(c.cx) + "." + std::to_string(c.cz) + ".bin", std::ios::binary);
    uint32_t magic = 0, raw = 0;
    if (!f.read(reinterpret_cast<char*>(&magic), 4)) return false;
    if (magic == 0x43505031) {
        std::vector<uint16_t> b(4096 + 64);
        for (auto& s : c.sections) {
            if (!f.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size() * 2))) return false;
            for (int i = 0; i < 4096; i++) s.blocks.set(i, b[size_t(i)]);
            for (int i = 0; i < 64; i++) s.biomes.set(i, b[size_t(4096 + i)]);
            s.blocks.compact(), s.biomes.compact();
        }
        return true;
    }
    if (magic != 0x43505032 || !f.read(reinterpret_cast<char*>(&raw), 4) || raw > (4u << 20)) return false;
    std::vector<uint8_t> z(std::istreambuf_iterator<char>(f), {}), out(raw);
    uLongf n = raw;
    if (uncompress(out.data(), &n, z.data(), uLong(z.size())) != Z_OK || n != raw) return false;
    Reader r(out);
    for (auto& s : c.sections) s.blocks.read(r), s.biomes.read(r);
    if (r.i < out.size())
        for (int k = r.varint(); k > 0; k--) {
            BlockEntity e;
            e.x = r.u8(), e.z = r.u8(), e.y = int16_t(r.varint()), e.type = r.varint();
            int len = r.varint();
            e.nbt.assign(out.begin() + std::ptrdiff_t(r.i), out.begin() + std::ptrdiff_t(r.i + size_t(len)));
            r.i += size_t(len);
            c.block_entities.push_back(std::move(e));
        }
    if (r.i < out.size()) {
        size_t len = size_t(r.varint());
        if (r.i + len <= out.size()) c.saved_light.assign(out.begin() + std::ptrdiff_t(r.i), out.begin() + std::ptrdiff_t(r.i + len));
        r.i += len;
    }
    if (r.i < r.n && r.u8() == 1) {  // level data (src/level): post-processing, tick lists
        c.level.post_processed = r.u8();
        c.level.post.resize(size_t(r.varint()));
        for (auto& v : c.level.post) v = uint32_t(r.i32());
        c.level.ticks.resize(size_t(r.varint()));
        for (auto& b : c.level.ticks) b = r.u8();
    }
    if (r.i < out.size() && chunk_load_extra[c.dimension]) chunk_load_extra[c.dimension](c, r);
    c.dirty = true;  // move it into the region file at the next save
    return true;
}

}  // namespace

bool load_chunk(const std::string& dir, Chunk& c) {
    auto raw = storage::read(storage::region_dir(c.dimension), c.cx, c.cz);
    if (raw.empty()) return (c.on_disk = load_legacy(dir, c));
    try {
        Nbt root = nbtio::parse(raw);
        // A proto chunk (vanilla ChunkStatus below full): not a World chunk yet; the generator continues it.
        if (storage::status_of(root) != "minecraft:full") return false;
        storage::decode(root, c, &c.saved_light);
        if (const Nbt* x = root.get("CraftPP"); x && chunk_load_extra[c.dimension])
            if (const Nbt* b = x->get("structures"); b && b->t == Nbt::ByteArray) {
                std::vector<uint8_t> blob(b->arr.begin(), b->arr.end());
                Reader r(blob);
                chunk_load_extra[c.dimension](c, r);
            }
        for (auto& h : storage::chunk_hooks())
            if (h.load) h.load(c, root);
        return c.on_disk = true;
    } catch (const std::exception& e) {  // vanilla: "Couldn't load chunk", then generates a new one
        log_error("Couldn't load chunk [%d, %d]: %s", c.cx, c.cz, e.what());
        for (auto& s : c.sections) s.blocks.fill(blocks::air), s.biomes.fill(biomes::plains);
        c.block_entities.clear(), c.saved_light.clear();
        return false;
    }
}

void save_chunk(const std::string&, Chunk& c) {
    uint32_t light_version = c.light_version;
    std::vector<uint8_t> light;  // fullbright (nether, end): no light engine, none saved (vanilla relights)
    if (!c.fullbright) light = light::save(c, light_version);  // before c.m: lock order is light, then chunk
    Nbt root;
    {
        std::lock_guard lock(c.m);
        root = storage::encode(c, "minecraft:full", light);
        if (chunk_save_extra[c.dimension]) {
            Writer w;
            chunk_save_extra[c.dimension](c, w);
            Nbt x = Nbt::compound(), b;
            b.t = Nbt::ByteArray;
            b.arr.assign(w.b.begin(), w.b.end());
            x.set("structures", b);
            root.set("CraftPP", x);
        }
        for (auto& h : storage::chunk_hooks())
            if (h.save) h.save(c, root);
        c.dirty = c.light_version != light_version;  // light changed since it was captured: save again
    }
    storage::write(storage::region_dir(c.dimension), c.cx, c.cz, nbtio::write(root));
    c.on_disk = true;
}
