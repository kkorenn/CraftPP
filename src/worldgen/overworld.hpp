// The overworld generator: TerrainGenerator (biomes, noise, material rules), carvers and biome decoration
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
  Overworld(int64_t seed, const std::string& stages, const std::string& dataRoot = "vanilla/src/data");
  ~Overworld();
  int64_t seed;
  std::shared_ptr<TerrainGenerator> terrain;
  std::unique_ptr<Carvers> carvers;
  std::unique_ptr<Features> features;
  Pipeline pipeline;
  // Structures hook (structure/hooks.cpp): Beardifier.forStructuresInChunk for the TERRAIN step, null = none.
  std::function<std::shared_ptr<const df::Sampler>(int cx, int cz)> beardifier;
};

}  // namespace worldgen
