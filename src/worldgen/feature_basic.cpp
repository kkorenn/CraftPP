// Feature types: simple_block, block_column, the selectors, sequence, overlay, ore, scattered_ore,
// replace_single_block, disk, lake, spring_feature, underwater_magma, blue_ice, freeze_top_layer, vines,
// multiface_growth, block_pile, block_blob, bamboo, no_op. Ports of levelgen/feature/*Feature.java (26.3).
#include <cmath>

#include "worldgen/features.hpp"

namespace worldgen::feat {
namespace {
const bs::Registry& R() { return bs::R(); }
bool is(int s, int def) { return R().is(s, def); }

struct SimpleBlock : Feature {
  const StateProvider* toPlace;
  bool scheduleTick;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    int s = toPlace->optional(l, r, o);
    if (s < 0 || !can_survive(l, s, o)) return false;
    const auto& b = R().block(R().block_of(s));
    if (b.is_a("DoublePlantBlock")) {
      int above = l.get(o.above());
      if (!R().is_air(above) && (R().fluid_packed(s) != R().fluid_packed(above) || !R().can_be_replaced(above))) return false;
      place_double_plant(l, s, o);
    } else if (b.is_a("MossyCarpetBlock")) {
      place_mossy_carpet(l, o, l.random());
    } else {
      l.set(o, s);
    }
    return true;
  }
};

struct BlockColumn : Feature {
  std::vector<std::pair<IntProvider, const StateProvider*>> layers;
  bs::Dir dir;
  const BlockPredicate* allowed;
  bool prioritizeTip;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    int n = int(layers.size());
    std::vector<int> h(static_cast<size_t>(n));
    int total = 0;
    for (int i = 0; i < n; i++) h[size_t(i)] = layers[size_t(i)].first.sample(r), total += h[size_t(i)];
    if (total == 0) return false;
    P place = o, next = o.rel(dir);
    for (int y = 0; y < total; y++) {
      if (!allowed->test(l, next)) {  // truncate
        int remove = total - y, step = prioritizeTip ? 1 : -1;
        for (int i = prioritizeTip ? 0 : n - 1; i != (prioritizeTip ? n : -1) && remove > 0; i += step) {
          int t = std::min(h[size_t(i)], remove);
          remove -= t, h[size_t(i)] -= t;
        }
        break;
      }
      next = next.rel(dir);
    }
    for (int i = 0; i < n; i++)
      for (int y = 0; y < h[size_t(i)]; y++) {
        l.set(place, layers[size_t(i)].second->get(l, r, place));
        place = place.rel(dir);
      }
    return true;
  }
};

struct RandomSelector : Feature {
  std::vector<std::pair<const Placed*, float>> features;
  const Placed* def;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    for (auto& [f, chance] : features)
      if (r.nextFloat() < chance) return f->place(l, r, o);
    return def->place(l, r, o);
  }
};
struct WeightedSelector : Feature {
  std::vector<std::pair<const Placed*, int>> features;
  int total = 0;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    if (features.empty()) return false;
    int sel = r.nextInt(total);
    for (auto& [f, w] : features)
      if ((sel -= w) < 0) return f->place(l, r, o);
    return false;
  }
};
struct SimpleRandomSelector : Feature {
  std::vector<const Placed*> features;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    return features[size_t(r.nextInt(int(features.size())))]->place(l, r, o);
  }
};
struct BooleanSelector : Feature {
  const Placed *t, *f;
  bool place(Level& l, RandomSource& r, const P& o) const override { return (r.nextBoolean() ? t : f)->place(l, r, o); }
};
struct Sequence : Feature {
  std::vector<const Placed*> features;
  bool overlay;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    bool any = false;
    for (auto* f : features) {
      bool ok = f->place(l, r, o);
      if (!overlay && !ok) return false;
      any |= ok;
    }
    return overlay ? any : true;
  }
};
struct NoOp : Feature {
  bool place(Level&, RandomSource&, const P&) const override { return true; }
};

