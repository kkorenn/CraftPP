#include "worldgen/features.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>

#include "worldgen/material.hpp"
#include "worldgen/noise.hpp"
#include "worldgen/overworld.hpp"

namespace worldgen {
namespace feat {
namespace {
const bs::Registry& R() { return bs::R(); }

P vec3(const Json* j) { return j ? P{(*j)[0].asInt(), (*j)[1].asInt(), (*j)[2].asInt()} : P{}; }

// material::MaterialContext for the biome zoom: noise biomes from the region's stored chunk biomes.
struct BiomeCtx : material::MaterialContext {
  Level& l;
  explicit BiomeCtx(Level& l) : l(l) {}
  float sampleDensity(const std::string&, int, int, int) override { return 0; }
  void sampleDensityVolume(const std::string&, DensityBuffer&, const DensityVolume&) override {}
  int noiseBiome(int qx, int qy, int qz) override {
    ProtoChunk* p = l.region().chunk(qx >> 2, qz >> 2);
    int y = qy - (MIN_Y >> 2);
    if (p && p->chunk) return p->chunk->get_biome(qx & 3, y, qz & 3);
    // Outside the readable region vanilla would crash; sample the biome source instead.
    auto& t = *l.features().ow.terrain;
    thread_local std::unique_ptr<TerrainGenerator::ClimateSampler> s;
    if (!s) s = t.climateSampler();
    return t.uncachedBiome(*s, qx, qy, qz);
  }
};
}  // namespace

bs::Dir dir_by_name(const std::string& n0) {
  std::string n = strip_ns(n0);
  for (int d = 0; d < 6; d++)
    if (n == dir_name(bs::Dir(d))) return bs::Dir(d);
  throw std::runtime_error("direction " + n);
}

// ---------------------------------------------------------------- Level
uint16_t Level::get(const P& p) const {
  if (outside(p.y)) return blocks::void_air;
  ProtoChunk* c = r.chunk(p.x >> 4, p.z >> 4);
  if (!c || !c->chunk) return blocks::air;  // outside the region: vanilla crashes; never expected
  return c->chunk->get(p.x & 15, p.y, p.z & 15);
}

bool Level::set(const P& p, int state) {
  if (!r.set(p.x, p.y, p.z, uint16_t(state))) return false;
  int be = R().block(R().block_of(state)).block_entity;
  if (be >= 0) {  // WorldGenRegion.setBlock: DUMMY block entity for proto chunks
    Chunk& c = *r.chunk(p.x >> 4, p.z >> 4)->chunk;
    c.block_entities.push_back({uint8_t(p.x & 15), uint8_t(p.z & 15), int16_t(p.y), be, {0x0a, 0x00}});
  }
  return true;
}

BlockEntity* Level::block_entity(const P& p) {
  ProtoChunk* c = r.chunk(p.x >> 4, p.z >> 4);
  if (!c || !c->chunk) return nullptr;
  for (auto& e : c->chunk->block_entities)
    if (e.x == (p.x & 15) && e.z == (p.z & 15) && e.y == p.y) return &e;
  return nullptr;
}

int Level::height(Heightmap h, int x, int z) const {
  ProtoChunk* c = r.chunk(x >> 4, z >> 4);
  return c && c->chunk ? c->height(h, x & 15, z & 15) : MIN_Y;
}

int Level::biome(const P& p) const {
  BiomeCtx ctx(const_cast<Level&>(*this));
  return f.materials->zoomedBiome(ctx, p.x, p.y, p.z, true);
}

RandomSource& Level::random() {
  if (!rand) rand = f.regionRandom->at(r.cx * 16, 0, r.cz * 16);
  return *rand;
}

int64_t Level::seed() const { return f.ow.seed; }

float Level::temperature(int biome, const P& p) const { return f.materials->biomeTemperature(biome, p.x, p.y, p.z); }

void Level::mark_post_processing(const P& p) {
  if (outside(p.y)) return;
  ProtoChunk* c = r.chunk(p.x >> 4, p.z >> 4);
  if (c && c->chunk) c->post_processing.push_back((p.y - MIN_Y) << 8 | (p.z & 15) << 4 | (p.x & 15));
}

void Level::schedule_leaf_tick(const P& p) {
  if (outside(p.y)) return;
  ProtoChunk* c = r.chunk(p.x >> 4, p.z >> 4);
  if (c && c->chunk) c->leaf_ticks.push_back((p.y - MIN_Y) << 8 | (p.z & 15) << 4 | (p.x & 15));
}

bool Level::should_freeze(int b, const P& p, bool checkNeighbors) const {
  if (temperature(b, p) >= 0.15F) return false;
  if (outside(p.y)) return false;  // block light is 0 in worldgen (< 10)
  int s = get(p);
  if (s == blocks::water) {  // Fluids.WATER source in a LiquidBlock
    if (!checkNeighbors) return true;
    bool surrounded = water_at(p.offset(-1, 0, 0)) && water_at(p.offset(1, 0, 0)) && water_at(p.offset(0, 0, -1)) &&
                      water_at(p.offset(0, 0, 1));
    if (!surrounded) return true;
  }
  return false;
}

bool Level::should_snow(int b, const P& p) const {
  if (!f.hasPrecipitation[size_t(b)] || temperature(b, p) >= 0.15F) return false;
  if (outside(p.y)) return false;
  int s = get(p);
  return (R().is_air(s) || R().is(s, blocks::snow)) && can_survive(const_cast<Level&>(*this), blocks::snow, p);
}

// ---------------------------------------------------------------- FeaturePlacer
bool Placed::place(Level& l, RandomSource& r, const P& origin, bool biomeCheck) const {
  PlacementContext ctx{l, biomeCheck ? id : -1};
  if (mods.empty()) return feature->place(l, r, origin);
  bool placedAny = false;
  std::vector<std::pair<P, int>> stack{{origin, 0}};
  std::vector<P> out;
  while (!stack.empty()) {
    auto [pos, mi] = stack.back();
    stack.pop_back();
    out.clear();
    mods[size_t(mi)]->modify(ctx, r, pos, out);
    if (mi + 1 < int(mods.size())) {
      for (size_t i = out.size(); i-- > 0;) stack.push_back({out[i], mi + 1});
    } else {
      for (auto& p : out) placedAny |= feature->place(l, r, p);
    }
  }
  return placedAny;
}

namespace {
// ---------------------------------------------------------------- placement modifiers
struct CountMod : Modifier {
  IntProvider n;
  void modify(PlacementContext&, RandomSource& r, const P& p, std::vector<P>& out) const override {
    int c = n.sample(r);
    for (int i = 0; i < c; i++) out.push_back(p);
  }
};
struct NoiseBasedCount : Modifier {
  int ratio;
  double factor, offset;
  const Features* f;
  void modify(PlacementContext&, RandomSource&, const P& p, std::vector<P>& out) const override {
    double v = f->biomeInfoNoise->get(p.x / factor, p.z / factor);
    int c = Mth::d2i(std::ceil((v + offset) * ratio));
    for (int i = 0; i < c; i++) out.push_back(p);
  }
};
struct NoiseThresholdCount : Modifier {
  double level;
  int below, above;
  const Features* f;
  void modify(PlacementContext&, RandomSource&, const P& p, std::vector<P>& out) const override {
    double v = f->biomeInfoNoise->get(p.x / 200.0, p.z / 200.0);
    int c = v < level ? below : above;
    for (int i = 0; i < c; i++) out.push_back(p);
  }
};
struct CountOnEveryLayer : Modifier {
  IntProvider n;
  static bool empty(int s) { return R().is_air(s) || R().is(s, blocks::water) || R().is(s, blocks::lava); }
  void modify(PlacementContext& c, RandomSource& r, const P& p, std::vector<P>& out) const override {
    int layer = 0;
    bool found;
    do {
      found = false;
      for (int i = 0; i < n.sample(r); i++) {
        int x = r.nextInt(16) + p.x;
        int z = r.nextInt(16) + p.z;
        int startY = c.level.height(MOTION_BLOCKING, x, z);
        int y = INT32_MAX;
        {  // findOnGroundYPosition
          int cur = 0;
          int curBlock = c.level.get(x, startY, z);
          for (int yy = startY; yy >= c.level.min_y() + 1; yy--) {
            int below = c.level.get(x, yy - 1, z);
            if (!empty(below) && empty(curBlock) && !R().is(below, blocks::bedrock)) {
              if (cur == layer) { y = yy - 1 + 1; break; }
              cur++;
            }
            curBlock = below;
          }
        }
        if (y != INT32_MAX) out.push_back({x, y, z}), found = true;
      }
      layer++;
    } while (found);
  }
};
struct Rarity : Modifier {
  int chance;
  void modify(PlacementContext&, RandomSource& r, const P& p, std::vector<P>& out) const override {
    if (r.nextFloat() < 1.0F / (float)chance) out.push_back(p);
  }
};
struct RandomChance : Modifier {
  float chance;
  void modify(PlacementContext&, RandomSource& r, const P& p, std::vector<P>& out) const override {
    if (r.nextFloat() < chance) out.push_back(p);
  }
};
struct InSquare : Modifier {
  void modify(PlacementContext&, RandomSource& r, const P& p, std::vector<P>& out) const override {
    int x = r.nextInt(16) + p.x;
    int z = r.nextInt(16) + p.z;
    out.push_back({x, p.y, z});
  }
};
struct HeightRange : Modifier {
  HeightProvider h;
  void modify(PlacementContext&, RandomSource& r, const P& p, std::vector<P>& out) const override { out.push_back({p.x, h.sample(r), p.z}); }
};
struct HeightmapMod : Modifier {
  Heightmap h;
  void modify(PlacementContext& c, RandomSource&, const P& p, std::vector<P>& out) const override {
    int y = c.level.height(h, p.x, p.z);
    if (y > c.level.min_y()) out.push_back({p.x, y, p.z});
  }
};
struct BiomeFilter : Modifier {
  const Features* f;
  void modify(PlacementContext& c, RandomSource&, const P& p, std::vector<P>& out) const override {
    if (c.top < 0) throw std::runtime_error("Tried to biome check an unregistered feature");
    int b = c.level.biome(p);
    if (f->biomeHas[size_t(b)][size_t(c.top)]) out.push_back(p);
  }
};
struct PredicateFilter : Modifier {
  const BlockPredicate* pred;
  void modify(PlacementContext& c, RandomSource&, const P& p, std::vector<P>& out) const override {
    if (pred->test(c.level, p)) out.push_back(p);
  }
};
struct SurfaceWaterDepth : Modifier {
  int max;
  void modify(PlacementContext& c, RandomSource&, const P& p, std::vector<P>& out) const override {
    int ocean = c.level.height(OCEAN_FLOOR, p.x, p.z), surface = c.level.height(WORLD_SURFACE, p.x, p.z);
    if (surface - ocean <= max) out.push_back(p);
  }
};
struct SurfaceRelative : Modifier {
  Heightmap h;
  int min, max;
  void modify(PlacementContext& c, RandomSource&, const P& p, std::vector<P>& out) const override {
    int64_t s = c.level.height(h, p.x, p.z);
    if (s + min <= p.y && p.y <= s + max) out.push_back(p);
  }
};
struct EnvironmentScan : Modifier {
  bs::Dir dir;
  const BlockPredicate *target, *allowed;
  int maxSteps;
  void modify(PlacementContext& c, RandomSource&, const P& origin, std::vector<P>& out) const override {
    P p = origin;
    Level& l = c.level;
    if (!allowed->test(l, p)) return;
    for (int i = 0; i < maxSteps; i++) {
      if (target->test(l, p)) { out.push_back(p); return; }
      p = p.rel(dir);
      if (l.outside(p.y)) return;
      if (!allowed->test(l, p)) break;
    }
    if (target->test(l, p)) out.push_back(p);
  }
};
struct Offset : Modifier {
  IntProvider x, y, z;
  void modify(PlacementContext&, RandomSource& r, const P& p, std::vector<P>& out) const override {
    int dx = x.sample(r);
    int dy = y.sample(r);
    int dz = z.sample(r);
    out.push_back(p.offset(dx, dy, dz));
  }
};
struct Fixed : Modifier {
  std::vector<P> positions;
  void modify(PlacementContext&, RandomSource&, const P& p, std::vector<P>& out) const override {
    for (auto& q : positions)
      if ((q.x >> 4) == (p.x >> 4) && (q.z >> 4) == (p.z >> 4)) out.push_back(q);
  }
};
struct RandomlySelected : Modifier {
  std::vector<const Modifier*> mods;
  void modify(PlacementContext& c, RandomSource& r, const P& p, std::vector<P>& out) const override {
    mods[size_t(r.nextInt(int(mods.size())))]->modify(c, r, p, out);
  }
};
struct Cuboid : Modifier {
  IntProvider xz, ys;
  bool edges, interior;
  void modify(PlacementContext&, RandomSource& r, const P& o, std::vector<P>& out) const override {
    int height = ys.sample(r);
    int width = xz.sample(r);
    int length = xz.sample(r);
    for (int x = 0; x <= width; x++)
      for (int y = 0; y <= height; y++)
        for (int z = 0; z <= length; z++)
          if ((edges || (x != 0 && x != width) || (y != 0 && y != height)) &&
              (edges || (z != 0 && z != length) || (y != 0 && y != height)) &&
              (edges || (x != 0 && x != width) || (z != 0 && z != length)) &&
              (interior || x == 0 || x == width || y == 0 || y == height || z == 0 || z == length))
            out.push_back(o.offset(x, y, z));
  }
};

// ---------------------------------------------------------------- block predicates
struct AllOf : BlockPredicate {
  std::vector<const BlockPredicate*> ps;
  bool test(Level& l, const P& p) const override {
    for (auto* q : ps)
      if (!q->test(l, p)) return false;
    return true;
  }
};
struct AnyOf : BlockPredicate {
  std::vector<const BlockPredicate*> ps;
  bool test(Level& l, const P& p) const override {
    for (auto* q : ps)
      if (q->test(l, p)) return true;
    return false;
  }
};
struct Not : BlockPredicate {
  const BlockPredicate* p;
  bool test(Level& l, const P& q) const override { return !p->test(l, q); }
};
struct True : BlockPredicate {
  bool test(Level&, const P&) const override { return true; }
};
struct MatchingBlocks : BlockPredicate {
  P off;
  bs::Tag set;
  bool test(Level& l, const P& p) const override { return R().in(l.get(p.offset(off.x, off.y, off.z)), set); }
};
struct MatchingFluids : BlockPredicate {
  P off;
  uint8_t mask;
  bool test(Level& l, const P& p) const override { return R().fluid_in(l.get(p.offset(off.x, off.y, off.z)), mask); }
};
struct MatchingBiomes : BlockPredicate {
  std::vector<uint8_t> biomes;
  bool test(Level& l, const P& p) const override { return biomes[size_t(l.biome(p))]; }
};
struct SturdyFace : BlockPredicate {
  P off;
  bs::Dir dir;
  bool test(Level& l, const P& p) const override { return R().sturdy(l.get(p.offset(off.x, off.y, off.z)), dir); }
};
struct Solid : BlockPredicate {
  P off;
  bool test(Level& l, const P& p) const override { return R().is_solid(l.get(p.offset(off.x, off.y, off.z))); }
};
struct Replaceable : BlockPredicate {
  P off;
  bool test(Level& l, const P& p) const override { return R().can_be_replaced(l.get(p.offset(off.x, off.y, off.z))); }
};
struct WouldSurvive : BlockPredicate {
  P off;
  int state;
  bool test(Level& l, const P& p) const override { return can_survive(l, state, p.offset(off.x, off.y, off.z)); }
};
struct InsideWorld : BlockPredicate {
  P off;
  bool test(Level& l, const P& p) const override { return !l.outside(p.y + off.y); }
};
struct HeightRangePred : BlockPredicate {
  VerticalAnchor min, max;
  bool test(Level&, const P& p) const override { return p.y >= min.resolve() && p.y <= max.resolve(); }
};
struct VolumeMatch : BlockPredicate {
  P min, max;
  const BlockPredicate* match;
  bool test(Level& l, const P& p) const override {
    for (int ox = min.x; ox <= max.x; ox++)
      for (int oz = min.z; oz <= max.z; oz++)
        for (int oy = min.y; oy <= max.y; oy++)
          if (!match->test(l, p.offset(ox, oy, oz))) return false;
    return true;
  }
};

// ---------------------------------------------------------------- state providers
struct Simple : StateProvider {
  int s;
  int get(Level&, RandomSource&, const P&) const override { return s; }
};
struct Weighted : StateProvider {
  std::vector<std::pair<int, int>> entries;  // state, weight
  int total = 0;
  int get(Level&, RandomSource& r, const P&) const override {
    int sel = r.nextInt(total);
    for (auto& [s, w] : entries)
      if ((sel -= w) < 0) return s;
    return entries.back().first;
  }
};
struct NoiseProv : StateProvider {
  std::shared_ptr<const Noise> noise;
  float scale;
  std::vector<int> states;
  float value(const P& p, double sc) const { return noise->get(p.x * sc, p.y * sc, p.z * sc); }
  static int pick(const std::vector<int>& states, float v) {
    float place = Mth::clamp((1.0F + v) / 2.0F, 0.0F, 0.9999F);
    return states[size_t((int)(place * (float)states.size()))];
  }
  int get(Level&, RandomSource&, const P& p) const override { return pick(states, value(p, scale)); }
};
struct DualNoise : NoiseProv {
  int vmin, vmax;
  std::shared_ptr<const Noise> slow;
  float slowScale;
  float slowValue(const P& p) const { return slow->get(p.x * slowScale, p.y * slowScale, p.z * slowScale); }
  int get(Level&, RandomSource&, const P& p) const override {
    double v = slowValue(p);
    double t = (v - -1.0) / (1.0 - -1.0);  // Mth.clampedMap(v, -1, 1, min, max + 1)
    int local = t < 0.0 ? vmin : t > 1.0 ? vmax + 1 : (int)Mth::lerp(t, (double)vmin, (double)(vmax + 1));
    std::vector<int> possible;
    for (int i = 0; i < local; i++) possible.push_back(pick(states, slowValue(p.offset(i * 54545, 0, i * 34234))));
    return pick(possible, value(p, scale));
  }
};
struct NoiseThreshold : StateProvider {
  std::shared_ptr<const Noise> noise;
  float scale, threshold, highChance;
  int def;
  std::vector<int> low, high;
  int get(Level&, RandomSource& r, const P& p) const override {
    double v = noise->get(p.x * (double)scale, p.y * (double)scale, p.z * (double)scale);
    if (v < threshold) return low[size_t(r.nextInt(int(low.size())))];
    return r.nextFloat() < highChance ? high[size_t(r.nextInt(int(high.size())))] : def;
  }
};
struct RandomBlock : StateProvider {
  std::vector<int> blocks;  // block indices
  int get(Level& l, RandomSource& r, const P& p) const override {
    int s = optional(l, r, p);
    return s >= 0 ? s : l.get(p);
  }
  int optional(Level&, RandomSource& r, const P&) const override {
    if (blocks.empty()) return -1;
    return R().default_state(blocks[size_t(r.nextInt(int(blocks.size())))]);
  }
};
struct RandomizedInt : StateProvider {
  const StateProvider* source;
  std::string prop;
  IntProvider values;
  int get(Level& l, RandomSource& r, const P& p) const override {
    int s = source->get(l, r, p);
    if (!R().has(s, prop)) return s;
    return R().with_int(s, prop, values.sample(r));
  }
};
struct Rotated : StateProvider {
  const StateProvider* state;
  int dir = -1;
  int get(Level& l, RandomSource& r, const P& p) const override {
    bs::Dir d = dir >= 0 ? bs::Dir(dir) : random_dir(r);
    int s = state->get(l, r, p);
    const char* axis = d <= bs::UP ? "y" : d <= bs::SOUTH ? "z" : "x";
    if (int t = R().with(s, "axis", axis); t >= 0) s = t;
    if (int t = R().with(s, "facing", dir_name(d)); t >= 0) s = t;  // FACING / HORIZONTAL_FACING share the name
    return s;
  }
};
struct CopyProperties : StateProvider {
  const StateProvider* source;
  int get(Level& l, RandomSource& r, const P& p) const override {
    int s = source->get(l, r, p);
    int from = l.get(p);  // BlockState.withPropertiesOf
    const auto& b = R().block(R().block_of(from));
    for (auto& pr : b.props)
      if (int t = R().with(s, pr.name, R().value(from, pr.name)); t >= 0) s = t;
    return s;
  }
};
struct RuleBased : StateProvider {
  const StateProvider* fallback = nullptr;
  std::vector<std::pair<const BlockPredicate*, const StateProvider*>> rules;
  int get(Level& l, RandomSource& r, const P& p) const override {
    int s = optional(l, r, p);
    return s >= 0 ? s : l.get(p);
  }
  int optional(Level& l, RandomSource& r, const P& p) const override {
    for (auto& [ifTrue, then] : rules)
      if (ifTrue->test(l, p))
        if (int s = then->optional(l, r, p); s >= 0) return s;
    return fallback ? fallback->optional(l, r, p) : -1;
  }
};

// ---------------------------------------------------------------- rule tests
struct AlwaysTrueTest : RuleTest {
  bool test(int, const P&, RandomSource&) const override { return true; }
};
struct BlockMatchTest : RuleTest {
  int block;
  bool test(int s, const P&, RandomSource&) const override { return R().block_of(s) == block; }
};
struct StateMatchTest : RuleTest {
  int state;
  bool test(int s, const P&, RandomSource&) const override { return s == state; }
};
struct TagMatchTest : RuleTest {
  const bs::Tag* tag;
  bool test(int s, const P&, RandomSource&) const override { return R().in(s, *tag); }
};
struct RandomBlockMatchTest : RuleTest {
  int block;
  float probability;
  bool test(int s, const P&, RandomSource& r) const override { return R().block_of(s) == block && r.nextFloat() < probability; }
};
struct RandomStateMatchTest : RuleTest {
  int state;
  float probability;
  bool test(int s, const P&, RandomSource& r) const override { return s == state && r.nextFloat() < probability; }
};
struct HeightMatchTest : RuleTest {
  int min, max;
  bool test(int, const P& p, RandomSource&) const override { return min <= p.y && p.y <= max; }
};
struct AllOfTest : RuleTest {
  std::vector<const RuleTest*> rs;
  bool test(int s, const P& p, RandomSource& r) const override {
    for (auto* t : rs)
      if (!t->test(s, p, r)) return false;
    return true;
  }
};
struct AnyOfTest : RuleTest {
  std::vector<const RuleTest*> rs;
  bool test(int s, const P& p, RandomSource& r) const override {
    for (auto* t : rs)
      if (t->test(s, p, r)) return true;
    return false;
  }
};
struct NotTest : RuleTest {
  const RuleTest* t;
  bool test(int s, const P& p, RandomSource& r) const override { return !t->test(s, p, r); }
};
}  // namespace

// ---------------------------------------------------------------- Loader
Json Loader::load(const std::string& kind, const std::string& id) const {
  return Json::parseFile(root + "/minecraft/worldgen/" + kind + "/" + strip_ns(id) + ".json");
}

int Loader::state(const Json& j) const {
  int s = -1;
  if (j.isString()) s = R().parse(j.str);
  else if (j.has("id") || j.has("Name")) {
    s = R().parse((j.has("id") ? j["id"] : j["Name"]).asString());
    const Json* props = j.find("properties");
    if (!props) props = j.find("Properties");
    if (props && s >= 0)
      for (auto& [k, v] : props->obj) {
        int t = R().with(s, k, v.isString() ? v.str : std::to_string(v.asInt()));
        if (t >= 0) s = t;
      }
  }
  if (s < 0) throw std::runtime_error("bad block state");
  return s;
}

std::vector<int> Loader::block_list(const Json& j) const {
  std::vector<int> out;
  auto add = [&](const std::string& id) {
    if (id[0] == '#') {
      const bs::Tag& t = R().tag(id);
      for (int b = 0; b < R().block_count(); b++)
        if (t.has(b)) out.push_back(b);
    } else if (int b = R().block_index(id); b >= 0) {
      out.push_back(b);
    }
  };
  if (j.isString()) add(j.str);
  else for (auto& x : j.arr) add(x.str);
  return out;
}

bs::Tag Loader::block_set(const Json& j) const {
  bs::Tag t;
  for (int b : block_list(j)) t.add(b);
  return t;
}

std::shared_ptr<const Noise> Loader::noise(const Json& params, int64_t seed) const {
  WorldgenRandom r(std::make_unique<LegacyRandomSource>(seed));
  return NormalNoise(NormalNoise::Parameters::fromJson(params)).create(r);
}

const Modifier* Loader::modifier(const Json& j) {
  std::string t = strip_ns(j["type"].asString());
  if (t == "count") { auto* m = make<CountMod>(); m->n = IntProvider(j["count"]); return m; }
  if (t == "noise_based_count") {
    auto* m = make<NoiseBasedCount>();
    m->ratio = j["noise_to_count_ratio"].asInt(), m->factor = j["noise_factor"].asDouble();
    m->offset = j.getDouble("noise_offset", 0.0), m->f = &f;
    return m;
  }
  if (t == "noise_threshold_count") {
    auto* m = make<NoiseThresholdCount>();
    m->level = j["noise_level"].asDouble(), m->below = j["below_noise"].asInt(), m->above = j["above_noise"].asInt(), m->f = &f;
    return m;
  }
  if (t == "count_on_every_layer") { auto* m = make<CountOnEveryLayer>(); m->n = IntProvider(j["count"]); return m; }
  if (t == "rarity_filter") { auto* m = make<Rarity>(); m->chance = j["chance"].asInt(); return m; }
  if (t == "random_chance") { auto* m = make<RandomChance>(); m->chance = j["chance"].asFloat(); return m; }
  if (t == "in_square") return make<InSquare>();
  if (t == "height_range") { auto* m = make<HeightRange>(); m->h = HeightProvider(j["height"]); return m; }
  if (t == "heightmap") { auto* m = make<HeightmapMod>(); m->h = heightmap_by_name(j["heightmap"].asString()); return m; }
  if (t == "biome") { auto* m = make<BiomeFilter>(); m->f = &f; return m; }
  if (t == "block_predicate_filter") { auto* m = make<PredicateFilter>(); m->pred = predicate(j["predicate"]); return m; }
  if (t == "surface_water_depth_filter") { auto* m = make<SurfaceWaterDepth>(); m->max = j["max_water_depth"].asInt(); return m; }
  if (t == "surface_relative_threshold_filter") {
    auto* m = make<SurfaceRelative>();
    m->h = heightmap_by_name(j["heightmap"].asString());
    m->min = j.getInt("min_inclusive", INT32_MIN), m->max = j.getInt("max_inclusive", INT32_MAX);
    return m;
  }
  if (t == "environment_scan") {
    auto* m = make<EnvironmentScan>();
    m->dir = dir_by_name(j["direction_of_search"].asString());
    m->target = predicate(j["target_condition"]);
    m->allowed = j.has("allowed_search_condition") ? predicate(j["allowed_search_condition"]) : make<True>();
    m->maxSteps = j["max_steps"].asInt();
    return m;
  }
  if (t == "offset") {
    auto* m = make<Offset>();
    m->x = IntProvider(j["x"]), m->y = IntProvider(j["y"]), m->z = IntProvider(j["z"]);
    return m;
  }
  if (t == "fixed_placement") {
    auto* m = make<Fixed>();
    for (auto& p : j["positions"].arr) m->positions.push_back(vec3(&p));
    return m;
  }
  if (t == "randomly_selected") {
    auto* m = make<RandomlySelected>();
    for (auto& p : j["placements"].arr) m->mods.push_back(modifier(p));
    return m;
  }
  if (t == "cuboid") {
    auto* m = make<Cuboid>();
    m->xz = IntProvider(j["xz_size"]), m->ys = IntProvider(j["y_size"]);
    m->edges = !j.has("include_edges") || j["include_edges"].asBool();
    m->interior = !j.has("include_interior") || j["include_interior"].asBool();
    return m;
  }
  throw std::runtime_error("placement modifier " + t);
}

const Placed* Loader::placed(const Json& j) {
  if (j.isString()) {
    std::string id = j.str.find(':') == std::string::npos ? "minecraft:" + j.str : j.str;
    if (auto it = placedById.find(id); it != placedById.end()) return it->second;
    placedById[id] = nullptr;  // cycle guard
    const Placed* p = placed(load("placed_feature", id));
    auto* q = const_cast<Placed*>(p);
    q->id = int(f.placedList.size());
    q->name = id;
    f.placedList.push_back(q);
    return placedById[id] = p;
  }
  auto* p = make<Placed>();
  p->feature = feature(j["feature"]);
  for (auto& m : j["placement"].arr) p->mods.push_back(modifier(m));
  return p;
}

const Feature* Loader::feature(const Json& j) {
  if (j.isString()) {
    std::string id = j.str.find(':') == std::string::npos ? "minecraft:" + j.str : j.str;
    if (auto it = featureById.find(id); it != featureById.end()) return it->second;
    try {
      return featureById[id] = feature(load("feature", id));
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string(e.what()) + " in feature " + id);
    }
  }
  std::string t = strip_ns(j["type"].asString());
  auto it = f.parsers.find(t);
  if (it == f.parsers.end()) {  // not ported yet: places nothing (reported once per type)
    struct Missing : Feature {
      bool place(Level&, RandomSource&, const P&) const override { return false; }
    };
    static std::set<std::string> warned;
    if (warned.insert(t).second) fprintf(stderr, "worldgen: feature type %s not implemented\n", t.c_str());
    return make<Missing>();
  }
  return it->second(*this, j);
}

const BlockPredicate* Loader::predicate(const Json& j) {
  std::string t = strip_ns(j["type"].asString());
  P off = vec3(j.find("offset"));
  if (t == "all_of" || t == "any_of") {
    std::vector<const BlockPredicate*> ps;
    for (auto& x : j["predicates"].arr) ps.push_back(predicate(x));
    if (t == "all_of") { auto* p = make<AllOf>(); p->ps = ps; return p; }
    auto* p = make<AnyOf>();
    p->ps = ps;
    return p;
  }
  if (t == "not") { auto* p = make<Not>(); p->p = predicate(j["predicate"]); return p; }
  if (t == "true") return make<True>();
  if (t == "matching_blocks") { auto* p = make<MatchingBlocks>(); p->off = off, p->set = block_set(j["blocks"]); return p; }
  if (t == "matching_block_tag") { auto* p = make<MatchingBlocks>(); p->off = off, p->set = R().tag(j["tag"].asString()); return p; }
  if (t == "matching_fluids") {
    auto* p = make<MatchingFluids>();
    p->off = off, p->mask = 0;
    const Json& fl = j["fluids"];
    if (fl.isString()) p->mask = R().fluids(fl.str);
    else for (auto& x : fl.arr) p->mask |= R().fluids(x.str);
    return p;
  }
  if (t == "matching_biomes") {
    auto* p = make<MatchingBiomes>();
    p->biomes.assign(biomes::COUNT, 0);
    auto add = [&](const std::string& id) {
      if (id[0] == '#') {
        Json tag = Json::parseFile(root + "/minecraft/tags/worldgen/biome/" + strip_ns(id.substr(1)) + ".json");
        for (auto& v : tag["values"].arr)
          if (int b = f.materials->biomeId(v.isString() ? v.str : v["id"].str); b >= 0) p->biomes[size_t(b)] = 1;
      } else if (int b = f.materials->biomeId(id); b >= 0) {
        p->biomes[size_t(b)] = 1;
      }
    };
    const Json& bj = j["biomes"];
    if (bj.isString()) add(bj.str);
    else for (auto& x : bj.arr) add(x.str);
    return p;
  }
  if (t == "has_sturdy_face") { auto* p = make<SturdyFace>(); p->off = off, p->dir = dir_by_name(j["direction"].asString()); return p; }
  if (t == "solid") { auto* p = make<Solid>(); p->off = off; return p; }
  if (t == "replaceable") { auto* p = make<Replaceable>(); p->off = off; return p; }
  if (t == "would_survive") { auto* p = make<WouldSurvive>(); p->off = off, p->state = state(j["state"]); return p; }
  if (t == "inside_world_bounds") { auto* p = make<InsideWorld>(); p->off = off; return p; }
  if (t == "unobstructed") return make<True>();  // no entities during worldgen
  if (t == "height_range") {
    auto* p = make<HeightRangePred>();
    p->min = VerticalAnchor(j["min_inclusive"]), p->max = VerticalAnchor(j["max_inclusive"]);
    return p;
  }
  if (t == "volume_match") {
    auto* p = make<VolumeMatch>();
    p->min = vec3(j.find("min")), p->max = vec3(j.find("max")), p->match = predicate(j["match"]);
    return p;
  }
  throw std::runtime_error("block predicate " + t);
}

const StateProvider* Loader::provider(const Json& j) {
  if (j.isString()) {
    std::string id = j.str.find(':') == std::string::npos ? "minecraft:" + j.str : j.str;
    if (auto it = providerById.find(id); it != providerById.end()) return it->second;
    if (std::filesystem::exists(root + "/minecraft/worldgen/block_state_provider/" + strip_ns(id) + ".json"))
      return providerById[id] = provider(load("block_state_provider", id));
    auto* p = make<Simple>();
    p->s = state(j);
    return providerById[id] = p;
  }
  if (!j.has("type")) { auto* p = make<Simple>(); p->s = state(j); return p; }
  std::string t = strip_ns(j["type"].asString());
  if (t == "simple") { auto* p = make<Simple>(); p->s = state(j["state"]); return p; }
  if (t == "weighted") {
    auto* p = make<Weighted>();
    for (auto& e : j["entries"].arr) p->entries.push_back({state(e["data"]), e["weight"].asInt()}), p->total += e["weight"].asInt();
    return p;
  }
  if (t == "noise" || t == "dual_noise") {
    NoiseProv* p = t == "noise" ? make<NoiseProv>() : make<DualNoise>();
    int64_t seed = (int64_t)j["seed"].asDouble();
    p->noise = noise(j["noise"], seed);
    p->scale = j["scale"].asFloat();
    for (auto& s : j["states"].arr) p->states.push_back(state(s));
    if (t == "dual_noise") {
      auto* d = static_cast<DualNoise*>(p);
      const Json& v = j["variety"];
      if (v.isArray()) d->vmin = v[0].asInt(), d->vmax = v[1].asInt();
      else if (v.isNumber()) d->vmin = d->vmax = v.asInt();
      else d->vmin = v["min_inclusive"].asInt(), d->vmax = v["max_inclusive"].asInt();
      d->slow = noise(j["slow_noise"], seed);
      d->slowScale = j["slow_scale"].asFloat();
    }
    return p;
  }
  if (t == "noise_threshold") {
    auto* p = make<NoiseThreshold>();
    p->noise = noise(j["noise"], (int64_t)j["seed"].asDouble());
    p->scale = j["scale"].asFloat(), p->threshold = j["threshold"].asFloat(), p->highChance = j["high_chance"].asFloat();
    p->def = state(j["default_state"]);
    for (auto& s : j["low_states"].arr) p->low.push_back(state(s));
    for (auto& s : j["high_states"].arr) p->high.push_back(state(s));
    return p;
  }
  if (t == "random_block") { auto* p = make<RandomBlock>(); p->blocks = block_list(j["blocks"]); return p; }
  if (t == "randomized_int") {
    auto* p = make<RandomizedInt>();
    p->source = provider(j["source"]), p->prop = j["property"].asString(), p->values = IntProvider(j["values"]);
    return p;
  }
  if (t == "rotated") {
    auto* p = make<Rotated>();
    p->state = provider(j["state"]);
    if (j.has("direction")) p->dir = dir_by_name(j["direction"].asString());
    return p;
  }
  if (t == "copy_properties") { auto* p = make<CopyProperties>(); p->source = provider(j["source"]); return p; }
  if (t == "rule_based") {
    auto* p = make<RuleBased>();
    if (j.has("fallback")) p->fallback = provider(j["fallback"]);
    for (auto& r : j["rules"].arr) p->rules.push_back({predicate(r["if_true"]), provider(r["then"])});
    return p;
  }
  throw std::runtime_error("block state provider " + t);
}

const RuleTest* Loader::rule(const Json& j) {
  std::string t = strip_ns(j["predicate_type"].asString());
  if (t == "always_true") return make<AlwaysTrueTest>();
  if (t == "block_match") { auto* r = make<BlockMatchTest>(); r->block = R().block_index(j["block"].asString()); return r; }
  if (t == "blockstate_match") { auto* r = make<StateMatchTest>(); r->state = state(j["block_state"]); return r; }
  if (t == "tag_match") { auto* r = make<TagMatchTest>(); r->tag = &R().tag(j["tag"].asString()); return r; }
  if (t == "random_block_match") {
    auto* r = make<RandomBlockMatchTest>();
    r->block = R().block_index(j["block"].asString()), r->probability = j["probability"].asFloat();
    return r;
  }
  if (t == "random_blockstate_match") {
    auto* r = make<RandomStateMatchTest>();
    r->state = state(j["block_state"]), r->probability = j["probability"].asFloat();
    return r;
  }
  if (t == "height_match") {
    auto* r = make<HeightMatchTest>();
    r->min = j["min_inclusive"].asInt(), r->max = j["max_inclusive"].asInt();
    return r;
  }
  if (t == "all_of" || t == "any_of") {
    std::vector<const RuleTest*> rs;
    for (auto& x : j["rules"].arr) rs.push_back(rule(x));
    if (t == "all_of") { auto* r = make<AllOfTest>(); r->rs = rs; return r; }
    auto* r = make<AnyOfTest>();
    r->rs = rs;
    return r;
  }
  if (t == "not") { auto* r = make<NotTest>(); r->t = rule(j["rule"]); return r; }
  throw std::runtime_error("rule test " + t);
}

}  // namespace feat

