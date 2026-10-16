#include "worldgen/pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

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

Heightmap heightmap_by_name(const std::string& n) {
  static const char* names[] = {"WORLD_SURFACE_WG", "WORLD_SURFACE", "OCEAN_FLOOR_WG", "OCEAN_FLOOR",
                                "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES"};
  for (int i = 0; i < 6; i++)
    if (n == names[i]) return Heightmap(i);
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

void ProtoChunk::prime_heightmaps() {
  const auto& op = opaque();
  int top = MIN_Y + HEIGHT - 1;
  for (int z = 0; z < 16; z++)
    for (int x = 0; x < 16; x++) {
      uint8_t remaining = 0x3F;
      for (auto& h : heightmaps) h[size_t(z * 16 + x)] = int16_t(MIN_Y);
      for (int y = top; y >= MIN_Y && remaining; y--) {
        uint8_t hit = op[chunk->get(x, y, z)] & remaining;
        if (!hit) continue;
        for (int h = 0; h < 6; h++)
          if ((hit >> h) & 1) heightmaps[size_t(h)][size_t(z * 16 + x)] = int16_t(y + 1);
        remaining &= uint8_t(~hit);
      }
    }
}

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

ProtoPtr Pipeline::get_or_create(int cx, int cz) {
  std::lock_guard lock(m);
  auto& slot = chunks[key(cx, cz)];
  if (!slot) slot = std::make_shared<ProtoChunk>(cx, cz, this);
  return slot;
}

ProtoPtr Pipeline::ensure(int cx, int cz, Status target) {
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
    p->taken = true;
  }
  {
    std::lock_guard lock(m);
    if (++takes % 64) return;
  }
  sweep();
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