struct Replacement {
  const RuleTest* target;
  int state;
};
std::vector<Replacement> replacements(Loader& L, const Json& j) {
  std::vector<Replacement> v;
  for (auto& t : j.arr) v.push_back({L.rule(t["target"]), L.state(t["state"])});
  return v;
}
// AbstractOreFeature.canPlaceOre
bool can_place_ore(int state, const std::function<int(const P&)>& get, RandomSource& r, const Replacement& t, const P& p, float discard) {
  if (!t.target->test(state, p, r)) return false;
  bool skip = discard <= 0.0F ? true : discard >= 1.0F ? false : r.nextFloat() >= discard;
  if (skip) return true;
  for (bs::Dir d : ALL_DIRS)
    if (R().is_air(get(p.rel(d)))) return false;
  return true;
}

struct Ore : Feature {
  std::vector<Replacement> targets;
  int size;
  float discard;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    float dir = r.nextFloat() * (float)M_PI;
    float spread = (float)size / 8.0F;
    int maxRadius = ceil_i((double)(((float)size / 16.0F * 2.0F + 1.0F) / 2.0F));
    double x0 = o.x + std::sin((double)dir) * spread, x1 = o.x - std::sin((double)dir) * spread;
    double z0 = o.z + std::cos((double)dir) * spread, z1 = o.z - std::cos((double)dir) * spread;
    double y0 = o.y + r.nextInt(3) - 2;
    double y1 = o.y + r.nextInt(3) - 2;
    int xStart = o.x - ceil_i(spread) - maxRadius, yStart = o.y - 2 - maxRadius, zStart = o.z - ceil_i(spread) - maxRadius;
    int sizeXZ = 2 * (ceil_i(spread) + maxRadius), sizeY = 2 * (2 + maxRadius);
    for (int xp = xStart; xp <= xStart + sizeXZ; xp++)
      for (int zp = zStart; zp <= zStart + sizeXZ; zp++)
        if (yStart <= l.height(OCEAN_FLOOR_WG, xp, zp)) return doPlace(l, r, x0, x1, z0, z1, y0, y1, xStart, yStart, zStart, sizeXZ, sizeY);
    return false;
  }
  bool doPlace(Level& l, RandomSource& r, double x0, double x1, double z0, double z1, double y0, double y1, int xStart,
               int yStart, int zStart, int sizeXZ, int sizeY) const {
    int placed = 0;
    std::vector<bool> tested(size_t(sizeXZ * sizeY * sizeXZ));
    std::vector<double> data(size_t(size * 4));
    for (int i = 0; i < size; i++) {
      float step = (float)i / (float)size;
      double xx = Mth::lerp((double)step, x0, x1), yy = Mth::lerp((double)step, y0, y1), zz = Mth::lerp((double)step, z0, z1);
      double ss = r.nextDouble() * size / 16.0;
      double rad = ((Mth::sin((double)((float)M_PI * step)) + 1.0F) * ss + 1.0) / 2.0;
      data[size_t(i * 4)] = xx, data[size_t(i * 4 + 1)] = yy, data[size_t(i * 4 + 2)] = zz, data[size_t(i * 4 + 3)] = rad;
    }
    for (int i1 = 0; i1 < size - 1; i1++) {
      if (data[size_t(i1 * 4 + 3)] <= 0.0) continue;
      for (int i2 = i1 + 1; i2 < size; i2++) {
        if (data[size_t(i2 * 4 + 3)] <= 0.0) continue;
        double dx = data[size_t(i1 * 4)] - data[size_t(i2 * 4)], dy = data[size_t(i1 * 4 + 1)] - data[size_t(i2 * 4 + 1)];
        double dz = data[size_t(i1 * 4 + 2)] - data[size_t(i2 * 4 + 2)], dr = data[size_t(i1 * 4 + 3)] - data[size_t(i2 * 4 + 3)];
        if (dr * dr > dx * dx + dy * dy + dz * dz) {
          if (dr > 0.0) data[size_t(i2 * 4 + 3)] = -1.0;
          else data[size_t(i1 * 4 + 3)] = -1.0;
        }
      }
    }
    // BulkSectionAccess: direct section access (no heightmap updates), air outside the build height.
    auto get = [&](const P& p) { return l.outside(p.y) ? int(blocks::air) : int(l.get(p)); };
    for (int i = 0; i < size; i++) {
      double rad = data[size_t(i * 4 + 3)];
      if (rad < 0.0) continue;
      double xx = data[size_t(i * 4)], yy = data[size_t(i * 4 + 1)], zz = data[size_t(i * 4 + 2)];
      int xMin = std::max(Mth::floor(xx - rad), xStart), yMin = std::max(Mth::floor(yy - rad), yStart), zMin = std::max(Mth::floor(zz - rad), zStart);
      int xMax = std::max(Mth::floor(xx + rad), xMin), yMax = std::max(Mth::floor(yy + rad), yMin), zMax = std::max(Mth::floor(zz + rad), zMin);
      for (int x = xMin; x <= xMax; x++) {
        double xd = (x + 0.5 - xx) / rad;
        if (!(xd * xd < 1.0)) continue;
        for (int y = yMin; y <= yMax; y++) {
          double yd = (y + 0.5 - yy) / rad;
          if (!(xd * xd + yd * yd < 1.0)) continue;
          for (int z = zMin; z <= zMax; z++) {
            double zd = (z + 0.5 - zz) / rad;
            if (!(xd * xd + yd * yd + zd * zd < 1.0) || l.outside(y)) continue;
            size_t bit = size_t(x - xStart + (y - yStart) * sizeXZ + (z - zStart) * sizeXZ * sizeY);
            if (tested[bit]) continue;
            tested[bit] = true;
            P p{x, y, z};
            if (!l.can_write(p)) continue;
            int state = l.get(p);
            for (auto& t : targets)
              if (can_place_ore(state, get, r, t, p, discard)) {
                l.region().chunk(x >> 4, z >> 4)->chunk->set(x & 15, y, z & 15, uint16_t(t.state));
                placed++;
                break;
              }
          }
        }
      }
    }
    return placed > 0;
  }
};

