#include "worldgen/structure/hooks.hpp"

#include <mutex>
#include <unordered_map>

#include "worldgen/features.hpp"
#include "worldgen/material.hpp"
#include "worldgen/overworld.hpp"
#include "world.hpp"

namespace structure {
namespace {

// ChunkGenerator view over the noise stage (NoiseBasedChunkGenerator.getBaseHeight / getBaseColumn).
class TerrainView : public Generator {
 public:
  explicit TerrainView(std::shared_ptr<worldgen::TerrainGenerator> t) : t_(std::move(t)) {}
  int baseHeight(int x, int z, Heightmap type) override {
    bool ocean = type == Heightmap::OCEAN_FLOOR_WG || type == Heightmap::OCEAN_FLOOR;
    int64_t key = (int64_t)(uint32_t)x | (int64_t)(uint32_t)z << 32;
    {
      std::lock_guard lock(m_);
      auto it = heights_.find(key);
      if (it != heights_.end()) return ocean ? it->second.second : it->second.first;
    }
    // One noise column gives both WORLD_SURFACE_WG and OCEAN_FLOOR_WG (same column iteration as vanilla).
    std::vector<int> col;
    t_->baseColumn(x, z, col);
    int surface = MIN_Y, floor = MIN_Y;
    for (int y = GEN_HEIGHT - 1; y >= 0 && (surface == MIN_Y || floor == MIN_Y); y--) {
      if (surface == MIN_Y && col[y] != blocks::air) surface = MIN_Y + y + 1;
      if (floor == MIN_Y && col[y] == blocks::stone) floor = MIN_Y + y + 1;
    }
    std::lock_guard lock(m_);
    if (heights_.size() > 1 << 16) heights_.clear();  // ponytail: crude bound; heights are cheap to recompute
    heights_.emplace(key, std::pair{surface, floor});
    return ocean ? floor : surface;
  }
  void baseColumn(int x, int z, std::vector<int>& out) override { t_->baseColumn(x, z, out); }
  int noiseBiome(int qx, int qy, int qz) override {
    thread_local std::unique_ptr<worldgen::TerrainGenerator::ClimateSampler> sampler;
    thread_local const worldgen::TerrainGenerator* owner = nullptr;
    if (!sampler || owner != t_.get()) sampler = t_->climateSampler(), owner = t_.get();
    return t_->uncachedBiome(*sampler, qx, qy, qz);
  }
  float biomeTemperature(int biome, BlockPos p) override {
    return t_->materials ? t_->materials->biomeTemperature(biome, p.x, p.y, p.z) : 0.8f;
  }
  int64_t seed() const override { return t_->seed; }

 private:
  std::shared_ptr<worldgen::TerrainGenerator> t_;
  std::mutex m_;
  std::unordered_map<int64_t, std::pair<int, int>> heights_;  // (WORLD_SURFACE_WG, OCEAN_FLOOR_WG)
};

worldgen::Heightmap mapHeightmap(Heightmap h) {
  switch (h) {
    case Heightmap::WORLD_SURFACE_WG: return worldgen::WORLD_SURFACE_WG;
    case Heightmap::OCEAN_FLOOR_WG: return worldgen::OCEAN_FLOOR_WG;
    case Heightmap::WORLD_SURFACE: return worldgen::WORLD_SURFACE;
    case Heightmap::OCEAN_FLOOR: return worldgen::OCEAN_FLOOR;
    case Heightmap::MOTION_BLOCKING: return worldgen::MOTION_BLOCKING;
    default: return worldgen::MOTION_BLOCKING_NO_LEAVES;
  }
}

// WorldGenLevel over the FEATURES region.
class RegionLevel : public Level {
 public:
  RegionLevel(worldgen::Region& r, worldgen::TerrainGenerator& t) : r_(r), t_(t) {}
  int getBlock(int x, int y, int z) override {
    worldgen::ProtoChunk* c = r_.chunk(x >> 4, z >> 4);
    return c && c->chunk ? c->get(x & 15, y, z & 15) : blocks::air;  // vanilla only reads inside the region
  }
  void setBlock(int x, int y, int z, int state) override {
    if (r_.chunk(x >> 4, z >> 4)) r_.set(x, y, z, (uint16_t)state);
  }
  int getHeight(Heightmap type, int x, int z) override {
    worldgen::ProtoChunk* c = r_.chunk(x >> 4, z >> 4);
    return c ? c->height(mapHeightmap(type), x & 15, z & 15) : MIN_Y;
  }
  int64_t seed() const override { return t_.seed; }
  // WorldGenRegion.getBiome: BiomeManager zoom over the region's stored noise biomes.
  int biomeAt(int x, int y, int z) override {
    struct Ctx : material::MaterialContext {
      worldgen::Region& r;
      explicit Ctx(worldgen::Region& r) : r(r) {}
      float sampleDensity(const std::string&, int, int, int) override { return 0; }
      void sampleDensityVolume(const std::string&, DensityBuffer&, const DensityVolume&) override {}
      int noiseBiome(int qx, int qy, int qz) override {
        worldgen::ProtoChunk* c = r.chunk(qx >> 2, qz >> 2);
        int y = std::clamp(qy - (MIN_Y >> 2), 0, GEN_HEIGHT / 4 - 1);
        return c && c->chunk ? c->chunk->get_biome(qx & 3, y, qz & 3) : 0;
      }
    } ctx(r_);
    return t_.materials ? t_.materials->zoomedBiome(ctx, x, y, z, true) : ctx.noiseBiome(x >> 2, y >> 2, z >> 2);
  }
  RandomSource& random() override {
    if (!rand_) rand_ = t_.dfs.randomState().getOrCreateRandomFactory("minecraft:worldgen_region_random").at(r_.cx * 16, 0, r_.cz * 16);
    return *rand_;
  }

