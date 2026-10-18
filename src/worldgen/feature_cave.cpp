// Cave features: vegetation_patch, waterlogged_vegetation_patch, root_system, speleothem (pointed dripstone),
// speleothem_cluster (dripstone cluster), large_dripstone, geode, monster_room. Ports of levelgen/feature (26.3).
#include <cmath>
#include <map>
#include <optional>

#include "commands/nbt.hpp"
#include "worldgen/features.hpp"
#include "worldgen/noise.hpp"
#include "worldgen/overworld.hpp"

namespace worldgen::feat {
namespace {
const bs::Registry& R() { return bs::R(); }
bool is(int s, int def) { return R().is(s, def); }
bool in(int s, const char* tag) { return R().in(s, R().tag(tag)); }

std::vector<uint8_t> nbt_bytes(const Nbt& root) {
  Writer w;
  w.u8(Nbt::Compound);
  nbt_payload(w, root);
  return w.b;
}

// ---------------------------------------------------------------- vegetation patches
struct VegetationPatch : Feature {
  bs::Tag replaceable;
  const StateProvider* ground;
  const Placed* vegetation;
  bs::Dir inwards;  // CaveSurface direction: floor = DOWN, ceiling = UP
  IntProvider depth, xzRadius;
  float extraBottom, vegetationChance, extraEdge;
  int verticalRange;
  bool waterlogged = false;

