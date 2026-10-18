// Biome decoration (26.3 ChunkGenerator.applyBiomeDecoration) and the feature system it runs:
//  - placed features (levelgen/placement: PlacedFeature, FeaturePlacer's depth-first modifier stack, every
//    placement modifier), configured features (levelgen/feature/*), block predicates, block state providers,
//    rule tests, value providers, parsed from data/vanilla/minecraft/worldgen/{placed_feature,feature,
//    block_state_provider,biome}.
//  - FeatureSorter.buildFeaturesPerStep over the biome source's possible biomes (global index per step),
//    WorldgenRandom(Xoroshiro).setDecorationSeed(seed, minX, minZ) and setFeatureSeed(decorationSeed, index,
//    step) before each feature, structures first in each step (Features::structure_placer hook).
// Feature implementations live in feature_*.cpp; they only use the API in this header.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "worldgen/block_state.hpp"
#include "worldgen/java_hash.hpp"
#include "worldgen/json.hpp"
#include "worldgen/pipeline.hpp"
#include "worldgen/providers.hpp"

namespace material { class MaterialSystem; }
class Noise;

namespace worldgen {

struct Overworld;
class Features;

// Structures hook: called at the start of each decoration step (before that step's features) with the
// decoration random. Vanilla seeds it per structure: random.setFeatureSeed(decorationSeed, index, step) for
// structure index 0..n-1 of the step (registry order), then StructureStart.placeInChunk.
using StructurePlacer = std::function<void(Region&, int step, WorldgenRandom& random, int64_t decorationSeed)>;

namespace feat {

struct P {
  int x = 0, y = 0, z = 0;
  P above(int n = 1) const { return {x, y + n, z}; }
  P below(int n = 1) const { return {x, y - n, z}; }
  P offset(int dx, int dy, int dz) const { return {x + dx, y + dy, z + dz}; }
  P rel(bs::Dir d, int n = 1) const { return {x + bs::DX[d] * n, y + bs::DY[d] * n, z + bs::DZ[d] * n}; }
  bool operator==(const P& o) const { return x == o.x && y == o.y && z == o.z; }
  bool operator!=(const P& o) const { return !(*this == o); }
  bool operator<(const P& o) const { return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z; }
  int64_t key() const {  // BlockPos.asLong
    return int64_t(uint64_t(int64_t(x) & 0x3FFFFFF) << 38 | uint64_t(int64_t(z) & 0x3FFFFFF) << 12 | uint64_t(int64_t(y) & 0xFFF));
  }
};
struct PHash {  // Vec3i.hashCode
  int32_t operator()(const P& p) const { return (int32_t)(((uint32_t)p.y + (uint32_t)p.z * 31u) * 31u + (uint32_t)p.x); }
};
using PosSet = JavaHashSet<P, PHash>;  // java.util.HashSet<BlockPos> (iteration order matters)
inline int dist_manhattan(const P& a, const P& b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z); }
inline double dist_sqr(const P& a, const P& b) {
  double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return dx * dx + dy * dy + dz * dz;
}
inline constexpr bs::Dir HORIZONTAL[4] = {bs::NORTH, bs::EAST, bs::SOUTH, bs::WEST};  // Direction.Plane.HORIZONTAL
inline constexpr bs::Dir ALL_DIRS[6] = {bs::DOWN, bs::UP, bs::NORTH, bs::SOUTH, bs::WEST, bs::EAST};
inline bs::Dir clockwise(bs::Dir d) { return d == bs::NORTH ? bs::EAST : d == bs::EAST ? bs::SOUTH : d == bs::SOUTH ? bs::WEST : bs::NORTH; }
inline bs::Dir counter_clockwise(bs::Dir d) { return d == bs::NORTH ? bs::WEST : d == bs::WEST ? bs::SOUTH : d == bs::SOUTH ? bs::EAST : bs::NORTH; }
inline const char* dir_name(bs::Dir d) {
  static const char* n[] = {"down", "up", "north", "south", "west", "east"};
  return n[d];
}
bs::Dir dir_by_name(const std::string& n);
inline thread_local bool debug_log = false;  // CRAFTPP_FEATURE_LOG chunk being decorated
inline bs::Dir random_dir(RandomSource& r) { return ALL_DIRS[r.nextInt(6)]; }             // Direction.getRandom
inline bs::Dir random_horizontal(RandomSource& r) { return HORIZONTAL[r.nextInt(4)]; }  // Plane.HORIZONTAL.getRandomDirection
inline int java_round(float v) { return Mth::floor((double)v + 0.5); }                    // Math.round(float)
inline int ceil_i(double v) { return Mth::d2i(std::ceil(v)); }                            // Mth.ceil

// WorldGenLevel over the FEATURES region (WorldGenRegion).
class Level {
 public:
  Level(Region& r, const Features& f) : r(&r), f(&f) {}
  explicit Level(Region& r) : r(&r), f(nullptr) {}  // block access only (no biome/seed/random queries)
  virtual ~Level() = default;
  virtual uint16_t get(const P& p) const;
  uint16_t get(int x, int y, int z) const { return get(P{x, y, z}); }
  virtual bool set(const P& p, int state);  // WorldGenRegion.setBlock (+ block entity bookkeeping)
  virtual bool can_write(const P& p) const { return r && r->writable(p.x, p.z); }  // ensureCanWrite
  virtual int height(Heightmap h, int x, int z) const;  // getHeight (first free y)
  virtual int biome(const P& p) const;                  // getBiome (BiomeManager zoom over stored noise biomes)
  bool empty(const P& p) const { return bs::R().is_air(get(p)); }  // isEmptyBlock
  bool outside(int y) const { return y < min_y() || y > max_y(); }
  bool water_at(const P& p) const { return bs::R().is_water(get(p)); }  // isWaterAt (FluidTags.WATER)
  virtual RandomSource& random();                 // WorldGenRegion.getRandom()
  virtual BlockEntity* block_entity(const P& p);  // the block entity at p, if the block has one
  virtual int64_t seed() const;
  virtual int sea_level() const { return GEN_SEA_LEVEL; }
  virtual int min_y() const;
  virtual int max_y() const;
  virtual float temperature(int biome, const P& p) const;  // Biome.getTemperature(pos, seaLevel)
  virtual bool should_freeze(int biome, const P& p, bool checkNeighbors) const;  // Biome.shouldFreeze
  virtual bool should_snow(int biome, const P& p) const;                         // Biome.shouldSnow
  virtual void mark_post_processing(const P& p);  // ChunkAccess.markPosForPostProcessing
  virtual void schedule_leaf_tick(const P& p);     // scheduleTick(pos, LeavesBlock, 1): run by Features::finish
  void mark_above_for_post_processing(const P& p) {  // Feature.markAboveForPostProcessing
    P q = p;
    for (int i = 0; i < 2; i++) {
      q = q.above();
      if (empty(q)) return;
      mark_post_processing(q);
    }
  }
  Region& region() { return *r; }
  const Features& features() const { return *f; }

