#include "worldgen/structure/blocks.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace structure {

static std::string g_root = "vanilla/src/data";
std::string dataRoot() { return g_root; }
void setDataRoot(const std::string& root) { g_root = root; }

std::string fullId(std::string_view id) {
  return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
}

static std::vector<std::string> split(const std::string& s, char c) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string t; std::getline(ss, t, c);) out.push_back(t);
  return out;
}

const Blocks& Blocks::get() {
  static const Blocks b;
  return b;
}

Blocks::Blocks() {
  std::ifstream f("java/src/main/resources/block-states-26.3.txt");
  if (!f) throw std::runtime_error("blocks: missing java/src/main/resources/block-states-26.3.txt");
  for (std::string line; std::getline(f, line);) {
    if (line.empty()) continue;
    auto parts = split(line, ' ');
    Block b{parts[0], std::stoi(parts[1]), std::stoi(parts[2]), {}};
    if (parts.size() > 3)
      for (auto& p : split(parts[3], ';')) {
        auto eq = p.find('=');
        b.props.push_back({p.substr(0, eq), split(p.substr(eq + 1), ',')});
      }
    blocks_.push_back(std::move(b));
  }
  std::sort(blocks_.begin(), blocks_.end(), [](const Block& a, const Block& b) { return a.first < b.first; });
  for (size_t i = 0; i < blocks_.size(); i++) {
    firstStates_.push_back(blocks_[i].first);
    byName_[blocks_[i].name] = (int)i;
  }
  std::ifstream d("data/block_states.bin", std::ios::binary);
  uint32_t n = 0;
  if (!d.read((char*)&n, 4)) throw std::runtime_error("blocks: missing data/block_states.bin (tools/gen_block_dump.py)");
  std::vector<uint16_t> raw((size_t)n * 7);  // 5 x u16 transforms + u32 flags, little-endian
  d.read((char*)raw.data(), (std::streamsize)(raw.size() * 2));
  transforms_.resize((size_t)n * 5);
  flags_.resize(n);
  for (uint32_t i = 0; i < n; i++) {
    for (int k = 0; k < 5; k++) transforms_[i * 5 + k] = raw[i * 7 + k];
    flags_[i] = raw[i * 7 + 5] | (uint32_t)raw[i * 7 + 6] << 16;
  }
}

int Blocks::blockOf(int state) const {
  return (int)(std::upper_bound(firstStates_.begin(), firstStates_.end(), state) - firstStates_.begin()) - 1;
}

int Blocks::blockIndex(std::string_view name) const {
  auto it = byName_.find(fullId(name));
  return it == byName_.end() ? -1 : it->second;
}

int Blocks::defaultState(std::string_view name) const {
  int b = blockIndex(name);
  return b < 0 ? -1 : blocks_[b].def;
}

namespace {
// Stride of property i (mixed radix, last property fastest).
int stride(const Blocks::Block& b, size_t i) {
  int s = 1;
  for (size_t k = i + 1; k < b.props.size(); k++) s *= (int)b.props[k].second.size();
  return s;
}
}  // namespace

std::string Blocks::prop(int state, std::string_view key) const {
  const Block& b = block(state);
  for (size_t i = 0; i < b.props.size(); i++)
    if (b.props[i].first == key) return b.props[i].second[(state - b.first) / stride(b, i) % b.props[i].second.size()];
  return "";
}

bool Blocks::hasProp(int state, std::string_view key) const {
  for (auto& p : block(state).props)
    if (p.first == key) return true;
  return false;
}

int Blocks::with(int state, std::string_view key, std::string_view value) const {
  const Block& b = block(state);
  for (size_t i = 0; i < b.props.size(); i++) {
    if (b.props[i].first != key) continue;
    auto& vals = b.props[i].second;
    auto it = std::find(vals.begin(), vals.end(), value);
    if (it == vals.end()) return state;
    int st = stride(b, i), cur = (state - b.first) / st % (int)vals.size();
    return state + ((int)(it - vals.begin()) - cur) * st;
  }
  return state;
}

int Blocks::state(std::string_view name, const std::vector<std::pair<std::string, std::string>>& props) const {
  int s = defaultState(name);
  if (s < 0) return -1;
  for (auto& [k, v] : props) s = with(s, k, v);
  return s;
}

int Blocks::parse(std::string_view s) const {
  size_t br = s.find('[');
  size_t nb = s.find('{');
  std::string_view name = s.substr(0, std::min(br, nb));
  int st = defaultState(name);
  if (st < 0 || br == std::string_view::npos || (nb != std::string_view::npos && nb < br)) return st;
  size_t end = s.find(']', br);
  if (end == std::string_view::npos) return -1;
  for (auto& kv : split(std::string(s.substr(br + 1, end - br - 1)), ',')) {
    auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    st = with(st, kv.substr(0, eq), kv.substr(eq + 1));
  }
  return st;
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

const std::vector<uint8_t>& Blocks::tagMask(std::string_view tag) const {
  std::string key = fullId(tag[0] == '#' ? tag.substr(1) : tag);
  std::lock_guard lock(tagLock_);
  auto it = tags_.find(key);
  if (it != tags_.end()) return it->second;
  std::vector<uint8_t> mask(blocks_.size(), 0);
  for (auto& n : resolveTag("block", key)) {
    int b = blockIndex(n);
    if (b >= 0) mask[b] = 1;
  }
  return tags_.emplace(key, std::move(mask)).first->second;
}

bool Blocks::inTag(int state, std::string_view tag) const { return tagMask(tag)[blockOf(state)]; }

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
