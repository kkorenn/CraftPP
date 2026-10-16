// The generator of one dimension (named for the overworld it was written for): TerrainGenerator (biomes, noise, material rules), carvers and biome decoration
// wired into a Pipeline (pipeline.hpp). World::generator = [ow](Chunk& c) { ow->pipeline.take(c.cx, c.cz, c); }.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "worldgen/carver.hpp"
#include "worldgen/pipeline.hpp"
#include "worldgen/terrain.hpp"

namespace worldgen {

class Features;

struct Overworld {
  // stages (server.properties worldgen-stages): noise | surface | carvers | features (= everything).
  // pack: optional datapack dir whose biome JSON overrides vanilla's (compare.py stage packs).
  // settings: noise_settings name (overworld, nether, end); chunkMinY/chunkHeight: the dimension type's layout.
  // The same pipeline (biomes, terrain + surface + carvers, features) generates every dimension.
  Overworld(int64_t seed, const std::string& stages, const std::string& dataRoot = "vanilla/src/data", const std::string& pack = "",
            const std::string& settings = "overworld", int chunkMinY = -64, int chunkHeight = 384);
  ~Overworld();
  int64_t seed;
  std::string pack;
  std::shared_ptr<TerrainGenerator> terrain;
  std::unique_ptr<Carvers> carvers;
  std::unique_ptr<Features> features;
  Pipeline pipeline;
  // Structures hook (structure/hooks.cpp): Beardifier.forStructuresInChunk for the TERRAIN step, null = none.
  std::function<std::shared_ptr<const df::Sampler>(int cx, int cz)> beardifier;
};

}  // namespace worldgen
