#include "worldgen/structure/nbt_io.hpp"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace structure {

namespace {
struct In {
  const uint8_t* p;
  size_t n, i = 0;
  uint8_t u8() {
    if (i >= n) throw std::runtime_error("nbt: truncated");
    return p[i++];
  }
  uint64_t be(int bytes) {
    uint64_t r = 0;
    for (int k = 0; k < bytes; k++) r = r << 8 | u8();
    return r;
  }
  std::string str() {
    size_t len = be(2);
    if (i + len > n) throw std::runtime_error("nbt: truncated string");
    std::string s((const char*)p + i, len);  // ponytail: modified UTF-8 read as UTF-8 (differs only for NUL/4-byte chars)
    i += len;
    return s;
  }
  Nbt payload(Nbt::Type t, int depth) {
    if (depth > 512) throw std::runtime_error("nbt: too deep");
    Nbt v;
    v.t = t;
    switch (t) {
      case Nbt::Byte: v.i = (int8_t)u8(); break;
      case Nbt::Short: v.i = (int16_t)be(2); break;
      case Nbt::Int: v.i = (int32_t)be(4); break;
      case Nbt::Long: v.i = (int64_t)be(8); break;
      case Nbt::Float: { uint32_t u = (uint32_t)be(4); float f; std::memcpy(&f, &u, 4); v.d = f; break; }
      case Nbt::Double: { uint64_t u = be(8); std::memcpy(&v.d, &u, 8); break; }
      case Nbt::String: v.s = str(); break;
      case Nbt::ByteArray: case Nbt::IntArray: case Nbt::LongArray: {
        int32_t len = (int32_t)be(4);
        int w = t == Nbt::ByteArray ? 1 : t == Nbt::IntArray ? 4 : 8;
        if (len < 0 || i + (size_t)len * w > n) throw std::runtime_error("nbt: bad array");
        v.arr.resize(len);
        for (auto& e : v.arr) e = w == 1 ? (int8_t)u8() : w == 4 ? (int32_t)be(4) : (int64_t)be(8);
        break;
      }
      case Nbt::List: {
        auto et = (Nbt::Type)u8();
        int32_t len = (int32_t)be(4);
        if (len < 0 || (size_t)len > n) throw std::runtime_error("nbt: bad list");
        v.list.reserve(len);
        for (int32_t k = 0; k < len; k++) v.list.push_back(payload(et, depth + 1));
        break;
      }
      case Nbt::Compound:
        for (;;) {
          auto ct = (Nbt::Type)u8();
          if (ct == Nbt::End) break;
          std::string name = str();
          v.comp.emplace_back(std::move(name), payload(ct, depth + 1));
        }
        break;
      default: throw std::runtime_error("nbt: bad tag type");
    }
    return v;
  }
};
}  // namespace

Nbt readNbt(const std::vector<uint8_t>& raw) {
  In in{raw.data(), raw.size()};
  auto t = (Nbt::Type)in.u8();
  if (t == Nbt::End) return {};
  in.str();  // root name
  return in.payload(t, 0);
}

std::vector<uint8_t> gunzip(const std::vector<uint8_t>& in) {
  z_stream zs{};
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) throw std::runtime_error("gzip: init failed");
  std::vector<uint8_t> out(in.size() * 4 + 1024);
  zs.next_in = const_cast<Bytef*>(in.data());
  zs.avail_in = (uInt)in.size();
  int r;
  do {
    if (zs.total_out == out.size()) out.resize(out.size() * 2);
    zs.next_out = out.data() + zs.total_out;
    zs.avail_out = (uInt)(out.size() - zs.total_out);
    r = inflate(&zs, Z_NO_FLUSH);
  } while (r == Z_OK);
  out.resize(zs.total_out);
  inflateEnd(&zs);
  if (r != Z_STREAM_END) throw std::runtime_error("gzip: corrupt data");
  return out;
}

Nbt readGzipNbtFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("nbt: cannot open " + path);
  std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return readNbt(gunzip(raw));
}

}  // namespace structure
