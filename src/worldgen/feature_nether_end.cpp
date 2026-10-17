// Nether and End feature types: delta_feature, stepped_column_cluster, netherrack_replace_blobs, huge_fungus,
// single_block_pillar, projected_random_patchy_square, random_neighbor_spread, end_spike, end_island,
// chorus_plant, end_platform, end_gateway. Ports of levelgen/feature/*Feature.java (26.3).
#include <algorithm>
#include <cmath>
#include <optional>

#include "gen/ids.hpp"
#include "worldgen/features.hpp"

namespace worldgen::feat {
namespace {
const bs::Registry& R() { return bs::R(); }
bool is(int s, int def) { return R().is(s, def); }

// BlockPos.manhattanOrdered (withinBoxByManhattanDistance: maxDepth = rx + ry + rz). fn returns false to stop.
template <class Fn>
void manhattan_ordered(const P& o, int rx, int ry, int rz, int maxDepth, Fn fn) {
  for (int depth = 0; depth <= maxDepth; depth++)
    for (int x = -std::min(rx, depth); x <= std::min(rx, depth); x++) {
      int my = std::min(ry, depth - std::abs(x));
      for (int y = -my; y <= my; y++) {
        int z = depth - std::abs(x) - std::abs(y);
        if (z > rz) continue;
        if (!fn(P{o.x + x, o.y + y, o.z + z})) return;
        if (z != 0 && !fn(P{o.x + x, o.y + y, o.z - z})) return;
      }
    }
}

struct Delta : Feature {
  int contents, rim;
  IntProvider size, rimSize;
  bool clear(Level& l, const P& p) const {
    int s = l.get(p);
    if (is(s, contents)) return false;
    static const int cannot[] = {blocks::bedrock, blocks::nether_bricks, blocks::nether_brick_fence, blocks::nether_brick_stairs,
                                 blocks::nether_wart, blocks::chest, blocks::spawner};
    for (int c : cannot)
      if (is(s, c)) return false;
    for (bs::Dir d : ALL_DIRS) {
      bool air = R().is_air(l.get(p.rel(d)));
      if ((air && d != bs::UP) || (!air && d == bs::UP)) return false;
    }
    return true;
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    bool any = false, spawnRim = r.nextDouble() < 0.9;
    int rimX = spawnRim ? rimSize.sample(r) : 0;
    int rimZ = spawnRim ? rimSize.sample(r) : 0;
    bool hasRim = spawnRim && rimX != 0 && rimZ != 0;
    int rx = size.sample(r);
    int rz = size.sample(r);
    int limit = std::max(rx, rz);
    manhattan_ordered(o, rx, 0, rz, rx + rz, [&](const P& p) {
      if (dist_manhattan(p, o) > limit) return false;
      if (clear(l, p)) {
        if (hasRim) any = true, l.set(p, rim);
        P q = p.offset(rimX, 0, rimZ);
        if (clear(l, q)) any = true, l.set(q, contents);
      }
      return true;
    });
    return any;
  }
};

struct SteppedColumnCluster : Feature {
  const StateProvider* block;
  const BlockPredicate *continueThrough, *canReplace;
  bs::Tag cannotPlaceOn;
  IntProvider clusterReach, columnCount, columnReach, height;
  bool can_place_at(Level& l, const P& c) const {
    if (!canReplace->test(l, c)) return false;
    int below = l.get(c.below());
    return !R().is_air(below) && !R().in(below, cannotPlaceOn);
  }
  std::optional<P> find_surface(Level& l, P c, int limit) const {
    while (c.y > l.min_y() + 1 && limit > 0) {
      limit--;
      if (can_place_at(l, c)) return c;
      c.y--;
    }
    return std::nullopt;
  }
  std::optional<P> find_air(Level& l, P c, int limit) const {
    while (c.y <= l.max_y() && limit > 0) {
      limit--;
      int s = l.get(c);
      if (R().in(s, cannotPlaceOn)) return std::nullopt;
      if (R().is_air(s)) return c;
      c.y++;
    }
    return std::nullopt;
  }
  bool place_column(Level& l, RandomSource& r, const P& o, int columnHeight, int reach) const {
    bool any = false;
    for (int z = o.z - reach; z <= o.z + reach; z++)
      for (int x = o.x - reach; x <= o.x + reach; x++) {
        P p{x, o.y, z};
        int stepLimit = dist_manhattan(p, o);
        std::optional<P> col = canReplace->test(l, p) ? find_surface(l, p, stepLimit) : find_air(l, p, stepLimit);
        if (!col) continue;
        P c = *col;
        for (int n = columnHeight - stepLimit / 2; n >= 0; n--) {
          if (canReplace->test(l, c)) {
            l.set(c, block->get(l, r, c));
            c.y++;
            any = true;
          } else {
            if (!continueThrough->test(l, c)) break;
            c.y++;
          }
        }
      }
    return any;
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    if (!can_place_at(l, o)) return false;
    int h = height.sample(r);
    int reach = std::min(h, clusterReach.sample(r));
    int count = columnCount.sample(r);
    bool placed = false;
    for (int i = 0; i < count; i++) {  // BlockPos.randomBetweenClosed, generated lazily between columns
      int x = o.x - reach + r.nextInt(2 * reach + 1);
      int y = o.y + r.nextInt(1);
      int z = o.z - reach + r.nextInt(2 * reach + 1);
      P p{x, y, z};
      int n = h - dist_manhattan(p, o);
      if (n >= 0) placed |= place_column(l, r, p, n, columnReach.sample(r));
    }
    return placed;
  }
};

struct ReplaceBlobs : Feature {
  int target, state;
  IntProvider radius;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    P c{o.x, std::clamp(o.y, l.min_y() + 1, l.max_y()), o.z};
    while (c.y > l.min_y() + 1 && !is(l.get(c), target)) c.y--;
    if (c.y <= l.min_y() + 1) return false;
    int rx = radius.sample(r);
    int ry = radius.sample(r);
    int rz = radius.sample(r);
    int maxR = std::max(rx, std::max(ry, rz));
    bool any = false;
    manhattan_ordered(c, rx, ry, rz, rx + ry + rz, [&](const P& p) {
      if (dist_manhattan(p, c) > maxR) return false;
      if (is(l.get(p), target)) l.set(p, state), any = true;
      return true;
    });
    return any;
  }
};

struct SingleBlockPillar : Feature {
  const StateProvider* block;
  const BlockPredicate* canReplace = nullptr;  // null = always true
  bs::Dir dir;
  float chance = 1.0f;
  const Placed* cap = nullptr;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    P p = o;
    while ((!canReplace || canReplace->test(l, p)) && r.nextFloat() < chance && !l.outside(p.y)) {
      l.set(p, block->get(l, r, p));
      p = p.rel(dir);
    }
    p = p.rel(bs::opposite(dir));
    if (cap) cap->place(l, r, p);
    return true;
  }
};