struct ScatteredOre : Feature {
  std::vector<Replacement> targets;
  int size;
  float discard;
  static int axis(RandomSource& r, int max) {
    float a = r.nextFloat();
    return java_round((a - r.nextFloat()) * (float)max);
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    int tries = r.nextInt(size + 1);
    auto get = [&](const P& p) { return int(l.get(p)); };
    for (int i = 0; i < tries; i++) {
      int m = std::min(i, 7);
      int xd = axis(r, m);
      int yd = axis(r, m);
      int zd = axis(r, m);
      P p = o.offset(xd, yd, zd);
      int s = l.get(p);
      for (auto& t : targets)
        if (can_place_ore(s, get, r, t, p, discard)) { l.set(p, t.state); break; }
    }
    return true;
  }
};

struct ReplaceSingle : Feature {
  std::vector<Replacement> targets;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    for (auto& t : targets)
      if (t.target->test(l.get(o), o, r)) { l.set(o, t.state); break; }
    return true;
  }
};

struct Disk : Feature {
  const StateProvider* provider;
  const BlockPredicate* target;
  IntProvider radius;
  int halfHeight;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    bool any = false;
    int top = o.y + halfHeight, bottom = o.y - halfHeight - 1;
    int rad = radius.sample(r);
    for (int z = o.z - rad; z <= o.z + rad; z++)  // BlockPos.betweenClosed: x fastest
      for (int x = o.x - rad; x <= o.x + rad; x++) {
        int xd = x - o.x, zd = z - o.z;
        if (xd * xd + zd * zd > rad * rad) continue;
        bool placedAbove = false;
        for (int y = top; y > bottom; y--) {
          P p{x, y, z};
          if (target->test(l, p)) {
            int s = provider->optional(l, r, p);
            if (s >= 0) {
              l.set(p, s);
              if (!placedAbove) l.mark_above_for_post_processing(p);
              any = true, placedAbove = true;
            }
          } else {
            placedAbove = false;
          }
        }
      }
    return any;
  }
};