  bool placeGround(Level& l, RandomSource& r, P& below, int d) const {
    for (int i = 0; i < d; i++) {
      int s = ground->get(l, r, below);
      int b = l.get(below);
      if (R().block_of(s) != R().block_of(b)) {
        if (!R().in(b, replaceable)) return i != 0;
        l.set(below, s);
        below = below.rel(inwards);
      }
    }
    return true;
  }
  PosSet groundPatch(Level& l, RandomSource& r, const P& o, int xr, int zr) const {
    bs::Dir out = bs::opposite(inwards);
    PosSet surface;
    for (int dx = -xr; dx <= xr; dx++) {
      bool xEdge = dx == -xr || dx == xr;
      for (int dz = -zr; dz <= zr; dz++) {
        bool zEdge = dz == -zr || dz == zr, edge = xEdge || zEdge, corner = xEdge && zEdge, edgeNotCorner = edge && !corner;
        if (corner || (edgeNotCorner && (extraEdge == 0.0F || r.nextFloat() > extraEdge))) continue;
        P p = o.offset(dx, 0, dz);
        for (int off = 0; R().is_air(l.get(p)) && off < verticalRange; off++) p = p.rel(inwards);
        for (int off = 0; !R().is_air(l.get(p)) && off < verticalRange; off++) p = p.rel(out);
        P below = p.rel(inwards);
        if (l.empty(p) && R().sturdy(l.get(below), out)) {
          int d = depth.sample(r);
          if (extraBottom > 0.0F && r.nextFloat() < extraBottom) d++;
          P g = below;
          if (placeGround(l, r, below, d)) surface.insert(g);
        }
      }
    }
    if (!waterlogged) return surface;
    PosSet water;
    for (auto& p : surface.items()) {
      bool exposed = false;
      for (bs::Dir d : {bs::NORTH, bs::EAST, bs::SOUTH, bs::WEST, bs::DOWN})
        if (!R().sturdy(l.get(p.rel(d)), bs::opposite(d))) { exposed = true; break; }
      if (!exposed) water.insert(p);
    }
    for (auto& p : water.items()) l.set(p, blocks::water);
    return water;
  }
  bool placeVegetation(Level& l, RandomSource& r, const P& p) const {
    if (!waterlogged) return vegetation->place(l, r, p.rel(bs::opposite(inwards)));
    if (!vegetation->place(l, r, p.below().rel(bs::opposite(inwards)))) return false;
    int s = l.get(p);
    if (R().has(s, "waterlogged") && !R().bool_value(s, "waterlogged")) l.set(p, R().with_bool(s, "waterlogged", true));
    return true;
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    int xr = xzRadius.sample(r) + 1;
    int zr = xzRadius.sample(r) + 1;
    PosSet surface = groundPatch(l, r, o, xr, zr);
    for (auto& p : surface.items())
      if (vegetationChance > 0.0F && r.nextFloat() < vegetationChance) placeVegetation(l, r, p);
    return !surface.empty();
  }
};

struct RootSystem : Feature {
  const Placed* tree;
  int requiredSpace, levelTestDistance, maxLevelDeviation, rootRadius, rootAttempts, rootColumnMaxHeight;
  int hangingRadius, hangingSpan, hangingAttempts, allowedWater;
  bs::Tag rootReplaceable;
  const StateProvider *rootProvider, *hangingProvider;
  const BlockPredicate* allowedTreePos;
  bool spaceForTree(Level& l, const P& p) const {
    P up = p;
    for (int i = 1; i <= requiredSpace; i++) {
      up = up.above();
      int s = l.get(up);
      if (!R().is_air(s) && !(i + 1 <= allowedWater && R().is_water(s))) return false;
    }
    if (levelTestDistance > 0) {
      static const bs::Dir dir2d[4] = {bs::SOUTH, bs::WEST, bs::NORTH, bs::EAST};  // Direction.from2DDataValue
      for (bs::Dir d : dir2d) {
        P c = p.rel(d, levelTestDistance);
        if (R().is_air(l.get(c.below(maxLevelDeviation))) || !R().is_air(l.get(c.above(maxLevelDeviation)))) return false;
      }
    }
    return true;
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    if (!R().is_air(l.get(o))) return false;
    P w = o;
    bool ok = false;
    for (int y = 0; y < rootColumnMaxHeight; y++) {
      w = w.above();
      if (l.height(WORLD_SURFACE, w.x, w.z) < w.y) break;
      if (allowedTreePos->test(l, w) && spaceForTree(l, w)) {
        int below = l.get(w.below());
        if (R().is_lava(below) || !R().is_solid(below)) break;
        if (tree->place(l, r, w)) {
          for (int yy = o.y; yy < o.y + y; yy++)  // placeDirt
            for (int i = 0; i < rootAttempts; i++) {
              int a = r.nextInt(rootRadius);
              int dx = a - r.nextInt(rootRadius);
              int b = r.nextInt(rootRadius);
              int dz = b - r.nextInt(rootRadius);
              P q{o.x + dx, yy, o.z + dz};
              if (R().in(l.get(q), rootReplaceable)) l.set(q, rootProvider->get(l, r, q));
            }
          ok = true;
          break;
        }
      }
    }
    if (ok)
      for (int i = 0; i < hangingAttempts; i++) {  // placeRoots
        int a = r.nextInt(hangingRadius);
        int dx = a - r.nextInt(hangingRadius);
        int b = r.nextInt(hangingSpan);
        int dy = b - r.nextInt(hangingSpan);
        int c = r.nextInt(hangingRadius);
        int dz = c - r.nextInt(hangingRadius);
        P q = o.offset(dx, dy, dz);
        if (!l.empty(q)) continue;
        int s = hangingProvider->get(l, r, q);
        if (can_survive(l, s, q) && R().sturdy(l.get(q.above()), bs::DOWN)) l.set(q, s);
      }
    return true;
  }
};

// ---------------------------------------------------------------- speleothems (SpeleothemUtils)
bool emptyOrWater(int s) { return R().is_air(s) || is(s, blocks::water); }
bool emptyOrWaterOrLava(int s) { return emptyOrWater(s) || is(s, blocks::lava); }
struct Speleo {
  int baseBlock = 0, pointedBlock = 0;  // block indices
  bs::Tag replaceable;
  bool isBase(int s) const { return R().block_of(s) == baseBlock || R().in(s, replaceable); }
  bool placeBase(Level& l, const P& p) const {
    if (!R().in(l.get(p), replaceable)) return false;
    l.set(p, R().default_state(baseBlock));
    return true;
  }
  void grow(Level& l, const P& start, bs::Dir tip, int height, bool merged) const {
    if (!isBase(l.get(start.rel(bs::opposite(tip))))) return;
    P p = start;
    auto put = [&](const char* thick) {
      int s = R().with(R().with(R().default_state(pointedBlock), "vertical_direction", dir_name(tip)), "thickness", thick);
      s = R().with_bool(s, "waterlogged", l.water_at(p));
      l.set(p, s);
      p = p.rel(tip);
    };
    if (height >= 3) {
      put("base");
      for (int i = 0; i < height - 3; i++) put("middle");
    }
    if (height >= 2) put("frustum");
    if (height >= 1) put(merged ? "tip_merge" : "tip");
  }
};
double speleothem_height(double xz, double radius, double scale, double bluntness) {
  if (xz < bluntness) xz = bluntness;
  double r = xz / radius * 0.384;
  double p1 = 0.75 * std::pow(r, 1.3333333333333333), p2 = std::pow(r, 0.6666666666666666), p3 = 0.3333333333333333 * std::log(r);
  double h = std::max(scale * (p1 - p2 - p3), 0.0);
  return h / 0.384 * radius;
}
bool circle_embedded(Level& l, const P& c, int r) {
  if (emptyOrWaterOrLava(l.get(c))) return false;
  float inc = 6.0F / (float)r;
  for (float a = 0.0F; a < (float)(M_PI * 2); a += inc) {
    int dx = (int)(Mth::cos(a) * (float)r), dz = (int)(Mth::sin(a) * (float)r);
    if (emptyOrWaterOrLava(l.get(c.offset(dx, 0, dz)))) return false;
  }
  return true;
}

struct SpeleothemF : Feature, Speleo {
  float taller, directional, spread2, spread3;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    bool above = isBase(l.get(o.above())), below = isBase(l.get(o.below()));
    bs::Dir tip;
    if (above && below) tip = r.nextBoolean() ? bs::DOWN : bs::UP;
    else if (above) tip = bs::DOWN;
    else if (below) tip = bs::UP;
    else return false;
    P root = o.rel(bs::opposite(tip));
    placeBase(l, root);
    for (bs::Dir d : HORIZONTAL) {
      if (r.nextFloat() > directional) continue;
      P p1 = root.rel(d);
      placeBase(l, p1);
      if (r.nextFloat() > spread2) continue;
      P p2 = p1.rel(random_dir(r));
      placeBase(l, p2);
      if (r.nextFloat() > spread3) continue;
      P p3 = p2.rel(random_dir(r));
      placeBase(l, p3);
    }
    int height = r.nextFloat() < taller && emptyOrWater(l.get(o.rel(tip))) ? 2 : 1;
    grow(l, o, tip, height, false);
    return true;
  }
};

struct SpeleothemCluster : Feature, Speleo {
  int searchRange, maxDiff, heightDeviation, maxEdge, maxCenter;
  IntProvider height, radius, thickness;
  FloatProvider density, wetness;
  float chanceAtMax;
  int speleoHeight(RandomSource& r, int dx, int dz, float dens, int maxHeight) const {
    if (r.nextFloat() > dens) return 0;
    int dist = std::abs(dx) + std::abs(dz);
    float mean = (float)mth::clampedMap((double)dist, 0.0, (double)maxCenter, maxHeight / 2.0, 0.0);
    return (int)Mth::clamp(mth::normal(r, mean, (float)heightDeviation), 0.0F, (float)maxHeight);
  }
  bool canPlacePool(Level& l, const P& p) const {
    int s = l.get(p);
    if (is(s, blocks::water) || R().block_of(s) == baseBlock || R().block_of(s) == pointedBlock) return false;
    if (R().is_water(l.get(p.above()))) return false;
    auto adj = [&](const P& q) { int t = l.get(q); return in(t, "minecraft:base_stone_overworld") || R().is_water(t); };
    for (bs::Dir d : HORIZONTAL)
      if (!adj(p.rel(d))) return false;
    return adj(p.below());
  }
  void replaceWithBase(Level& l, P p, int n, bs::Dir d) const {
    for (int i = 0; i < n; i++) {
      if (!placeBase(l, p)) return;
      p = p.rel(d);
    }
  }
  void column(Level& l, RandomSource& r, const P& pos, int dx, int dz, float wet, double chance, int clusterH, float dens) const {
    auto c = column_scan(l, pos, searchRange, emptyOrWater, [](int s) { return !emptyOrWater(s); });
    if (!c.found || (!c.ceiling && !c.floor)) return;
    auto ceiling = c.ceiling;
    auto floor = c.floor;
    bool pool = r.nextFloat() < wet;
    if (pool && c.floor && canPlacePool(l, {pos.x, *c.floor, pos.z})) {
      floor = *c.floor - 1;
      l.set({pos.x, *c.floor, pos.z}, blocks::water);
    }
    int stalactite = 0, stalagmite = 0;
    bool wantTite = r.nextDouble() < chance;
    if (ceiling && wantTite && !is(l.get({pos.x, *ceiling, pos.z}), blocks::lava)) {
      int t = thickness.sample(r);
      replaceWithBase(l, {pos.x, *ceiling, pos.z}, t, bs::UP);
      int maxH = floor ? std::min(clusterH, *ceiling - *floor) : clusterH;
      stalactite = speleoHeight(r, dx, dz, dens, maxH);
    }
    bool wantMite = r.nextDouble() < chance;
    if (floor && wantMite && !is(l.get({pos.x, *floor, pos.z}), blocks::lava)) {
      int t = thickness.sample(r);
      replaceWithBase(l, {pos.x, *floor, pos.z}, t, bs::DOWN);
      stalagmite = ceiling ? std::max(0, stalactite + mth::randomBetweenInclusive(r, -maxDiff, maxDiff)) : speleoHeight(r, dx, dz, dens, clusterH);
    }
    int aTite = stalactite, aMite = stalagmite;
    if (ceiling && floor && *ceiling - stalactite <= *floor + stalagmite) {
      int lowest = std::max(*ceiling - stalactite, *floor + 1), highest = std::min(*floor + stalagmite, *ceiling - 1);
      int bottom = mth::randomBetweenInclusive(r, lowest, highest + 1);
      aTite = *ceiling - bottom, aMite = bottom - 1 - *floor;
    }
    bool merge = r.nextBoolean() && aTite > 0 && aMite > 0 && ceiling && floor && aTite + aMite == *ceiling - *floor - 1;
    if (ceiling) grow(l, {pos.x, *ceiling - 1, pos.z}, bs::DOWN, aTite, merge);
    if (floor) grow(l, {pos.x, *floor + 1, pos.z}, bs::UP, aMite, merge);
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    if (!emptyOrWater(l.get(o))) return false;
    int h = height.sample(r);
    float wet = wetness.sample(r);
    float dens = density.sample(r);
    int xr = radius.sample(r);
    int zr = radius.sample(r);
    for (int dx = -xr; dx <= xr; dx++)
      for (int dz = -zr; dz <= zr; dz++) {
        int edge = std::min(xr - std::abs(dx), zr - std::abs(dz));
        double chance = (double)mth::clampedMap((float)edge, 0.0F, (float)maxEdge, chanceAtMax, 1.0F);
        column(l, r, o.offset(dx, 0, dz), dx, dz, wet, chance, h, dens);
      }
    return true;
  }
};

struct LargeDripstone : Feature {
  bs::Tag replaceable;
  int searchRange, minRadiusForWind;
  IntProvider columnRadius;
  FloatProvider heightScale, titeBluntness, miteBluntness, windSpeed;
  float maxRatio, minBluntnessForWind;
  struct Wind {
    bool on = false;
    int originY = 0, maxOffset = 0;
    double wx = 0, wz = 0;
    P offset(const P& p) const {
      if (!on) return p;
      int dy = originY - p.y;
      int dx = std::clamp(Mth::floor(wx * dy), -maxOffset, maxOffset), dz = std::clamp(Mth::floor(wz * dy), -maxOffset, maxOffset);
      return p.offset(dx, 0, dz);
    }
  };
  struct Drip {
    P root;
    bool up;
    int radius;
    double bluntness, scale;
    int heightAt(float rr) const { return (int)speleothem_height(rr, radius, scale, bluntness); }
    bool moveBack(Level& l, const Wind& w) {
      while (radius > 1) {
        P nr = root;
        int tries = std::min(10, heightAt(0.0F));
        for (int i = 0; i < tries; i++) {
          if (is(l.get(nr), blocks::lava)) return false;
          if (circle_embedded(l, w.offset(nr), radius)) { root = nr; return true; }
          nr = nr.rel(up ? bs::DOWN : bs::UP);
        }
        radius /= 2;
      }
      return false;
    }
    void placeBlocks(Level& l, RandomSource& r, const Wind& w) const {
      for (int dx = -radius; dx <= radius; dx++)
        for (int dz = -radius; dz <= radius; dz++) {
          float cr = (float)std::sqrt((double)(float)(dx * dx + dz * dz));
          if (cr > (float)radius) continue;
          int h = heightAt(cr);
          if (h <= 0) continue;
          if ((double)r.nextFloat() < 0.2) h = (int)((float)h * mth::randomBetween(r, 0.8F, 1.0F));
          P p = root.offset(dx, 0, dz);
          bool out = false;
          int maxY = up ? l.height(WORLD_SURFACE_WG, p.x, p.z) : INT32_MAX;
          for (int i = 0; i < h && p.y < maxY; i++) {
            P wp = w.offset(p);
            if (emptyOrWaterOrLava(l.get(wp))) out = true, l.set(wp, blocks::dripstone_block);
            else if (out && in(l.get(wp), "minecraft:base_stone_overworld")) break;
            p = p.rel(up ? bs::UP : bs::DOWN);
          }
        }
    }
  };
  bool place(Level& l, RandomSource& r, const P& o) const override {
    if (!emptyOrWater(l.get(o))) return false;
    static const int dripstoneBlock = R().block_of(blocks::dripstone_block);
    auto c = column_scan(l, o, searchRange, emptyOrWater, [&](int s) {
      return R().block_of(s) == dripstoneBlock || R().in(s, replaceable) || is(s, blocks::lava);
    });
    if (!c.found || !c.floor || !c.ceiling) return false;
    int colHeight = *c.ceiling - *c.floor - 1;
    if (colHeight < 4) return false;
    int maxByHeight = (int)((float)colHeight * maxRatio);
    int maxR = std::clamp(maxByHeight, columnRadius.minValue(), columnRadius.maxValue());
    int radius = mth::randomBetweenInclusive(r, columnRadius.minValue(), maxR);
    auto make = [&](P root, bool up, const FloatProvider& blunt) {
      double b = blunt.sample(r);
      double s = heightScale.sample(r);
      return Drip{root, up, radius, b, s};
    };
    Drip tite = make({o.x, *c.ceiling - 1, o.z}, false, titeBluntness);
    Drip mite = make({o.x, *c.floor + 1, o.z}, true, miteBluntness);
    Wind w;
    auto suitable = [&](const Drip& d) { return d.radius >= minRadiusForWind && d.bluntness >= minBluntnessForWind; };
    if (suitable(tite) && suitable(mite)) {
      w.on = true, w.originY = o.y, w.maxOffset = 16 - radius;
      float speed = windSpeed.sample(r);
      float dir = mth::randomBetween(r, 0.0F, (float)M_PI);
      w.wx = (double)(Mth::cos(dir) * speed), w.wz = (double)(Mth::sin(dir) * speed);
    }
    bool a = tite.moveBack(l, w);
    bool b = mite.moveBack(l, w);
    if (a) tite.placeBlocks(l, r, w);
    if (b) mite.placeBlocks(l, r, w);
    return true;
  }
};

// ---------------------------------------------------------------- geode
struct Geode : Feature {
  const StateProvider *filling, *inner, *alternateInner, *middle, *outer;
  std::vector<int> innerPlacements;
  bs::Tag cannotReplace, invalid;
  double filling_ = 1.7, innerLayer = 2.2, middleLayer = 3.2, outerLayer = 4.2;
  double generateCrackChance = 1.0, baseCrackSize = 2.0;
  int crackPointOffset = 2;
  double usePotential = 0.35, useAlternate = 0.0, noiseMultiplier = 0.05;
  bool requireAlternate = true;
  IntProvider outerWallDistance, distributionPoints, pointOffset;
  int minGen = -16, maxGen = 16, invalidThreshold = 0;
  std::shared_ptr<const Noise> noise;  // NormalNoise.createParity(-4, 1.0) over WorldgenRandom(Legacy(seed))

