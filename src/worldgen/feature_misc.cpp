// Feature types: huge_brown_mushroom, huge_red_mushroom, coral_tree, coral_claw, spike (ice spikes), iceberg.
// Ports of levelgen/feature (26.3).
#include <cmath>

#include "worldgen/features.hpp"

namespace worldgen::feat {
namespace {
const bs::Registry& R() { return bs::R(); }
bool is(int s, int def) { return R().is(s, def); }

struct HugeMushroom : Feature {
  const StateProvider *cap, *stem;
  int foliageRadius;
  const BlockPredicate* canPlaceOn;
  bool red;
  void put(Level& l, const P& p, int s) const {
    int cur = l.get(p);
    if (R().is_air(cur) || R().in(cur, R().tag("minecraft:replaceable_by_mushrooms"))) l.set(p, s);
  }
  int radiusAt(int treeHeight, int yo) const {
    if (!red) return yo <= 3 ? 0 : foliageRadius;
    return (yo < treeHeight && yo >= treeHeight - 3) || yo == treeHeight ? foliageRadius : 0;
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    int h = r.nextInt(3) + 4;
    if (r.nextInt(12) == 0) h *= 2;
    if (o.y < l.min_y() + 1 || o.y + h + 1 > l.max_y()) return false;  // isValidPosition
    if (!canPlaceOn->test(l, o.below())) return false;
    for (int dy = 0; dy <= h; dy++) {
      int rad = radiusAt(-1, dy);  // getTreeRadiusForHeight(-1, -1, radius, dy)
      for (int dx = -rad; dx <= rad; dx++)
        for (int dz = -rad; dz <= rad; dz++) {
          int s = l.get(o.offset(dx, dy, dz));
          if (!R().is_air(s) && !R().in(s, R().tag("minecraft:leaves"))) return false;
        }
    }
    auto faces = [](int s, std::initializer_list<std::pair<const char*, bool>> f) {
      for (auto& [k, v] : f)
        if (!R().has(s, k)) return s;
      for (auto& [k, v] : f) s = R().with_bool(s, k, v);
      return s;
    };
    if (!red) {
      int fr = foliageRadius;
      for (int dx = -fr; dx <= fr; dx++)
        for (int dz = -fr; dz <= fr; dz++) {
          bool minX = dx == -fr, maxX = dx == fr, minZ = dz == -fr, maxZ = dz == fr, xe = minX || maxX, ze = minZ || maxZ;
          if (xe && ze) continue;
          P p = o.offset(dx, h, dz);
          bool w = minX || (ze && dx == 1 - fr), e = maxX || (ze && dx == fr - 1);
          bool n = minZ || (xe && dz == 1 - fr), so = maxZ || (xe && dz == fr - 1);
          put(l, p, faces(cap->get(l, r, o), {{"west", w}, {"east", e}, {"north", n}, {"south", so}}));
        }
    } else {
      for (int dy = h - 3; dy <= h; dy++) {
        int rad = dy < h ? foliageRadius : foliageRadius - 1, center = foliageRadius - 2;
        for (int dx = -rad; dx <= rad; dx++)
          for (int dz = -rad; dz <= rad; dz++) {
            bool xe = dx == -rad || dx == rad, ze = dz == -rad || dz == rad;
            if (!(dy >= h || xe != ze)) continue;
            P p = o.offset(dx, dy, dz);
            int s = cap->get(l, r, o);
            if (R().has(s, "up"))
              s = faces(s, {{"up", dy >= h - 1}, {"west", dx < -center}, {"east", dx > center}, {"north", dz < -center}, {"south", dz > center}});
            put(l, p, s);
          }
      }
    }
    for (int dy = 0; dy < h; dy++) put(l, o.above(dy), stem->get(l, r, o));
    return true;
  }
};

struct CoralTree : Feature {
  const Placed* f;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    P p = o;
    int trunk = r.nextInt(3) + 1;
    for (int i = 0; i < trunk; i++) {
      if (!f->place(l, r, p)) return true;
      p = p.above();
    }
    P top = p;
    int n = r.nextInt(3) + 2;
    std::vector<bs::Dir> dirs(HORIZONTAL, HORIZONTAL + 4);
    shuffle(dirs, r);
    for (int b = 0; b < n; b++) {
      bs::Dir d = dirs[size_t(b)];
      p = top.rel(d);
      int bh = r.nextInt(5) + 2, seg = 0;
      for (int j = 0; j < bh && f->place(l, r, p); j++) {
        seg++;
        p = p.above();
        if (j == 0 || (seg >= 2 && r.nextFloat() < 0.25F)) p = p.rel(d), seg = 0;
      }
    }
    return true;
  }
};
struct CoralClaw : Feature {
  const Placed* f;
  bool place(Level& l, RandomSource& r, const P& o) const override {
    if (!f->place(l, r, o)) return false;
    bs::Dir claw = random_horizontal(r);
    int n = r.nextInt(2) + 2;
    std::vector<bs::Dir> dirs{claw, clockwise(claw), counter_clockwise(claw)};
    shuffle(dirs, r);
    for (int b = 0; b < n; b++) {
      bs::Dir bd = dirs[size_t(b)];
      P p = o;
      int side = r.nextInt(2) + 1;
      p = p.rel(bd);
      int inway;
      bs::Dir seg;
      if (bd == claw) {
        seg = claw;
        inway = r.nextInt(3) + 2;
      } else {
        p = p.above();
        bs::Dir opts[2] = {bd, bs::UP};
        seg = opts[r.nextInt(2)];
        inway = r.nextInt(3) + 3;
      }
      for (int i = 0; i < side && f->place(l, r, p); i++) p = p.rel(seg);
      p = p.rel(bs::opposite(seg)).above();
      for (int i = 0; i < inway; i++) {
        p = p.rel(claw);
        if (!f->place(l, r, p)) break;
        if (r.nextFloat() < 0.25F) p = p.above();
      }
    }
    return true;
  }
};

struct Spike : Feature {
  int state;
  const BlockPredicate *canPlaceOn, *canReplace;
  bool place(Level& l, RandomSource& r, const P& origin) const override {
    P o = origin;
    while (l.empty(o) && o.y > l.min_y() + 2) o = o.below();
    if (!canPlaceOn->test(l, o)) return false;
    o = o.above(r.nextInt(4));
    int height = r.nextInt(4) + 7;
    int width = height / 4 + r.nextInt(2);
    if (width > 1 && r.nextInt(60) == 0) o = o.above(10 + r.nextInt(30));
    for (int y = 0; y < height; y++) {
      float scale = (1.0F - (float)y / (float)height) * (float)width;
      int nw = ceil_i(scale);
      for (int xo = -nw; xo <= nw; xo++) {
        float dx = (float)std::abs(xo) - 0.25F;
        for (int zo = -nw; zo <= nw; zo++) {
          float dz = (float)std::abs(zo) - 0.25F;
          if (((xo == 0 && zo == 0) || !(dx * dx + dz * dz > scale * scale)) &&
              ((xo != -nw && xo != nw && zo != -nw && zo != nw) || !(r.nextFloat() > 0.75F))) {
            P pp = o.offset(xo, y, zo);
            if (R().is_air(l.get(pp)) || canReplace->test(l, pp)) l.set(pp, state);
            if (y != 0 && nw > 1) {
              P np = o.offset(xo, -y, zo);
              if (R().is_air(l.get(np)) || canReplace->test(l, np)) l.set(np, state);
            }
          }
        }
      }
    }
    int pw = std::clamp(width - 1, 0, 1);
    for (int xo = -pw; xo <= pw; xo++)
      for (int zo = -pw; zo <= pw; zo++) {
        P c = o.offset(xo, -1, zo);
        int run = 50;
        if (std::abs(xo) == 1 && std::abs(zo) == 1) run = r.nextInt(5);
        while (c.y > 50) {
          int s = l.get(c);
          if (!R().is_air(s) && !canReplace->test(l, c) && s != state) break;
          l.set(c, state);
          c = c.below();
          if (--run <= 0) {
            c = c.below(r.nextInt(5) + 1);
            run = r.nextInt(5);
          }
        }
      }
    return true;
  }
};

struct Iceberg : Feature {
  int state;
  static bool iceberg(int s) { return is(s, blocks::packed_ice) || is(s, blocks::snow_block) || is(s, blocks::blue_ice); }
  static double ellipse(int xo, int zo, const P& o, int a, int c, double angle) {
    double x = xo - o.x, z = zo - o.z;
    return std::pow((x * std::cos(angle) - z * std::sin(angle)) / a, 2.0) + std::pow((x * std::sin(angle) + z * std::cos(angle)) / c, 2.0) - 1.0;
  }
  static double circle(int xo, int zo, int radius, RandomSource& r) {
    float off = 10.0F * Mth::clamp(r.nextFloat(), 0.2F, 0.8F) / (float)radius;
    return off + std::pow((double)xo, 2.0) + std::pow((double)zo, 2.0) - std::pow((double)radius, 2.0);
  }
  static int radiusRound(RandomSource& r, int yOff, int height, int width) {
    float k = 3.5F - r.nextFloat();
    float scale = (1.0F - (float)std::pow((double)yOff, 2.0) / ((float)height * k)) * (float)width;
    if (height > 15 + r.nextInt(5)) {
      int t = yOff < 3 + r.nextInt(6) ? yOff / 2 : yOff;
      scale = (1.0F - (float)t / ((float)height * k * 0.4F)) * (float)width;
    }
    return ceil_i(scale / 2.0F);
  }
  static int radiusEllipse(int yOff, int height, int width) {
    float scale = (1.0F - (float)std::pow((double)yOff, 2.0) / ((float)height * 1.0F)) * (float)width;
    return ceil_i(scale / 2.0F);
  }
  static int radiusSteep(RandomSource& r, int yOff, int height, int width) {
    float k = 1.0F + r.nextFloat() / 2.0F;
    float scale = (1.0F - (float)yOff / ((float)height * k)) * (float)width;
    return ceil_i(scale / 2.0F);
  }
  void setBlock(Level& l, RandomSource& r, const P& p, int hDiff, int height, bool isEllipse, bool snow) const {
    int s = l.get(p);
    if (!(R().is_air(s) || is(s, blocks::snow_block) || is(s, blocks::ice) || is(s, blocks::water))) return;
    bool randomness = !isEllipse || r.nextDouble() > 0.05;
    int div = isEllipse ? 3 : 2;
    if (snow && !is(s, blocks::water)) {
      int a = r.nextInt(std::max(1, height / div));
      if ((double)hDiff <= a + height * 0.6 && randomness) {
        l.set(p, blocks::snow_block);
        return;
      }
    }
    l.set(p, state);
  }
  void block(Level& l, RandomSource& r, const P& o, int height, int xo, int yOff, int zo, int radius, int a, bool isEllipse, int c,
             double angle, bool snow) const {
    double sd;
    if (isEllipse) {
      int cc = c;
      if (yOff > 0 && height - yOff <= 3) cc = c - (4 - (height - yOff));
      sd = ellipse(xo, zo, {}, a, cc, angle);
    } else {
      sd = circle(xo, zo, radius, r);
    }
    if (!(sd < 0.0)) return;
    P p = o.offset(xo, yOff, zo);
    double cmp = isEllipse ? -0.5 : -6 - r.nextInt(3);
    if (sd > cmp && r.nextDouble() > 0.9) return;
    setBlock(l, r, p, height - yOff, height, isEllipse, snow);
  }
  void carve(Level& l, int radius, int yOff, const P& g, bool under, double angle, const P& lo, int ea, int ec) const {
    int a = radius + 1 + ea / 3, c = std::min(radius - 3, 3) + ec / 2 - 1;
    for (int xo = -a; xo < a; xo++)
      for (int zo = -a; zo < a; zo++) {
        if (!(ellipse(xo, zo, lo, a, c, angle) < 0.0)) continue;
        P p = g.offset(xo, yOff, zo);
        int s = l.get(p);
        if (!(iceberg(s) || is(s, blocks::snow_block))) continue;
        if (under) {
          l.set(p, blocks::water);
        } else {
          l.set(p, blocks::air);
          if (is(l.get(p.above()), blocks::snow)) l.set(p.above(), blocks::air);
        }
      }
  }
  bool place(Level& l, RandomSource& r, const P& origin) const override {
    P o{origin.x, l.sea_level(), origin.z};
    bool snow = r.nextDouble() > 0.7;
    double angle = r.nextDouble() * 2.0 * M_PI;
    int ea = 11 - r.nextInt(5);
    int ec = 3 + r.nextInt(3);
    bool isEllipse = r.nextDouble() > 0.7;
    int over = isEllipse ? r.nextInt(6) + 6 : r.nextInt(15) + 3;
    if (!isEllipse && r.nextDouble() > 0.9) over += r.nextInt(19) + 7;
    int under = std::min(over + r.nextInt(11), 18);
    int w1 = r.nextInt(7);
    int width = std::min(over + w1 - r.nextInt(5), 11);
    int a = isEllipse ? ea : 11;
    for (int xo = -a; xo < a; xo++)
      for (int zo = -a; zo < a; zo++)
        for (int y = 0; y < over; y++) {
          int radius = isEllipse ? radiusEllipse(y, over, width) : radiusRound(r, y, over, width);
          if (isEllipse || xo < radius) block(l, r, o, over, xo, y, zo, radius, a, isEllipse, ec, angle, snow);
        }
    {  // smooth
      int sa = isEllipse ? ea : width / 2;
      for (int x = -sa; x <= sa; x++)
        for (int z = -sa; z <= sa; z++)
          for (int y = 0; y <= over; y++) {
            P p = o.offset(x, y, z);
            int s = l.get(p);
            if (!(iceberg(s) || is(s, blocks::snow))) continue;
            if (R().is_air(l.get(p.below()))) {
              l.set(p, blocks::air);
              l.set(p.above(), blocks::air);
            } else if (iceberg(s)) {
              int cnt = 0;
              for (P q : {p.offset(-1, 0, 0), p.offset(1, 0, 0), p.offset(0, 0, -1), p.offset(0, 0, 1)}) cnt += !iceberg(l.get(q));
              if (cnt >= 3) l.set(p, blocks::air);
            }
          }
    }
    for (int xo = -a; xo < a; xo++)
      for (int zo = -a; zo < a; zo++)
        for (int y = -1; y > -under; y--) {
          int na = isEllipse ? ceil_i((double)((float)a * (1.0F - (float)std::pow((double)y, 2.0) / ((float)under * 8.0F)))) : a;
          int radius = radiusSteep(r, -y, under, width);
          if (xo < radius) block(l, r, o, under, xo, y, zo, radius, na, isEllipse, ec, angle, snow);
        }
    bool cut = isEllipse ? r.nextDouble() > 0.1 : r.nextDouble() > 0.7;
    if (cut) {  // generateCutOut
      int sx = r.nextBoolean() ? -1 : 1;
      int sz = r.nextBoolean() ? -1 : 1;
      int xOff = r.nextInt(std::max(width / 2 - 2, 1));
      if (r.nextBoolean()) xOff = width / 2 + 1 - r.nextInt(std::max(width - width / 2 - 1, 1));
      int zOff = r.nextInt(std::max(width / 2 - 2, 1));
      if (r.nextBoolean()) zOff = width / 2 + 1 - r.nextInt(std::max(width - width / 2 - 1, 1));
      if (isEllipse) xOff = zOff = r.nextInt(std::max(ea - 5, 1));
      P lo{sx * xOff, 0, sz * zOff};
      double ang = isEllipse ? angle + M_PI / 2 : r.nextDouble() * 2.0 * M_PI;
      for (int y = 0; y < over - 3; y++) carve(l, radiusRound(r, y, over, width), y, o, false, ang, lo, ea, ec);
      for (int y = -1; y > -over + r.nextInt(5); y--) carve(l, radiusSteep(r, -y, over, width), y, o, true, ang, lo, ea, ec);
    }
    return true;
  }
};
}  // namespace