// ---------------------------------------------------------------- Features
Features::~Features() = default;

Features::Features(Overworld& ow, const std::string& root) : ow(ow) {
  using namespace feat;
  materials = ow.terrain->materials.get();
  if (!materials) throw std::runtime_error("features need the surface stage");
  {
    LegacyRandomSource r(2345L);  // Biome.BIOME_INFO_NOISE = new SimplexNoise(new WorldgenRandom(new LegacyRandomSource(2345)))
    biomeInfoNoise = std::make_shared<SimplexNoise>(r, true);
  }
  regionRandom = &ow.terrain->dfs.randomState().getOrCreateRandomFactory("minecraft:worldgen_region_random");
  register_basic_features(parsers);
  register_tree_features(parsers);
  register_cave_features(parsers);
  register_misc_features(parsers);
  loader = std::make_unique<Loader>(*this, root);

  // Biomes in registry order (biomes:: ids) and the biome source's possible biomes in parameter-list order.
  std::vector<std::string> names;
  for (auto& e : std::filesystem::directory_iterator(root + "/minecraft/worldgen/biome"))
    if (e.path().extension() == ".json") names.push_back(e.path().stem().string());
  std::sort(names.begin(), names.end());
  std::vector<int> sources;
  possibleBiome.assign(names.size(), 0);
  for (auto& [pt, b] : ow.terrain->biomeSource.values())
    if (!possibleBiome[size_t(b)]) possibleBiome[size_t(b)] = 1, sources.push_back(b);

  // Feature lists of every biome (placed features parsed on first reference).
  std::vector<std::vector<std::vector<const Placed*>>> lists(names.size());
  hasPrecipitation.assign(names.size(), 1);
  for (size_t b = 0; b < names.size(); b++) {
    Json j = Json::parseFile(biome_file(root, ow.pack, names[b]));
    hasPrecipitation[b] = j["has_precipitation"].asBool();
    if (!possibleBiome[b]) continue;  // other dimensions: never decorated here
    for (auto& step : j["features"].arr) {
      auto& l = lists[b].emplace_back();
      auto add = [&](const Json& id) {
        if (id.isString() && id.str[0] == '#') throw std::runtime_error("placed feature tags unsupported");
        const Placed* p = loader->placed(id);
        if (p) l.push_back(p);
      };
      if (step.isString()) add(step);
      else for (auto& x : step.arr) add(x);
    }
  }
  biomeHas.assign(names.size(), std::vector<uint8_t>(placedList.size() + 1, 0));
  for (size_t b = 0; b < names.size(); b++)
    for (auto& l : lists[b])
      for (auto* p : l) biomeHas[b][size_t(p->id)] = 1;

  // FeatureSorter.buildFeaturesPerStep(possibleBiomes, features, true).
  struct FD {
    int index, step;
    const Placed* p;
    bool operator<(const FD& o) const { return step != o.step ? step < o.step : index < o.index; }
  };
  std::map<const Placed*, int> featureIndex;
  std::map<FD, std::set<FD>> edges;
  size_t maxStep = 0;
  for (int b : sources) {
    std::vector<FD> fl;
    auto& steps_ = lists[size_t(b)];
    maxStep = std::max(maxStep, steps_.size());
    for (size_t i = 0; i < steps_.size(); i++)
      for (auto* p : steps_[i]) {
        auto [it, ins] = featureIndex.try_emplace(p, int(featureIndex.size()));
        fl.push_back({it->second, int(i), p});
      }
    for (size_t i = 0; i < fl.size(); i++) {
      auto& set = edges[fl[i]];
      if (i + 1 < fl.size()) set.insert(fl[i + 1]);
    }
  }
  std::set<FD> discovered, visiting;
  std::vector<FD> sorted;
  std::function<bool(const FD&)> dfs = [&](const FD& c) -> bool {
    if (discovered.count(c)) return false;
    if (visiting.count(c)) return true;
    visiting.insert(c);
    if (auto it = edges.find(c); it != edges.end())
      for (auto& n : it->second)
        if (dfs(n)) return true;
    visiting.erase(c);
    discovered.insert(c);
    sorted.push_back(c);
    return false;
  };
  for (auto& [k, v] : edges)
    if (!discovered.count(k) && dfs(k)) throw std::runtime_error("Feature order cycle found");
  std::reverse(sorted.begin(), sorted.end());
  steps.assign(maxStep, {});
  std::map<const Placed*, int> indexInStep;
  for (auto& fd : sorted) indexInStep[fd.p] = int(steps[size_t(fd.step)].size()), steps[size_t(fd.step)].push_back(fd.p);
  biomeSteps.assign(names.size(), {});
  for (size_t b = 0; b < names.size(); b++) {
    if (!possibleBiome[b]) continue;
    for (size_t s = 0; s < lists[b].size(); s++) {
      auto& v = biomeSteps[b].emplace_back();
      for (auto* p : lists[b][s]) v.push_back(indexInStep[p]);
    }
  }
}

