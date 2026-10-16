// Port of the 26.3 overworld noise stage: NoiseBasedChunkGenerator.doFill (final_density volume -> stone /
// aquifer fluid / air), Aquifer.NoiseBasedAquifer, ChunkGenerator.doCreateBiomes (MultiNoiseBiomeSource
// createResolverForChunk + fillBiomesFromNoise) and getBaseHeight. No structures (beardifier 0), no blending.
// Writes blocks only through Chunk::set and biomes through Chunk::set_biome.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
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
  FluidStatus globalFluid(int y) const;
  int getIndex(int gx, int gy, int gz) const;
  const FluidStatus& status(int index);
  FluidStatus computeFluid(int x, int y, int z);
  int surfaceLevel(int x, int z);
  int maxSurfaceLevel(int minX, int minZ, int maxX, int maxZ);
  double calculatePressure(int x, int y, int z, double& barrier, const FluidStatus& a, const FluidStatus& b);

  TerrainGenerator& g;
  bool enabled;  // false: Aquifer.createDisabled (no aquifers section: nether, end)
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
  // Noise range (noise_settings noise, clamped to the chunk), chunk layout (dimension type), settings values.
  int MIN_Y = -64, HEIGHT = 384, SEA_LEVEL = 63, chunkMinY = -64, chunkHeight = 384;
  int defaultBlock, defaultFluid;
  bool endBiomes = false;  // TheEndBiomeSource instead of multi-noise

  // NoiseChunk: the caching sampler context + aquifer of one chunk's noise stage. Later stages that vanilla
  // runs on noiseChunk.cachingSamplers() (material rules) must sample through `ctx` to stay exact.
  struct NoiseChunk {
    df::SamplerContext ctx{true};
    DensityVolume volume;
    std::unique_ptr<Aquifer> aquifer;
  };

  // surface=false stops after the noise stage (no material rules: bedrock, ore veins, surface blocks).
  // settings: noise_settings name (overworld, nether, end); chunkMinY/chunkHeight: the dimension type's layout.
  explicit TerrainGenerator(int64_t seed, const std::string& dataRoot = "vanilla/src/data", bool surface = true,
                            const std::string& settings = "overworld", int chunkMinY = -64, int chunkHeight = 384);
  ~TerrainGenerator();
  void generate(Chunk& c);  // biomes, noise fill, material rules
  void createBiomes(Chunk& c);
  void chunkBiomes(int cx, int cz, const BiomeSource* source, std::vector<uint16_t>& out);
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
  std::unique_ptr<BiomeSource> biomeSource, neighbourBiomeSource;  // null for the end
  std::unique_ptr<material::MaterialSystem> materials;
};

}  // namespace worldgen
