#include "worldgen/pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "storage/chunk_nbt.hpp"
#include "storage/nbt_io.hpp"
#include "storage/region.hpp"
#include "worldgen/block_state.hpp"

namespace worldgen {
namespace {
int64_t key(int cx, int cz) { return int64_t(uint64_t(uint32_t(cx)) << 32 | uint32_t(cz)); }

// Per state: bit h set if the state counts for heightmap h (Heightmap.Types.isOpaque).
const std::vector<uint8_t>& opaque() {
  static const std::vector<uint8_t> t = [] {
    const auto& R = bs::R();
    const auto &motion = R.tag("minecraft:blocks_motion_in_heightmap"),
               &noLeaves = R.tag("minecraft:blocks_motion_in_heightmap_no_leaves");
    std::vector<uint8_t> v(size_t(R.state_count()));
    for (int s = 0; s < R.state_count(); s++) {
      uint8_t f = 0;
      if (!R.is_air(s)) f |= 1 << WORLD_SURFACE_WG | 1 << WORLD_SURFACE;
      if (R.in(s, motion)) f |= 1 << OCEAN_FLOOR_WG | 1 << OCEAN_FLOOR;
      if (R.in(s, motion) || R.has_fluid(s)) f |= 1 << MOTION_BLOCKING;
      if (R.in(s, noLeaves) || R.has_fluid(s)) f |= 1 << MOTION_BLOCKING_NO_LEAVES;
      v[size_t(s)] = f;
    }
    return v;
  }();
  return t;
}
}  // namespace

const char* status_name(Status s) {
  static const char* n[] = {"empty", "structure_starts", "structure_references", "biomes", "terrain",
                            "features", "initialize_light", "light", "spawn", "full"};
  return n[int(s)];
}

static const char* heightmap_names[] = {"WORLD_SURFACE_WG", "WORLD_SURFACE", "OCEAN_FLOOR_WG", "OCEAN_FLOOR",
                                        "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES"};
const char* heightmap_name(Heightmap h) { return heightmap_names[h]; }

Heightmap heightmap_by_name(const std::string& n) {
  for (int i = 0; i < 6; i++)
    if (n == heightmap_names[i]) return Heightmap(i);
  throw std::runtime_error("unknown heightmap " + n);
}

// ---------------------------------------------------------------- ProtoChunk
void ProtoChunk::set(int x, int y, int z, uint16_t state) {
  if (y < MIN_Y || y >= MIN_Y + HEIGHT) return;
  uint16_t old = chunk->get(x, y, z);
  chunk->set(x, y, z, state);
  Status st = get_status();
  if (st < Status::TERRAIN) return;
  // Heightmap.update for the final heightmaps (ProtoChunk.setBlockState, heightmapsAfter = FINAL_HEIGHTMAPS).
  const auto& op = opaque();
  for (int h : {WORLD_SURFACE, OCEAN_FLOOR, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES}) {
    int16_t& first = heightmaps[size_t(h)][size_t(z * 16 + x)];
    if (y <= first - 2) continue;
    if ((op[state] >> h) & 1) {
      if (y >= first) first = int16_t(y + 1);
    } else if (first - 1 == y) {
      int yy = y - 1;
      while (yy >= MIN_Y && !((op[chunk->get(x, yy, z)] >> h) & 1)) yy--;
      first = int16_t(yy + 1);
    }
  }
  if (st >= Status::INITIALIZE_LIGHT && owner->on_block_changed) owner->on_block_changed(*this, x, y, z, old, state);
}

void prime_heightmaps(const Chunk& c, std::array<std::array<int16_t, 256>, 6>& heightmaps) {
  const auto& op = opaque();
  int top = MIN_Y + HEIGHT - 1;
  for (int z = 0; z < 16; z++)
    for (int x = 0; x < 16; x++) {
      uint8_t remaining = 0x3F;
      for (auto& h : heightmaps) h[size_t(z * 16 + x)] = int16_t(MIN_Y);
      for (int y = top; y >= MIN_Y && remaining; y--) {
        uint8_t hit = op[c.get(x, y, z)] & remaining;
        if (!hit) continue;
        for (int h = 0; h < 6; h++)
          if ((hit >> h) & 1) heightmaps[size_t(h)][size_t(z * 16 + x)] = int16_t(y + 1);
        remaining &= uint8_t(~hit);
      }
    }
}

void ProtoChunk::prime_heightmaps() { worldgen::prime_heightmaps(*chunk, heightmaps); }

// ---------------------------------------------------------------- Pipeline
Pipeline::Pipeline() {
  // ChunkPyramid.GENERATION_PYRAMID requirements and ChunkStep.Builder dependency arithmetic.
  struct Req { Status s; int radius; };
  std::array<std::vector<Req>, STATUS_COUNT> reqs;
  reqs[int(Status::STRUCTURE_REFERENCES)] = {{Status::STRUCTURE_STARTS, 8}};
  reqs[int(Status::BIOMES)] = {{Status::STRUCTURE_STARTS, 8}};
  reqs[int(Status::TERRAIN)] = {{Status::STRUCTURE_STARTS, 8}, {Status::BIOMES, 1}};
  reqs[int(Status::FEATURES)] = {{Status::STRUCTURE_STARTS, 8}, {Status::TERRAIN, 1}};
  reqs[int(Status::LIGHT)] = {{Status::INITIALIZE_LIGHT, 1}};
  reqs[int(Status::SPAWN)] = {{Status::BIOMES, 1}};
  steps[int(Status::TERRAIN)].write_radius = 0;
  steps[int(Status::FEATURES)].write_radius = 1;
  for (int i = 1; i < STATUS_COUNT; i++) {
    Step& st = steps[size_t(i)];
    st.direct = {Status(i - 1)};
    for (auto& r : reqs[size_t(i)]) {
      size_t n = size_t(r.radius + 1);
      std::vector<Status> prev = st.direct;
      if (n > prev.size()) st.direct.assign(n, r.s);
      for (size_t k = 0; k < std::min(n, prev.size()); k++) st.direct[k] = std::max(prev[k], r.s);
    }
    const Step& parent = steps[size_t(i - 1)];
    int parentRadius = 0;
    for (int k = int(st.direct.size()) - 1; k >= 0; k--)
      if (st.direct[size_t(k)] >= Status(i - 1)) { parentRadius = k; break; }
    size_t len = std::max(size_t(parentRadius) + parent.accumulated.size(), st.direct.size());
    st.accumulated.resize(len);
    for (size_t d = 0; d < len; d++) {
      int dip = int(d) - parentRadius;
      if (dip < 0 || dip >= int(parent.accumulated.size())) st.accumulated[d] = st.direct[d];
      else if (d >= st.direct.size()) st.accumulated[d] = parent.accumulated[size_t(dip)];
      else st.accumulated[d] = std::max(st.direct[d], parent.accumulated[size_t(dip)]);
    }
  }
  steps[0].accumulated = {Status::EMPTY};
}

void Pipeline::set_step(Status s, StepFn fn, int lock_radius) {
  steps[size_t(s)].fn = std::move(fn);
  steps[size_t(s)].lock_radius = lock_radius;
}

size_t Pipeline::loaded() const {
  std::lock_guard lock(m);
  return chunks.size();
}

ProtoPtr Pipeline::find(int cx, int cz) const {
  std::lock_guard lock(m);
  auto it = chunks.find(key(cx, cz));
  return it == chunks.end() ? nullptr : it->second;
}

// Shared hold on `gen` for the outermost ensure/take of this thread (they nest; a shared_mutex is not recursive).
struct Pipeline::GenLock {
  static thread_local int depth;
  Pipeline& p;
  explicit GenLock(Pipeline& p) : p(p) { if (depth++ == 0) p.gen.lock_shared(); }
  ~GenLock() { if (--depth == 0) p.gen.unlock_shared(); }
};
thread_local int Pipeline::GenLock::depth = 0;

ProtoPtr Pipeline::get_or_create(int cx, int cz) {
  {
    std::lock_guard lock(m);
    auto it = chunks.find(key(cx, cz));
    if (it != chunks.end()) return it->second;
  }
  ProtoPtr loaded = persist ? load(cx, cz) : nullptr;  // outside the lock: disk I/O
  std::lock_guard lock(m);
  auto& slot = chunks[key(cx, cz)];
  if (!slot) slot = loaded ? loaded : std::make_shared<ProtoChunk>(cx, cz, this);
  return slot;
}

ProtoPtr Pipeline::ensure(int cx, int cz, Status target) {
  GenLock hold(*this);
  ProtoPtr p = get_or_create(cx, cz);
  for (Status s = Status(int(p->get_status()) + 1); s <= target; s = Status(int(s) + 1)) {
    if (p->get_status() >= s) continue;
    const auto& d = steps[size_t(s)].direct;
    for (int r = 1; r < int(d.size()); r++)
      for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx += (dz == -r || dz == r) ? 1 : 2 * r) ensure(cx + dx, cz + dz, d[size_t(r)]);
    run_step(*p, s);
  }
  return p;
}