  void safeSet(Level& l, const P& p, int s) const {
    if (!R().in(l.get(p), cannotReplace)) l.set(p, s);
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    std::vector<std::pair<P, int>> points;
    int n = distributionPoints.sample(r);
    std::vector<P> crack;
    double adj = (double)n / outerWallDistance.maxValue();
    double innerAir = 1.0 / std::sqrt(filling_), innermost = 1.0 / std::sqrt(innerLayer + adj);
    double innerCrust = 1.0 / std::sqrt(middleLayer + adj), outerCrust = 1.0 / std::sqrt(outerLayer + adj);
    double crackSize = 1.0 / std::sqrt(baseCrackSize + r.nextDouble() / 2.0 + (n > 3 ? adj : 0.0));
    bool shouldCrack = (double)r.nextFloat() < generateCrackChance;
    int invalidCount = 0;
    for (int i = 0; i < n; i++) {
      int x = outerWallDistance.sample(r);
      int y = outerWallDistance.sample(r);
      int z = outerWallDistance.sample(r);
      P p = o.offset(x, y, z);
      int s = l.get(p);
      if ((R().is_air(s) || R().in(s, invalid)) && ++invalidCount > invalidThreshold) return false;
      points.push_back({p, pointOffset.sample(r)});
    }
    if (shouldCrack) {
      int idx = r.nextInt(4), co = n * 2 + 1;
      int cx = idx == 0 || idx == 2 ? co : 0, cz = idx == 1 || idx == 2 ? co : 0;
      for (int y : {7, 5, 1}) crack.push_back(o.offset(cx, y, cz));
    }
    std::vector<P> potential;
    for (int z = o.z + minGen; z <= o.z + maxGen; z++)
      for (int y = o.y + minGen; y <= o.y + maxGen; y++)
        for (int x = o.x + minGen; x <= o.x + maxGen; x++) {
          P p{x, y, z};
          double nOff = (double)noise->get(x, y, z) * noiseMultiplier;
          double shell = 0.0;
          for (auto& [q, off] : points) shell += 1.0 / std::sqrt(dist_sqr(p, q) + off) + nOff;
          if (shell < outerCrust) continue;
          if (shell >= innerAir) {
            safeSet(l, p, filling->get(l, r, p));
            continue;
          }
          double cs = 0.0;
          for (auto& q : crack) cs += 1.0 / std::sqrt(dist_sqr(p, q) + crackPointOffset) + nOff;
          if (shouldCrack && cs >= crackSize) {
            safeSet(l, p, blocks::air);
          } else if (shell >= innermost) {
            bool alt = (double)r.nextFloat() < useAlternate;
            safeSet(l, p, (alt ? alternateInner : inner)->get(l, r, p));
            if ((!requireAlternate || alt) && (double)r.nextFloat() < usePotential) potential.push_back(p);
          } else if (shell >= innerCrust) {
            safeSet(l, p, middle->get(l, r, p));
          } else {
            safeSet(l, p, outer->get(l, r, p));
          }
        }
    for (auto& cp : potential) {
      int s = innerPlacements[size_t(r.nextInt(int(innerPlacements.size())))];
      for (bs::Dir d : ALL_DIRS) {
        if (R().has(s, "facing")) s = R().with(s, "facing", dir_name(d));
        P pp = cp.rel(d);
        int ps = l.get(pp);
        if (R().has(s, "waterlogged")) s = R().with_bool(s, "waterlogged", R().is_source(ps));
        if (R().is_air(ps) || (is(ps, blocks::water) && R().fluid_amount(ps) == 8)) {  // canClusterGrowAtState
          safeSet(l, pp, s);
          break;
        }
      }
    }
    return true;
  }
};

// ---------------------------------------------------------------- monster room
int reorient(Level& l, const P& p, int state) {  // StructurePiece.reorient
  int solid = -1;
  for (bs::Dir d : HORIZONTAL) {
    int s = l.get(p.rel(d));
    if (is(s, blocks::chest)) return state;
    if (R().flags(s) & bs::SOLID_RENDER) {
      if (solid >= 0) { solid = -1; break; }
      solid = d;
    }
  }
  if (solid >= 0) return R().with(state, "facing", dir_name(bs::opposite(bs::Dir(solid))));
  bs::Dir lock = dir_by_name(std::string(R().value(state, "facing")));
  auto sr = [&](bs::Dir d) { return (R().flags(l.get(p.rel(d))) & bs::SOLID_RENDER) != 0; };
  if (sr(lock)) lock = bs::opposite(lock);
  if (sr(lock)) lock = clockwise(lock);
  if (sr(lock)) lock = bs::opposite(lock);
  return R().with(state, "facing", dir_name(lock));
}

struct MonsterRoom : Feature {
  bool place(Level& l, RandomSource& r, const P& o) const override {
    static const bs::Tag& noReplace = R().tag("minecraft:features_cannot_replace");
    auto safe = [&](const P& p, int s) {
      if (!R().in(l.get(p), noReplace)) l.set(p, s);
    };
    int xr = r.nextInt(2) + 2, minX = -xr - 1, maxX = xr + 1;
    int zr = r.nextInt(2) + 2, minZ = -zr - 1, maxZ = zr + 1;
    int holes = 0;
    for (int dx = minX; dx <= maxX; dx++)
      for (int dy = -1; dy <= 4; dy++)
        for (int dz = minZ; dz <= maxZ; dz++) {
          P p = o.offset(dx, dy, dz);
          bool solid = R().is_solid(l.get(p));
          if ((dy == -1 || dy == 4) && !solid) return false;
          if ((dx == minX || dx == maxX || dz == minZ || dz == maxZ) && dy == 0 && l.empty(p) && l.empty(p.above())) holes++;
        }
    if (holes < 1 || holes > 5) return false;
    for (int dx = minX; dx <= maxX; dx++)
      for (int dy = 3; dy >= -1; dy--)
        for (int dz = minZ; dz <= maxZ; dz++) {
          P p = o.offset(dx, dy, dz);
          int s = l.get(p);
          if (dx == minX || dy == -1 || dz == minZ || dx == maxX || dy == 4 || dz == maxZ) {
            if (p.y >= l.min_y() && !R().is_solid(l.get(p.below()))) l.set(p, blocks::cave_air);
            else if (R().is_solid(s) && !is(s, blocks::chest))
              safe(p, dy == -1 && r.nextInt(4) != 0 ? int(blocks::mossy_cobblestone) : int(blocks::cobblestone));
          } else if (!is(s, blocks::chest) && !is(s, blocks::spawner)) {
            safe(p, blocks::cave_air);
          }
        }
    for (int cc = 0; cc < 2; cc++)
      for (int i = 0; i < 3; i++) {
        int xc = o.x + r.nextInt(xr * 2 + 1) - xr;
        int zc = o.z + r.nextInt(zr * 2 + 1) - zr;
        P cp{xc, o.y, zc};
        if (!l.empty(cp)) continue;
        int walls = 0;
        for (bs::Dir d : HORIZONTAL) walls += R().is_solid(l.get(cp.rel(d)));
        if (walls != 1) continue;
        safe(cp, reorient(l, cp, blocks::chest));
        if (BlockEntity* be = l.block_entity(cp); be && R().is(l.get(cp), blocks::chest)) {  // setBlockEntityLootTable
          Nbt n = Nbt::compound();
          n.set("LootTable", Nbt::str("minecraft:chests/simple_dungeon"));
          n.set("LootTableSeed", Nbt::num(Nbt::Long, r.nextLong()));
          be->nbt = nbt_bytes(n);
        }
        break;
      }
    safe(o, blocks::spawner);
    if (BlockEntity* be = l.block_entity(o); be && R().is(l.get(o), blocks::spawner)) {
      static const char* mobs[4] = {"minecraft:skeleton", "minecraft:zombie", "minecraft:zombie", "minecraft:spider"};
      Nbt id = Nbt::compound(), entity = Nbt::compound(), n = Nbt::compound();
      id.set("id", Nbt::str(mobs[r.nextInt(4)]));
      entity.set("entity", id);
      n.set("SpawnData", entity);
      be->nbt = nbt_bytes(n);
    }
    return true;
  }
};
// ---------------------------------------------------------------- sculk patch (SculkSpreader, world-gen config)
struct SculkPatch : Feature {
  int chargeCount, amountPerCharge, spreadAttempts, growthRounds, spreadRounds;
  struct Cursor {
    P pos;
    int charge, updateDelay = 0, decayDelay = 1;
    bool hasFacings = false;
    uint8_t facings = 0;  // bit per Dir
  };
  enum Beh { DEFAULT_B, SCULK_B, VEIN_B };
  static Beh beh(int s) { return is(s, blocks::sculk) ? SCULK_B : is(s, blocks::sculk_vein) ? VEIN_B : DEFAULT_B; }
  static bool face(int s, bs::Dir d) { return R().bool_value(s, dir_name(d)); }
  static Spreader vein() { return {R().block_of(blocks::sculk_vein), true, false}; }
  static Spreader sameSpace() { return {R().block_of(blocks::sculk_vein), true, true}; }