struct Lake : Feature {
  const StateProvider *fluid, *barrier;
  const BlockPredicate *canPlace, *canReplaceAirOrFluid, *canReplaceBarrier;
  bool place(Level& l, RandomSource& r, const P& origin) const override {
    if (origin.y <= l.min_y() + 4) return false;
    P o = origin.offset(-8, -4, -8);
    bool grid[2048] = {};
    int spots = r.nextInt(4) + 4;
    for (int i = 0; i < spots; i++) {
      double xr = r.nextDouble() * 6.0 + 3.0, yr = r.nextDouble() * 4.0 + 2.0, zr = r.nextDouble() * 6.0 + 3.0;
      double xp = r.nextDouble() * (16.0 - xr - 2.0) + 1.0 + xr / 2.0;
      double yp = r.nextDouble() * (8.0 - yr - 4.0) + 2.0 + yr / 2.0;
      double zp = r.nextDouble() * (16.0 - zr - 2.0) + 1.0 + zr / 2.0;
      for (int xx = 1; xx < 15; xx++)
        for (int zz = 1; zz < 15; zz++)
          for (int yy = 1; yy < 7; yy++) {
            double xd = (xx - xp) / (xr / 2.0), yd = (yy - yp) / (yr / 2.0), zd = (zz - zp) / (zr / 2.0);
            if (xd * xd + yd * yd + zd * zd < 1.0) grid[(xx * 16 + zz) * 8 + yy] = true;
          }
    }
    auto g = [&](int x, int z, int y) { return grid[(x * 16 + z) * 8 + y]; };
    auto edge = [&](int x, int z, int y) {
      return !g(x, z, y) && ((x < 15 && g(x + 1, z, y)) || (x > 0 && g(x - 1, z, y)) || (z < 15 && g(x, z + 1, y)) ||
                             (z > 0 && g(x, z - 1, y)) || (y < 7 && g(x, z, y + 1)) || (y > 0 && g(x, z, y - 1)));
    };
    int fl = fluid->get(l, r, o);
    for (int xx = 0; xx < 16; xx++)
      for (int zz = 0; zz < 16; zz++)
        for (int y = 0; y < 8; y++)
          if (edge(xx, zz, y)) {
            P p = o.offset(xx, y, zz);
            int s = l.get(p);
            if (y >= 4 && R().liquid(s)) return false;
            if (y < 4 && !R().is_solid(s) && s != fl) return false;
            if (!canPlace->test(l, p)) return false;
          }
    for (int xx = 0; xx < 16; xx++)
      for (int zz = 0; zz < 16; zz++)
        for (int y = 0; y < 8; y++)
          if (g(xx, zz, y)) {
            P p = o.offset(xx, y, zz);
            if (!canReplaceAirOrFluid->test(l, p)) continue;
            bool air = y >= 4;
            l.set(p, air ? int(blocks::cave_air) : fl);
            if (air) l.mark_above_for_post_processing(p);
          }
    int bar = barrier->get(l, r, o);
    if (!R().is_air(bar))
      for (int xx = 0; xx < 16; xx++)
        for (int zz = 0; zz < 16; zz++)
          for (int y = 0; y < 8; y++)
            if (edge(xx, zz, y) && (y < 4 || r.nextInt(2) != 0)) {
              P p = o.offset(xx, y, zz);
              if (R().is_solid(l.get(p)) && canReplaceBarrier->test(l, p)) {
                l.set(p, bar);
                l.mark_above_for_post_processing(p);
              }
            }
    if (R().is_water(fl))
      for (int xx = 0; xx < 16; xx++)
        for (int zz = 0; zz < 16; zz++) {
          P p = o.offset(xx, 4, zz);
          if (l.should_freeze(l.biome(p), p, false) && canReplaceAirOrFluid->test(l, p)) l.set(p, blocks::ice);
        }
    return true;
  }
};

struct Spring : Feature {
  int state;  // FluidState.createLegacyBlock
  bool requiresBelow;
  int rockCount, holeCount;
  bs::Tag valid;
  bool place(Level& l, RandomSource&, const P& o) const override {
    auto ok = [&](const P& p) { return R().in(l.get(p), valid); };
    if (!ok(o.above())) return false;
    if (requiresBelow && !ok(o.below())) return false;
    int cur = l.get(o);
    if (!R().is_air(cur) && !R().in(cur, valid)) return false;
    int rocks = 0, holes = 0;
    for (P p : {o.offset(-1, 0, 0), o.offset(1, 0, 0), o.offset(0, 0, -1), o.offset(0, 0, 1), o.below()}) rocks += ok(p);
    for (P p : {o.offset(-1, 0, 0), o.offset(1, 0, 0), o.offset(0, 0, -1), o.offset(0, 0, 1), o.below()}) holes += l.empty(p);
    if (rocks == rockCount && holes == holeCount) {
      l.set(o, state);
      return true;
    }
    return false;
  }
};