void Features::finish(Region& r) const {
  using namespace feat;
  Level level(r, *this);
  ProtoChunk& c = r.center();
  auto unpack = [&](int32_t v) { return P{c.cx * 16 + (v & 15), (v >> 8) + MIN_Y, c.cz * 16 + (v >> 4 & 15)}; };
  auto writable = [&](const P& p) {
    ProtoChunk* n = r.chunk(p.x >> 4, p.z >> 4);
    return n && n->chunk && !n->taken;
  };
  for (int32_t v : c.post_processing) {
    P p = unpack(v);
    int s = level.get(p);
    if (R().has_fluid(s)) continue;  // ponytail: fluid ticks (flowing water / lava after generation) not simulated
    int n = update_from_neighbour_shapes(level, s, p);
    if (n != s && writable(p)) level.set(p, n);
  }
  c.post_processing.clear();
  c.post_processing.shrink_to_fit();
  // Leaf ticks: Bellman-Ford style relaxation, FIFO like same-tick scheduled ticks.
  static const bs::Tag& noDecay = R().tag("minecraft:prevents_nearby_leaf_decay");
  auto distAt = [&](int s) { return R().in(s, noDecay) ? 0 : R().has(s, "distance") ? R().int_value(s, "distance") : 7; };
  std::vector<P> queue;
  for (int32_t v : c.leaf_ticks) queue.push_back(unpack(v));
  c.leaf_ticks.clear();
  c.leaf_ticks.shrink_to_fit();
  static const bs::Tag& leaves = R().tag("minecraft:leaves");
  for (size_t i = 0; i < queue.size() && i < 1u << 16; i++) {
    P p = queue[i];
    int s = level.get(p);
    if (!R().in(s, leaves) || !R().has(s, "distance")) continue;
    int nd = 7;
    for (bs::Dir d : ALL_DIRS) {
      nd = std::min(nd, distAt(level.get(p.rel(d))) + 1);
      if (nd == 1) break;
    }
    if (nd == R().int_value(s, "distance") || !writable(p)) continue;
    level.set(p, R().with_int(s, "distance", nd));
    for (bs::Dir d : ALL_DIRS) {  // neighbours' updateShape schedules their tick
      P n = p.rel(d);
      if (R().in(level.get(n), leaves)) queue.push_back(n);
    }
  }
}