  static void discharged(Level& l, int s, const P& p) {  // SculkVeinBlock.onDischarged
    if (!is(s, blocks::sculk_vein)) return;
    for (bs::Dir d : ALL_DIRS)
      if (face(s, d) && is(l.get(p.rel(d)), blocks::sculk)) s = R().with_bool(s, dir_name(d), false);
    bool any = false;
    for (bs::Dir d : ALL_DIRS) any = any || face(s, d);
    if (!any) s = R().has_fluid(l.get(p)) ? int(blocks::water) : int(blocks::air);
    l.set(p, s);
  }
  static bool spreadVein(Level& l, Beh b, const P& p, int s, const Cursor& c) {  // attemptSpreadVein
    if (b == DEFAULT_B) {
      if (!c.hasFacings) return multiface_spread_all(l, sameSpace(), l.get(p), p, true) > 0;
      if (c.facings) {
        if (!R().is_air(s) && R().fluid_type(s) != bs::WATER) return false;
        int ns = blocks::sculk_vein;  // SculkVeinBlock.regrow
        bool any = false;
        for (bs::Dir d : ALL_DIRS)
          if ((c.facings >> d & 1) && multiface_can_attach(l, d, p.rel(d))) ns = R().with_bool(ns, dir_name(d), true), any = true;
        if (!any) return false;
        if (R().has_fluid(s)) ns = R().with_bool(ns, "waterlogged", true);
        l.set(p, ns);
        return true;
      }
    }
    return multiface_spread_all(l, vein(), s, p, true) > 0;
  }
  int useCharge(Level& l, Beh b, Cursor& c, const P& origin, RandomSource& r, bool spreadVeins) const {
    static const bs::Tag& replace = R().tag("minecraft:sculk_replaceable_world_gen");
    if (b == DEFAULT_B) return c.decayDelay > 0 ? c.charge : 0;
    if (b == VEIN_B) {
      if (spreadVeins) {  // attemptPlaceSculk
        int s = l.get(c.pos);
        for (bs::Dir support : all_shuffled(r)) {
          if (!face(s, support)) continue;
          P sp = c.pos.rel(support);
          if (!R().in(l.get(sp), replace)) continue;
          l.set(sp, blocks::sculk);
          multiface_spread_all(l, vein(), blocks::sculk, sp, true);
          for (bs::Dir d : ALL_DIRS)
            if (d != bs::opposite(support) && is(l.get(sp.rel(d)), blocks::sculk_vein)) discharged(l, l.get(sp.rel(d)), sp.rel(d));
          return c.charge - 1;
        }
      }
      return r.nextInt(5) == 0 ? Mth::floor((float)c.charge * 0.5F) : c.charge;
    }
    int charge = c.charge;  // SculkBlock
    if (charge == 0 || r.nextInt(5) != 0) return charge;
    bool close = dist_sqr(c.pos, origin) < 1.0;
    bool growth = false;
    if (!close) {  // canPlaceGrowth
      int above = l.get(c.pos.above());
      if (R().is_air(above) || (is(above, blocks::water) && R().fluid_type(above) == bs::WATER)) {
        static const bs::Tag& inhib = R().tag("minecraft:sculk_growth_inhibitors");
        int n = 0;
        for (int x = -4; x <= 4 && n <= 2; x++)
          for (int y = 0; y <= 2 && n <= 2; y++)
            for (int z = -4; z <= 4 && n <= 2; z++) n += R().in(l.get(c.pos.offset(x, y, z)), inhib);
        growth = n <= 2;
      }
    }
    if (growth) {
      if (r.nextInt(50) < charge) {
        P g = c.pos.above();
        int st = r.nextInt(11) == 0 ? R().with_bool(blocks::sculk_shrieker, "can_summon", true) : int(blocks::sculk_sensor);
        if (R().has(st, "waterlogged") && R().has_fluid(l.get(g))) st = R().with_bool(st, "waterlogged", true);
        l.set(g, st);
      }
      return std::max(0, charge - 50);
    }
    if (r.nextInt(10) != 0) return charge;
    if (close) return charge - 1;
    float outer = Mth::square((float)std::sqrt(dist_sqr(c.pos, origin)) - 1.0F);
    float factor = std::min(1.0F, outer / (float)Mth::square(23.0F));
    return charge - std::max(1, (int)((float)charge * factor * 0.5F));
  }
  static bool unobstructed(Level& l, const P& from, bs::Dir d) { return !R().sturdy(l.get(from.rel(d)), bs::opposite(d)); }
  static bool movementOk(Level& l, const P& from, const P& to) {
    if (dist_manhattan(from, to) == 1) return true;
    int dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    bs::Dir X = dx < 0 ? bs::WEST : bs::EAST, Y = dy < 0 ? bs::DOWN : bs::UP, Z = dz < 0 ? bs::NORTH : bs::SOUTH;
    if (dx == 0) return unobstructed(l, from, Y) || unobstructed(l, from, Z);
    if (dy == 0) return unobstructed(l, from, X) || unobstructed(l, from, Z);
    return unobstructed(l, from, X) || unobstructed(l, from, Y);
  }
  static std::optional<P> movement(Level& l, const P& pos, RandomSource& r, const P& origin) {
    static const std::vector<P> offsets = [] {
      std::vector<P> v;
      for (int z = -1; z <= 1; z++)
        for (int y = -1; y <= 1; y++)
          for (int x = -1; x <= 1; x++)
            if ((x == 0 || y == 0 || z == 0) && !(x == 0 && y == 0 && z == 0)) v.push_back({x, y, z});
      return v;
    }();
    static const bs::Tag& replaceable = R().tag("minecraft:sculk_replaceable");
    auto offs = offsets;
    shuffle(offs, r);
    P best = pos;
    for (auto& o : offs) {
      P n = pos.offset(o.x, o.y, o.z);
      if ((origin.x - n.x) * (origin.x - n.x) + (origin.z - n.z) * (origin.z - n.z) > 144) continue;
      int t = l.get(n);
      if (beh(t) == DEFAULT_B || !movementOk(l, pos, n)) continue;
      best = n;
      if (is(t, blocks::sculk_vein)) {  // hasSubstrateAccess
        bool access = false;
        for (bs::Dir d : ALL_DIRS) access = access || (face(t, d) && R().in(l.get(n.rel(d)), replaceable));
        if (access) break;
      }
    }
    if (best == pos) return std::nullopt;
    return best;
  }
  void update(Level& l, Cursor& c, const P& origin, RandomSource& r, bool spreadVeins) const {
    if (c.charge <= 0) return;
    if (c.updateDelay > 0) { c.updateDelay--; return; }
    int s = l.get(c.pos);
    Beh b = beh(s);
    if (spreadVeins && spreadVein(l, b, c.pos, s, c) && b != SCULK_B) s = l.get(c.pos), b = beh(s);
    c.charge = useCharge(l, b, c, origin, r, spreadVeins);
    if (c.charge <= 0) {
      if (b == VEIN_B) discharged(l, s, c.pos);
      return;
    }
    auto t = movement(l, c.pos, r, origin);
    if (!t) {
      if (b == VEIN_B) discharged(l, s, c.pos);
      c.charge = 0;
      return;
    }
    if (b == VEIN_B) discharged(l, s, c.pos);
    c.pos = *t;
    s = l.get(*t);
    if (beh(s) != DEFAULT_B) {  // MultifaceBlock.availableFaces (empty for the sculk block)
      c.hasFacings = true, c.facings = 0;
      if (is(s, blocks::sculk_vein))
        for (bs::Dir d : ALL_DIRS) c.facings |= uint8_t(face(s, d) << d);
    }
    c.decayDelay = b == DEFAULT_B ? std::max(c.decayDelay - 1, 0) : 1;
    c.updateDelay = 1;
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    int st = l.get(o);
    if (beh(st) == DEFAULT_B) {  // canSpreadFrom
      if (!R().is_air(st) && !(is(st, blocks::water) && R().is_source(st))) return false;
      bool any = false;
      for (bs::Dir d : ALL_DIRS) any = any || (R().flags(l.get(o.rel(d))) & bs::FULL_COLLISION);
      if (!any) return false;
    }
    std::vector<Cursor> cursors;
    for (int round = 0; round < spreadRounds + growthRounds; round++) {
      for (int i = 0; i < chargeCount; i++)
        for (int charge = amountPerCharge; charge > 0;) {
          int cc = std::min(charge, 1000);
          if (cursors.size() < 32) cursors.push_back({o, cc});
          charge -= cc;
        }
      bool spreadVeins = round < spreadRounds;
      for (int i = 0; i < spreadAttempts && !cursors.empty(); i++) {
        std::vector<Cursor> processed;
        std::map<P, size_t> merge;  // pos -> index in processed of the mergeable cursor
        for (auto& c : cursors) {
          if (std::max({std::abs(c.pos.x - o.x), std::abs(c.pos.y - o.y), std::abs(c.pos.z - o.z)}) > 1024) continue;
          update(l, c, o, r, spreadVeins);
          if (c.charge <= 0) continue;
          auto it = merge.find(c.pos);
          processed.push_back(c);
          if (it == merge.end()) merge[c.pos] = processed.size() - 1;
          else if (c.charge < processed[it->second].charge) it->second = processed.size() - 1;
        }
        cursors = std::move(processed);
      }
      cursors.clear();
    }
    return true;
  }
};
}  // namespace