struct ProjectedPatchySquare : Feature {
  const StateProvider* block;
  const BlockPredicate* projectThrough;
  IntProvider size;
  int maxProjection;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    int s = size.sample(r), bound = s * s + 1;
    for (int dx = -s; dx <= s; dx++)
      for (int dz = -s; dz <= s; dz++) {
        if (r.nextInt(bound) >= bound - std::abs(dx) * std::abs(dz)) continue;
        P b = o.offset(dx, 0, dz);
        int drop = maxProjection;
        while (projectThrough->test(l, b.below())) {
          b.y--;
          if (--drop <= 0) break;
        }
        int st = block->optional(l, r, b);
        if (st >= 0) l.set(b, st);
      }
    return true;
  }
};

struct RandomNeighborSpread : Feature {
  const StateProvider* block;
  bs::Tag accepted;
  const BlockPredicate* canReplace;
  IntProvider attempts, xzOffset, yOffset;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    l.set(o, block->get(l, r, o));
    int n = attempts.sample(r);
    for (int i = 0; i < n; i++) {
      int dx = xzOffset.sample(r);
      int dy = yOffset.sample(r);
      int dz = xzOffset.sample(r);
      P p = o.offset(dx, dy, dz);
      if (!canReplace->test(l, p)) continue;
      int neighbours = 0;
      for (bs::Dir d : ALL_DIRS) {
        if (R().in(l.get(p.rel(d)), accepted)) neighbours++;
        if (neighbours > 1) break;
      }
      if (neighbours == 1) l.set(p, block->get(l, r, p));
    }
    return true;
  }
};

