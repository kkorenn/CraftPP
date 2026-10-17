#include "worldgen/material.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>
#include <stdexcept>

#include "world.hpp"
#include "worldgen/block_state.hpp"
#include "worldgen/json.hpp"
#include "worldgen/mth.hpp"

namespace material {

struct Node {
  enum Kind { Block, Bandlands, Sequence, Cond, OreVein,
              Biome, NoiseThreshold, VerticalGradient, YAbove, Water, Temperature, Steep, Not, Hole, AbovePrelim,
              StoneDepth } kind;
  int state = -1;                  // Block
  std::vector<const Node*> kids;   // Sequence; Cond {if_true, then_run}; Not {invert}
  std::vector<uint8_t> biomes;     // Biome: biome id -> in set
  int slot = -1;                   // NoiseThreshold noise slot / OreVein index
  double minT = 0, maxT = 0;
  int a = 0, b = 0, mult = 0, secondary = 0;  // anchors or offset, surface_depth_multiplier, secondary_depth_range
  bool flag = false, ceiling = false;         // add_stone_depth or add_surface_depth; surface_type == ceiling
  PosRandom random;
  int ore = -1, raw = -1, filler = -1;
  float rawChance = 0;
  std::string density, richness, gap;
};

static std::string fullId(const std::string& id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; }
static std::string dataPath(const std::string& root, const std::string& id, const char* folder) {
  std::string f = fullId(id);
  size_t c = f.find(':');
  return root + "/" + f.substr(0, c) + "/worldgen/" + folder + "/" + f.substr(c + 1) + ".json";
}

// --------------------------------------------------------------------------------------------- loading

struct MaterialSystem::Loader {
  struct State { int id; std::vector<std::pair<std::string, std::string>> props; };
  struct BlockInfo { int def = 0; std::vector<State> states; };
  MaterialSystem& s;
  std::map<std::string, BlockInfo> blocks;
  std::map<std::string, const Node*> rules, conds;
  std::map<std::pair<std::string, bool>, int> noiseSlot;

  explicit Loader(MaterialSystem& s) : s(s) {}

  const BlockInfo& block(const std::string& name) {  // from data/blocks.bin (bs::R), ids = global state ids
    std::string id = fullId(name);
    auto it = blocks.find(id);
    if (it != blocks.end()) return it->second;
    const auto& R = bs::R();
    int bi = R.block_index(id);
    if (bi < 0) throw std::runtime_error("material: unknown block " + name);
    const bs::Block& b = R.block(bi);
    BlockInfo& info = blocks[id];
    info.def = b.def - b.first;
    for (int st = b.first; st < b.first + b.count; st++) {
      State x{st, {}};
      for (auto& p : b.props) x.props.push_back({p.name, std::string(R.value(st, p.name))});
      info.states.push_back(std::move(x));
    }
    return info;
  }

  // BlockState.CODEC: block name (default state) or {Name, Properties}.
  int state(const Json& j) {
    const BlockInfo& b = block(j.isString() ? j.asString() : j["Name"].asString());
    const State& def = b.states[b.def];
    if (j.isString() || !j.has("Properties")) return def.id;
    for (auto& st : b.states) {
      bool ok = true;
      for (size_t i = 0; i < st.props.size(); i++) {
        auto* want = j["Properties"].find(st.props[i].first);
        ok = ok && st.props[i].second == (want ? want->asString() : def.props[i].second);
      }
      if (ok) return st.id;
    }
    throw std::runtime_error("material: no matching state for " + j["Name"].asString());
  }

  Node* node(Node::Kind k) {
    s.nodes.push_back(std::make_unique<Node>());
    s.nodes.back()->kind = k;
    return s.nodes.back().get();
  }

  // VerticalAnchor.resolveY with WorldGenerationContext(minGenY = minY, genDepth = height).
  int anchor(const Json& j) {
    if (auto* v = j.find("absolute")) return v->asInt();
    if (auto* v = j.find("above_bottom")) return s.minY + v->asInt();
    if (auto* v = j.find("below_top")) return s.minY + s.height - 1 - v->asInt();
    throw std::runtime_error("material: bad vertical anchor");
  }

