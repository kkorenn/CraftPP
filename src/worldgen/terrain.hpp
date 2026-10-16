// Port of the 26.3 overworld noise stage: NoiseBasedChunkGenerator.doFill (final_density volume -> stone /
// aquifer fluid / air), Aquifer.NoiseBasedAquifer, ChunkGenerator.doCreateBiomes (MultiNoiseBiomeSource
// createResolverForChunk + fillBiomesFromNoise) and getBaseHeight. No structures (beardifier 0), no blending.
// Writes blocks only through Chunk::set and biomes through Chunk::set_biome.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "worldgen/biome_source.hpp"
#include "worldgen/density.hpp"

struct Chunk;
namespace material { class MaterialSystem; }

namespace worldgen {

class TerrainGenerator;

// Aquifer.NoiseBasedAquifer bound to one NoiseChunk's caching context.
class Aquifer {
 public:
  Aquifer(TerrainGenerator& gen, df::SamplerContext& ctx, const DensityVolume& volume);
  // Block state for a position, or -1 (vanilla null) when solid: caller then uses default_block.
  int computeSubstance(int x, int y, int z, double density);

 private:
  struct FluidStatus {
    int level, type;
    int at(int y) const;
    bool operator==(const FluidStatus& o) const { return level == o.level && type == o.type; }
  };
  struct Location { int x, y, z; bool set = false; };
  static FluidStatus globalFluid(int y);
  int getIndex(int gx, int gy, int gz) const;
  const FluidStatus& status(int index);
  FluidStatus computeFluid(int x, int y, int z);
  int surfaceLevel(int x, int z);
  int maxSurfaceLevel(int minX, int minZ, int maxX, int maxZ);
  double calculatePressure(int x, int y, int z, double& barrier, const FluidStatus& a, const FluidStatus& b);

  df::Bound barrier, floodedness, spread, lava, exclusion, surface;
  const PositionalRandomFactory& random;
  std::vector<FluidStatus> statusCache;
  std::vector<bool> statusSet;
  std::vector<Location> locationCache;
  std::unordered_map<int64_t, int> surfaceLevelCache;
  int skipSamplingAboveY, minGridX, minGridY, minGridZ, gridSizeX, gridSizeZ;
};

class TerrainGenerator {
 public:
  static constexpr int MIN_Y = -64, HEIGHT = 384, SEA_LEVEL = 63;

  // NoiseChunk: the caching sampler context + aquifer of one chunk's noise stage. Later stages that vanilla
  // runs on noiseChunk.cachingSamplers() (material rules) must sample through `ctx` to stay exact.
  struct NoiseChunk {
    df::SamplerContext ctx{true};
    DensityVolume volume;
    std::unique_ptr<Aquifer> aquifer;
  };

  // surface=false stops after the noise stage (no material rules: bedrock, ore veins, surface blocks).
  explicit TerrainGenerator(int64_t seed, const std::string& dataRoot = "vanilla/src/data", bool surface = true);
  ~TerrainGenerator();
  void generate(Chunk& c);  // biomes, noise fill, material rules
  void createBiomes(Chunk& c);
  void chunkBiomes(int cx, int cz, const BiomeSource& source, std::vector<uint16_t>& out);
  std::unique_ptr<NoiseChunk> fillFromNoise(Chunk& c);
  std::unique_ptr<NoiseChunk> createNoiseChunk(const DensityVolume& volume);
  // getBaseHeight(x, z, WORLD_SURFACE_WG): y above the top non-air noise-stage block, or MIN_Y.
  int baseHeight(int x, int z);
  // NoiseSpawnFinder.findSpawnPosition: world spawn search origin (block x, z) from noise_settings spawn_target.
  std::pair<int, int> findSpawnPosition();

  df::DensityFunctions dfs;
  // noise_settings/overworld.json noise_router + aquifers
  df::NodeP temperature, vegetation, continents, erosion, depth, ridges, chunkSurfaceLevel, finalDensity;
  df::NodeP aqBarrier, aqFloodedness, aqSpread, aqLava, aqExclusion, aqSurfaceLevel;
  const PositionalRandomFactory* aquiferRandom;
  std::vector<std::vector<std::pair<std::string, std::pair<int64_t, int64_t>>>> spawnTarget;
  BiomeSource biomeSource, neighbourBiomeSource;
  // Recent chunks' biome grids (chunkBiomes layout, ids < 256): the surface stage reads neighbours' biomes like
  // vanilla's WorldGenRegion does, so a neighbour generated earlier is reused instead of recomputed.
  std::shared_ptr<const std::vector<uint8_t>> cachedBiomes(int cx, int cz);
  std::shared_ptr<const std::vector<uint8_t>> storeBiomes(int cx, int cz, const std::vector<uint16_t>& q);
  std::mutex biomeCacheMutex;
  std::map<std::pair<int, int>, std::shared_ptr<const std::vector<uint8_t>>> biomeCache;
  std::deque<std::pair<int, int>> biomeCacheOrder;
  std::unique_ptr<material::MaterialSystem> materials;
};

}  // namespace worldgen