struct HugeFungus : Feature {
  int validBase, stem, hat, decor;
  const BlockPredicate* replaceable;
  bool planted = false;
  bool replaceable_at(Level& l, const P& p, bool checkPlants) const {
    return R().can_be_replaced(l.get(p)) || (checkPlants && replaceable->test(l, p));
  }
  static void vines(Level& l, RandomSource& r, const P& hatPos) {  // tryPlaceWeepingVines
    P p = hatPos.below();
    if (!l.empty(p)) return;
    int h = mth::nextInt(r, 1, 5);
    if (r.nextInt(7) == 0) h *= 2;
    for (int i = 0; i <= h; i++) {  // placeWeepingVinesColumn(23, 25)
      if (l.empty(p)) {
        if (i == h || !l.empty(p.below())) {
          l.set(p, R().with_int(blocks::weeping_vines, "age", mth::nextInt(r, 23, 25)));
          break;
        }
        l.set(p, blocks::weeping_vines_plant);
      }
      p.y--;
    }
  }
  void hat_block(Level& l, RandomSource& r, const P& p, float decorP, float hatP, float vinesP) const {
    if (r.nextFloat() < decorP) {
      l.set(p, decor);
    } else if (r.nextFloat() < hatP) {
      l.set(p, hat);
      if (r.nextFloat() < vinesP) vines(l, r, p);
    }
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    if (!is(l.get(o.below()), validBase)) return false;
    int total = mth::nextInt(r, 4, 13);
    if (r.nextInt(12) == 0) total *= 2;
    if (!planted && o.y + total + 1 >= GEN_DEPTH) return false;
    bool huge = !planted && r.nextFloat() < 0.06f;
    l.set(o, blocks::air);
    // ponytail: planted (bone meal) destroyBlock drops are not modelled; worldgen never sets planted.
    int sr = huge ? 1 : 0;  // placeStem
    for (int dx = -sr; dx <= sr; dx++)
      for (int dz = -sr; dz <= sr; dz++) {
        bool corner = huge && std::abs(dx) == sr && std::abs(dz) == sr;
        for (int dy = 0; dy < total; dy++) {
          P p = o.offset(dx, dy, dz);
          if (!replaceable_at(l, p, true)) continue;
          if (planted) {
            l.set(p, stem);
          } else if (corner) {
            if (r.nextFloat() < 0.1f) l.set(p, stem);
          } else {
            l.set(p, stem);
          }
        }
      }
    bool placeVines = is(hat, blocks::nether_wart_block);  // placeHat
    int hatHeight = std::min(r.nextInt(1 + total / 3) + 5, total), hatStart = total - hatHeight;
    for (int dy = hatStart; dy <= total; dy++) {
      int radius = dy < total - r.nextInt(3) ? 2 : 1;
      if (hatHeight > 8 && dy < hatStart + 4) radius = 3;
      if (huge) radius++;
      for (int dx = -radius; dx <= radius; dx++)
        for (int dz = -radius; dz <= radius; dz++) {
          bool edgeX = dx == -radius || dx == radius, edgeZ = dz == -radius || dz == radius;
          bool inside = !edgeX && !edgeZ && dy != total, corner = edgeX && edgeZ, bottom = dy < hatStart + 3;
          P p = o.offset(dx, dy, dz);
          if (!replaceable_at(l, p, false)) continue;
          if (bottom) {
            if (inside) continue;
            if (is(l.get(p.below()), hat)) {  // placeHatDropBlock
              l.set(p, hat);
            } else if (r.nextFloat() < 0.15) {
              l.set(p, hat);
              if (placeVines && r.nextInt(11) == 0) vines(l, r, p);
            }
          } else if (inside) {
            hat_block(l, r, p, 0.1f, 0.2f, placeVines ? 0.1f : 0.0f);
          } else if (corner) {
            hat_block(l, r, p, 0.01f, 0.7f, placeVines ? 0.083f : 0.0f);
          } else {
            hat_block(l, r, p, 5.0E-4f, 0.98f, placeVines ? 0.07f : 0.0f);
          }
        }
    }
    return true;
  }
};