  const Node* cond(const Json& j) {
    if (j.isString()) {
      auto key = fullId(j.asString());
      auto it = conds.find(key);
      if (it != conds.end()) return it->second;
      const Node* n = cond(Json::parseFile(dataPath(s.dataRoot, key, "material_condition")));
      return conds[key] = n;
    }
    std::string t = fullId(j["type"].asString());
    Node* n;
    if (t == "minecraft:biome") {
      n = node(Node::Biome);
      n->slot = (int)s.biomeRules.size();
      s.biomeRules.push_back(n);
      n->biomes.assign(s.biomeNames.size(), 0);
      const Json& b = j["biome_is"];
      auto add = [&](const Json& e) {
        int id = s.biomeId(e.asString());  // ponytail: no "#tag" support; vanilla data uses none
        if (id < 0) throw std::runtime_error("material: unknown biome " + e.asString());
        n->biomes[id] = 1;
      };
      if (b.isArray()) for (auto& e : b.arr) add(e);
      else add(b);
    } else if (t == "minecraft:noise_threshold") {
      n = node(Node::NoiseThreshold);
      bool is3d = j.find("is_3d") && j["is_3d"].asBool();
      std::string id = fullId(j["noise"].asString());
      auto& slot = noiseSlot[{id, is3d}];
      if (slot == 0) slot = (int)s.slotNoise.size() + 1, s.slotNoise.push_back(s.randomState->getOrCreateNoise(id)),
        s.slotIs3d.push_back(is3d);
      n->slot = slot - 1;
      n->minT = j["min_threshold"].asDouble();
      n->maxT = j["max_threshold"].asDouble();
    } else if (t == "minecraft:vertical_gradient") {
      n = node(Node::VerticalGradient);
      n->a = anchor(j["true_at_and_below"]);
      n->b = anchor(j["false_at_and_above"]);
      n->random = s.positional(fullId(j["random_name"].asString()));
    } else if (t == "minecraft:y_above") {
      n = node(Node::YAbove);
      n->a = anchor(j["anchor"]);
      n->mult = j["surface_depth_multiplier"].asInt();
      n->flag = j["add_stone_depth"].asBool();
    } else if (t == "minecraft:water") {
      n = node(Node::Water);
      n->a = j["offset"].asInt();
      n->mult = j["surface_depth_multiplier"].asInt();
      n->flag = j["add_stone_depth"].asBool();
    } else if (t == "minecraft:temperature") {
      n = node(Node::Temperature);
    } else if (t == "minecraft:steep") {
      n = node(Node::Steep);
    } else if (t == "minecraft:not") {
      const Node* inv = cond(j["invert"]);
      n = node(Node::Not);
      n->kids = {inv};
    } else if (t == "minecraft:hole") {
      n = node(Node::Hole);
    } else if (t == "minecraft:above_preliminary_surface") {
      n = node(Node::AbovePrelim);
    } else if (t == "minecraft:stone_depth") {
      n = node(Node::StoneDepth);
      n->a = j["offset"].asInt();
      n->flag = j["add_surface_depth"].asBool();
      n->secondary = j["secondary_depth_range"].asInt();
      n->ceiling = j["surface_type"].asString() == "ceiling";
    } else {
      throw std::runtime_error("material: unknown condition type " + t);
    }
    return n;
  }

