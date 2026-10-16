// Port of levelgen/carver (26.3): CaveWorldCarver, CanyonWorldCarver, WorldCarver.carveEllipsoid and
// CarvingMask, driven like NoiseBasedChunkGenerator.generateCarvers (17x17 source chunks, WorldgenRandom over a
// LegacyRandomSource reseeded with setLargeFeatureSeed(seed + carverIndex, sourceX, sourceZ)).
// Carvers only mark the mask; applyCarvingMask (terrain.cpp) turns marked blocks into aquifer fluid / air.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "worldgen/providers.hpp"

namespace worldgen {

struct CarvingMask {  // net.minecraft.world.level.chunk.CarvingMask
  int minY, maxY, height;
  std::vector<uint64_t> bits;
  CarvingMask(int minY, int maxY) : minY(minY), maxY(maxY), height(maxY - minY + 1), bits(size_t((256 * height + 63) / 64)) {}
  void carve(int x, int y, int z) {
    size_t i = size_t(y - minY + (z + (x << 4)) * height);
    bits[i >> 6] |= 1ull << (i & 63);
  }
  bool empty() const {
    for (uint64_t b : bits) if (b) return false;
    return true;
  }
  // visit(x, z, bottomY, topY) per column run, in bit order (CarvingMask.visit / visitSegment).
  void visit(const std::function<void(int, int, int, int)>& f) const;
};

class Carvers {
 public:
  explicit Carvers(const std::string& dataRoot = "vanilla/src/data");
  // generateCarvers for chunk (cx, cz): biomeAt(sx, sz) = biome id (biomes::) of source chunk (sx, sz) at quart
  // (minBlockX >> 2, 0, minBlockZ >> 2) (getBiomeGenerationSettingsForCarver).
  void carve(int64_t seed, int cx, int cz, const std::function<int(int, int)>& biomeAt, CarvingMask& mask) const;

 private:
  struct Carver {
    bool canyon = false;
    float probability = 0;
    HeightProvider y;
    // cave
    IntProvider count;
    FloatProvider thickness, roomVerticalRadius, horizontalRadius, verticalRadius, startVerticalRadius, floorLevel;
    bool weirdThicknessBias = false;
    // canyon
    FloatProvider verticalRotation, distanceFactor, horizontalRadiusFactor, yScale;
    int widthSmoothness = 1;
    float verticalRadiusDefaultFactor = 0, verticalRadiusCenterFactor = 0;
  };
  std::vector<Carver> carvers;
  std::vector<std::vector<int>> biomeCarvers;  // biome id -> carver indices, in BiomeGenerationSettings order

  void carveCave(const Carver& c, RandomSource& r, int cx, int cz, int sx, int sz, CarvingMask& m) const;
  void carveCanyon(const Carver& c, RandomSource& r, int cx, int cz, int sx, int sz, CarvingMask& m) const;
  void caveTunnel(const Carver& c, int cx, int cz, int64_t seed, double x, double y, double z, double hMul, double vMul,
                  float thickness, float hRot, float vRot, int step, int dist, double yScale, double floor,
                  CarvingMask& m) const;
};

}  // namespace worldgen