// ---------- the End ----------
struct EndSpikeF : Feature {
  std::vector<EndSpike> spikes;  // empty: end_spikes(seed)
  bool invulnerable = false;
  std::optional<P> beamTarget;
  static void place_spike(Level& l, RandomSource& r, const EndSpike& s) {
    int rad = s.radius;
    for (int z = s.z - rad; z <= s.z + rad; z++)  // betweenClosed: x fastest, then y, then z
      for (int y = l.min_y(); y <= s.height + 10; y++)
        for (int x = s.x - rad; x <= s.x + rad; x++) {
          double dx = x - s.x, dz = z - s.z;  // distToLowCornerSqr(centerX, y, centerZ)
          if (dx * dx + dz * dz <= rad * rad + 1 && y < s.height) l.set({x, y, z}, blocks::obsidian);
          else if (y > 65) l.set({x, y, z}, blocks::air);
        }
    if (s.guarded)
      for (int dx = -2; dx <= 2; dx++)
        for (int dz = -2; dz <= 2; dz++)
          for (int dy = 0; dy <= 3; dy++) {
            bool xSide = std::abs(dx) == 2, zSide = std::abs(dz) == 2, top = dy == 3;
            if (!xSide && !zSide && !top) continue;
            bool xEdge = dx == -2 || dx == 2 || top, zEdge = dz == -2 || dz == 2 || top;
            int st = blocks::iron_bars;
            st = R().with_bool(st, "north", xEdge && dz != -2);
            st = R().with_bool(st, "south", xEdge && dz != 2);
            st = R().with_bool(st, "west", zEdge && dx != -2);
            st = R().with_bool(st, "east", zEdge && dx != 2);
            l.set({s.x + dx, s.height + dy, s.z + dz}, st);
          }
    // EndCrystal at (x + 0.5, height + 1, z + 0.5), yaw random.nextFloat() * 360 (consumed for parity).
    (void)r.nextFloat();
    P c{s.x, s.height + 1, s.z};
    if (ProtoChunk* pc = l.region().chunk(c.x >> 4, c.z >> 4); pc && pc->chunk)
      // ponytail: crystal beam target / invulnerability are not carried; only the dragon fight sets them.
      pc->chunk->structure_mobs.push_back({entity_types::end_crystal, c.x + 0.5, double(c.y), c.z + 0.5, false, false, false});
    l.set(c.below(), blocks::bedrock);
    l.set(c, blocks::fire);  // FireBlock.getState over bedrock: default fire
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    std::vector<EndSpike> list = spikes.empty() ? end_spikes(l.seed()) : spikes;
    for (auto& s : list)
      if ((o.x >> 4) == (s.x >> 4) && (o.z >> 4) == (s.z >> 4)) place_spike(l, r, s);
    return true;
  }
};

struct EndIsland : Feature {
  bool place(Level& l, RandomSource& r, const P& o) const override {
    float size = float(r.nextInt(3)) + 4.0f;
    for (int y = 0; size > 0.5f; y--) {
      for (int x = Mth::floor(-size); x <= ceil_i(size); x++)
        for (int z = Mth::floor(-size); z <= ceil_i(size); z++)
          if (float(x * x + z * z) <= (size + 1.0f) * (size + 1.0f)) l.set(o.offset(x, y, z), blocks::end_stone);
      size -= float(r.nextInt(2)) + 0.5f;
    }
    return true;
  }
};

// ChorusPlantBlock.getStateWithConnections
int chorus_state(Level& l, const P& p) {
  static const bs::Tag& supports = R().tag("minecraft:supports_chorus_plant");
  auto chorus = [&](int s) { return is(s, blocks::chorus_plant) || is(s, blocks::chorus_flower); };
  int s = blocks::chorus_plant, down = l.get(p.below());
  s = R().with_bool(s, "down", chorus(down) || R().in(down, supports));
  s = R().with_bool(s, "up", chorus(l.get(p.above())));
  for (bs::Dir d : HORIZONTAL) s = R().with_bool(s, dir_name(d), chorus(l.get(p.rel(d))));
  return s;
}
bool all_neighbors_empty(Level& l, const P& p, int ignore) {
  for (bs::Dir d : HORIZONTAL)
    if (int(d) != ignore && !l.empty(p.rel(d))) return false;
  return true;
}
void grow_chorus(Level& l, const P& cur, RandomSource& r, const P& start, int spread, int depth) {  // growTreeRecursive
  int h = r.nextInt(4) + 1;
  if (depth == 0) h++;
  for (int i = 0; i < h; i++) {
    P t = cur.above(i + 1);
    if (!all_neighbors_empty(l, t, -1)) return;
    l.set(t, chorus_state(l, t));
    l.set(t.below(), chorus_state(l, t.below()));
  }
  bool placedStem = false;
  if (depth < 4) {
    int stems = r.nextInt(4);
    if (depth == 0) stems++;
    for (int i = 0; i < stems; i++) {
      bs::Dir d = random_horizontal(r);
      P t = cur.above(h).rel(d);
      if (std::abs(t.x - start.x) < spread && std::abs(t.z - start.z) < spread && l.empty(t) && l.empty(t.below()) &&
          all_neighbors_empty(l, t, bs::opposite(d))) {
        placedStem = true;
        l.set(t, chorus_state(l, t));
        P back = t.rel(bs::opposite(d));
        l.set(back, chorus_state(l, back));
        grow_chorus(l, t, r, start, spread, depth + 1);
      }
    }
  }
  if (!placedStem) l.set(cur.above(h), R().with_int(blocks::chorus_flower, "age", 5));
}

