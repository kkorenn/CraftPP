#include "worldgen/structure/blocks.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace structure {

static std::string g_root = "data/vanilla";
std::string dataRoot() { return g_root; }
void setDataRoot(const std::string& root) { g_root = root; }

std::string fullId(std::string_view id) {
  return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
}


const Blocks& Blocks::get() {
  static const Blocks b;
  return b;
}

Blocks::Blocks() {
  std::ifstream d("data/block_states.bin", std::ios::binary);
  uint32_t n = 0;
  if (!d.read((char*)&n, 4)) throw std::runtime_error("blocks: missing data/block_states.bin (tools/gen_block_dump.py)");
  std::vector<uint16_t> raw((size_t)n * 7);  // 5 x u16 transforms + u32 flags, little-endian
  d.read((char*)raw.data(), (std::streamsize)(raw.size() * 2));
  if (!d) throw std::runtime_error("blocks: truncated data/block_states.bin");
  transforms_.resize((size_t)n * 5);
  flags_.resize(n);
  for (uint32_t i = 0; i < n; i++) {
    for (int k = 0; k < 5; k++) transforms_[i * 5 + k] = raw[i * 7 + k];
    flags_[i] = raw[i * 7 + 5] | (uint32_t)raw[i * 7 + 6] << 16;
  }
}

int Blocks::defaultState(std::string_view name) const {
  int b = blockIndex(name);
  return b < 0 ? -1 : bs::R().default_state(b);
}

int Blocks::with(int state, std::string_view key, std::string_view value) const {
  int r = bs::R().with(state, key, value);
  return r < 0 ? state : r;
}

std::vector<std::string> Blocks::propNames(int state) const {
  std::vector<std::string> out;
  for (auto& p : bs::R().block(blockOf(state)).props) out.push_back(p.name);
  return out;
}

int Blocks::state(std::string_view name, const std::vector<std::pair<std::string, std::string>>& props) const {
  int s = defaultState(name);
  if (s < 0) return -1;
  for (auto& [k, v] : props) s = with(s, k, v);
  return s;
}

int Blocks::parse(std::string_view s) const {
  size_t nb = s.find('{');
  return bs::R().parse(nb == std::string_view::npos ? s : s.substr(0, nb));
}

int Blocks::rotate(int state, Rotation r) const {
  return r == Rotation::NONE ? state : transforms_[(size_t)state * 5 + (int)r - 1];
}
int Blocks::mirror(int state, Mirror m) const {
  return m == Mirror::NONE ? state : transforms_[(size_t)state * 5 + 2 + (int)m];
}

// ------------------------------------------------------------------------------------------- tags
std::vector<std::string> resolveTag(const std::string& kind, std::string_view tag) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  std::function<void(const std::string&)> add = [&](const std::string& id) {
    if (!seen.insert(id).second) return;
    auto c = id.find(':');
    std::string path = dataRoot() + "/" + id.substr(0, c) + "/tags/" + kind + "/" + id.substr(c + 1) + ".json";
    if (!std::filesystem::exists(path)) return;
    Json j = Json::parseFile(path);
    for (auto& v : j["values"].arr) {
      std::string e = v.isString() ? v.asString() : v["id"].asString();
      if (e[0] == '#') add(fullId(e.substr(1)));
      else if (std::find(out.begin(), out.end(), fullId(e)) == out.end()) out.push_back(fullId(e));
    }
  };
  std::string_view t = tag[0] == '#' ? tag.substr(1) : tag;
  add(fullId(t));
  return out;
}

const std::vector<std::string>& biomeNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (auto& e : std::filesystem::directory_iterator(dataRoot() + "/minecraft/worldgen/biome"))
      if (e.path().extension() == ".json") v.push_back("minecraft:" + e.path().stem().string());
    std::sort(v.begin(), v.end());
    return v;
  }();
  return names;
}

int biomeId(std::string_view name) {
  auto& n = biomeNames();
  auto it = std::lower_bound(n.begin(), n.end(), fullId(name));
  return it != n.end() && *it == fullId(name) ? (int)(it - n.begin()) : -1;
}

std::vector<uint8_t> biomeSet(const Json& j) {
  std::vector<uint8_t> mask(biomeNames().size(), 0);
  auto add = [&](const std::string& e) {
    if (e[0] == '#') {
      for (auto& n : resolveTag("worldgen/biome", e))
        if (int id = biomeId(n); id >= 0) mask[id] = 1;
    } else if (int id = biomeId(e); id >= 0) {
      mask[id] = 1;
    }
  };
  if (j.isString()) add(j.asString());
  else
    for (auto& e : j.arr) add(e.asString());
  return mask;
}

}  // namespace structure
