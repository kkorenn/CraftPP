#include "chunk_nbt.hpp"

#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "../world.hpp"
#include "../worldgen/block_state.hpp"
#include "../worldgen/pipeline.hpp"
#include "nbt_io.hpp"
#include "region.hpp"

namespace storage {
namespace {

constexpr int LIGHT_SECTIONS = SECTIONS + 2, MIN_SECTION = (MIN_Y >> 4);  // light sections y -5..20

Nbt I(int64_t v) { return Nbt::num(Nbt::Int, v); }
Nbt L(int64_t v) { return Nbt::num(Nbt::Long, v); }
Nbt B(int64_t v) { return Nbt::num(Nbt::Byte, v); }
int64_t num(const Nbt* n, int64_t d = 0) { return n && n->is_num() ? n->i : d; }

// Strategy.getConfigurationForPaletteSize -> bits on disk.
int block_bits(size_t n) { return n <= 1 ? 0 : std::max(4, int(std::bit_width(n - 1))); }
int biome_bits(size_t n) { return n <= 1 ? 0 : int(std::bit_width(n - 1)); }

Nbt pack(const std::vector<uint16_t>& idx, int bits) {  // SimpleBitStorage: entries never span longs
    Nbt a;
    a.t = Nbt::LongArray;
    int per = 64 / bits;
    a.arr.assign((idx.size() + size_t(per) - 1) / size_t(per), 0);
    for (size_t i = 0; i < idx.size(); i++) a.arr[i / size_t(per)] |= int64_t(uint64_t(idx[i]) << ((i % size_t(per)) * size_t(bits)));
    return a;
}

// Container -> {palette, data}. Palette in first-use order, like PalettedContainer.pack.
template <class P>
Nbt container(const P& p, int n, int (*bits_for)(size_t), const std::function<Nbt(uint16_t)>& name) {
    Nbt out = Nbt::compound(), pal = Nbt::make_list();
    std::vector<uint16_t> values, idx = std::vector<uint16_t>(size_t(n));
    if (!p.bits) {
        values.push_back(p.palette[0]);
    } else {
        std::unordered_map<uint16_t, uint16_t> at;
        for (int i = 0; i < n; i++) {
            uint16_t v = p.get(i);
            auto [it, fresh] = at.emplace(v, uint16_t(values.size()));
            if (fresh) values.push_back(v);
            idx[size_t(i)] = it->second;
        }
    }
    for (uint16_t v : values) pal.list.push_back(name(v));
    out.set("palette", pal);
    if (int bits = bits_for(values.size())) out.set("data", pack(idx, bits));
    return out;
}

// {palette, data} -> container. Palette indices out of range read as entry 0 (vanilla: error, partial).
template <class P>
void uncontainer(P& p, int n, const Nbt* tag, int (*bits_for)(size_t), int max_bits, const std::function<uint16_t(const Nbt&)>& id) {
    const Nbt* pal = tag ? tag->get("palette") : nullptr;
    if (!pal || pal->list.empty()) return;  // vanilla: default container (air / plains)
    std::vector<uint16_t> values;
    for (auto& e : pal->list) values.push_back(id(e));
    int bits = bits_for(values.size());
    if (!bits) return p.fill(values[0]);
    const Nbt* data = tag->get("data");
    int per = 64 / bits;
    size_t longs = size_t((n + per - 1) / per);
    if (!data || data->t != Nbt::LongArray || data->arr.size() != longs) throw std::runtime_error("bad paletted container data");
    if (bits <= max_bits) {  // same encoding as memory: adopt it; pad the palette so every index is valid
        p.bits = uint8_t(bits);
        values.resize(size_t(1) << bits, values[0]);
        p.palette = std::move(values);
        p.data.assign(data->arr.begin(), data->arr.end());
        return;
    }
    p.fill(values[0]);
    for (int i = 0; i < n; i++) {
        size_t k = uint64_t(data->arr[size_t(i / per)]) >> ((i % per) * bits) & ((1u << bits) - 1);
        p.set(i, values[k < values.size() ? k : 0]);
    }
}

const std::unordered_map<std::string, int>& name_map(const char* const* names, size_t n) {
    static std::unordered_map<const char* const*, std::unordered_map<std::string, int>> maps;  // filled once per table at startup-ish
    static std::mutex m;
    std::lock_guard lock(m);
    auto& map = maps[names];
    if (map.empty())
        for (size_t i = 0; i < n; i++) map.emplace(names[i], int(i));
    return map;
}
std::string full(const std::string& s) { return s.find(':') == std::string::npos ? "minecraft:" + s : s; }

int biome_id(const std::string& name) {
    auto& m = name_map(biomes::NAMES, std::size(biomes::NAMES));
    auto it = m.find(full(name));
    return it == m.end() ? biomes::plains : it->second;
}
int block_entity_type(const std::string& name) {
    auto& m = name_map(block_entity_types::NAMES, std::size(block_entity_types::NAMES));
    auto it = m.find(full(name));
    return it == m.end() ? -1 : it->second;
}

Nbt state_nbt(uint16_t s) {  // BlockState.CODEC: default state = its block id, else {id, properties}
    const auto& R = bs::Registry::get();
    const bs::Block& b = R.block(R.block_of(s));
    std::string id = "minecraft:" + b.name;
    if (s == b.def) return Nbt::str(id);
    Nbt c = Nbt::compound(), props = Nbt::compound();
    c.set("id", Nbt::str(id));
    for (auto& p : b.props) props.set(p.name, Nbt::str(std::string(R.value(s, p.name))));
    c.set("properties", props);
    return c;
}
uint16_t state_id(const Nbt& e) {
    const auto& R = bs::Registry::get();
    const Nbt* name = e.t == Nbt::String ? &e : e.get("id") ? e.get("id") : e.get("Name");  // Name: pre-1.21.x
    int b = name ? R.block_index(name->s) : -1;
    if (b < 0) return blocks::air;
    int s = R.default_state(b);
    if (e.t == Nbt::Compound)
        if (const Nbt* props = e.get("properties") ? e.get("properties") : e.get("Properties"))
            for (auto& [k, v] : props->comp)
                if (int t = R.with(s, k, v.s); t >= 0) s = t;
    return uint16_t(s);
}

Nbt byte_array(const uint8_t* p, uint8_t fill) {
    Nbt a;
    a.t = Nbt::ByteArray;
    a.arr.resize(2048);
    for (int i = 0; i < 2048; i++) a.arr[size_t(i)] = int8_t(p ? p[i] : fill);
    return a;
}

}  // namespace

std::string dimension_dir() { return world_dir + "/dimensions/minecraft/overworld"; }

std::vector<ChunkHook>& chunk_hooks() {
    static std::vector<ChunkHook> h;
    return h;
}

Nbt encode(const Chunk& c, const std::string& status, const std::vector<uint8_t>& light, const Heightmaps* hm, unsigned hm_mask) {
    Nbt root = Nbt::compound();
    root.set("DataVersion", I(DATA_VERSION));
    root.set("xPos", I(c.cx));
    root.set("yPos", I(MIN_SECTION));
    root.set("zPos", I(c.cz));
    root.set("LastUpdate", L(0));
    root.set("InhabitedTime", L(0));
    root.set("Status", Nbt::str(status));

    // Light (light::save): per light section a flags byte (1 present, 2 sky array, 4 block array), then sky and
    // block as 2048 bytes or one uniform value. Vanilla stores a layer unless it is uniformly 0.
    std::array<Nbt, LIGHT_SECTIONS> sky, block;
    size_t at = 0;
    bool lit = !light.empty();
    for (int si = 0; lit && si < LIGHT_SECTIONS; si++) {
        if (at >= light.size()) { lit = false; break; }
        uint8_t f = light[at++];
        if (!(f & 1)) continue;
        for (int k = 0; k < 2; k++) {
            Nbt& out = k ? block[size_t(si)] : sky[size_t(si)];
            if (f & (k ? 4 : 2)) {
                if (at + 2048 > light.size()) { lit = false; break; }
                out = byte_array(light.data() + at, 0), at += 2048;
            } else {
                if (at >= light.size()) { lit = false; break; }
                uint8_t def = light[at++];
                if (def) out = byte_array(nullptr, uint8_t(def * 0x11));
            }
        }
    }
    Nbt sections = Nbt::make_list();
    for (int si = 0; si < LIGHT_SECTIONS; si++) {
        Nbt s = Nbt::compound();
        int y = si + MIN_SECTION - 1;
        if (si >= 1 && si <= SECTIONS) {
            const Section& sec = c.sections[size_t(si - 1)];
            s.set("block_states", container(sec.blocks, 4096, block_bits, state_nbt));
            s.set("biomes", container(sec.biomes, 64, biome_bits, [](uint16_t b) { return Nbt::str(biomes::NAMES[b]); }));
        }
        if (lit && block[size_t(si)].t) s.set("BlockLight", std::move(block[size_t(si)]));
        if (lit && sky[size_t(si)].t) s.set("SkyLight", std::move(sky[size_t(si)]));
        if (s.comp.empty()) continue;
        s.set("Y", B(y));
        sections.list.push_back(std::move(s));
    }
    root.set("sections", sections);
    if (lit) root.set("isLightOn", B(1));

    Nbt bes = Nbt::make_list();
    for (auto& e : c.block_entities) {
        Nbt t = nbtio::parse_unnamed(e.nbt);
        if (t.t != Nbt::Compound) t = Nbt::compound();
        if (e.type >= 0 && size_t(e.type) < std::size(block_entity_types::NAMES))
            t.set("id", Nbt::str(block_entity_types::NAMES[e.type]));
        t.set("x", I(c.cx * 16 + e.x)), t.set("y", I(e.y)), t.set("z", I(c.cz * 16 + e.z));
        bes.list.push_back(std::move(t));
    }
    root.set("block_entities", bes);
    root.set("block_ticks", Nbt::make_list());
    root.set("fluid_ticks", Nbt::make_list());
    Nbt pp = Nbt::make_list();
    for (int i = 0; i < SECTIONS; i++) pp.list.push_back(Nbt::make_list());
    root.set("PostProcessing", pp);

    Heightmaps computed;
    if (!hm) {  // LevelChunk: the FINAL heightmaps (Heightmap.Types.WORLD_SURFACE ... MOTION_BLOCKING_NO_LEAVES)
        worldgen::prime_heightmaps(c, computed);
        hm = &computed;
        hm_mask = 1 << worldgen::WORLD_SURFACE | 1 << worldgen::OCEAN_FLOOR | 1 << worldgen::MOTION_BLOCKING |
                  1 << worldgen::MOTION_BLOCKING_NO_LEAVES;
    }
    Nbt hms = Nbt::compound();
    for (int h = 0; h < 6; h++) {
        if (!(hm_mask >> h & 1)) continue;
        std::vector<uint16_t> v(256);
        for (int i = 0; i < 256; i++) v[size_t(i)] = uint16_t((*hm)[size_t(h)][size_t(i)] - MIN_Y);
        hms.set(worldgen::heightmap_name(worldgen::Heightmap(h)), pack(v, 9));
    }
    root.set("Heightmaps", hms);
    Nbt st = Nbt::compound();
    st.set("starts", Nbt::compound());
    st.set("References", Nbt::compound());
    root.set("structures", st);
    return root;
}

std::string status_of(const Nbt& root) {
    const Nbt* s = root.get("Status");
    if (!s || s->t != Nbt::String || s->s.empty()) return "";
    return s->s.find(':') == std::string::npos ? "minecraft:" + s->s : s->s;
}

unsigned decode_heightmaps(const Nbt& root, Heightmaps& hm) {
    unsigned mask = 0;
    const Nbt* hms = root.get("Heightmaps");
    for (int h = 0; hms && h < 6; h++) {
        const Nbt* a = hms->get(worldgen::heightmap_name(worldgen::Heightmap(h)));
        if (!a || a->t != Nbt::LongArray || a->arr.size() != 37) continue;
        for (int i = 0; i < 256; i++)
            hm[size_t(h)][size_t(i)] = int16_t(MIN_Y + int(uint64_t(a->arr[size_t(i / 7)]) >> ((i % 7) * 9) & 511));
        mask |= 1u << h;
    }
    return mask;
}

void decode(const Nbt& root, Chunk& c, std::vector<uint8_t>* light) {
    bool lit = num(root.get("isLightOn")) != 0 && light;
    std::array<const Nbt*, LIGHT_SECTIONS> sky{}, block{};
    if (const Nbt* secs = root.get("sections"))
        for (auto& s : secs->list) {
            int y = int(int8_t(num(s.get("Y"))));
            int si = y - MIN_SECTION + 1;
            if (si < 0 || si >= LIGHT_SECTIONS) continue;
            if (si >= 1 && si <= SECTIONS) {
                Section& sec = c.sections[size_t(si - 1)];
                uncontainer(sec.blocks, 4096, s.get("block_states"), block_bits, 8, state_id);
                uncontainer(sec.biomes, 64, s.get("biomes"), biome_bits, 3, [](const Nbt& e) { return uint16_t(biome_id(e.s)); });
            }
            auto arr = [](const Nbt* a) { return a && a->t == Nbt::ByteArray && a->arr.size() == 2048 ? a : nullptr; };
            sky[size_t(si)] = arr(s.get("SkyLight")), block[size_t(si)] = arr(s.get("BlockLight"));
        }
    if (lit) {
        light->clear();
        for (int si = 0; si < LIGHT_SECTIONS; si++) {
            const Nbt *s = sky[size_t(si)], *b = block[size_t(si)];
            light->push_back(uint8_t((s || b ? 1 : 0) | (s ? 2 : 0) | (b ? 4 : 0)));
            for (const Nbt* l : {s, b}) {
                if (!(s || b)) break;
                if (!l) { light->push_back(0); continue; }
                for (auto v : l->arr) light->push_back(uint8_t(v));
            }
        }
    }
    c.block_entities.clear();
    if (const Nbt* bes = root.get("block_entities"))
        for (auto& e : bes->list) {
            const Nbt* id = e.get("id");
            int type = id ? block_entity_type(id->s) : -1;
            int64_t x = num(e.get("x")), y = num(e.get("y")), z = num(e.get("z"));
            if (type < 0 || (x >> 4) != c.cx || (z >> 4) != c.cz || y < MIN_Y || y >= MIN_Y + HEIGHT) continue;
            Nbt rest = Nbt::compound();
            for (auto& [k, v] : e.comp)
                if (k != "id" && k != "x" && k != "y" && k != "z" && k != "keepPacked") rest.comp.emplace_back(k, v);
            BlockEntity be{uint8_t(x & 15), uint8_t(z & 15), int16_t(y), type, {}};
            Writer w;
            write_nbt(w, rest);
            be.nbt = std::move(w.b);
            c.block_entities.push_back(std::move(be));
        }
}

void save_entities(int cx, int cz, const std::vector<Nbt>& entities) {
    Nbt root = Nbt::compound();
    root.set("DataVersion", I(DATA_VERSION));
    Nbt pos;
    pos.t = Nbt::IntArray;
    pos.arr = {cx, cz};
    root.set("Position", pos);
    Nbt list = Nbt::make_list();
    list.list = entities;
    root.set("Entities", list);
    write(entities_dir(), cx, cz, nbtio::write(root));
}

std::vector<Nbt> load_entities(int cx, int cz) {
    auto raw = read(entities_dir(), cx, cz);
    if (raw.empty()) return {};
    try {
        Nbt root = nbtio::parse(raw);
        if (const Nbt* l = root.get("Entities")) return l->list;
    } catch (const std::exception&) {
    }
    return {};
}

}  // namespace storage