  const Node* rule(const Json& j) {
    if (j.isString()) {
      auto key = fullId(j.asString());
      auto it = rules.find(key);
      if (it != rules.end()) return it->second;
      const Node* n = rule(Json::parseFile(dataPath(s.dataRoot, key, "material_rule")));
      return rules[key] = n;
    }
    std::string t = fullId(j["type"].asString());
    Node* n;
    if (t == "minecraft:block") {
      n = node(Node::Block);
      n->state = state(j["result_state"]);
    } else if (t == "minecraft:bandlands") {
      n = node(Node::Bandlands);
    } else if (t == "minecraft:sequence") {
      if (j["sequence"].size() == 0) throw std::runtime_error("material: need at least 1 rule for a sequence");
      std::vector<const Node*> kids;
      for (auto& e : j["sequence"].arr) kids.push_back(rule(e));
      n = node(Node::Sequence);
      n->kids = std::move(kids);
    } else if (t == "minecraft:condition") {
      const Node* c = cond(j["if_true"]);
      const Node* r = rule(j["then_run"]);
      n = node(Node::Cond);
      n->kids = {c, r};
    } else if (t == "minecraft:ore_vein") {
      n = node(Node::OreVein);
      n->ore = state(j["ore_block"]);
      n->raw = state(j["raw_ore_block"]);
      n->filler = state(j["filler_block"]);
      n->rawChance = (float)j["raw_ore_chance"].asDouble();
      // ponytail: density functions must be registry ids (all vanilla data is); inline ones need a DF handle API.
      n->density = fullId(j["density"].asString());
      n->richness = fullId(j["richness"].asString());
      n->gap = fullId(j["filler_gap"].asString());
      n->slot = (int)s.oreRules.size();
      n->random = s.positional("minecraft:ore");
      s.oreRules.push_back(n);
    } else {
      throw std::runtime_error("material: unknown rule type " + t);
    }
    return n;
  }
};

int MaterialSystem::stateId(const std::string& name) const {
  try {
    auto& b = loader->block(name);
    return b.states[b.def].id;
  } catch (const std::runtime_error&) {
    return -1;
  }
}

int MaterialSystem::biomeId(const std::string& name) const {
  std::string f = fullId(name);
  auto it = std::lower_bound(biomeNames.begin(), biomeNames.end(), f);
  return it != biomeNames.end() && *it == f ? (int)(it - biomeNames.begin()) : -1;
}

static Json jsonString(const std::string& v) {
  Json j;
  j.type = Json::String, j.str = v;
  return j;
}
void MaterialSystem::parseRuleFile(const std::string& id) { loader->rule(jsonString(id)); }
void MaterialSystem::parseConditionFile(const std::string& id) { loader->cond(jsonString(id)); }

MaterialSystem::~MaterialSystem() = default;

PosRandom MaterialSystem::positional(const std::string& id) const {
  auto r = randomState->random().fromHashOf(fullId(id));  // fromHashOf(id).forkPositional() = two nextLong()
  PosRandom p;
  p.legacy = randomState->legacy();  // Legacy forkPositional = one nextLong()
  p.lo = r->nextLong();
  if (!p.legacy) p.hi = r->nextLong();
  return p;
}

MaterialSystem::MaterialSystem(RandomState& rs, const std::string& settingsId, const std::string& root,
                               bool keepLoader)
    : randomState(&rs), dataRoot(root) {
  loader = std::make_unique<Loader>(*this);
  auto range = [&](const char* name, int& lo, int& hi) {
    lo = INT_MAX, hi = INT_MIN;
    for (auto& st : loader->block(name).states) lo = std::min(lo, st.id), hi = std::max(hi, st.id);
  };
  range("minecraft:water", waterMin, waterMax);
  range("minecraft:lava", lavaMin, lavaMax);
  air = stateId("air"), caveAir = stateId("cave_air"), voidAir = stateId("void_air");
  snowBlock = stateId("snow_block"), packedIce = stateId("packed_ice");

  for (auto& e : std::filesystem::directory_iterator(root + "/minecraft/worldgen/biome"))
    if (e.path().extension() == ".json") biomeNames.push_back("minecraft:" + e.path().stem().string());
  std::sort(biomeNames.begin(), biomeNames.end());
  for (auto& b : biomeNames) {
    Json j = Json::parseFile(dataPath(root, b, "biome"));
    auto* m = j.find("temperature_modifier");
    climate.push_back({(float)j["temperature"].asDouble(), m && m->asString() == "frozen"});
  }
  erodedBadlands = biomeId("eroded_badlands");
  frozenOcean = biomeId("frozen_ocean");
  deepFrozenOcean = biomeId("deep_frozen_ocean");
  biomeZoomSeed = obfuscateSeed(rs.seed());

  {  // Biome.TEMPERATURE_NOISE / FROZEN_TEMPERATURE_NOISE / BIOME_INFO_NOISE (WorldgenRandom over legacy = legacy)
    LegacyRandomSource r1(1234L), r2(3456L), r3(2345L);
    temperatureNoise = std::make_shared<SimplexNoise>(r1, true);
    NoiseStack::Builder b;
    b.add(std::make_shared<SimplexNoise>(r2, true), 1.0, 0.14285715F);
    b.add(std::make_shared<SimplexNoise>(r2, true), 0.5, 0.2857143F);
    b.add(std::make_shared<SimplexNoise>(r2, true), 0.25, 0.5714286F);
    frozenTemperatureNoise = b.build();
    biomeInfoNoise = std::make_shared<SimplexNoise>(r3, true);
  }

  Json settings = Json::parseFile(dataPath(root, settingsId, "noise_settings"));
  defaultBlock = loader->state(settings["default_block"]);
  seaLevel_ = settings["sea_level"].asInt();
  minY = settings["noise"]["min_y"].asInt();
  height = settings["noise"]["height"].asInt();
  // ponytail: biome y clamp + maxY use the noise range, not the chunk's (same result: every vanilla dimension's
  // biomes are y-independent above the noise top); pass the chunk layout if a datapack needs it.
  const Json& csl = settings["noise_router"]["chunk_surface_level"];
  if (csl.isString()) preliminarySurfaceFunction = fullId(csl.asString());  // constant (nether, end): never sampled

  if (rs.legacy() != (settings.find("legacy_random_source") && settings["legacy_random_source"].asBool()))
    throw std::runtime_error("material: RandomState legacy flag != noise_settings legacy_random_source");
  noiseRandom.legacy = rs.legacy();  // RandomState.random = newInstance(seed).forkPositional()
  if (rs.legacy()) {
    LegacyRandomSource r(rs.seed());
    noiseRandom.lo = r.nextLong();
  } else {
    XoroshiroRandomSource r(rs.seed());
    noiseRandom.lo = r.nextLong();
    noiseRandom.hi = r.nextLong();
  }
  clayBandsOffsetNoise = rs.getOrCreateNoise("minecraft:clay_bands_offset");
  {  // generateBands(noiseRandom.fromHashOf("minecraft:clay_bands"))
    auto random = rs.random().fromHashOf("minecraft:clay_bands");
    RandomSource& r = *random;
    const int n = (int)clayBands.size();
    clayBands.fill(blocks::terracotta);
    for (int i = 0; i < n; i++) {
      i += r.nextInt(5) + 1;
      if (i < n) clayBands[i] = blocks::orange_terracotta;
    }
    auto makeBands = [&](int baseWidth, int state) {
      int bandCount = r.nextIntBetweenInclusive(6, 15);
      for (int i = 0; i < bandCount; i++) {
        int width = baseWidth + r.nextInt(3);
        int start = r.nextInt(n);
        for (int p = 0; start + p < n && p < width; p++) clayBands[start + p] = state;
      }
    };
    makeBands(1, blocks::yellow_terracotta);
    makeBands(2, blocks::brown_terracotta);
    makeBands(1, blocks::red_terracotta);
    int whiteBandCount = r.nextIntBetweenInclusive(9, 15);
    for (int ix = 0, start = 0; ix < whiteBandCount && start < n; start += r.nextInt(16) + 4) {
      clayBands[start] = blocks::white_terracotta;
      if (start - 1 > 0 && r.nextBoolean()) clayBands[start - 1] = blocks::light_gray_terracotta;
      if (start + 1 < n && r.nextBoolean()) clayBands[start + 1] = blocks::light_gray_terracotta;
      ix++;
    }
  }
  surfaceNoise = rs.getOrCreateNoise("minecraft:surface");
  surfaceSecondaryNoise = rs.getOrCreateNoise("minecraft:surface_secondary");
  badlandsPillarNoise = rs.getOrCreateNoise("minecraft:badlands_pillar");
  badlandsPillarRoofNoise = rs.getOrCreateNoise("minecraft:badlands_pillar_roof");
  badlandsSurfaceNoise = rs.getOrCreateNoise("minecraft:badlands_surface");
  icebergPillarNoise = rs.getOrCreateNoise("minecraft:iceberg_pillar");
  icebergPillarRoofNoise = rs.getOrCreateNoise("minecraft:iceberg_pillar_roof");
  icebergSurfaceNoise = rs.getOrCreateNoise("minecraft:iceberg_surface");

  root_ = loader->rule(settings["material_rule"]);
  if (!keepLoader) loader.reset();
}

// --------------------------------------------------------------------------------------------- MaterialSystem helpers

int MaterialSystem::getSurfaceDepth(int blockX, int blockZ) const {
  double noiseValue = surfaceNoise->get((double)blockX, 0.0, (double)blockZ);
  return Mth::d2i(noiseValue * 2.75 + 3.0 + noiseRandom.at(blockX, 0, blockZ).nextDouble() * 0.25);
}

double MaterialSystem::getSurfaceSecondary(int blockX, int blockZ) const {
  return surfaceSecondaryNoise->get((double)blockX, 0.0, (double)blockZ);
}

int MaterialSystem::getBand(int x, int y, int z) const {
  float v = clayBandsOffsetNoise->get((double)x, 0.0, (double)z) * 4.0F;
  int offset = Mth::d2i(std::floor((double)v + 0.5));  // Math.round(float): exact in double
  int n = (int)clayBands.size();
  return clayBands[(y + offset + n) % n];
}

// BiomeManager.getFiddledDistance's per-corner offsets: depend only on the quart corner (memoized per chunk by Eval).
MaterialSystem::Fiddle MaterialSystem::fiddles(int cx, int cy, int cz) const {
  auto fiddle = [](int64_t r) { return ((double)(((r >> 24) % 1024 + 1024) % 1024) / 1024.0 - 0.5) * 0.9; };
  auto lcg = [](int64_t r, int64_t c) {
    uint64_t u = (uint64_t)r;
    u *= u * 6364136223846793005ull + 1442695040888963407ull;
    return (int64_t)(u + (uint64_t)c);
  };
  int64_t r = lcg(biomeZoomSeed, cx);
  r = lcg(r, cy), r = lcg(r, cz), r = lcg(r, cx), r = lcg(r, cy), r = lcg(r, cz);
  Fiddle f;
  f.x = fiddle(r);
  r = lcg(r, biomeZoomSeed);
  f.y = fiddle(r);
  r = lcg(r, biomeZoomSeed);
  f.z = fiddle(r);
  return f;
}

template <class FiddleAt>
int MaterialSystem::zoomed(MaterialContext& ctx, int x, int y, int z, bool clampY, FiddleAt&& at) const {
  int absX = x - 2, absY = y - 2, absZ = z - 2;
  int parentX = absX >> 2, parentY = absY >> 2, parentZ = absZ >> 2;
  double fractX = (absX & 3) / 4.0, fractY = (absY & 3) / 4.0, fractZ = (absZ & 3) / 4.0;
  int minI = 0;
  double minFiddledDistance = INFINITY;
  for (int i = 0; i < 8; i++) {
    bool xEven = (i & 4) == 0, yEven = (i & 2) == 0, zEven = (i & 1) == 0;
    int cx = xEven ? parentX : parentX + 1, cy = yEven ? parentY : parentY + 1, cz = zEven ? parentZ : parentZ + 1;
    double dx = xEven ? fractX : fractX - 1.0, dy = yEven ? fractY : fractY - 1.0, dz = zEven ? fractZ : fractZ - 1.0;
    const Fiddle& f = at(cx, cy, cz);
    double next = Mth::square(dz + f.z) + Mth::square(dy + f.y) + Mth::square(dx + f.x);
    if (minFiddledDistance > next) minI = i, minFiddledDistance = next;
  }
  int bx = (minI & 4) == 0 ? parentX : parentX + 1;
  int by = (minI & 2) == 0 ? parentY : parentY + 1;
  int bz = (minI & 1) == 0 ? parentZ : parentZ + 1;
  if (clampY) by = std::clamp(by, minY >> 2, (minY >> 2) + (height >> 2) - 1);  // ChunkAccess.getNoiseBiome
  return ctx.noiseBiome(bx, by, bz);
}

int MaterialSystem::zoomedBiome(MaterialContext& ctx, int x, int y, int z, bool clampY) const {
  Fiddle tmp;
  return zoomed(ctx, x, y, z, clampY, [&](int cx, int cy, int cz) -> const Fiddle& { return tmp = fiddles(cx, cy, cz); });
}

float MaterialSystem::biomeTemperature(int biome, int x, int y, int z) const {
  float t = climate[biome].temperature;
  if (climate[biome].frozen) {  // TemperatureModifier.FROZEN
    double large = (double)(frozenTemperatureNoise->get(x * 0.05, z * 0.05) * 7.0F);
    double edge = (double)biomeInfoNoise->get(x * 0.2, z * 0.2);
    if (large + edge < 0.3 && (double)biomeInfoNoise->get(x * 0.09, z * 0.09) < 0.8) t = 0.2F;
  }
  int snowLevel = seaLevel_ + 17;
  if (y > snowLevel) {  // getHeightAdjustedTemperature
    float v = temperatureNoise->get((double)((float)x / 8.0F), (double)((float)z / 8.0F)) * 8.0F;
    return t - (v + (float)y - (float)snowLevel) * 0.05F / 40.0F;
  }
  return t;
}

int64_t MaterialSystem::obfuscateSeed(int64_t seed) {
  // Guava Hashing.sha256().hashLong(seed).asLong(): SHA-256 of the 8 little-endian bytes, first 8 bytes read LE.
  static const uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
      0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
      0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
      0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  uint8_t blk[64] = {};
  for (int i = 0; i < 8; i++) blk[i] = (uint8_t)((uint64_t)seed >> (8 * i));
  blk[8] = 0x80;
  blk[63] = 64;  // message length in bits
  uint32_t w[64], h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](uint32_t x, int n) { return x >> n | x << (32 - n); };
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 | (uint32_t)blk[4 * i + 2] << 8 | blk[4 * i + 3];
  for (int i = 16; i < 64; i++)
    w[i] = w[i - 16] + (rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
           (rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ w[i - 2] >> 10);
  uint32_t v[8];
  std::copy(h, h + 8, v);
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = v[7] + (rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K[i] + w[i];
    uint32_t t2 = (rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
    std::copy_backward(v, v + 7, v + 8);
    v[4] += t1;
    v[0] = t1 + t2;
  }
  uint64_t out = 0;
  for (int i = 0; i < 8; i++) out |= (uint64_t)(((h[i / 4] + v[i / 4]) >> (24 - 8 * (i % 4))) & 0xFF) << (8 * i);
  return (int64_t)out;
}

// --------------------------------------------------------------------------------------------- evaluation
// MaterialRuleContext + BlockColumn + WORLD_SURFACE_WG heightmap of the chunk being built. The lazy XZ/Y caches of
// vanilla are pure (no side effects), so only the expensive ones are kept (biome, noises, secondary, min surface).

struct Eval {
  const MaterialSystem& s;
  MaterialContext& ctx;
  Chunk& chunk;
  int minX, minZ;
  bool clampBiomeY;
  DensityVolume volume{}, prelimVolume{};
  std::vector<DensityBuffer> ore;  // 2 per ore rule: density, richness (prefilled over volume)
  std::unique_ptr<DensityBuffer> prelim;
  int hm[256];  // Heightmap firstAvailable (top non-air + 1, or minY), index z*16+x
  int col = 0;  // current column x + z*16 (local)

  int64_t lastXZ = 0, lastY = 0;
  int blockX = 0, blockZ = 0, gradX = 0, gradZ = 0, surfaceDepth = 0;
  int blockY = 0, waterHeight = 0, stoneBelow = 0, stoneAbove = 0;
  int64_t secondaryAt = -1, minSurfaceAt = -1, biomeAt = -1;
  double secondary = 0;
  int minSurface = 0, biome = 0;
  std::vector<double> noiseVal;
  std::vector<int64_t> noiseAt;
  std::vector<int8_t> biomeFixed;  // per biome rule: -1 lazy, 0 never, 1 always (BiomeCondition.compile)

  Eval(const MaterialSystem& s, MaterialContext& ctx, Chunk& chunk, bool clampBiomeY)
      : s(s), ctx(ctx), chunk(chunk), minX(chunk.cx * 16), minZ(chunk.cz * 16), clampBiomeY(clampBiomeY),
        noiseVal(s.slotNoise.size()), noiseAt(s.slotNoise.size(), -1), biomeFixed(s.biomeRules.size(), -1) {
    std::fill(hm, hm + 256, INT_MIN);
  }
  // MaterialRuleContext ctor + OreVeinRule.compile (getDensitiesInChunk(density/richness, prefill=true)).
  void prefill(const DensityVolume& v) {
    volume = v;
    prelimVolume = {v.sizeX, 1, v.sizeZ, v.minBlockX, 0, v.minBlockZ};
    for (const Node* r : s.oreRules)
      for (const std::string* df : {&r->density, &r->richness}) {
        ore.emplace_back(volume.size());
        ctx.sampleDensityVolume(*df, ore.back(), volume);
      }
  }

  int& firstAvailable(int i) {  // lazily primed from the blocks (== vanilla's incrementally kept heightmap)
    if (hm[i] == INT_MIN) {
      int y = s.minY + s.height - 1;
      while (y >= s.minY && s.isAir(chunk.get(i & 15, y, i >> 4))) y--;
      hm[i] = y + 1;
    }
    return hm[i];
  }
  int height(int x, int z) { return firstAvailable((z & 15) * 16 + (x & 15)) - 1; }  // ChunkAccess.getHeight
  int get(int y) const {
    if (y < s.minY || y >= s.minY + s.height) return s.voidAir;
    return chunk.get(col & 15, y, col >> 4);
  }
  void set(int y, int state) {  // BlockColumn.setBlock -> ProtoChunk.setBlockState (+ Heightmap.update NOT_AIR)
    if (y < s.minY || y > s.minY + s.height - 1) return;
    int x = col & 15, z = col >> 4;
    chunk.set(x, y, z, (uint16_t)state);
    if (state_flags()[size_t(state)] & STATE_FLUID) chunk.level.post.push_back(uint32_t(x | z << 4 | (y - chunk.min_y) << 8));
    int& fa = firstAvailable(col);
    if (y <= fa - 2) return;
    if (!s.isAir(state)) {
      if (y >= fa) fa = y + 1;
    } else if (fa - 1 == y) {
      int yy = y - 1;
      while (yy >= s.minY && s.isAir(chunk.get(x, yy, z))) yy--;
      fa = yy + 1;
    }
  }

  void updateXZ(int x, int z, int gx, int gz) {
    lastXZ++, lastY++;
    blockX = x, blockZ = z, gradX = gx, gradZ = gz;
    surfaceDepth = s.getSurfaceDepth(x, z);
  }
  void updateY(int above, int below, int water, int y) {
    lastY++;
    blockY = y, waterHeight = water, stoneBelow = below, stoneAbove = above;
  }

  double getSurfaceSecondary() {
    if (secondaryAt != lastXZ) secondaryAt = lastXZ, secondary = s.getSurfaceSecondary(blockX, blockZ);
    return secondary;
  }
  int getBiome() {
    if (biomeAt != lastY) biomeAt = lastY, biome = zoomedBiome(blockX, blockY, blockZ, clampBiomeY);
    return biome;
  }
  // MaterialSystem::zoomedBiome with the corner offsets memoized over this chunk's quart corners.
  static constexpr int FX = 6, FY = 100;
  std::vector<MaterialSystem::Fiddle> fid;
  std::vector<uint8_t> fidSet;
  MaterialSystem::Fiddle fidTmp;
  int zoomedBiome(int x, int y, int z, bool clampY) {
    if (fid.empty()) fid.resize(FX * FX * FY), fidSet.assign(FX * FX * FY, 0);
    int qx0 = (minX - 2) >> 2, qz0 = (minZ - 2) >> 2, qy0 = (s.minY - 2) >> 2;
    return s.zoomed(ctx, x, y, z, clampY, [&](int cx, int cy, int cz) -> const MaterialSystem::Fiddle& {
      unsigned ix = unsigned(cx - qx0), iy = unsigned(cy - qy0), iz = unsigned(cz - qz0);
      if (ix >= FX || iz >= FX || iy >= FY) return fidTmp = s.fiddles(cx, cy, cz);
      size_t i = (iy * FX + iz) * FX + ix;
      if (!fidSet[i]) fid[i] = s.fiddles(cx, cy, cz), fidSet[i] = 1;
      return fid[i];
    });
  }
  int getMinSurfaceLevel() {
    if (minSurfaceAt != lastXZ) {
      minSurfaceAt = lastXZ;
      int rx = blockX - prelimVolume.minBlockX, rz = blockZ - prelimVolume.minBlockZ;
      float level;
      if (rx >= 0 && rz >= 0 && rx < prelimVolume.sizeX && rz < prelimVolume.sizeZ) {
        if (!prelim) {
          prelim = std::make_unique<DensityBuffer>(prelimVolume.size());
          ctx.sampleDensityVolume(s.preliminarySurfaceFunction, *prelim, prelimVolume);
        }
        level = prelim->get(rx + rz * prelimVolume.sizeX);
      } else {
        level = ctx.sampleDensity(s.preliminarySurfaceFunction, blockX, 0, blockZ);
      }
      minSurface = Mth::floor(level) + surfaceDepth - 8;
    }
    return minSurface;
  }
  double noise(int slot) {
    int64_t stamp = s.slotIs3d[slot] ? lastY : lastXZ;
    if (noiseAt[slot] != stamp) {
      noiseAt[slot] = stamp;
      noiseVal[slot] = s.slotIs3d[slot] ? s.slotNoise[slot]->get((double)blockX, (double)blockY, (double)blockZ)
                                        : s.slotNoise[slot]->get((double)blockX, 0.0, (double)blockZ);
    }
    return noiseVal[slot];
  }
  float oreDensity(int buffer, const std::string& df) {
    int rx = blockX - volume.minBlockX, ry = blockY - volume.minBlockY, rz = blockZ - volume.minBlockZ;
    if (rx >= 0 && ry >= 0 && rz >= 0 && rx < volume.sizeX && ry < volume.sizeY && rz < volume.sizeZ)
      return ore[buffer].get(ry + (rx + rz * volume.sizeX) * volume.sizeY);
    return ctx.sampleDensity(df, blockX, blockY, blockZ);
  }

  bool test(const Node* n) {
    switch (n->kind) {
      case Node::Biome: return biomeFixed[n->slot] >= 0 ? biomeFixed[n->slot] : n->biomes[getBiome()];
      case Node::NoiseThreshold: {
        double v = noise(n->slot);
        return v >= n->minT && v <= n->maxT;
      }
      case Node::VerticalGradient: {
        if (blockY <= n->a) return true;
        if (blockY >= n->b) return false;
        double t = ((double)blockY - (double)n->a) / ((double)n->b - (double)n->a);
        double probability = 1.0 + t * (0.0 - 1.0);  // Mth.map(y, a, b, 1, 0)
        return (double)n->random.at(blockX, blockY, blockZ).nextFloat() < probability;
      }
      case Node::YAbove:
        return blockY + (n->flag ? stoneAbove : 0) >= n->a + surfaceDepth * n->mult;
      case Node::Water:
        return waterHeight == INT_MIN || blockY + (n->flag ? stoneAbove : 0) >= waterHeight + n->a + surfaceDepth * n->mult;
      case Node::Temperature:  // coldEnoughToSnow = !warmEnoughToRain
        return !(s.biomeTemperature(getBiome(), blockX, blockY, blockZ) >= 0.15F);
      case Node::Steep: return gradX <= -4 || gradZ >= 4;
      case Node::Not: return !test(n->kids[0]);
      case Node::Hole: return surfaceDepth <= 0;
      case Node::AbovePrelim: return blockY >= getMinSurfaceLevel();
      case Node::StoneDepth: {
        int stoneDepth = n->ceiling ? stoneBelow : stoneAbove;
        int sd = n->flag ? surfaceDepth : 0;
        int sec = 0;
        if (n->secondary != 0) {
          double t = (getSurfaceSecondary() - -1.0) / (1.0 - -1.0);
          sec = Mth::d2i(0.0 + t * ((double)n->secondary - 0.0));  // (int)Mth.map(v, -1, 1, 0, range)
        }
        return stoneDepth <= 1 + n->a + sd + sec;
      }
      default: throw std::logic_error("material: rule used as condition");
    }
  }

  int apply(const Node* n) {  // RuleEvaluator.tryApply at (blockX, blockY, blockZ); -1 = null
    switch (n->kind) {
      case Node::Block: return n->state;
      case Node::Bandlands: return s.getBand(blockX, blockY, blockZ);
      case Node::Sequence:
        for (const Node* k : n->kids) {
          int r = apply(k);
          if (r >= 0) return r;
        }
        return -1;
      case Node::Cond: return test(n->kids[0]) ? apply(n->kids[1]) : -1;
      case Node::OreVein: {
        float density = oreDensity(2 * n->slot, n->density);
        if (density <= 0.0F) return -1;
        auto random = n->random.at(blockX, blockY, blockZ);
        if (random.nextFloat() > density) return -1;
        float richness = oreDensity(2 * n->slot + 1, n->richness);
        if (random.nextFloat() < richness && ctx.sampleDensity(n->gap, blockX, blockY, blockZ) < 0.0F)
          return random.nextFloat() < n->rawChance ? n->raw : n->ore;
        return n->filler;
      }
      default: throw std::logic_error("material: condition used as rule");
    }
  }
};

static int gradientX(Eval& ev, int x, int z) { return ev.height(std::min(x + 1, 15), z) - ev.height(std::max(x - 1, 0), z); }
static int gradientZ(Eval& ev, int x, int z) { return ev.height(x, std::min(z + 1, 15)) - ev.height(x, std::max(z - 1, 0)); }

void MaterialSystem::buildSurface(Chunk& chunk, MaterialContext& ctx, const std::vector<uint8_t>* possible) const {
  Eval ev(*this, ctx, chunk, true);
  if (possible)
    for (const Node* n : biomeRules) {
      bool any = false, all = true;
      for (size_t b = 0; b < n->biomes.size(); b++)
        if (b < possible->size() && (*possible)[b]) any |= n->biomes[b] != 0, all &= n->biomes[b] != 0;
      ev.biomeFixed[n->slot] = !any ? 0 : all ? 1 : -1;
    }
  int top = minY - 1;  // highest non-air block = getHighestFilledSectionIndex (section with any non-air)
  for (int i = 0; i < 256; i++) top = std::max(top, ev.firstAvailable(i) - 1);
  int maxBlockY = (top < minY ? (minY >> 4) - 1 : top >> 4) * 16 + 15;
  ev.prefill({16, std::max(maxBlockY - minY + 1, 1), 16, ev.minX, minY, ev.minZ});
  const int maxY = minY + height - 1;

  for (int x = 0; x < 16; x++) {
    for (int z = 0; z < 16; z++) {
      int blockX = ev.minX + x, blockZ = ev.minZ + z;
      ev.col = x + z * 16;
      int startingHeight = ev.height(x, z) + 1;
      int surfaceBiome = ev.zoomedBiome(blockX, startingHeight, blockZ, true);
      if (surfaceBiome == erodedBadlands) erodedBadlandsExtension(ev, blockX, blockZ, startingHeight);

      int h = ev.height(x, z) + 1;
      ev.updateXZ(blockX, blockZ, gradientX(ev, x, z), gradientZ(ev, x, z));
      int stoneAboveDepth = 0, waterHeight = INT_MIN, nextCeilingStoneY = INT_MAX;
      const int endY = minY;
      for (int y = h; y >= endY; y--) {
        int old = ev.get(y);
        if (isAir(old)) {
          stoneAboveDepth = 0;
          waterHeight = INT_MIN;
        } else if (hasFluid(old)) {
          if (waterHeight == INT_MIN) waterHeight = y + 1;
        } else {
          if (nextCeilingStoneY >= y) {
            nextCeilingStoneY = MIN_Y << 4;  // DimensionType.WAY_BELOW_MIN_Y
            for (int ly = y - 1; ly >= endY - 1; ly--) {
              int next = ev.get(ly);
              if (isAir(next) || hasFluid(next)) {  // !isStone
                nextCeilingStoneY = ly + 1;
                break;
              }
            }
          }
          stoneAboveDepth++;
          int stoneBelowDepth = y - nextCeilingStoneY + 1;
          ev.updateY(stoneAboveDepth, stoneBelowDepth, waterHeight, y);
          if (y >= minY && y <= maxY) {
            int state = ev.apply(root_);
            if (state >= 0) ev.set(y, state);
          }
        }
      }
      if (surfaceBiome == frozenOcean || surfaceBiome == deepFrozenOcean)
        frozenOceanExtension(ev, ev.getMinSurfaceLevel(), surfaceBiome, blockX, blockZ, startingHeight);
    }
  }
}

int MaterialSystem::topMaterial(Chunk& chunk, MaterialContext& ctx, int x, int y, int z, bool underFluid) const {
  Eval ev(*this, ctx, chunk, false);
  ev.prefill({1, 1, 1, x, y, z});
  int lx = x & 15, lz = z & 15;
  ev.col = lx + lz * 16;
  ev.updateXZ(x, z, gradientX(ev, lx, lz), gradientZ(ev, lx, lz));
  ev.updateY(1, 1, underFluid ? y + 1 : INT_MIN, y);
  return ev.apply(root_);
}

void MaterialSystem::erodedBadlandsExtension(Eval& ev, int blockX, int blockZ, int h) const {
  double pillarBuffer = std::min(std::abs(badlandsSurfaceNoise->get((double)blockX, 0.0, (double)blockZ) * 8.25),
                                 (double)(badlandsPillarNoise->get(blockX * 0.2, 0.0, blockZ * 0.2) * 15.0F));
  if (pillarBuffer <= 0.0 || std::isnan(pillarBuffer)) return;
  double pillarFloor = std::abs(badlandsPillarRoofNoise->get(blockX * 0.75, 0.0, blockZ * 0.75) * 1.5);
  double extensionTop = 64.0 + std::min(pillarBuffer * pillarBuffer * 2.5, std::ceil(pillarFloor * 50.0) + 24.0);
  int startY = Mth::floor(extensionTop);
  if (h > startY) return;
  for (int y = startY; y >= minY; y--) {
    int old = ev.get(y);
    if (old == defaultBlock) break;  // ponytail: is(defaultBlock.getBlock()) == state compare (stone has one state)
    if (isWater(old)) return;
  }
  for (int y = startY; y >= minY && isAir(ev.get(y)); y--) ev.set(y, defaultBlock);
}

void MaterialSystem::frozenOceanExtension(Eval& ev, int minSurfaceLevel, int biome, int blockX, int blockZ, int h) const {
  double iceberg = std::min(std::abs(icebergSurfaceNoise->get((double)blockX, 0.0, (double)blockZ) * 8.25),
                            (double)(icebergPillarNoise->get(blockX * 1.28, 0.0, blockZ * 1.28) * 15.0F));
  if (iceberg <= 1.8 || std::isnan(iceberg)) return;
  double icebergRoof = std::abs(icebergPillarRoofNoise->get(blockX * 1.17, 0.0, blockZ * 1.17) * 1.5);
  double top = std::min(iceberg * iceberg * 1.2, std::ceil(icebergRoof * 40.0) + 14.0);
  if (biomeTemperature(biome, blockX, seaLevel_, blockZ) > 0.1F) top -= 2.0;  // shouldMeltFrozenOceanIcebergSlightly
  if (top <= 2.0 || std::isnan(top)) return;
  double extensionBottom = seaLevel_ - top - 7.0;
  top += seaLevel_;
  double extensionTop = top;
  auto random = noiseRandom.at(blockX, 0, blockZ);
  int maxSnowDepth = 2 + random.nextInt(4);
  int minSnowHeight = seaLevel_ + 18 + random.nextInt(10);
  int snowDepth = 0;
  for (int y = std::max(h, Mth::d2i(top) + 1); y >= minSurfaceLevel; y--) {
    if ((isAir(ev.get(y)) && y < Mth::d2i(extensionTop) && random.nextDouble() > 0.01) ||
        (isWater(ev.get(y)) && y > Mth::d2i(extensionBottom) && y < seaLevel_ && random.nextDouble() > 0.15)) {
      if (snowDepth <= maxSnowDepth && y > minSnowHeight) {
        ev.set(y, snowBlock);
        snowDepth++;
      } else {
        ev.set(y, packedIce);
      }
    }
  }
}

}  // namespace material
