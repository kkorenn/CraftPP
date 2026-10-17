// Port of net.minecraft.world.level.levelgen.material (26.3): MaterialSystem, MaterialRuleContext, and every
// material_rule / material_condition type (block, bandlands, sequence, condition, ore_vein; biome, noise_threshold,
// vertical_gradient, y_above, water, temperature, steep, not, hole, above_preliminary_surface, stone_depth),
// loaded from data/<ns>/worldgen/material_{rule,condition}. Bit-exact with vanilla.
//
// INTEGRATION (vanilla NoiseBasedChunkGenerator.buildTerrain -> buildSurface):
//  - Build one MaterialSystem per world: MaterialSystem(randomState) right after RandomState. Thread-safe const
//    afterwards (it pre-creates every noise / positional random it needs from the RandomState).
//  - Call buildSurface(chunk, ctx) right after the noise fill (doFill: final_density + aquifer -> stone/water/lava/air)
//    and BEFORE carvers. The chunk must contain only the fill result (heightmaps are recomputed from blocks).
//    Carvers' applyCarvingMask later calls topMaterial(...) for dirt under carved grass.
//  - MaterialContext (implemented by the density engine, one per chunk = vanilla noiseChunk.cachingSamplers()):
//      sampleDensity(id, x,y,z)       DensitySamplerSet.sampleValue (point path)
//      sampleDensityVolume(id, buf, v) DensitySampler.Bound.sampleVolume (volume path; buf already sized v.size(),
//                                     index = y + (x + z*sizeX)*sizeY, step 1)
//      Ids used for the overworld: "minecraft:overworld/chunk_surface_level" (volume 16x1x16 at y=0, chunk min
//      corner), "minecraft:overworld/ore_vein/{copper_density,iron_density,richness}" (volume 16 x n x 16 from
//      y=-64, n = up to the top of the highest non-air section; point path outside it),
//      "minecraft:overworld/ore_vein/gap" (always point path).
//      noiseBiome(qx,qy,qz)  biome id (gen/ids.hpp biomes::) of the multi-noise biome source at quart coords, NOT
//      clamped (buildSurface clamps qy to the chunk like ChunkAccess.getNoiseBiome; topMaterial does not, like the
//      carver's uncached BiomeResolver). Called for quarts up to 1 outside the chunk (BiomeManager zoom).
//  - Fluids are not scheduled for post-processing (vanilla markPosForPostProcessing); add when fluid ticking exists.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "worldgen/mth.hpp"
#include "worldgen/noise.hpp"

struct Chunk;

namespace material {

struct MaterialContext {
  virtual ~MaterialContext() = default;
  virtual float sampleDensity(const std::string& dfId, int x, int y, int z) = 0;
  virtual void sampleDensityVolume(const std::string& dfId, DensityBuffer& out, const DensityVolume& volume) = 0;
  virtual int noiseBiome(int quartX, int quartY, int quartZ) = 0;
};

// Xoroshiro or Legacy (legacy_random_source: nether, end) PositionalRandomFactory by value: at() builds the
// RandomSource on the stack (no per-block heap).
struct PosRng {
  bool legacy;
  XoroshiroRandomSource x;
  LegacyRandomSource l;
  RandomSource& r() { return legacy ? static_cast<RandomSource&>(l) : x; }
  int64_t nextLong() { return r().nextLong(); }
  int nextInt(int bound) { return r().nextInt(bound); }
  float nextFloat() { return r().nextFloat(); }
  double nextDouble() { return r().nextDouble(); }
};
struct PosRandom {
  int64_t lo = 0, hi = 0;  // legacy: lo = LegacyPositionalRandomFactory seed
  bool legacy = false;
  PosRng at(int x, int y, int z) const {
    int64_t s = Mth::getSeed(x, y, z) ^ lo;
    return PosRng{legacy, XoroshiroRandomSource(s, hi), LegacyRandomSource(s)};
  }
};

struct Node;  // parsed rule / condition (material.cpp)
struct Eval;

class MaterialSystem {
 public:
  // Reads noise_settings/<settingsId> (default_block, sea_level, material_rule, noise.min_y/height,
  // noise_router.chunk_surface_level) plus the referenced rules/conditions and biome temperatures; block states from data/blocks.bin.
  explicit MaterialSystem(RandomState& randomState, const std::string& settingsId = "minecraft:overworld",
                          const std::string& dataRoot = "vanilla/src/data",
                          bool keepLoader = false);  // true: keep blocks table so stateId/parse*File work later
  ~MaterialSystem();