void register_misc_features(ParserMap& m) {
  for (const char* t : {"huge_brown_mushroom", "huge_red_mushroom"})
    m[t] = [t](Loader& L, const Json& j) {
      auto* f = L.make<HugeMushroom>();
      f->red = std::string(t) == "huge_red_mushroom";
      f->cap = L.provider(j["cap_provider"]), f->stem = L.provider(j["stem_provider"]);
      f->foliageRadius = j.getInt("foliage_radius", 2);
      f->canPlaceOn = L.predicate(j["can_place_on"]);
      return f;
    };
  m["coral_tree"] = [](Loader& L, const Json& j) { auto* f = L.make<CoralTree>(); f->f = L.placed(j["feature"]); return f; };
  m["coral_claw"] = [](Loader& L, const Json& j) { auto* f = L.make<CoralClaw>(); f->f = L.placed(j["feature"]); return f; };
  m["spike"] = [](Loader& L, const Json& j) {
    auto* f = L.make<Spike>();
    f->state = L.state(j["state"]), f->canPlaceOn = L.predicate(j["can_place_on"]), f->canReplace = L.predicate(j["can_replace"]);
    return f;
  };
  m["iceberg"] = [](Loader& L, const Json& j) { auto* f = L.make<Iceberg>(); f->state = L.state(j["state"]); return f; };
}

}  // namespace worldgen::feat
