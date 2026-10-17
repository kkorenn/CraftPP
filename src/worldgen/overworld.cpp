#include "worldgen/overworld.hpp"

#include "worldgen/features.hpp"

namespace worldgen {

Overworld::~Overworld() = default;

Overworld::Overworld(int64_t seed, const std::string& stages, const std::string& root, const std::string& pack,
                     const std::string& settings, int chunkMinY, int chunkHeight)
    : seed(seed), pack(pack),
      terrain(std::make_shared<TerrainGenerator>(seed, root, stages != "noise", settings, chunkMinY, chunkHeight)) {
  pipeline.min_y = chunkMinY, pipeline.height = chunkHeight;
  pipeline.context = {terrain->MIN_Y, terrain->HEIGHT, terrain->SEA_LEVEL};  // WorldGenerationContext
  if (stages == "carvers" || stages == "features") carvers = std::make_unique<Carvers>(root, pack);
  if (stages == "features") features = std::make_unique<Features>(*this, root);

  pipeline.set_step(Status::BIOMES, [this](Region& r) { terrain->createBiomes(*r.center().chunk); });
  pipeline.set_step(Status::TERRAIN, [this](Region& r) {
    // WorldGenRegion.getNoiseBiome over stored biomes (neighbours are at BIOMES).
    auto noiseBiome = [&r](int qx, int qy, int qz) {
      ProtoChunk* p = r.chunk(qx >> 2, qz >> 2);
      return int(p->chunk->get_biome(qx & 3, qy, qz & 3));
    };
    std::unique_ptr<TerrainGenerator::ClimateSampler> sampler;
    auto carverBiome = [&](int sx, int sz) {
      ProtoChunk* p = r.chunk(sx, sz);
      int16_t b = p->carver_biome.load(std::memory_order_relaxed);
      if (b < 0) {
        if (!sampler) sampler = terrain->climateSampler();
        b = int16_t(terrain->uncachedBiome(*sampler, (sx * 16) >> 2, 0, (sz * 16) >> 2));
        p->carver_biome.store(b, std::memory_order_relaxed);
      }
      return int(b);
    };
    auto beard = beardifier ? beardifier(r.cx, r.cz) : nullptr;
    terrain->buildTerrain(*r.center().chunk, noiseBiome, carvers.get(), carverBiome, beard.get());
  });
  if (features) {
    pipeline.set_step(Status::FEATURES, [this](Region& r) { features->decorate(r); });
    pipeline.set_step(Status::FULL, [this](Region& r) { features->finish(r); }, 1);
  }
}

}  // namespace worldgen