struct UnderwaterMagma : Feature {
  int searchRange, radius;
  float probability;
  static bool visible(Level& l, const P& p, bs::Dir covered) { return !R().face_occlusion_full(l.get(p), covered); }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    auto c = column_scan(l, o, searchRange, [](int s) { return is(s, blocks::water); }, [](int s) { return !is(s, blocks::water); });
    if (!c.found || !c.floor) return false;
    P f{o.x, *c.floor, o.z};
    int count = 0;
    for (int z = f.z - radius; z <= f.z + radius; z++)  // betweenClosedStream: x fastest, then y, then z
      for (int y = f.y - radius; y <= f.y + radius; y++)
        for (int x = f.x - radius; x <= f.x + radius; x++) {
          if (!(r.nextFloat() < probability)) continue;
          P p{x, y, z};
          int s = l.get(p);
          if (is(s, blocks::water) || R().is_air(s) || visible(l, p.below(), bs::UP)) continue;
          bool ok = true;
          for (bs::Dir d : HORIZONTAL)
            if (visible(l, p.rel(d), bs::opposite(d))) { ok = false; break; }
          if (!ok) continue;
          l.set(p, blocks::magma_block);
          count++;
        }
    return count > 0;
  }
};

struct BlueIce : Feature {
  bool place(Level& l, RandomSource& r, const P& o) const override {
    if (o.y > l.sea_level() - 1) return false;
    if (!is(l.get(o), blocks::water) && !is(l.get(o.below()), blocks::water)) return false;
    bool packed = false;
    for (bs::Dir d : ALL_DIRS)
      if (d != bs::DOWN && is(l.get(o.rel(d)), blocks::packed_ice)) { packed = true; break; }
    if (!packed) return false;
    l.set(o, blocks::blue_ice);
    for (int i = 0; i < 200; i++) {
      int a = r.nextInt(5);
      int yOff = a - r.nextInt(6);
      int xz = 3;
      if (yOff < 2) xz += yOff / 2;
      if (xz < 1) continue;
      int x1 = r.nextInt(xz);
      int dx = x1 - r.nextInt(xz);
      int z1 = r.nextInt(xz);
      int dz = z1 - r.nextInt(xz);
      P p = o.offset(dx, yOff, dz);
      int s = l.get(p);
      if (R().is_air(s) || is(s, blocks::water) || is(s, blocks::packed_ice) || is(s, blocks::ice))
        for (bs::Dir d : ALL_DIRS)
          if (is(l.get(p.rel(d)), blocks::blue_ice)) { l.set(p, blocks::blue_ice); break; }
    }
    return true;
  }
};

struct FreezeTopLayer : Feature {
  bool place(Level& l, RandomSource&, const P& o) const override {
    for (int dx = 0; dx < 16; dx++)
      for (int dz = 0; dz < 16; dz++) {
        int x = o.x + dx, z = o.z + dz;
        P top{x, l.height(MOTION_BLOCKING, x, z), z}, below = top.below();
        int b = l.biome(top);
        if (l.should_freeze(b, below, false)) l.set(below, blocks::ice);
        if (l.should_snow(b, top)) {
          l.set(top, blocks::snow);
          int bs_ = l.get(below);
          if (R().has(bs_, "snowy")) l.set(below, R().with_bool(bs_, "snowy", true));
        }
      }
    return true;
  }
};

struct Vines : Feature {
  bool place(Level& l, RandomSource&, const P& o) const override {
    if (!l.empty(o)) return false;
    for (bs::Dir d : ALL_DIRS)
      if (d != bs::DOWN && multiface_can_attach(l, d, o.rel(d))) {
        l.set(o, R().with_bool(blocks::vine, dir_name(d), true));
        return true;
      }
    return false;
  }
};

