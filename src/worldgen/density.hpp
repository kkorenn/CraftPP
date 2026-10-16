// Port of net.minecraft.world.level.levelgen.densityfunction (26.3): density function trees parsed from
// data/<ns>/worldgen/density_function/*.json, DensityFunctionCompiler (reference inlining, cache dedup,
// SliceUniformAxes rewrite) and the compiled samplers with vanilla's sampleValue (point) and sampleVolume
// paths. Bit-exact: both paths and SamplerContext caching (incl. point lookups served from a cached volume)
// behave like vanilla, so call sites must use the same path, volumes and call order as vanilla.
//
// API (for terrain / material rules):
//   DensityFunctions dfs(seed);                       // one per world seed (owns RandomState + compiler)
//   NodeP f = dfs.function("minecraft:overworld/chunk_surface_level");   // registry reference, like a holder
//   NodeP g = dfs.parse(json);                        // inline DF json (numbers, "id" strings, objects)
//   float v = dfs.sample("minecraft:overworld/depth", x, y, z);          // point path, uncached context
//   SamplerContext ctx(true);                         // vanilla SamplerContext.builder().enableCaches()
//   Bound b = dfs.bind(f, ctx);  b.sampleValue(x,y,z);  b.sampleVolume(buf, volume);
//   TerrainGenerator::NoiseChunk::ctx is the noise stage's caching context; reuse it (and dfs.bind) for
//   anything vanilla samples through noiseChunk.cachingSamplers() (e.g. material rules), in vanilla order.
// Thread safety: function/parse/bind/sampler are internally locked; a SamplerContext is single-threaded.
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "worldgen/json.hpp"
#include "worldgen/noise.hpp"

namespace df {

// net.minecraft.util.Interval with NaI = NaN bounds.
struct Iv {
  float min, max;
  bool isNaI() const { return std::isnan(min); }
};

class SamplerContext;
struct Sampler {
  virtual ~Sampler() = default;
  virtual void sampleVolume(SamplerContext& ctx, DensityBuffer& out, const DensityVolume& v) const = 0;
  virtual float sampleValue(SamplerContext& ctx, int x, int y, int z) const = 0;
};
using SamplerP = std::shared_ptr<const Sampler>;

class SamplerContext {
 public:
  explicit SamplerContext(bool enableCaches) : caches(enableCaches) {}
  // User field Beardifier.CONTEXT_KEY (NoiseChunk): the "beardifier" function samples this, 0 when null.
  const Sampler* beardifier = nullptr;
  void sampleVolumeCached(int id, const Sampler& input, DensityBuffer& out, const DensityVolume& v);
  float sampleValueCached(int id, const Sampler& input, int x, int y, int z);

 private:
  struct Cell {
    bool hasVolume = false;
    DensityVolume volume{1, 1, 1, 0, 0, 0};
    std::vector<float> buffer;
    int64_t valueKey = 0;
    float value = NAN;
  };
  bool caches;
  std::vector<Cell> cells;
};

enum class K {
  Constant, Ref, Noise, ShiftA, ShiftB, Shift, Gradient, Unary, Binary, Clamp, Lerp, RangeChoice, IntervalSelect,
  Cache, Interpolated, Slice, FindTopSurface, Spline, BlendAlpha, BlendOffset, Beardifier, BlendDensity,
  OldBlendedNoise, Prepared, DistanceToPoint, EndIslands
};
enum Unary { ABS, SQUARE, CUBE, SQRT, HALF_NEGATIVE, QUARTER_NEGATIVE, RECIPROCAL, NEGATE, SQUEEZE, LOG, SIGN };
enum Binary { ADD, SUB, MUL, DIV, MIN, MAX };

struct Node;
using NodeP = std::shared_ptr<const Node>;
struct Spline {  // CubicSpline<Coordinate>: constant when coordinate is null
  float value = 0;
  NodeP coordinate;
  std::vector<float> locations, derivatives;
  std::vector<std::shared_ptr<const Spline>> values;
};

// One immutable density function node. Field use per kind is documented in density.cpp (parse()).
struct Node {
  K k;
  int op = 0;  // unary/binary type, axis (0 x, 1 y, 2 z)
  std::vector<float> f;
  std::vector<int> i;
  std::vector<double> d;
  std::string id;  // noise id (Noise/Shift*), function id (Ref)
  std::vector<NodeP> kids;
  std::shared_ptr<const Spline> spline;
  NodeP target;  // Ref: the referenced function
  // Prepared (DensityFunctionCompiler.PreparedCache)
  int cacheId = -1;
  Iv range{0, 0};
  int axes = 0;
  SamplerP sampler;
};

struct Bound {  // DensitySampler.Bound
  SamplerP sampler;
  SamplerContext* ctx;
  float sampleValue(int x, int y, int z) const { return sampler->sampleValue(*ctx, x, y, z); }
  void sampleVolume(DensityBuffer& out, const DensityVolume& v) const { sampler->sampleVolume(*ctx, out, v); }
};

class DensityFunctions {
 public:
  // legacyRandom: noise_settings legacy_random_source (nether, end).
  explicit DensityFunctions(int64_t seed, std::string dataRoot = "vanilla/src/data", bool legacyRandom = false);
  NodeP function(const std::string& id);  // HolderHolder of a registry entry
  NodeP parse(const Json& j);
  SamplerP sampler(const NodeP& f);  // RandomState.getSampler
  Bound bind(const NodeP& f, SamplerContext& ctx) { return {sampler(f), &ctx}; }
  float sample(const std::string& id, int x, int y, int z);  // sampleBlockValueUncached
  RandomState& randomState() { return rs; }
  const std::string& dataRoot() const { return root; }

  // Exposed for tests.
  static Iv range(const NodeP& f);
  static int domainAxes(const NodeP& f);

 private:
  NodeP parseLocked(const Json& j);
  NodeP functionLocked(const std::string& id);
  NodeP optimize(const NodeP& f);
  NodeP cacheRule(const NodeP& f);
  NodeP sliceRule(int parentAxes, const NodeP& f);
  SamplerP compile(const NodeP& f);

  RandomState rs;
  std::string root;
  std::recursive_mutex m;
  std::map<std::string, NodeP> registry;
  std::map<const Node*, SamplerP> samplers;
  std::vector<NodeP> keepAlive;  // keys of `samplers`
  std::map<std::string, NodeP> preparedCaches;  // key: structural form of cache.input()
  int nextCacheId = 0;
};

}  // namespace df
