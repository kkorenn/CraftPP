#pragma once
// Binary NBT on disk (named root compound, vanilla NbtIo) and zlib/gzip helpers. The value type is Nbt
// (commands/nbt.hpp). Files are untrusted: every read is bounds-checked and depth-limited (NbtAccounter: 512).
#include "../commands/nbt.hpp"

#include <stdexcept>
#include <vector>
#include <zlib.h>

namespace nbtio {

inline Nbt read_payload(Reader& r, uint8_t t, int depth = 0) {
    if (depth > 512) throw std::runtime_error("nbt too deep");
    auto len = [&](size_t unit) {
        int32_t n = r.i32();
        if (n < 0 || size_t(n) * unit > r.n - r.i) throw std::runtime_error("bad nbt length");
        return size_t(n);
    };
    auto string = [&] {
        size_t n = r.u16();
        if (n > r.n - r.i) throw std::runtime_error("bad nbt string");
        std::string s(reinterpret_cast<const char*>(r.p + r.i), n);
        r.i += n;
        return s;
    };
    Nbt v;
    v.t = Nbt::Type(t);
    switch (t) {
        case Nbt::Byte: v.i = int8_t(r.u8()); break;
        case Nbt::Short: v.i = int16_t(r.u16()); break;
        case Nbt::Int: v.i = r.i32(); break;
        case Nbt::Long: v.i = r.i64(); break;
        case Nbt::Float: v.d = r.f32(); break;
        case Nbt::Double: v.d = r.f64(); break;
        case Nbt::String: v.s = string(); break;
        case Nbt::ByteArray: v.arr.resize(len(1)); for (auto& x : v.arr) x = int8_t(r.u8()); break;
        case Nbt::IntArray: v.arr.resize(len(4)); for (auto& x : v.arr) x = r.i32(); break;
        case Nbt::LongArray: v.arr.resize(len(8)); for (auto& x : v.arr) x = r.i64(); break;
        case Nbt::List: {
            uint8_t et = r.u8();
            size_t n = len(1);
            if (et > Nbt::LongArray || (et == Nbt::End && n)) throw std::runtime_error("bad nbt list");
            v.list.reserve(n);
            for (size_t k = 0; k < n; k++) {
                v.list.push_back(read_payload(r, et, depth + 1));
                Nbt& e = v.list.back();  // ListTag.tryUnwrap: {"": x} stands for x in a mixed list
                if (e.t == Nbt::Compound && e.comp.size() == 1 && e.comp[0].first.empty()) e = Nbt(std::move(e.comp[0].second));
            }
            break;
        }
        case Nbt::Compound:
            for (uint8_t et; (et = r.u8()) != Nbt::End;) {
                if (et > Nbt::LongArray) throw std::runtime_error("bad nbt tag");
                std::string k = string();
                v.comp.emplace_back(std::move(k), read_payload(r, et, depth + 1));
            }
            break;
        default: throw std::runtime_error("bad nbt tag");
    }
    return v;
}

// Root of a file: tag type, (empty) name, payload.
inline Nbt parse(const uint8_t* p, size_t n) {
    Reader r(p, n);
    uint8_t t = r.u8();
    if (t != Nbt::Compound) throw std::runtime_error("nbt root is not a compound");
    r.i += r.u16();
    return read_payload(r, t);
}
inline Nbt parse(const std::vector<uint8_t>& b) { return parse(b.data(), b.size()); }
// Network form (Chunk/BlockEntity blobs): tag type then payload, no name.
inline Nbt parse_unnamed(const std::vector<uint8_t>& b) {
    Reader r(b.data(), b.size());
    uint8_t t = r.u8();
    return t == Nbt::End ? Nbt::compound() : read_payload(r, t);
}

// Mixed lists (strings and compounds, e.g. block state palettes) wrap non-compounds as {"": x}, like ListTag.
inline void write_payload(Writer& w, const Nbt& n) {
    if (n.t == Nbt::List && n.list.size() > 1) {
        bool mixed = false;
        for (auto& e : n.list) mixed |= e.t != n.list[0].t;
        if (mixed) {
            w.u8(Nbt::Compound).i32(int32_t(n.list.size()));
            for (auto& e : n.list) {
                if (e.t == Nbt::Compound && !(e.comp.size() == 1 && e.comp[0].first.empty())) { write_payload(w, e); continue; }
                w.u8(e.t), nbt_string(w, ""), write_payload(w, e), w.u8(0);
            }
            return;
        }
    }
    if (n.t == Nbt::List) {
        w.u8(n.list.empty() ? 0 : n.list[0].t).i32(int32_t(n.list.size()));
        for (auto& e : n.list) write_payload(w, e);
    } else if (n.t == Nbt::Compound) {
        for (auto& [k, v] : n.comp) { w.u8(v.t); nbt_string(w, k); write_payload(w, v); }
        w.u8(0);
    } else {
        nbt_payload(w, n);
    }
}
inline std::vector<uint8_t> write(const Nbt& root) {
    Writer w;
    w.u8(Nbt::Compound).u8(0).u8(0);
    write_payload(w, root);
    return std::move(w.b);
}

// zlib (windowBits 15) or gzip (31) streams. inflate returns false on corrupt input or more than `max` bytes.
inline std::vector<uint8_t> deflate_bytes(const uint8_t* p, size_t n, bool gz, int level) {
    z_stream s{};
    deflateInit2(&s, level, Z_DEFLATED, gz ? 31 : 15, 8, Z_DEFAULT_STRATEGY);
    std::vector<uint8_t> out(deflateBound(&s, uLong(n)));
    s.next_in = const_cast<Bytef*>(p), s.avail_in = uInt(n);
    s.next_out = out.data(), s.avail_out = uInt(out.size());
    deflate(&s, Z_FINISH);
    out.resize(s.total_out);
    deflateEnd(&s);
    return out;
}
inline bool inflate_bytes(const uint8_t* p, size_t n, std::vector<uint8_t>& out, size_t max = 64u << 20) {
    z_stream s{};
    if (inflateInit2(&s, 15 + 32) != Z_OK) return false;  // +32: zlib or gzip header, auto-detected
    s.next_in = const_cast<Bytef*>(p), s.avail_in = uInt(n);
    out.resize(std::max<size_t>(n * 4, 4096));
    int rc;
    do {
        if (s.total_out == out.size()) {
            if (out.size() >= max) break;
            out.resize(std::min(out.size() * 2, max));
        }
        s.next_out = out.data() + s.total_out, s.avail_out = uInt(out.size() - s.total_out);
        rc = inflate(&s, Z_NO_FLUSH);
    } while (rc == Z_OK);
    out.resize(s.total_out);
    inflateEnd(&s);
    return rc == Z_STREAM_END;
}

// level.dat, players/data/<uuid>.dat, data/minecraft/*.dat: gzip'd NBT (NbtIo.writeCompressed).
inline bool read_file(const std::string& path, Nbt& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> z;
    uint8_t buf[16384];
    for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0;) z.insert(z.end(), buf, buf + k);
    fclose(f);
    std::vector<uint8_t> raw;
    if (z.size() >= 2 && z[0] == 0x1f && z[1] == 0x8b) {
        if (!inflate_bytes(z.data(), z.size(), raw)) return false;
    } else {
        raw = std::move(z);
    }
    try {
        out = parse(raw);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
// Written to <path>_tmp then renamed over: a crash never leaves a half-written file (vanilla does the same).
inline bool write_file(const std::string& path, const Nbt& root) {
    auto raw = write(root);
    auto z = deflate_bytes(raw.data(), raw.size(), true, 6);
    std::string tmp = path + "_tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(z.data(), 1, z.size(), f) == z.size();
    ok &= fclose(f) == 0;
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace nbtio