void Pipeline::run_step(ProtoChunk& c, Status s) {
  const Step& st = steps[size_t(s)];
  int lr = st.lock_radius >= 0 ? st.lock_radius : std::max(st.write_radius, 0);
  // Chunks inside the lock radius are visible too (and writable when the step locks them, see set_step).
  Region region{*this, s, c.cx, c.cz, std::max(int(st.direct.size()) - 1, lr), st.lock_radius > 0 ? std::max(st.write_radius, lr) : st.write_radius, {}};
  int w = 2 * region.radius + 1;
  region.grid.resize(size_t(w * w));
  std::vector<ProtoChunk*> locks;
  {
    std::lock_guard lock(m);
    for (int dz = -region.radius; dz <= region.radius; dz++)
      for (int dx = -region.radius; dx <= region.radius; dx++) {
        auto it = chunks.find(key(c.cx + dx, c.cz + dz));
        ProtoChunk* p = it == chunks.end() ? nullptr : it->second.get();
        region.grid[size_t((dz + region.radius) * w + dx + region.radius)] = p;
        if (p && std::max(std::abs(dx), std::abs(dz)) <= lr) locks.push_back(p);
      }
  }
  std::sort(locks.begin(), locks.end(), [](ProtoChunk* a, ProtoChunk* b) { return a->cx != b->cx ? a->cx < b->cx : a->cz < b->cz; });
  std::vector<std::unique_lock<std::mutex>> held;
  held.reserve(locks.size());
  for (ProtoChunk* p : locks) held.emplace_back(p->m);
  if (c.get_status() >= s) return;  // another thread did it while we waited
  if (s == Status::BIOMES && !c.chunk) {
    c.chunk = std::make_unique<Chunk>();
    c.chunk->cx = c.cx, c.chunk->cz = c.cz;
  }
  auto t0 = std::chrono::steady_clock::now();
  if (st.fn) st.fn(region);
  if (s == Status::TERRAIN) c.prime_heightmaps();
  step_ns[size_t(s)] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
  step_count[size_t(s)]++;
  c.status.store(s, std::memory_order_release);
}