const feat::Placed* Features::load_placed(const std::string& id) {
  Json j;
  j.type = Json::String;
  j.str = id;
  return loader->placed(j);
}

void Features::decorate(Region& r) const {
  using namespace feat;
  Level level(r, *this);
  int ox = r.cx * 16, oz = r.cz * 16;
  WorldgenRandom random(std::make_unique<XoroshiroRandomSource>(0));
  int64_t decorationSeed = random.setDecorationSeed(ow.seed, ox, oz);
  std::vector<uint8_t> present(possibleBiome.size(), 0);
  for (int dz = -1; dz <= 1; dz++)
    for (int dx = -1; dx <= 1; dx++) {
      Chunk& c = *r.chunk(r.cx + dx, r.cz + dz)->chunk;
      for (auto& s : c.sections)
        for (int i = 0; i < 64; i++) present[s.biomes.get(i)] = 1;
    }
  P origin{ox, MIN_Y, oz};
  int nsteps = std::max(11, int(steps.size()));
  std::vector<int> idx;
  for (int step = 0; step < nsteps; step++) {
    if (structure_placer) structure_placer(r, step, random, decorationSeed);
    if (step >= int(steps.size())) continue;
    idx.clear();
    for (size_t b = 0; b < present.size(); b++)
      if (present[b] && possibleBiome[b] && size_t(step) < biomeSteps[b].size())
        idx.insert(idx.end(), biomeSteps[b][size_t(step)].begin(), biomeSteps[b][size_t(step)].end());
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    static const char* dbg = getenv("CRAFTPP_FEATURE_LOG");  // "cx,cz": log the features of that chunk
    bool log = dbg && std::to_string(r.cx) + "," + std::to_string(r.cz) == dbg;
    for (int i : idx) {
      random.setFeatureSeed(decorationSeed, i, step);
      bool ok = steps[size_t(step)][size_t(i)]->place(level, random, origin, true);
      if (log) fprintf(stderr, "step %d idx %d %s -> %d\n", step, i, steps[size_t(step)][size_t(i)]->name.c_str(), ok);
    }
  }
}

}  // namespace worldgen
