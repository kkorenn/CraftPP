#include "worldgen/block_state.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>

#include "worldgen/json.hpp"

namespace bs {
namespace {
std::string_view strip_ns(std::string_view n) { return n.substr(0, 10) == "minecraft:" ? n.substr(10) : n; }
}  // namespace

const Registry& Registry::get() {
  static const Registry r;
  return r;
}

Registry::Registry() {
  std::ifstream f("data/blocks.bin", std::ios::binary);
  if (!f) throw std::runtime_error("data/blocks.bin missing (tools/gen_block_data.sh)");
  const std::string buf(std::istreambuf_iterator<char>(f), {});
  size_t at = 0;
  bool truncated = false;
  auto u8 = [&] {
    if (at >= buf.size()) return truncated = true, uint8_t(0);
    return uint8_t(buf[at++]);
  };
  auto u16 = [&] { int a = u8(); return uint16_t(a << 8 | u8()); };
  auto u32 = [&] { uint32_t a = u16(); return a << 16 | u16(); };
  auto str = [&] {
    size_t n = u16();
    if (at + n > buf.size()) return truncated = true, std::string();
    return at += n, buf.substr(at - n, n);
  };
  if (u32() != 0x43424432) throw std::runtime_error("data/blocks.bin: bad magic");
  uint32_t nblocks = u32(), nstates = u32();
  blocks.resize(nblocks);
  state_block.resize(nstates);
  for (uint32_t b = 0; b < nblocks; b++) {
    Block& k = blocks[b];
    k.name = str();
    k.first = int(u32());
    k.def = int(u32());
    k.block_entity = int16_t(u16());
    k.classes = str();
    k.props.resize(u8());
    k.count = 1;
    for (auto& p : k.props) {
      p.name = str();
      p.values.resize(u8());
      for (auto& v : p.values) v = str();
    }
    for (size_t i = k.props.size(); i-- > 0;) k.props[i].stride = k.count, k.count *= int(k.props[i].values.size());
    for (int s = k.first; s < k.first + k.count; s++) state_block[size_t(s)] = uint16_t(b);
    by_name[k.name] = int(b);
  }
  state_flags.resize(nstates);
  state_fluid.resize(nstates);
  state_flags2.resize(nstates);
  for (uint32_t s = 0; s < nstates; s++) state_flags[s] = u32(), state_fluid[s] = u8(), state_flags2[s] = u16();
  if (truncated) throw std::runtime_error("data/blocks.bin: truncated");

  // Block tags: load every file, resolve references recursively.
  std::unordered_map<std::string, std::vector<std::string>> raw;
  const std::string root = "vanilla/src/data/minecraft/tags/block";
  for (auto& e : std::filesystem::recursive_directory_iterator(root)) {
    if (e.path().extension() != ".json") continue;
    std::string rel = e.path().generic_string();  // <root>/<name>.json
    rel = rel.substr(root.size() + 1, rel.size() - root.size() - 6);
    Json j = Json::parseFile(e.path().string());
    auto& v = raw["minecraft:" + rel];
    for (auto& x : j["values"].arr) v.push_back(x.isString() ? x.str : x["id"].str);
  }
  std::function<const Tag&(const std::string&)> resolve = [&](const std::string& t) -> const Tag& {
    if (auto it = tags.find(t); it != tags.end()) return it->second;
    tags[t];  // placeholder breaks reference cycles
    Tag acc;
    if (auto it = raw.find(t); it != raw.end())
      for (auto& v : it->second) {
        if (v[0] == '#') {
          const Tag& sub = resolve(v.substr(1));
          for (size_t i = 0; i < sub.bits.size(); i++)
            for (int bit = 0; bit < 64; bit++)
              if ((sub.bits[i] >> bit) & 1) acc.add(int(i * 64 + size_t(bit)));
        } else if (int b = block_index(v); b >= 0) {
          acc.add(b);
        }
      }
    return tags[t] = std::move(acc);
  };
  for (auto& [k, v] : raw) resolve(k);

  const std::string froot = "vanilla/src/data/minecraft/tags/fluid";
  std::unordered_map<std::string, std::vector<std::string>> fraw;
  for (auto& e : std::filesystem::directory_iterator(froot))
    if (e.path().extension() == ".json") {
      Json j = Json::parseFile(e.path().string());  // keep the array owner alive throughout iteration
      for (auto& x : j["values"].arr) fraw["minecraft:" + e.path().stem().string()].push_back(x.isString() ? x.str : x["id"].str);
    }
  std::function<uint8_t(const std::string&, int)> fres = [&](const std::string& t, int depth) -> uint8_t {
    uint8_t m = 0;
    if (depth > 8) return 0;
    for (auto& v : fraw[t]) m |= v[0] == '#' ? fres(v.substr(1), depth + 1) : fluids(v);
    return m;
  };
  for (auto& [k, v] : fraw) fluid_tags[k] = fres(k, 0);
}

uint8_t Registry::fluids(std::string_view n) const {
  if (!n.empty() && n[0] == '#') {
    std::string t(n.substr(1));
    if (t.find(':') == std::string::npos) t = "minecraft:" + t;
    auto it = fluid_tags.find(t);
    return it == fluid_tags.end() ? 0 : it->second;
  }
  n = strip_ns(n);
  if (n == "empty") return 1 << NO_FLUID;
  if (n == "water") return 1 << WATER;
  if (n == "flowing_water") return 1 << FLOWING_WATER;
  if (n == "lava") return 1 << LAVA;
  if (n == "flowing_lava") return 1 << FLOWING_LAVA;
  return 0;
}

int Registry::block_index(std::string_view name) const {
  auto it = by_name.find(std::string(strip_ns(name)));
  return it == by_name.end() ? -1 : it->second;
}

const Prop* Registry::prop(int b, std::string_view name) const {
  for (auto& p : blocks[size_t(b)].props)
    if (p.name == name) return &p;
  return nullptr;
}

int Registry::value_index(int state, std::string_view name) const {
  int b = block_of(state);
  const Prop* p = prop(b, name);
  return p ? (state - blocks[size_t(b)].first) / p->stride % int(p->values.size()) : -1;
}

std::string_view Registry::value(int state, std::string_view name) const {
  int b = block_of(state);
  const Prop* p = prop(b, name);
  return p ? std::string_view(p->values[size_t((state - blocks[size_t(b)].first) / p->stride % int(p->values.size()))]) : "";
}

int Registry::int_value(int state, std::string_view name) const {
  auto v = value(state, name);
  return v.empty() ? -1 : std::stoi(std::string(v));
}

int Registry::with(int state, std::string_view name, int vi) const {
  int b = block_of(state);
  const Prop* p = prop(b, name);
  if (!p || vi < 0 || vi >= int(p->values.size())) return -1;
  int cur = (state - blocks[size_t(b)].first) / p->stride % int(p->values.size());
  return state + (vi - cur) * p->stride;
}

int Registry::with(int state, std::string_view name, std::string_view value) const {
  const Prop* p = prop(block_of(state), name);
  if (!p) return -1;
  for (size_t i = 0; i < p->values.size(); i++)
    if (p->values[i] == value) return with(state, name, int(i));
  return -1;
}

int Registry::parse(std::string_view spec) const {
  size_t br = spec.find('[');
  int b = block_index(spec.substr(0, br));
  if (b < 0) return -1;
  int s = blocks[size_t(b)].def;
  if (br == std::string_view::npos) return s;
  std::string_view rest = spec.substr(br + 1, spec.size() - br - 2);
  while (!rest.empty()) {
    size_t comma = rest.find(','), eq = rest.find('=');
    std::string_view kv = rest.substr(0, comma);
    s = with(s, kv.substr(0, eq), kv.substr(eq + 1));
    if (s < 0) return -1;
    rest = comma == std::string_view::npos ? "" : rest.substr(comma + 1);
  }
  return s;
}

std::string Registry::name(int state) const {
  const Block& k = blocks[size_t(block_of(state))];
  std::string n = "minecraft:" + k.name;
  for (size_t i = 0; i < k.props.size(); i++)
    n += (i ? "," : "[") + k.props[i].name + "=" + std::string(value(state, k.props[i].name)) + (i + 1 == k.props.size() ? "]" : "");
  return n;
}

int Registry::fluid_block(int s) const {
  // FlowingFluid.getLegacyLevel: source 0, flowing 8 - amount (+8 when falling).
  static const int water = R().block_index("water"), lava = R().block_index("lava");
  uint8_t t = fluid_type(s);
  if (t == NO_FLUID) return blocks[0].def;
  int b = t <= FLOWING_WATER ? water : lava;
  int amount = fluid_amount(s);
  int level = (t == WATER || t == LAVA) ? 0 : 8 - amount + (state_fluid[size_t(s)] & 0x80 ? 8 : 0);
  return with(blocks[size_t(b)].def, "level", std::to_string(level));
}

const Tag& Registry::tag(std::string_view name) const {
  if (!name.empty() && name[0] == '#') name.remove_prefix(1);
  std::string n(name);
  if (n.find(':') == std::string::npos) n = "minecraft:" + n;
  auto it = tags.find(n);
  return it == tags.end() ? empty : it->second;
}

}  // namespace bs