void register_cave_features(ParserMap& m) {
  for (const char* t : {"vegetation_patch", "waterlogged_vegetation_patch"})
    m[t] = [t](Loader& L, const Json& j) {
      auto* f = L.make<VegetationPatch>();
      f->waterlogged = std::string(t) == "waterlogged_vegetation_patch";
      f->replaceable = L.block_set(j["replaceable"]);
      f->ground = L.provider(j["ground_state"]);
      f->vegetation = L.placed(j["vegetation_feature"]);
      f->inwards = j["surface"].asString() == "ceiling" ? bs::UP : bs::DOWN;
      f->depth = IntProvider(j["depth"]);
      f->extraBottom = j["extra_bottom_block_chance"].asFloat();
      f->verticalRange = j["vertical_range"].asInt();
      f->vegetationChance = j["vegetation_chance"].asFloat();
      f->xzRadius = IntProvider(j["xz_radius"]);
      f->extraEdge = j["extra_edge_column_chance"].asFloat();
      return f;
    };
  m["root_system"] = [](Loader& L, const Json& j) {
    auto* f = L.make<RootSystem>();
    f->tree = L.placed(j["feature"]);
    f->requiredSpace = j["required_vertical_space_for_tree"].asInt(), f->levelTestDistance = j["level_test_distance"].asInt();
    f->maxLevelDeviation = j["max_level_deviation"].asInt(), f->rootRadius = j["root_radius"].asInt();
    f->rootReplaceable = L.block_set(j["root_replaceable"]), f->rootProvider = L.provider(j["root_state_provider"]);
    f->rootAttempts = j["root_placement_attempts"].asInt(), f->rootColumnMaxHeight = j["root_column_max_height"].asInt();
    f->hangingRadius = j["hanging_root_radius"].asInt(), f->hangingSpan = j["hanging_roots_vertical_span"].asInt();
    f->hangingProvider = L.provider(j["hanging_root_state_provider"]);
    f->hangingAttempts = j["hanging_root_placement_attempts"].asInt();
    f->allowedWater = j["allowed_vertical_water_for_tree"].asInt();
    f->allowedTreePos = L.predicate(j["allowed_tree_position"]);
    return f;
  };
  auto speleo = [](Loader& L, const Json& j, Speleo& s) {
    s.baseBlock = R().block_of(L.state(j["base_block"]));
    s.pointedBlock = R().block_of(L.state(j["pointed_block"]));
    s.replaceable = L.block_set(j["replaceable_blocks"]);
  };
  m["speleothem"] = [speleo](Loader& L, const Json& j) {
    auto* f = L.make<SpeleothemF>();
    speleo(L, j, *f);
    auto fl = [&](const char* k, float d) { return j.has(k) ? j[k].asFloat() : d; };
    f->taller = fl("chance_of_taller_generation", 0.2F), f->directional = fl("chance_of_directional_spread", 0.7F);
    f->spread2 = fl("chance_of_spread_radius2", 0.5F), f->spread3 = fl("chance_of_spread_radius3", 0.5F);
    return f;
  };
  m["speleothem_cluster"] = [speleo](Loader& L, const Json& j) {
    auto* f = L.make<SpeleothemCluster>();
    speleo(L, j, *f);
    f->searchRange = j["floor_to_ceiling_search_range"].asInt();
    f->height = IntProvider(j["height"]), f->radius = IntProvider(j["radius"]);
    f->maxDiff = j["max_stalagmite_stalactite_height_diff"].asInt(), f->heightDeviation = j["height_deviation"].asInt();
    f->thickness = IntProvider(j["speleothem_block_layer_thickness"]);
    f->density = FloatProvider(j["density"]), f->wetness = FloatProvider(j["wetness"]);
    f->chanceAtMax = j["chance_of_speleothem_at_max_distance_from_center"].asFloat();
    f->maxEdge = j["max_distance_from_edge_affecting_chance_of_speleothem"].asInt();
    f->maxCenter = j["max_distance_from_center_affecting_height_bias"].asInt();
    return f;
  };
  m["large_dripstone"] = [](Loader& L, const Json& j) {
    auto* f = L.make<LargeDripstone>();
    f->replaceable = L.block_set(j["replaceable_blocks"]);
    f->searchRange = j.getInt("floor_to_ceiling_search_range", 30);
    f->columnRadius = IntProvider(j["column_radius"]), f->heightScale = FloatProvider(j["height_scale"]);
    f->maxRatio = j["max_column_radius_to_cave_height_ratio"].asFloat();
    f->titeBluntness = FloatProvider(j["stalactite_bluntness"]), f->miteBluntness = FloatProvider(j["stalagmite_bluntness"]);
    f->windSpeed = FloatProvider(j["wind_speed"]), f->minRadiusForWind = j["min_radius_for_wind"].asInt();
    f->minBluntnessForWind = j["min_bluntness_for_wind"].asFloat();
    return f;
  };
  m["geode"] = [](Loader& L, const Json& j) {
    auto* f = L.make<Geode>();
    const Json &b = j["blocks"], &ly = j["layers"], &cr = j["crack"];
    f->filling = L.provider(b["filling_provider"]), f->inner = L.provider(b["inner_layer_provider"]);
    f->alternateInner = L.provider(b["alternate_inner_layer_provider"]), f->middle = L.provider(b["middle_layer_provider"]);
    f->outer = L.provider(b["outer_layer_provider"]);
    for (auto& s : b["inner_placements"].arr) f->innerPlacements.push_back(L.state(s));
    f->cannotReplace = L.block_set(b["cannot_replace"]), f->invalid = L.block_set(b["invalid_blocks"]);
    f->filling_ = ly.getDouble("filling", 1.7), f->innerLayer = ly.getDouble("inner_layer", 2.2);
    f->middleLayer = ly.getDouble("middle_layer", 3.2), f->outerLayer = ly.getDouble("outer_layer", 4.2);
    f->generateCrackChance = cr.getDouble("generate_crack_chance", 1.0), f->baseCrackSize = cr.getDouble("base_crack_size", 2.0);
    f->crackPointOffset = cr.getInt("crack_point_offset", 2);
    f->usePotential = j.getDouble("use_potential_placements_chance", 0.35), f->useAlternate = j.getDouble("use_alternate_layer0_chance", 0.0);
    f->requireAlternate = !j.has("placements_require_layer0_alternate") || j["placements_require_layer0_alternate"].asBool();
    auto uni = [](int a, int b) {
      IntProvider p;
      p.t = IntProvider::UNIFORM, p.a = a, p.b = b;
      return p;
    };
    f->outerWallDistance = j.has("outer_wall_distance") ? IntProvider(j["outer_wall_distance"]) : uni(4, 5);
    f->distributionPoints = j.has("distribution_points") ? IntProvider(j["distribution_points"]) : uni(3, 4);
    f->pointOffset = j.has("point_offset") ? IntProvider(j["point_offset"]) : uni(1, 2);
    f->minGen = j.getInt("min_gen_offset", -16), f->maxGen = j.getInt("max_gen_offset", 16);
    f->noiseMultiplier = j.getDouble("noise_multiplier", 0.05), f->invalidThreshold = j["invalid_blocks_threshold"].asInt();
    {  // NormalNoise.createParity(-4, 1.0): base amplitude = old parity normalization / new normalization
      double nf = 1.0 * 0.3333333333333333 / (std::sqrt(Mth::square(PerlinNoise::STANDARD_DEVIATION * 1.0)) * std::sqrt(2.0));
      double old = 1.0 * 0.5 * 0.3333333333333333 / (0.1 * (1.0 + 1.0 / (0 + 1)));
      NormalNoise::Parameters p;
      p.baseOctave = -4, p.octaveCount = 1, p.baseAmplitude = old / nf;
      WorldgenRandom wr(std::make_unique<LegacyRandomSource>(L.f ? L.f->ow.seed : 0));
      f->noise = NormalNoise(p).create(wr);
    }
    return f;
  };
  m["monster_room"] = [](Loader& L, const Json&) { return L.make<MonsterRoom>(); };
  m["sculk_patch"] = [](Loader& L, const Json& j) {
    auto* f = L.make<SculkPatch>();
    f->chargeCount = j["charge_count"].asInt(), f->amountPerCharge = j["amount_per_charge"].asInt();
    f->spreadAttempts = j["spread_attempts"].asInt(), f->growthRounds = j["growth_rounds"].asInt();
    f->spreadRounds = j["spread_rounds"].asInt();
    return f;
  };
}

}  // namespace worldgen::feat
