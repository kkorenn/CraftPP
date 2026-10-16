// Proto-chunk generation pipeline: vanilla ChunkStatus / ChunkStep / ChunkPyramid.GENERATION_PYRAMID (26.3).
//
// A chunk advances one status at a time. Before the step to status S runs on chunk C, every chunk at chessboard
// distance r from C is brought to deps(S)[r] (ChunkStep.directDependencies, computed exactly like vanilla's
// ChunkStep.Builder from the requirements in ChunkPyramid):
//
//   status                 requirements (status, radius)              write radius   task here
//   structure_starts       -                                          -              hook (structures agent)
//   structure_references   structure_starts 8                         -              hook (structures agent)
//   biomes                 structure_starts 8                         -              TerrainGenerator::createBiomes
//   terrain                structure_starts 8, biomes 1               0              noise + surface + carvers
//   features               structure_starts 8, terrain 1              1              biome decoration (+ structures)
//   initialize_light       -                                          -              hook (lighting agent)
//   light                  initialize_light 1                         -              hook (lighting agent)
//   spawn                  biomes 1                                   -              (none yet)
//   full                   -                                          -              chunk handed to World
//
// THREADING. Pipeline::ensure / take may be called from any number of threads at once (the World worker pool
// calls World::generator -> Pipeline::take). A step is a plain function over a Region (the chunk + neighbours,
// = vanilla WorldGenRegion); the pipeline runs it on the calling thread after its dependencies, holding the
// mutexes of every chunk within lock_radius of the center (sorted order: no deadlocks). Steps never call ensure.
//
// HOOKS for other agents (set before the first ensure, never while generating):
//   pipeline.set_step(Status::STRUCTURE_STARTS, fn)      fn(Region&) fills r.center().structure_data
//   pipeline.set_step(Status::STRUCTURE_REFERENCES, fn)  reads neighbours' structure_data (radius 8)
//   pipeline.structure_placer = fn                       called by the features step once per decoration step,
//                                                        before that step's features (ChunkGenerator.
//                                                        applyBiomeDecoration: structures first, same random);
//                                                        signature in features.hpp (StructurePlacer)
//   pipeline.set_step(Status::INITIALIZE_LIGHT, fn) / set_step(Status::LIGHT, fn, /*lock_radius*/ 1)
//                                                        light data goes in ProtoChunk::light_data; blocks of
//                                                        chunks at >= INITIALIZE_LIGHT can still change (features
//                                                        of a neighbour write radius 1): set on_block_changed to
//                                                        hear about them (ProtoChunk::set calls it from that status on)
// A step that is not set is a pass-through. The default steps (biomes, terrain, features) are set by
// worldgen::Overworld (overworld.hpp).
//
// MEMORY. Block storage (a Chunk) is allocated when a chunk reaches BIOMES; below that a ProtoChunk is ~100
// bytes + its structure_data. take() copies the FULL chunk out to the World. A taken FULL chunk frees its block
// storage once all 8 neighbours are past LIGHT (nothing reads its blocks any more); the header and
// structure_data stay, so later steps of chunks up to 8 away still see its structure starts.
// ponytail: chunks below FULL keep their blocks (they may hold feature writes from neighbours) and headers are
// never dropped; vanilla saves proto chunks to disk instead. Add proto persistence if long exploration
// sessions grow memory on the Pi. A freed chunk requested again (World lost it) gets terrain only.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "world.hpp"

namespace worldgen {

enum class Status : uint8_t {
  EMPTY, STRUCTURE_STARTS, STRUCTURE_REFERENCES, BIOMES, TERRAIN, FEATURES, INITIALIZE_LIGHT, LIGHT, SPAWN, FULL, COUNT
};
inline constexpr int STATUS_COUNT = int(Status::COUNT);
const char* status_name(Status s);

// Heightmap.Types, vanilla ordinal order.
enum Heightmap { WORLD_SURFACE_WG, WORLD_SURFACE, OCEAN_FLOOR_WG, OCEAN_FLOOR, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES };
Heightmap heightmap_by_name(const std::string& name);

class Pipeline;

struct ProtoChunk {
  const int cx, cz;
  Pipeline* const owner;
  std::atomic<Status> status{Status::EMPTY};
  std::mutex m;                  // held by the step running on this chunk or on a neighbour that locks it
  std::unique_ptr<Chunk> chunk;  // allocated at BIOMES
  // First free y per column (highest matching block + 1, MIN_Y if none), index z * 16 + x. The *_WG maps are
  // computed at the end of TERRAIN and then frozen (vanilla only updates them before TERRAIN is persisted);
  // the four final maps are kept up to date by set().
  std::array<std::array<int16_t, 256>, 6> heightmaps{};
  bool taken = false;  // handed to the World
  std::atomic<int16_t> carver_biome{-1};  // ChunkAccess.carverBiome cache (biome id), -1 = not computed
  std::vector<int32_t> post_processing;   // markPosForPostProcessing: (y - MIN_Y) << 8 | z << 4 | x
  std::shared_ptr<void> structure_data, light_data;  // owned by the structures / lighting steps