  // possibleBiomes (optional, vanilla ChunkStatusTasks.collectPossibleBiomes(region, 1)): biome id -> 1 if it occurs in
  // the stored biomes of this chunk or its 8 neighbours. Pure speed-up (biome conditions resolve per chunk); must be a
  // superset of what the zoom can reach or results change.
  void buildSurface(Chunk& chunk, MaterialContext& ctx, const std::vector<uint8_t>* possibleBiomes = nullptr) const;
  // MaterialSystem.topMaterial (carver grass fix-up): state id or -1. x,y,z world coords inside chunk.
  int topMaterial(Chunk& chunk, MaterialContext& ctx, int x, int y, int z, bool underFluid) const;

  // Exposed for tests / other stages.
  int getSurfaceDepth(int blockX, int blockZ) const;
  double getSurfaceSecondary(int blockX, int blockZ) const;
  int getBand(int x, int y, int z) const;
  int zoomedBiome(MaterialContext& ctx, int x, int y, int z, bool clampY) const;  // BiomeManager.getBiome
  float biomeTemperature(int biome, int x, int y, int z) const;                  // Biome.getTemperature
  static int64_t obfuscateSeed(int64_t seed);                                     // BiomeManager.obfuscateSeed
  int stateId(const std::string& blockName) const;  // default state, -1 if unknown (needs keepLoader)
  int biomeId(const std::string& name) const;
  int seaLevel() const { return seaLevel_; }
  int ruleCount() const { return (int)nodes.size(); }
  PosRandom randomFactory(const std::string& id) const { return positional(id); }
  // Parses data/<ns>/worldgen/material_{rule,condition}/<path>.json (and what it references); throws on error.
  void parseRuleFile(const std::string& id);
  void parseConditionFile(const std::string& id);

 private:
  friend struct Eval;
  int defaultBlock = 0, seaLevel_ = 63, minY = -64, height = 384;
  int64_t biomeZoomSeed = 0;
  std::string preliminarySurfaceFunction;
  std::array<int, 192> clayBands{};
  std::shared_ptr<const Noise> clayBandsOffsetNoise, badlandsPillarNoise, badlandsPillarRoofNoise, badlandsSurfaceNoise,
      icebergPillarNoise, icebergPillarRoofNoise, icebergSurfaceNoise, surfaceNoise, surfaceSecondaryNoise;
  PosRandom noiseRandom;  // RandomState.random
  std::shared_ptr<const Noise> temperatureNoise, frozenTemperatureNoise, biomeInfoNoise;
  int air = 0, caveAir = 0, voidAir = 0, waterMin = 0, waterMax = 0, lavaMin = 0, lavaMax = 0;
  int snowBlock = 0, packedIce = 0, erodedBadlands = -1, frozenOcean = -1, deepFrozenOcean = -1;
  std::vector<std::string> biomeNames;  // sorted registry order = biomes:: ids
  struct Climate { float temperature = 0; bool frozen = false; };
  std::vector<Climate> climate;

  std::vector<std::unique_ptr<Node>> nodes;
  const Node* root_ = nullptr;
  std::vector<std::shared_ptr<const Noise>> slotNoise;  // noise_threshold noises, one slot per (id, is_3d)
  std::vector<bool> slotIs3d;
  std::vector<const Node*> oreRules, biomeRules;
  struct Loader;                   // parsing (material.cpp)
  std::unique_ptr<Loader> loader;  // compact blocks table + parsed-file memo
  RandomState* randomState;
  PosRandom positional(const std::string& id) const;  // RandomState.getOrCreateRandomFactory(id)
  std::string dataRoot;

  struct Fiddle { double x, y, z; };
  Fiddle fiddles(int quartX, int quartY, int quartZ) const;
  template <class FiddleAt>
  int zoomed(MaterialContext& ctx, int x, int y, int z, bool clampY, FiddleAt&& at) const;
  bool isAir(int s) const { return s == air || s == caveAir || s == voidAir; }
  bool isWater(int s) const { return s >= waterMin && s <= waterMax; }
  bool hasFluid(int s) const { return isWater(s) || (s >= lavaMin && s <= lavaMax); }
  void erodedBadlandsExtension(Eval& ev, int x, int z, int height) const;
  void frozenOceanExtension(Eval& ev, int minSurfaceLevel, int biome, int x, int z, int height) const;
};

}  // namespace material