void Pipeline::take(int cx, int cz, Chunk& out) {
  GenLock hold(*this);
  ProtoPtr p = ensure(cx, cz, Status::FULL);
  {
    std::lock_guard lock(p->m);
    if (!p->chunk) {  // freed after an earlier take: the World lost the chunk
      static std::once_flag warned;
      std::call_once(warned, [] { fprintf(stderr, "worldgen: regenerating a dropped chunk without features\n"); });
      Region region{*this, Status::TERRAIN, cx, cz, 0, 0, {p.get()}};
      p->chunk = std::make_unique<Chunk>();
      p->chunk->cx = cx, p->chunk->cz = cz;
      Status keep = p->get_status();
      p->status.store(Status::STRUCTURE_REFERENCES);
      for (Status s : {Status::BIOMES, Status::TERRAIN})
        if (steps[size_t(s)].fn) region.step = s, steps[size_t(s)].fn(region);
      p->status.store(keep);
    }
    out.sections = p->chunk->sections;
    out.block_entities = p->chunk->block_entities;
    out.level.post = p->chunk->level.post;  // ProtoChunk.postProcessing -> LevelChunk (src/level/ runs it)
    if (on_take) on_take(*p, out);
    p->taken = true;
  }
  {
    std::lock_guard lock(m);
    if (++takes % 64) return;
  }
  sweep();
}

// ---------------------------------------------------------------- persistence (proto chunks)
namespace {
Status status_from(const std::string& name) {
  for (int i = 0; i < STATUS_COUNT; i++)
    if (name == std::string("minecraft:") + status_name(Status(i))) return Status(i);
  return Status::EMPTY;
}
}  // namespace

void Pipeline::save(ProtoChunk& c) {  // gen held exclusively: no step runs
  Status st = c.get_status();
  if (st >= Status::INITIALIZE_LIGHT) st = Status::FEATURES;  // light is not saved: redo INITIALIZE_LIGHT, LIGHT
  unsigned mask = st >= Status::TERRAIN ? 0x3F : 0;
  Nbt root = storage::encode(*c.chunk, std::string("minecraft:") + status_name(st), {}, &c.heightmaps, mask);
  // ProtoChunk.getPostProcessing: per section, packed x | z << 4 | y << 8 (section-local).
  Nbt& pp = *root.get("PostProcessing");  // encode wrote chunk->level.post (terrain's fluid marks) already
  for (int32_t v : c.post_processing)
    pp.list[size_t((v >> 8) >> 4)].list.push_back(Nbt::num(Nbt::Short, (v & 255) | ((v >> 8) & 15) << 8));
  Nbt& ticks = *root.get("block_ticks");  // leaf ticks scheduled by worldgen (SavedTick: i, x, y, z, t, p)
  for (int32_t v : c.leaf_ticks) {
    int x = c.cx * 16 + (v & 15), z = c.cz * 16 + (v >> 4 & 15), y = (v >> 8) + MIN_Y;
    Nbt t = Nbt::compound();
    t.set("i", Nbt::str("minecraft:" + bs::R().block(bs::R().block_of(c.chunk->get(v & 15, y, v >> 4 & 15))).name));
    t.set("x", Nbt::num(Nbt::Int, x)), t.set("y", Nbt::num(Nbt::Int, y)), t.set("z", Nbt::num(Nbt::Int, z));
    t.set("t", Nbt::num(Nbt::Int, 0)), t.set("p", Nbt::num(Nbt::Int, 0));
    ticks.list.push_back(t);
  }
  storage::write(storage::region_dir(), c.cx, c.cz, nbtio::write(root));
}