struct ChorusPlant : Feature {
  bool place(Level& l, RandomSource& r, const P& o) const override {
    static const bs::Tag& supports = R().tag("minecraft:supports_chorus_plant");
    if (!l.empty(o) || !R().in(l.get(o.below()), supports)) return false;
    l.set(o, chorus_state(l, o));  // ChorusFlowerBlock.generatePlant(level, origin, random, 8)
    grow_chorus(l, o, r, o, 8, 0);
    return true;
  }
};

struct EndPlatform : Feature {
  bool place(Level& l, RandomSource&, const P& o) const override {
    for (int dz = -2; dz <= 2; dz++)
      for (int dx = -2; dx <= 2; dx++)
        for (int dy = -1; dy < 3; dy++) {
          int b = dy == -1 ? blocks::obsidian : blocks::air;
          P p = o.offset(dx, dy, dz);
          if (!is(l.get(p), b)) l.set(p, b);
        }
    return true;
  }
};

struct EndGatewayF : Feature {
  std::optional<P> exit;
  bool exact = false;
  bool place(Level& l, RandomSource&, const P& o) const override {
    for (int z = o.z - 1; z <= o.z + 1; z++)
      for (int y = o.y - 2; y <= o.y + 2; y++)
        for (int x = o.x - 1; x <= o.x + 1; x++) {
          bool sx = x == o.x, sy = y == o.y, sz = z == o.z, end = std::abs(y - o.y) == 2;
          P p{x, y, z};
          if (sx && sy && sz) {
            l.set(p, blocks::end_gateway);
            if (exit)
              if (BlockEntity* be = l.block_entity(p)) be->nbt = end_gateway_nbt(exit->x, exit->y, exit->z, exact);
          } else if (sy) {
            l.set(p, blocks::air);
          } else if (end && sx && sz) {
            l.set(p, blocks::bedrock);
          } else if ((sx || sz) && !end) {
            l.set(p, blocks::bedrock);
          } else {
            l.set(p, blocks::air);
          }
        }
    return true;
  }
};

P vec3(const Json& j) { return {j.arr[0].asInt(), j.arr[1].asInt(), j.arr[2].asInt()}; }
}  // namespace

std::vector<EndSpike> end_spikes(int64_t seed) {
  int64_t key = SingleThreadedRandomSource(seed).nextLong() & 65535;  // getSpikesForLevel
  SingleThreadedRandomSource r(key);
  std::vector<int> sizes(10);
  for (int i = 0; i < 10; i++) sizes[size_t(i)] = i;
  shuffle(sizes, r);  // Util.toShuffledList
  std::vector<EndSpike> out;
  for (int i = 0; i < 10; i++) {
    int x = Mth::floor(42.0 * std::cos(2.0 * (-M_PI + (M_PI / 10) * i)));
    int z = Mth::floor(42.0 * std::sin(2.0 * (-M_PI + (M_PI / 10) * i)));
    int size = sizes[size_t(i)];
    out.push_back({x, z, 2 + size / 3, 76 + size * 3, size == 1 || size == 2});
  }
  return out;
}

std::vector<uint8_t> end_gateway_nbt(int x, int y, int z, bool exact) {  // TheEndGatewayBlockEntity.saveAdditional
  std::vector<uint8_t> n{0x0a};
  auto name = [&](uint8_t tag, const std::string& s) {
    n.push_back(tag), n.push_back(uint8_t(s.size() >> 8)), n.push_back(uint8_t(s.size()));
    n.insert(n.end(), s.begin(), s.end());
  };
  auto i32 = [&](int32_t v) { for (int sh = 24; sh >= 0; sh -= 8) n.push_back(uint8_t(uint32_t(v) >> sh)); };
  name(0x04, "Age");
  for (int i = 0; i < 8; i++) n.push_back(0);
  name(0x0b, "exit_portal");
  i32(3), i32(x), i32(y), i32(z);
  if (exact) name(0x01, "ExactTeleport"), n.push_back(1);
  n.push_back(0);
  return n;
}