 protected:
  Level() = default;
  Region* r = nullptr;
  const Features* f = nullptr;
  std::unique_ptr<RandomSource> rand;
};

struct BlockPredicate {
  virtual ~BlockPredicate() = default;
  virtual bool test(Level& l, const P& p) const = 0;
};
struct StateProvider {  // BlockStateProvider: get never -1; optional -1 = null
  virtual ~StateProvider() = default;
  virtual int get(Level& l, RandomSource& r, const P& p) const = 0;
  virtual int optional(Level& l, RandomSource& r, const P& p) const { return get(l, r, p); }
};
struct RuleTest {  // structure.templatesystem.RuleTest
  virtual ~RuleTest() = default;
  virtual bool test(int state, const P& p, RandomSource& r) const = 0;
};
struct Feature {
  virtual ~Feature() = default;
  virtual bool place(Level& l, RandomSource& r, const P& origin) const = 0;
};
struct PlacementContext {
  Level& level;
  int top;  // placed feature id for the biome filter (-1: none)
};
struct Modifier {
  virtual ~Modifier() = default;
  virtual void modify(PlacementContext& c, RandomSource& r, const P& p, std::vector<P>& out) const = 0;
};
struct Placed {
  const Feature* feature = nullptr;
  std::vector<const Modifier*> mods;
  int id = -1;  // index into Features::placedList for registry placed features (biome filter identity)
  std::string name;  // registry id (debug)
  bool place(Level& l, RandomSource& r, const P& origin, bool biomeCheck = false) const;  // FeaturePlacer.place
};

// Column.scan: floor / ceiling of the run of insideColumn states through p (nullopt edge = none found).
struct ColumnScan {
  bool found = false;
  std::optional<int> floor, ceiling;
};
template <class In, class Edge>
ColumnScan column_scan(Level& l, const P& p, int range, In inside, Edge edge) {
  ColumnScan c;
  if (!inside(l.get(p))) return c;
  c.found = true;
  for (int dir : {1, -1}) {
    P q = p;
    for (int i = 1; i < range && inside(l.get(q)); i++) q.y += dir;
    if (edge(l.get(q))) (dir > 0 ? c.ceiling : c.floor) = q.y;
  }
  return c;
}
template <class T>
void shuffle(std::vector<T>& v, RandomSource& r) {  // Util.shuffle
  for (int i = int(v.size()); i > 1; i--) std::swap(v[size_t(i - 1)], v[size_t(r.nextInt(i))]);
}
inline std::vector<bs::Dir> all_shuffled(RandomSource& r) {  // Direction.allShuffled
  std::vector<bs::Dir> v(ALL_DIRS, ALL_DIRS + 6);
  shuffle(v, r);
  return v;
}

// Block behaviour needed by features (block_behavior.cpp).
bool can_survive(Level& l, int state, const P& p);         // BlockState.canSurvive
void place_double_plant(Level& l, int state, const P& p);  // DoublePlantBlock.placeAt
void place_mossy_carpet(Level& l, const P& p, RandomSource& r);  // MossyCarpetBlock.placeAt
bool multiface_can_attach(Level& l, bs::Dir dir, const P& neighbour);  // MultifaceBlock.canAttachTo
// MultifaceBlock.getStateForPlacement (-1 = null) and the default MultifaceSpreader.
int multiface_placement(Level& l, int block, int oldState, const P& p, bs::Dir dir);
struct Spreader {  // MultifaceSpreader config: default, or SculkVeinSpreaderConfig (sculk = true)
  int block;  // block index of the multiface block
  bool sculk = false, sameSpaceOnly = false;
};
bool multiface_spread_from_face_random(Level& l, const Spreader& sp, int state, const P& p, bs::Dir face, RandomSource& r, bool postProcess);
long multiface_spread_all(Level& l, const Spreader& sp, int state, const P& p, bool postProcess);
// BlockState.updateShape(dir, neighbour) for the blocks worldgen touches (survival, snowy, multiface, vines);
// other blocks are returned unchanged. update_from_neighbour_shapes = Block.updateFromNeighbourShapes.
int update_shape(Level& l, int state, const P& p, bs::Dir dir, const P& neighbourPos, int neighbour);
int update_from_neighbour_shapes(Level& l, int state, const P& p);
// StructureTemplate.updateShapeAtEdge(level, flags, shape, min) over the box min + [0,sx)x[0,sy)x[0,sz) where
// full(x, y, z) is the voxel shape; includes LeavesBlock.updateShape's scheduled leaf ticks.
void update_shape_at_edge(Level& l, const P& min, int sx, int sy, int sz, const std::function<bool(int, int, int)>& full);
// LevelChunk.postProcessGeneration for the region's center chunk plus the leaf ticks worldgen scheduled
// (the FULL step; Features::finish). Needs no Features (structures-only stage runs it too).
void post_process(Region& r);

class Loader;
using FeatureParser = std::function<const Feature*(Loader&, const Json&)>;
using ParserMap = std::unordered_map<std::string, FeatureParser>;
void register_basic_features(ParserMap& out);  // feature_basic.cpp
void register_tree_features(ParserMap& out);   // feature_tree.cpp
void register_cave_features(ParserMap& out);   // feature_cave.cpp
void register_misc_features(ParserMap& out);   // feature_misc.cpp
void register_nether_end_features(ParserMap& out);  // feature_nether_end.cpp
// EndSpikeFeature.EndSpike / getSpikesForLevel (dragon fight: crystal respawn) and the end gateway block entity
// NBT with an exit (EndGatewayFeature.knownExit; mobs-bosses places gateways on dragon death).
struct EndSpike { int x, z, radius, height; bool guarded; };
std::vector<EndSpike> end_spikes(int64_t seed);
std::vector<uint8_t> end_gateway_nbt(int x, int y, int z, bool exact);
// Extra feature types registered by other subsystems before the Overworld is built (static init or setup):
// e.g. the structures agent adds "template" and "fossil" (StructureTemplate.placeInWorld + processors), which
// this branch leaves unimplemented (they place nothing and are reported once at load).
ParserMap& extra_feature_parsers();

// Loads a configured feature by name (e.g. "oak", "huge_brown_mushroom").
const Feature* load_feature_by_name(const std::string& name, const std::string& root = "data/vanilla");

// Parses worldgen JSON into owned objects; registry ids are memoized.
class Loader {
 public:
  Loader(Features& f, const std::string& root) : f(&f), root(root) {}
  Loader(ParserMap& parsers, const std::string& root) : f(nullptr), parsers_override(&parsers), root(root) {}
  const Placed* placed(const Json& j);
  const Feature* feature(const Json& j);
  const BlockPredicate* predicate(const Json& j);
  const StateProvider* provider(const Json& j);
  const RuleTest* rule(const Json& j);
  const Modifier* modifier(const Json& j);
  int state(const Json& j) const;  // "minecraft:x", "minecraft:x[a=b]", {"id","properties"}, {"Name","Properties"}
  bs::Tag block_set(const Json& j) const;  // HolderSet<Block>: "id", "#tag", [ids]
  std::vector<int> block_list(const Json& j) const;  // same, as block indices in order (tags in tag order)
  std::shared_ptr<const Noise> noise(const Json& params, int64_t seed) const;  // NormalNoise(p).create(WorldgenRandom(Legacy(seed)))
  Json load(const std::string& kind, const std::string& id) const;  // worldgen/<kind>/<id>.json
  template <class T, class... A>
  T* make(A&&... a) {
    auto* h = new Holder<T>(std::forward<A>(a)...);
    owned.emplace_back(h);
    return &h->v;
  }
  Features* f = nullptr;
  ParserMap* parsers_override = nullptr;
  std::string root;