struct MultifaceGrowth : Feature {
  int block, searchRange;
  bool floor, ceiling, wall;
  float chance;
  bs::Tag placeOn;
  std::vector<bs::Dir> valid() const {
    std::vector<bs::Dir> v;
    if (ceiling) v.push_back(bs::UP);
    if (floor) v.push_back(bs::DOWN);
    if (wall) v.insert(v.end(), HORIZONTAL, HORIZONTAL + 4);
    return v;
  }
  static bool airOrWater(int s) { return R().is_air(s) || is(s, blocks::water); }
  bool grow(Level& l, const P& p, int old, RandomSource& r, const std::vector<bs::Dir>& dirs) const {
    for (bs::Dir d : dirs) {
      if (!R().in(l.get(p.rel(d)), placeOn)) continue;
      int ns = multiface_placement(l, block, old, p, d);
      if (ns < 0) return false;
      l.set(p, ns);
      l.mark_post_processing(p);
      if (r.nextFloat() < chance) multiface_spread_from_face_random(l, ns, p, d, r, true);
      return true;
    }
    return false;
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    if (!airOrWater(l.get(o))) return false;
    auto dirs = valid();
    shuffle(dirs, r);
    if (grow(l, o, l.get(o), r, dirs)) return true;
    for (bs::Dir search : dirs) {
      std::vector<bs::Dir> place;
      for (bs::Dir d : valid())
        if (d != bs::opposite(search)) place.push_back(d);
      shuffle(place, r);
      P p = o;
      for (int i = 0; i < searchRange; i++) {
        p = o.rel(search);  // pos.setWithOffset(origin, searchDirection): always one step from origin
        int s = l.get(p);
        if (!airOrWater(s) && R().block_of(s) != block) break;
        if (grow(l, p, s, r, place)) return true;
      }
    }
    return false;
  }
};

struct BlockPile : Feature {
  const StateProvider* provider;
  void tryPlace(Level& l, const P& p, RandomSource& r) const {
    if (!l.empty(p)) return;
    int below = l.get(p.below());
    bool may = is(below, blocks::dirt_path) ? r.nextBoolean() : R().sturdy(below, bs::UP);
    if (may) l.set(p, provider->get(l, r, p));
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    if (o.y < l.min_y() + 5) return false;
    int xr = 2 + r.nextInt(2);
    int zr = 2 + r.nextInt(2);
    for (int z = o.z - zr; z <= o.z + zr; z++)
      for (int y = o.y; y <= o.y + 1; y++)
        for (int x = o.x - xr; x <= o.x + xr; x++) {
          int xd = o.x - x, zd = o.z - z;
          float a = r.nextFloat();
          if ((float)(xd * xd + zd * zd) <= a * 10.0F - r.nextFloat() * 6.0F) tryPlace(l, {x, y, z}, r);
          else if ((double)r.nextFloat() < 0.031) tryPlace(l, {x, y, z}, r);
        }
    return true;
  }
};

struct BlockBlob : Feature {
  int state;
  const BlockPredicate* canPlaceOn;
  bool place(Level& l, RandomSource& r, const P& origin) const override {
    P o = origin;
    while (o.y > l.min_y() + 3 && !canPlaceOn->test(l, o.below())) o = o.below();
    if (o.y <= l.min_y() + 3) return false;
    for (int c = 0; c < 3; c++) {
      int xr = r.nextInt(2);
      int yr = r.nextInt(2);
      int zr = r.nextInt(2);
      float tr = (float)(xr + yr + zr) * 0.333F + 0.5F;
      for (int z = o.z - zr; z <= o.z + zr; z++)
        for (int y = o.y - yr; y <= o.y + yr; y++)
          for (int x = o.x - xr; x <= o.x + xr; x++)
            if (dist_sqr({x, y, z}, o) <= (double)(tr * tr)) l.set({x, y, z}, state);
      int dx = -1 + r.nextInt(2);
      int dy = -r.nextInt(2);
      int dz = -1 + r.nextInt(2);
      o = o.offset(dx, dy, dz);
    }
    return true;
  }
};