void register_nether_end_features(ParserMap& m) {
  m["delta_feature"] = [](Loader& L, const Json& j) {
    auto* f = L.make<Delta>();
    f->contents = L.state(j["contents"]), f->rim = L.state(j["rim"]);
    f->size = IntProvider(j["size"]), f->rimSize = IntProvider(j["rim_size"]);
    return f;
  };
  m["stepped_column_cluster"] = [](Loader& L, const Json& j) {
    auto* f = L.make<SteppedColumnCluster>();
    f->block = L.provider(j["block"]);
    f->continueThrough = L.predicate(j["continue_through"]), f->canReplace = L.predicate(j["can_replace"]);
    f->cannotPlaceOn = L.block_set(j["cannot_place_on"]);
    f->clusterReach = IntProvider(j["cluster_reach"]), f->columnCount = IntProvider(j["column_count"]);
    f->columnReach = IntProvider(j["column_reach"]), f->height = IntProvider(j["height"]);
    return f;
  };
  m["netherrack_replace_blobs"] = [](Loader& L, const Json& j) {
    auto* f = L.make<ReplaceBlobs>();
    f->target = L.state(j["target"]), f->state = L.state(j["state"]), f->radius = IntProvider(j["radius"]);
    return f;
  };
  m["single_block_pillar"] = [](Loader& L, const Json& j) {
    auto* f = L.make<SingleBlockPillar>();
    f->block = L.provider(j["block"]);
    if (auto* c = j.find("can_replace")) f->canReplace = L.predicate(*c);
    f->dir = dir_by_name(strip_ns(j["direction"].asString()));
    f->chance = float(j.getDouble("chance_to_continue", 1.0));
    if (auto* c = j.find("cap_feature")) f->cap = L.placed(*c);
    return f;
  };
  m["projected_random_patchy_square"] = [](Loader& L, const Json& j) {
    auto* f = L.make<ProjectedPatchySquare>();
    f->block = L.provider(j["block"]), f->projectThrough = L.predicate(j["project_through"]);
    f->size = IntProvider(j["size"]), f->maxProjection = j["max_projection_height"].asInt();
    return f;
  };
  m["random_neighbor_spread"] = [](Loader& L, const Json& j) {
    auto* f = L.make<RandomNeighborSpread>();
    f->block = L.provider(j["block"]), f->accepted = L.block_set(j["accepted_neighbors"]);
    f->canReplace = L.predicate(j["can_replace"]);
    f->attempts = IntProvider(j["attempts"]), f->xzOffset = IntProvider(j["xz_offset"]), f->yOffset = IntProvider(j["y_offset"]);
    return f;
  };
  m["huge_fungus"] = [](Loader& L, const Json& j) {
    auto* f = L.make<HugeFungus>();
    f->validBase = L.state(j["valid_base_block"]), f->stem = L.state(j["stem_state"]);
    f->hat = L.state(j["hat_state"]), f->decor = L.state(j["decor_state"]);
    f->replaceable = L.predicate(j["replaceable_blocks"]);
    if (auto* p = j.find("planted")) f->planted = p->asBool();
    return f;
  };
  m["end_spike"] = [](Loader& L, const Json& j) {
    auto* f = L.make<EndSpikeF>();
    for (auto& s : j["spikes"].arr)
      f->spikes.push_back({s.getInt("centerX", 0), s.getInt("centerZ", 0), s.getInt("radius", 0), s.getInt("height", 0),
                           s.find("guarded") && s["guarded"].asBool()});
    if (auto* b = j.find("crystal_invulnerable")) f->invulnerable = b->asBool();
    if (auto* t = j.find("crystal_beam_target")) f->beamTarget = vec3(*t);
    return f;
  };
  m["end_island"] = [](Loader& L, const Json&) { return L.make<EndIsland>(); };
  m["chorus_plant"] = [](Loader& L, const Json&) { return L.make<ChorusPlant>(); };
  m["end_platform"] = [](Loader& L, const Json&) { return L.make<EndPlatform>(); };
  m["end_gateway"] = [](Loader& L, const Json& j) {
    auto* f = L.make<EndGatewayF>();
    if (auto* e = j.find("exit")) f->exit = vec3(*e);
    f->exact = j["exact"].asBool();
    return f;
  };
}

}  // namespace worldgen::feat