 private:
  worldgen::Region& r_;
  worldgen::TerrainGenerator& t_;
  std::unique_ptr<RandomSource> rand_;
};

struct ChunkStructures {  // ProtoChunk::structure_data
  std::vector<StartP> starts;
  StructureManager::References references;
};

std::vector<uint8_t> possibleBiomes(const worldgen::TerrainGenerator& t) {
  std::vector<uint8_t> out(biomeNames().size(), 0);
  for (auto& [point, biome] : t.biomeSource.values()) out[biome] = 1;
  return out;
}
}  // namespace

struct StructureWorld::Impl {
  TerrainView view;
  StructureManager manager;
  explicit Impl(std::shared_ptr<worldgen::TerrainGenerator> t) : view(t), manager(view, possibleBiomes(*t)) {}
};

StructureWorld::StructureWorld(worldgen::Overworld& ow, bool place) : impl_(std::make_unique<Impl>(ow.terrain)) {
  StructureManager* m = &impl_->manager;
  ow.pipeline.set_step(worldgen::Status::STRUCTURE_STARTS, [m](worldgen::Region& r) {
    auto d = std::make_shared<ChunkStructures>();
    d->starts = m->starts(r.cx, r.cz);
    r.center().structure_data = d;
  });
  ow.pipeline.set_step(worldgen::Status::STRUCTURE_REFERENCES, [m](worldgen::Region& r) {
    auto d = std::static_pointer_cast<ChunkStructures>(r.center().structure_data);
    if (d) d->references = m->references(r.cx, r.cz);
  });
  ow.beardifier = [m](int cx, int cz) { return m->beardifier(cx, cz); };
  // Persistence: piece state per start region file, starts/references in each chunk file.
  m->setPersistDir(world_dir + "/structures");
  chunk_save_extra = [m](const Chunk& c, Writer& w) {
    std::vector<uint8_t> blob;
    m->encodeChunk(c.cx, c.cz, blob);
    w.i32(int32_t(blob.size())).bytes(blob.data(), blob.size());
  };
  chunk_load_extra = [m](Chunk& c, Reader& r) {
    int32_t n = r.i32();
    if (n < 0 || r.i + size_t(n) > r.n) return;
    m->decodeChunk(c.cx, c.cz, r.p + r.i, size_t(n));
    r.i += size_t(n);
  };
  if (!place) return;
  worldgen::TerrainGenerator* terrain = ow.terrain.get();
  worldgen::Features* features = ow.features.get();
  // ponytail: the features step runs all structure steps before the features; switch to the features agent's
  // per-step structure_placer hook (same placeStep call) once it lands, so steps interleave like vanilla.
  ow.pipeline.set_step(worldgen::Status::FEATURES, [m, terrain, features](worldgen::Region& r) {
    RegionLevel level(r, *terrain);
    m->placeAll(level, r.cx, r.cz);
    if (features) features->decorate(r);
  });
}

StructureWorld::~StructureWorld() = default;
StructureManager& StructureWorld::manager() { return impl_->manager; }

}  // namespace structure