  ProtoChunk(int cx, int cz, Pipeline* owner) : cx(cx), cz(cz), owner(owner) {}
  Status get_status() const { return status.load(std::memory_order_acquire); }
  // Local x, z (0..15), world y. Outside the build height: void_air / ignored, like ProtoChunk.
  uint16_t get(int x, int y, int z) const {
    return y < MIN_Y || y >= MIN_Y + HEIGHT ? uint16_t(blocks::void_air) : chunk->get(x, y, z);
  }
  // ProtoChunk.setBlockState: stores the state and updates the final heightmaps (from TERRAIN on).
  void set(int x, int y, int z, uint16_t state);
  int height(Heightmap h, int x, int z) const { return heightmaps[h][size_t(z * 16 + x)]; }
  void prime_heightmaps();  // all six from the blocks (Heightmap.primeHeightmaps)
};
using ProtoPtr = std::shared_ptr<ProtoChunk>;

// WorldGenRegion: the chunks a step may see. Block coordinates are world coordinates.
struct Region {
  Pipeline& pipeline;
  Status step;           // status being generated
  int cx, cz;            // center chunk
  int radius;            // chunks with chessboard distance <= radius are available (directDependencies)
  int write_radius;      // ChunkStep.blockStateWriteRadius (-1: none)
  std::vector<ProtoChunk*> grid;  // (2*radius+1)^2, row-major by z

  ProtoChunk& center() const { return *grid[grid.size() / 2]; }
  ProtoChunk* chunk(int ccx, int ccz) const {  // nullptr outside the region
    int dx = ccx - cx + radius, dz = ccz - cz + radius, w = 2 * radius + 1;
    return dx < 0 || dz < 0 || dx >= w || dz >= w ? nullptr : grid[size_t(dz * w + dx)];
  }
  uint16_t get(int x, int y, int z) const { return chunk(x >> 4, z >> 4)->get(x & 15, y, z & 15); }
  // WorldGenRegion.setBlock: false (and no write) outside the write radius or the build height.
  bool set(int x, int y, int z, uint16_t state) const {
    if (std::abs((x >> 4) - cx) > write_radius || std::abs((z >> 4) - cz) > write_radius) return false;
    if (y < MIN_Y || y >= MIN_Y + HEIGHT) return false;
    chunk(x >> 4, z >> 4)->set(x & 15, y, z & 15, state);
    return true;
  }
  bool writable(int x, int z) const {
    return std::abs((x >> 4) - cx) <= write_radius && std::abs((z >> 4) - cz) <= write_radius;
  }
  // WorldGenRegion.getHeight: heightmap first free y (= LevelReader height).
  int height(Heightmap h, int x, int z) const { return chunk(x >> 4, z >> 4)->height(h, x & 15, z & 15); }
};

using StepFn = std::function<void(Region&)>;

class Pipeline {
 public:
  Pipeline();
  void set_step(Status s, StepFn fn, int lock_radius = -1);  // lock_radius -1: max(write radius, 0)
  // Called by ProtoChunk::set for chunks at >= INITIALIZE_LIGHT (lighting hook). May be empty.
  std::function<void(ProtoChunk&, int x, int y, int z, uint16_t old_state, uint16_t new_state)> on_block_changed;

  // Brings (cx, cz) to at least `target` (and its dependencies), on the calling thread.
  ProtoPtr ensure(int cx, int cz, Status target);
  // ensure(FULL), then copies blocks, biomes and block entities into `out` (World::generator).
  void take(int cx, int cz, Chunk& out);

  // Dependency tables (ChunkStep): deps(s)[r] = status needed at chessboard distance r before step s runs.
  const std::vector<Status>& deps(Status s) const { return steps[size_t(s)].direct; }
  const std::vector<Status>& accumulated(Status s) const { return steps[size_t(s)].accumulated; }
  int write_radius(Status s) const { return steps[size_t(s)].write_radius; }
  size_t loaded() const;  // proto chunks held

 private:
  struct Step {
    std::vector<Status> direct, accumulated;
    int write_radius = -1, lock_radius = -1;
    StepFn fn;
  };
  std::array<Step, STATUS_COUNT> steps;
  mutable std::mutex m;
  std::unordered_map<int64_t, ProtoPtr> chunks;
  int takes = 0;

  ProtoPtr get_or_create(int cx, int cz);
  ProtoPtr find(int cx, int cz) const;
  void run_step(ProtoChunk& c, Status s);
  void sweep();
};

}  // namespace worldgen