ProtoPtr Pipeline::load(int cx, int cz) {
  auto raw = storage::read(storage::region_dir(), cx, cz);
  if (raw.empty()) return nullptr;
  try {
    Nbt root = nbtio::parse(raw);
    Status st = status_from(storage::status_of(root));
    if (st == Status::EMPTY) return nullptr;
    if (st >= Status::INITIALIZE_LIGHT && st < Status::FULL) st = Status::FEATURES;  // relight (as saved)
    auto p = std::make_shared<ProtoChunk>(cx, cz, this);
    if (st == Status::FULL) {  // the World's chunk: a header only (nothing reads a FULL chunk's blocks)
      p->taken = true;
    } else if (st >= Status::BIOMES) {
      p->chunk = std::make_unique<Chunk>();
      p->chunk->cx = cx, p->chunk->cz = cz;
      storage::decode(root, *p->chunk, nullptr);
      if (st >= Status::TERRAIN) {
        storage::Heightmaps hm;
        unsigned got = storage::decode_heightmaps(root, hm);
        if (got != 0x3F) p->prime_heightmaps();
        else p->heightmaps = hm;
      }
      // One PostProcessing list on disk; the FULL step sends fluid marks on to Chunk::level.post again.
      for (uint32_t v : p->chunk->level.post) p->post_processing.push_back(int32_t(v));
      p->chunk->level.post.clear(), p->chunk->level.post_processed = true;
      if (const Nbt* t = root.get("block_ticks"))
        for (auto& e : t->list) {
          auto I = [&](const char* k) { const Nbt* n = e.get(k); return n ? int(n->i) : 0; };
          int x = I("x"), y = I("y"), z = I("z");
          if ((x >> 4) == cx && (z >> 4) == cz && y >= MIN_Y && y < MIN_Y + HEIGHT)
            p->leaf_ticks.push_back((y - MIN_Y) << 8 | (z & 15) << 4 | (x & 15));
        }
    }
    // Structure starts / references are deterministic: recompute them like the steps did.
    for (Status s : {Status::STRUCTURE_STARTS, Status::STRUCTURE_REFERENCES})
      if (st >= s && steps[size_t(s)].fn) {
        Region r{*this, s, cx, cz, 0, -1, {p.get()}};
        steps[size_t(s)].fn(r);
      }
    p->status.store(st, std::memory_order_release);
    return p;
  } catch (const std::exception& e) {
    fprintf(stderr, "worldgen: couldn't load proto chunk [%d, %d]: %s\n", cx, cz, e.what());
    return nullptr;
  }
}

size_t Pipeline::evict(const std::function<bool(int, int)>& keep, bool all) {
  std::unique_lock exclusive(gen);  // waits for running steps; ensure/take wait meanwhile
  std::vector<ProtoPtr> drop;
  {
    std::lock_guard lock(m);
    for (auto it = chunks.begin(); it != chunks.end();) {
      ProtoChunk& c = *it->second;
      bool kept = keep(c.cx, c.cz);
      if (kept && !(all && persist)) { ++it; continue; }
      if (kept) {  // save-all: store the work in progress, keep it in memory
        if (!c.taken && c.chunk && c.get_status() >= Status::TERRAIN) drop.push_back(it->second);
        ++it;
        continue;
      }
      drop.push_back(it->second);
      it = chunks.erase(it);
    }
  }
  size_t dropped = 0;
  for (auto& p : drop) {
    dropped += p.use_count() == 1;
    if (persist && !p->taken && p->chunk && p->get_status() >= Status::TERRAIN) save(*p);
  }
  if (on_evict) on_evict(keep);
  return dropped;
}

void Pipeline::sweep() {
  std::vector<ProtoPtr> all;
  {
    std::lock_guard lock(m);
    for (auto& [k, p] : chunks)
      if (p->taken && p->chunk) all.push_back(p);
  }
  for (auto& p : all) {
    bool idle = true;
    for (int dz = -1; dz <= 1 && idle; dz++)
      for (int dx = -1; dx <= 1 && idle; dx++) {
        ProtoPtr n = find(p->cx + dx, p->cz + dz);
        idle = n && n->get_status() > Status::LIGHT;
      }
    if (!idle) continue;
    std::lock_guard lock(p->m);
    p->chunk.reset();
  }
}

}  // namespace worldgen