 private:
  struct Owned { virtual ~Owned() = default; };
  template <class T> struct Holder : Owned {
    T v;
    template <class... A> explicit Holder(A&&... a) : v(std::forward<A>(a)...) {}
  };
  std::vector<std::unique_ptr<Owned>> owned;
  std::unordered_map<std::string, const Placed*> placedById;
  std::unordered_map<std::string, const Feature*> featureById;
  std::unordered_map<std::string, const StateProvider*> providerById;
};

}  // namespace feat

class Features {
 public:
  Features(Overworld& ow, const std::string& dataRoot);
  ~Features();
  void decorate(Region& r) const;    // the FEATURES step
  // The FULL step: LevelChunk.postProcessGeneration (marked non-fluid blocks: updateFromNeighbourShapes) and the
  // leaf ticks worldgen scheduled (LeavesBlock.tick -> updateDistance, cascading to neighbours that change), which
  // vanilla runs once the chunk ticks. Writes into neighbours already handed to the World are skipped.
  void finish(Region& r) const;
  StructurePlacer structure_placer;  // structures hook (see top)
  // Loads a placed feature by id (structures: feature pool elements). Not thread-safe: call during setup.
  const feat::Placed* load_placed(const std::string& id);

  // Internal (feat::Level and feature code).
  Overworld& ow;
  const material::MaterialSystem* materials = nullptr;
  std::shared_ptr<const Noise> biomeInfoNoise;            // Biome.BIOME_INFO_NOISE
  const PositionalRandomFactory* regionRandom = nullptr;  // "minecraft:worldgen_region_random"
  feat::ParserMap parsers;
  std::vector<const feat::Placed*> placedList;            // registry placed features by id
  std::vector<std::vector<uint8_t>> biomeHas;             // biome -> placed id -> BiomeGenerationSettings.hasFeature
  std::vector<std::vector<std::vector<int>>> biomeSteps;  // biome -> step -> global feature indices
  std::vector<std::vector<const feat::Placed*>> steps;    // FeatureSorter: step -> global index -> placed
  std::vector<uint8_t> possibleBiome;                     // biome -> in biomeSource.possibleBiomes()
  std::vector<uint8_t> hasPrecipitation;                  // biome -> has_precipitation

 private:
  std::unique_ptr<feat::Loader> loader;
};

}  // namespace worldgen
