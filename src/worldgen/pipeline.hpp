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
//   full                   -                                          -              post-processing, leaf ticks; to World
//
// THREADING. Pipeline::ensure / take may be called from any number of threads at once (the World worker pool
// calls World::generator -> Pipeline::take). A step is a plain function over a Region (the chunk + neighbours,
// = vanilla WorldGenRegion); the pipeline runs it on the calling thread after its dependencies, holding the
// mutexes of every chunk within lock_radius of the center (sorted order: no deadlocks). Steps never call ensure.
//
// HOOKS for other agents (set before the first ensure, never while generating):
//   pipeline.set_step(Status::STRUCTURE_STARTS, fn)      fn(Region&) fills r.center().structure_data
//   pipeline.set_step(Status::STRUCTURE_REFERENCES, fn)  reads neighbours' structure_data (radius 8)
//   overworld.features->structure_placer = fn            called by the features step once per decoration step,
//                                                        before that step's features (ChunkGenerator.
//                                                        applyBiomeDecoration: structures first, same random);
//                                                        signature in features.hpp (StructurePlacer)
//   feat::extra_feature_parsers()["template"] = parser   feature types implemented elsewhere (template, fossil
//                                                        need StructureTemplate); fill before building Overworld
//   pipeline.set_step(Status::INITIALIZE_LIGHT, fn) / set_step(Status::LIGHT, fn, /*lock_radius*/ 1)
//                                                        light data goes in ProtoChunk::light_data; blocks of
//                                                        chunks at >= INITIALIZE_LIGHT can still change (features
//                                                        of a neighbour write radius 1): set on_block_changed to
//                                                        hear about them (ProtoChunk::set calls it from that status on)
// A step that is not set is a pass-through. The default steps (biomes, terrain, features, full) are set by
// worldgen::Overworld (overworld.hpp). FULL runs with lock radius 1: post-processing and the leaf ticks
// worldgen scheduled may write into neighbours (never into chunks already taken by the World).
//
// DECORATION ORDER. Features of neighbouring chunks overlap (write radius 1), so the order chunks are decorated
// in decides blocks. Vanilla runs every ChunkGenerationTask on one consecutive "worldgen" executor, lowest
// ticket level (nearest the player, chessboard distance) first, and a FULL task at C decorates the 3x3 around C
// in x-major order (ChunkGenerationTask.scheduleLayer); only the async noise/biome work runs in parallel. Here a
// Ticket is such a task: ticket(cx, cz) queues one (call in vanilla ticket order from one thread, as
// update_view does), and FEATURES only ever runs inside a ticket's layer, tickets strictly in queue order, so
// the result never depends on worker timing. A chunk whose features are needed but no pending ticket covers
// gets a radius-0 ticket at the end of the queue. Threads waiting for an earlier ticket generate terrain for
// pending tickets meanwhile (the parallel part). Measured against vanilla's own run-to-run spread (JFR
// minecraft.ChunkGeneration order, test/compare.py --jfr): the ring order agrees with vanilla's pairwise
// decoration order more often than one vanilla run agrees with another.
//
// MEMORY. Block storage (a Chunk) is allocated when a chunk reaches BIOMES; below that a ProtoChunk is ~100
// bytes + its structure_data. take() copies the FULL chunk out to the World. A taken FULL chunk frees its block
// storage once all 8 neighbours are past LIGHT (nothing reads its blocks any more); the header and
// structure_data stay, so later steps of chunks up to 8 away still see its structure starts.
// EVICTION (vanilla: proto chunks without a ticket are saved and unloaded). evict(keep), from the World's
// autosave, drops every chunk `keep` rejects, under an exclusive lock that waits for running generation:
//   status < TERRAIN      dropped: recomputed from the seed when needed again (no neighbour writes yet)
//   TERRAIN .. LIGHT      saved to the region file as a vanilla proto chunk (Status + sections + heightmaps +
//                         PostProcessing + block_ticks; INITIALIZE_LIGHT and later are saved as FEATURES: their
//                         light is recomputed), then dropped; get_or_create loads it back from disk
//   FULL (taken)          dropped: the World saved it; loaded back as a header if a neighbour needs it
// on_evict (lighting) then drops the light columns of the same chunks. Steps never see a chunk vanish: ensure()
// and take() hold the lock shared. A freed chunk requested again (World lost it) gets terrain only.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "world.hpp"
#include "worldgen/providers.hpp"