struct Bamboo : Feature {
  float probability;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    static const int trunk = R().parse("minecraft:bamboo[age=1,leaves=none,stage=0]");
    static const int finalLarge = R().parse("minecraft:bamboo[age=1,leaves=large,stage=1]");
    static const int topLarge = R().parse("minecraft:bamboo[age=1,leaves=large,stage=0]");
    static const int topSmall = R().parse("minecraft:bamboo[age=1,leaves=small,stage=0]");
    if (!l.empty(o)) return false;
    if (can_survive(l, blocks::bamboo, o)) {
      int height = r.nextInt(12) + 5;
      if (r.nextFloat() < probability) {
        int rad = r.nextInt(4) + 1;
        for (int xx = o.x - rad; xx <= o.x + rad; xx++)
          for (int zz = o.z - rad; zz <= o.z + rad; zz++) {
            int xd = xx - o.x, zd = zz - o.z;
            if (xd * xd + zd * zd > rad * rad) continue;
            P p{xx, l.height(WORLD_SURFACE, xx, zz) - 1, zz};
            if (R().in(l.get(p), R().tag("minecraft:beneath_bamboo_podzol_replaceable"))) l.set(p, blocks::podzol);
          }
      }
      P p = o;
      for (int i = 0; i < height && l.empty(p); i++) l.set(p, trunk), p = p.above();
      if (p.y - o.y >= 3) {
        l.set(p, finalLarge);
        l.set(p.below(), topLarge);
        l.set(p.below(2), topSmall);
      }
    }
    return true;
  }
};
}  // namespace

