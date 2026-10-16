#include "worldgen/structure/hooks.hpp"

#include <climits>
#include <filesystem>
#include <functional>
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
  using FeatureMap = std::unordered_map<std::string, const worldgen::feat::Placed*>;
  RegionLevel(worldgen::Region& r, worldgen::TerrainGenerator& t, const worldgen::Features* f = nullptr,
              const FeatureMap* fm = nullptr)
      : r_(r), t_(t), f_(f), fm_(fm) {}
  // FeaturePoolElement.place: PlacedFeature.place(level, generator, random, pos) through the features code.
  bool placeFeature(const std::string& id, RandomSource& random, BlockPos pos) override {
    if (!f_ || !fm_) return false;
    auto it = fm_->find(id);
    if (it == fm_->end() || !it->second) return false;
    if (!fl_) fl_ = std::make_unique<worldgen::feat::Level>(r_, *f_);
    // ponytail: the feature's level.getRandom() is its own region random, not shared with this RegionLevel's
    return it->second->place(*fl_, random, {pos.x, pos.y, pos.z});
  }
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
  void markPostProcessing(int x, int y, int z) override { blockLevel().mark_post_processing({x, y, z}); }
  void updatePlacedShapes(BlockPos mn, BlockPos mx, const std::vector<BlockPos>& placed) override {
    namespace feat = worldgen::feat;
    feat::Level& l = blockLevel();
    int sx = mx.x - mn.x + 1, sy = mx.y - mn.y + 1, sz = mx.z - mn.z + 1;
    std::vector<uint8_t> shape(size_t(sx) * size_t(sy) * size_t(sz));
    auto cell = [&](int x, int y, int z) -> uint8_t& { return shape[(size_t(x) * size_t(sy) + size_t(y)) * size_t(sz) + size_t(z)]; };
    for (auto& p : placed) cell(p.x - mn.x, p.y - mn.y, p.z - mn.z) = 1;
    feat::update_shape_at_edge(l, {mn.x, mn.y, mn.z}, sx, sy, sz, [&](int x, int y, int z) { return cell(x, y, z) != 0; });
    for (auto& p : placed) {  // Block.updateFromNeighbourShapes (flags | UPDATE_KNOWN_SHAPE: no post-process mark)
      worldgen::feat::P q{p.x, p.y, p.z};
      int st = l.get(q), ns = feat::update_from_neighbour_shapes(l, st, q);
      if (ns != st) setBlock(p.x, p.y, p.z, ns);
    }
  }
  RandomSource& random() override {
    if (!rand_) rand_ = t_.dfs.randomState().getOrCreateRandomFactory("minecraft:worldgen_region_random").at(r_.cx * 16, 0, r_.cz * 16);
    return *rand_;
  }

 private:
  worldgen::Region& r_;
  worldgen::TerrainGenerator& t_;
  const worldgen::Features* f_;
  const FeatureMap* fm_;
  std::unique_ptr<worldgen::feat::Level> fl_, bl_;
  worldgen::feat::Level& blockLevel() {
    if (!bl_) bl_ = f_ ? std::make_unique<worldgen::feat::Level>(r_, *f_) : std::make_unique<worldgen::feat::Level>(r_);
    return *bl_;
  }
  std::unique_ptr<RandomSource> rand_;
};

struct ChunkStructures {  // ProtoChunk::structure_data
  std::vector<StartP> starts;
  StructureManager::References references;
};

std::vector<uint8_t> possibleBiomes(const worldgen::TerrainGenerator& t) {
  std::vector<uint8_t> out(biomeNames().size(), 0);
  for (auto& [point, biome] : t.biomeSource->values()) out[biome] = 1;
  return out;
}
}  // namespace

struct StructureWorld::Impl {
  TerrainView view;
  StructureManager manager;
  std::unordered_map<std::string, const worldgen::feat::Placed*> features;  // feature pool elements
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
  if (!features) {  // "structures" stage: structure steps only
    ow.pipeline.set_step(worldgen::Status::FEATURES, [m, terrain](worldgen::Region& r) {
      RegionLevel level(r, *terrain);
      m->placeAll(level, r.cx, r.cz);
    });
    ow.pipeline.set_step(worldgen::Status::FULL, [](worldgen::Region& r) { worldgen::feat::post_process(r); }, 1);
    return;
  }
  // Feature pool elements (village trees, hay, ...): load their placed features up front (loader not thread-safe).
  for (auto& e : std::filesystem::recursive_directory_iterator(dataRoot() + "/minecraft/worldgen/template_pool")) {
    if (e.path().extension() != ".json") continue;
    std::function<void(const Json&)> scan = [&](const Json& j) {
      if (j.isObject()) {
        if (auto* t = j.find("element_type"); t && t->isString() && t->str == "minecraft:feature_pool_element")
          impl_->features.emplace(fullId(j["feature"].asString()), nullptr);
        for (auto& [k, v] : j.obj) scan(v);
      } else if (j.isArray()) {
        for (auto& v : j.arr) scan(v);
      }
    };
    scan(Json::parseFile(e.path().string()));
  }
  for (auto& [id, p] : impl_->features) p = features->load_placed(id);
  const RegionLevel::FeatureMap* fm = &impl_->features;
  // ChunkGenerator.applyBiomeDecoration: each step's structures before that step's features, same random.
  features->structure_placer = [m, terrain, features, fm](worldgen::Region& r, int step, WorldgenRandom& random, int64_t seed) {
    // One level (and region random) per decoration of a chunk, like the WorldGenRegion of the step.
    thread_local const worldgen::Region* at = nullptr;
    thread_local StructureManager::References refs;
    thread_local std::unique_ptr<RegionLevel> level;
    if (step == 0 || at != &r) {
      refs = m->references(r.cx, r.cz);
      level = std::make_unique<RegionLevel>(r, *terrain, features, fm);
      at = &r;
    }
    m->placeStep(*level, r.cx, r.cz, step, random, seed, refs);
    if (step == STEP_COUNT - 1) level.reset(), at = nullptr;
  };
}

StructureWorld::~StructureWorld() = default;
StructureManager& StructureWorld::manager() { return impl_->manager; }

}  // namespace structure