namespace worldgen {

enum class Status : uint8_t {
  EMPTY, STRUCTURE_STARTS, STRUCTURE_REFERENCES, BIOMES, TERRAIN, FEATURES, INITIALIZE_LIGHT, LIGHT, SPAWN, FULL, COUNT
};
inline constexpr int STATUS_COUNT = int(Status::COUNT);
const char* status_name(Status s);

// Heightmap.Types, vanilla ordinal order.
enum Heightmap { WORLD_SURFACE_WG, WORLD_SURFACE, OCEAN_FLOOR_WG, OCEAN_FLOOR, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES };
Heightmap heightmap_by_name(const std::string& name);
const char* heightmap_name(Heightmap h);  // serialization key, e.g. "MOTION_BLOCKING"
// Heightmap.primeHeightmaps for all six types (first free y per column, index z * 16 + x).
void prime_heightmaps(const Chunk& c, std::array<std::array<int16_t, 256>, 6>& out);

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
  std::vector<int32_t> leaf_ticks;        // scheduled LeavesBlock ticks (same packing), run at FULL
  std::shared_ptr<void> structure_data, light_data;  // owned by the structures / lighting steps

  ProtoChunk(int cx, int cz, Pipeline* owner) : cx(cx), cz(cz), owner(owner) {}
  Status get_status() const { return status.load(std::memory_order_acquire); }
  // Local x, z (0..15), world y. Outside the build height: void_air / ignored, like ProtoChunk.
  uint16_t get(int x, int y, int z) const;
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
    if (y < chunk(x >> 4, z >> 4)->chunk->min_y || y >= chunk(x >> 4, z >> 4)->chunk->min_y + chunk(x >> 4, z >> 4)->chunk->height()) return false;
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
  // Dimension layout (chunk min_y / height) and WorldGenerationContext; set before the first ensure.
  int min_y = MIN_Y, height = HEIGHT;
  GenContext context;
  // lock_radius -1: max(write radius, 0). A larger lock radius also makes those chunks readable and writable
  // in the Region (used by the FULL step for post-processing and scheduled leaf ticks, which cross borders).
  void set_step(Status s, StepFn fn, int lock_radius = -1);
  // Protects palettes exposed to the light engine while generation continues writing nearby chunks.
  std::mutex* write_lock = nullptr;
  // Called by ProtoChunk::set at >= INITIALIZE_LIGHT with write_lock held. May be empty.
  std::function<void(ProtoChunk&, int x, int y, int z, uint16_t old_state, uint16_t new_state)> on_block_changed;
  // Called by take() under the chunk's mutex after copying into `out`, before the proto storage may be freed
  // (lighting hook: stop reading the proto blocks). May be empty.
  std::function<void(ProtoChunk&, Chunk& out)> on_take;

  // Proto chunks are saved to / loaded from the region files (storage/) on eviction. Off: never read from disk.
  int dimension = 0;  // storage dimension index
  bool persist = false;
  // Drops what keep(cx, cz) rejects (see EVICTION). `all`: also save the kept proto chunks (shutdown, save-all).
  // Returns the number of chunks dropped.
  size_t evict(const std::function<bool(int, int)>& keep, bool all);
  std::function<void(const std::function<bool(int, int)>& keep)> on_evict;  // lighting: drop its columns too

  // Brings (cx, cz) to at least `target` (and its dependencies), on the calling thread.
  ProtoPtr ensure(int cx, int cz, Status target);
  // ensure(FULL), then copies blocks, biomes and block entities into `out` (World::generator).
  void take(int cx, int cz, Chunk& out);
  // Decoration order (see DECORATION ORDER): queue a FULL task's 3x3 layer at (cx, cz) / drop it if not started.
  void ticket(int cx, int cz);
  void cancel(int cx, int cz);

  // Dependency tables (ChunkStep): deps(s)[r] = status needed at chessboard distance r before step s runs.
  const std::vector<Status>& deps(Status s) const { return steps[size_t(s)].direct; }
  const std::vector<Status>& accumulated(Status s) const { return steps[size_t(s)].accumulated; }
  int write_radius(Status s) const { return steps[size_t(s)].write_radius; }
  size_t loaded() const;  // proto chunks held
  // Per-status time spent in step functions (ns) and step count, for profiling.
  std::array<std::atomic<int64_t>, STATUS_COUNT> step_ns{}, step_count{};

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

  struct Ticket {
    int cx, cz, radius;
    bool prefetching = false, running = false;
  };
  std::mutex om;  // order: tickets below (never held while a step runs)
  std::condition_variable ocv;
  std::map<uint64_t, Ticket> pending;            // by queue position
  std::unordered_map<int64_t, uint64_t> pending_at;  // center -> queue position
  uint64_t next_ticket = 0;
  uint64_t add_ticket(int cx, int cz, int radius);  // om held; the pending one there if any
  uint64_t covering(int cx, int cz);                 // om held; first pending ticket that decorates (cx, cz), or ~0
  bool terrain_ready(const Ticket& t);
  void run_until(uint64_t seq);  // runs the layers of every pending ticket up to seq, in order

  std::shared_mutex gen;  // shared: generating (ensure / take); exclusive: evict
  struct GenLock;
  ProtoPtr get_or_create(int cx, int cz);
  ProtoPtr load(int cx, int cz);  // from the region file, null if not stored
  void save(ProtoChunk& c);
  ProtoPtr find(int cx, int cz) const;
  void run_step(ProtoChunk& c, Status s);
  void sweep();
};

}  // namespace worldgen