void register_basic_features(ParserMap& m) {
  m["no_op"] = [](Loader& L, const Json&) { return L.make<NoOp>(); };
  m["simple_block"] = [](Loader& L, const Json& j) {
    auto* f = L.make<SimpleBlock>();
    f->toPlace = L.provider(j["to_place"]);
    f->scheduleTick = j.has("schedule_tick") && j["schedule_tick"].asBool();
    return f;
  };
  m["block_column"] = [](Loader& L, const Json& j) {
    auto* f = L.make<BlockColumn>();
    for (auto& layer : j["layers"].arr) f->layers.push_back({IntProvider(layer["height"]), L.provider(layer["provider"])});
    f->dir = dir_by_name(j["direction"].asString());
    f->allowed = L.predicate(j["allowed_placement"]);
    f->prioritizeTip = j["prioritize_tip"].asBool();
    return f;
  };
  m["random_selector"] = [](Loader& L, const Json& j) {
    auto* f = L.make<RandomSelector>();
    for (auto& e : j["features"].arr) f->features.push_back({L.placed(e["feature"]), e["chance"].asFloat()});
    f->def = L.placed(j["default"]);
    return f;
  };
  m["weighted_random_selector"] = [](Loader& L, const Json& j) {
    auto* f = L.make<WeightedSelector>();
    for (auto& e : j["features"].arr) f->features.push_back({L.placed(e["data"]), e["weight"].asInt()}), f->total += e["weight"].asInt();
    return f;
  };
  m["simple_random_selector"] = [](Loader& L, const Json& j) {
    auto* f = L.make<SimpleRandomSelector>();
    for (auto& e : j["features"].arr) f->features.push_back(L.placed(e));
    return f;
  };
  m["random_boolean_selector"] = [](Loader& L, const Json& j) {
    auto* f = L.make<BooleanSelector>();
    f->t = L.placed(j["feature_true"]), f->f = L.placed(j["feature_false"]);
    return f;
  };
  for (const char* t : {"sequence", "overlay"})
    m[t] = [t](Loader& L, const Json& j) {
      auto* f = L.make<Sequence>();
      for (auto& e : j["features"].arr) f->features.push_back(L.placed(e));
      f->overlay = std::string(t) == "overlay";
      return f;
    };
  m["ore"] = [](Loader& L, const Json& j) {
    auto* f = L.make<Ore>();
    f->targets = replacements(L, j["targets"]);
    f->size = j["size"].asInt();
    f->discard = j["discard_chance_on_air_exposure"].asFloat();
    return f;
  };
  m["scattered_ore"] = [](Loader& L, const Json& j) {
    auto* f = L.make<ScatteredOre>();
    f->targets = replacements(L, j["targets"]);
    f->size = j["size"].asInt();
    f->discard = j["discard_chance_on_air_exposure"].asFloat();
    return f;
  };
  m["replace_single_block"] = [](Loader& L, const Json& j) {
    auto* f = L.make<ReplaceSingle>();
    f->targets = replacements(L, j["targets"]);
    return f;
  };
  m["disk"] = [](Loader& L, const Json& j) {
    auto* f = L.make<Disk>();
    f->provider = L.provider(j["state_provider"]);
    f->target = L.predicate(j["target"]);
    f->radius = IntProvider(j["radius"]);
    f->halfHeight = j["half_height"].asInt();
    return f;
  };
  m["lake"] = [](Loader& L, const Json& j) {
    auto* f = L.make<Lake>();
    f->fluid = L.provider(j["fluid"]), f->barrier = L.provider(j["barrier"]);
    f->canPlace = L.predicate(j["can_place_feature"]);
    f->canReplaceAirOrFluid = L.predicate(j["can_replace_with_air_or_fluid"]);
    f->canReplaceBarrier = L.predicate(j["can_replace_with_barrier"]);
    return f;
  };
  m["spring_feature"] = [](Loader& L, const Json& j) {
    auto* f = L.make<Spring>();
    const Json& st = j["state"];
    std::string fluid = strip_ns(st["Name"].asString());
    bool falling = st.has("Properties") && st["Properties"].has("falling") && st["Properties"]["falling"].asString() == "true";
    f->state = R().parse(fluid == "lava" || fluid == "flowing_lava" ? "minecraft:lava" : "minecraft:water");
    if (falling) f->state = R().with(f->state, "level", "8");
    f->requiresBelow = !j.has("requires_block_below") || j["requires_block_below"].asBool();
    f->rockCount = j.getInt("rock_count", 4), f->holeCount = j.getInt("hole_count", 1);
    f->valid = L.block_set(j["valid_blocks"]);
    return f;
  };
  m["underwater_magma"] = [](Loader& L, const Json& j) {
    auto* f = L.make<UnderwaterMagma>();
    f->searchRange = j["floor_search_range"].asInt(), f->radius = j["placement_radius_around_floor"].asInt();
    f->probability = j["placement_probability_per_valid_position"].asFloat();
    return f;
  };
  m["blue_ice"] = [](Loader& L, const Json&) { return L.make<BlueIce>(); };
  m["freeze_top_layer"] = [](Loader& L, const Json&) { return L.make<FreezeTopLayer>(); };
  m["vines"] = [](Loader& L, const Json&) { return L.make<Vines>(); };
  m["multiface_growth"] = [](Loader& L, const Json& j) {
    auto* f = L.make<MultifaceGrowth>();
    f->block = R().block_index(j["block"].asString());
    f->searchRange = j.getInt("search_range", 10);
    f->floor = j.has("can_place_on_floor") && j["can_place_on_floor"].asBool();
    f->ceiling = j.has("can_place_on_ceiling") && j["can_place_on_ceiling"].asBool();
    f->wall = j.has("can_place_on_wall") && j["can_place_on_wall"].asBool();
    f->chance = j.has("chance_of_spreading") ? j["chance_of_spreading"].asFloat() : 0.5F;
    f->placeOn = L.block_set(j["can_be_placed_on"]);
    return f;
  };
  m["block_pile"] = [](Loader& L, const Json& j) {
    auto* f = L.make<BlockPile>();
    f->provider = L.provider(j["state_provider"]);
    return f;
  };
  m["block_blob"] = [](Loader& L, const Json& j) {
    auto* f = L.make<BlockBlob>();
    f->state = L.state(j["state"]), f->canPlaceOn = L.predicate(j["can_place_on"]);
    return f;
  };
  m["bamboo"] = [](Loader& L, const Json& j) {
    auto* f = L.make<Bamboo>();
    f->probability = j["probability"].asFloat();
    return f;
  };
}

}  // namespace worldgen::feat
