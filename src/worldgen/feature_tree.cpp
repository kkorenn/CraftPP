// Trees (26.3 TreeFeature, every trunk / foliage / root placer, feature sizes, tree decorators) and
// FallenTreeFeature. Position sets are java.util.HashSet emulations: their iteration order drives the leaf
// distance flood fill and the decorators' random calls.
#include <algorithm>
#include <cmath>
#include <optional>
#include <set>

#include "commands/nbt.hpp"
#include "worldgen/features.hpp"
#include "worldgen/java_hash.hpp"

namespace worldgen::feat {
namespace {
const bs::Registry& R() { return bs::R(); }
bool is(int s, int def) { return R().is(s, def); }
bool in(int s, const char* tag) { return R().in(s, R().tag(tag)); }

const char* axis_of(bs::Dir d) { return d <= bs::UP ? "y" : d <= bs::SOUTH ? "z" : "x"; }
int with_axis(int s, const char* axis) {  // trySetValue(AXIS, ...)
  int t = R().with(s, "axis", axis);
  return t >= 0 ? t : s;
}
bool valid_tree_pos(Level& l, const P& p) {  // TreeFeature.validTreePos
  int s = l.get(p);
  return R().is_air(s) || in(s, "minecraft:replaceable_by_trees");
}

struct Attachment {
  P pos;
  int radiusOffset = 0, heightOffset = 0, sizeX = 1, sizeZ = 1;
  Attachment(P p, int r, bool doubleTrunk) : pos(p), radiusOffset(r), sizeX(doubleTrunk ? 2 : 1), sizeZ(doubleTrunk ? 2 : 1) {}
  bool doubleTrunk() const { return sizeX == 2 && sizeZ == 2; }
};

struct Tree;
struct TreeCtx {
  Level& l;
  const Tree& tree;
  PosSet roots, trunks, foliage, decorations;
  void root(const P& p, int s) { roots.insert(p), l.set(p, s); }
  void trunk(const P& p, int s) { trunks.insert(p), l.set(p, s); }
  void leaf(const P& p, int s) { foliage.insert(p), l.set(p, s); }
  bool leafSet(const P& p) const { return foliage.contains(p); }
};

// TreeDecorator.Context
struct DecoCtx {
  Level& l;
  RandomSource& r;
  std::vector<P> logs, leaves, roots;
  std::function<void(const P&, int)> set;
  DecoCtx(Level& l, RandomSource& r, const PosSet& t, const PosSet& f, const PosSet& ro, std::function<void(const P&, int)> set)
      : l(l), r(r), logs(t.items()), leaves(f.items()), roots(ro.items()), set(std::move(set)) {
    auto byY = [](const P& a, const P& b) { return a.y < b.y; };
    std::stable_sort(logs.begin(), logs.end(), byY);
    std::stable_sort(leaves.begin(), leaves.end(), byY);
    std::stable_sort(roots.begin(), roots.end(), byY);
  }
  bool air(const P& p) const { return R().is_air(l.get(p)); }
  void vine(const P& p, bs::Dir face) { set(p, R().with_bool(blocks::vine, dir_name(face), true)); }
  bool waterNearby(const P& p) const {
    for (P q : {p, p.offset(1, 0, 0), p.offset(-1, 0, 0), p.offset(0, 0, -1), p.offset(0, 0, 1)})
      if (is(l.get(q), blocks::water)) return true;
    return false;
  }
  std::vector<P> lowestTrunkOrRoot() const {  // TreeFeature.getLowestTrunkOrRootOfTree
    std::vector<P> v;
    if (roots.empty()) v = logs;
    else if (!logs.empty() && roots[0].y == logs[0].y) v = logs, v.insert(v.end(), roots.begin(), roots.end());
    else v = roots;
    return v;
  }
};

// ---------------------------------------------------------------- decorators
struct Decorator {
  virtual ~Decorator() = default;
  virtual void place(DecoCtx& c) const = 0;
};
struct TrunkVine : Decorator {
  void place(DecoCtx& c) const override {
    for (auto& p : c.logs) {
      if (c.r.nextInt(3) > 0 && c.air(p.offset(-1, 0, 0))) c.vine(p.offset(-1, 0, 0), bs::EAST);
      if (c.r.nextInt(3) > 0 && c.air(p.offset(1, 0, 0))) c.vine(p.offset(1, 0, 0), bs::WEST);
      if (c.r.nextInt(3) > 0 && c.air(p.offset(0, 0, -1))) c.vine(p.offset(0, 0, -1), bs::SOUTH);
      if (c.r.nextInt(3) > 0 && c.air(p.offset(0, 0, 1))) c.vine(p.offset(0, 0, 1), bs::NORTH);
    }
  }
};
struct LeaveVine : Decorator {
  float probability;
  static void hanging(DecoCtx& c, P p, bs::Dir face) {
    c.vine(p, face);
    int max = 4;
    for (p = p.below(); c.air(p) && max > 0; max--) c.vine(p, face), p = p.below();
  }
  void place(DecoCtx& c) const override {
    for (auto& p : c.leaves) {
      if (c.r.nextFloat() < probability && c.air(p.offset(-1, 0, 0))) hanging(c, p.offset(-1, 0, 0), bs::EAST);
      if (c.r.nextFloat() < probability && c.air(p.offset(1, 0, 0))) hanging(c, p.offset(1, 0, 0), bs::WEST);
      if (c.r.nextFloat() < probability && c.air(p.offset(0, 0, -1))) hanging(c, p.offset(0, 0, -1), bs::SOUTH);
      if (c.r.nextFloat() < probability && c.air(p.offset(0, 0, 1))) hanging(c, p.offset(0, 0, 1), bs::NORTH);
    }
  }
};
struct Cocoa : Decorator {
  float probability;
  void place(DecoCtx& c) const override {
    if (c.r.nextFloat() >= probability || c.logs.empty()) return;
    int treeY = c.logs[0].y;
    for (auto& p : c.logs) {
      if (p.y - treeY > 2) continue;
      for (bs::Dir d : HORIZONTAL)
        if (c.r.nextFloat() <= 0.25F) {
          bs::Dir o = bs::opposite(d);
          P q = p.offset(bs::DX[o], 0, bs::DZ[o]);
          if (c.air(q)) {
            int age = c.r.nextInt(3);
            c.set(q, R().with(R().with_int(blocks::cocoa, "age", age), "facing", dir_name(d)));
          }
        }
    }
  }
};
struct Beehive : Decorator {
  float probability;
  void place(DecoCtx& c) const override {
    if (c.logs.empty() || c.r.nextFloat() >= probability) return;
    int hiveY = !c.leaves.empty() ? std::max(c.leaves.front().y - 1, c.logs.front().y + 1)
                                  : std::min(c.logs.front().y + 1 + c.r.nextInt(3), c.logs.back().y);
    static const bs::Dir spawn[3] = {bs::EAST, bs::SOUTH, bs::WEST};  // HORIZONTAL without NORTH (= WORLDGEN_FACING.getOpposite())
    std::vector<P> places;
    for (auto& p : c.logs)
      if (p.y == hiveY)
        for (bs::Dir d : spawn) places.push_back(p.rel(d));
    if (places.empty()) return;
    shuffle(places, c.r);
    for (auto& p : places)
      if (c.air(p) && c.air(p.rel(bs::SOUTH))) {
        c.set(p, R().with(blocks::bee_nest, "facing", "south"));
        if (BlockEntity* be = c.l.block_entity(p)) {
          int n = 2 + c.r.nextInt(2);
          Nbt bees = Nbt::make_list();
          for (int i = 0; i < n; i++) {  // BeehiveBlockEntity.Occupant.create(ticksInHive)
            Nbt o = Nbt::compound(), e = Nbt::compound();
            e.set("id", Nbt::str("minecraft:bee"));
            o.set("entity_data", e);
            o.set("ticks_in_hive", Nbt::num(Nbt::Int, c.r.nextInt(599)));
            o.set("min_ticks_in_hive", Nbt::num(Nbt::Int, 600));
            bees.list.push_back(o);
          }
          Nbt root = Nbt::compound();
          root.set("bees", bees);
          Writer w;
          w.u8(Nbt::Compound);
          nbt_payload(w, root);
          be->nbt = w.b;
        }
        return;
      }
  }
};
struct AlterGround : Decorator {
  const StateProvider* provider;
  void block(DecoCtx& c, const P& p) const {
    for (int dy = 2; dy >= -3; dy--) {
      P q = p.above(dy);
      int s = provider->optional(c.l, c.r, q);
      if (s >= 0) { c.set(q, s); break; }
      if (!c.air(q) && dy < 0) break;
    }
  }
  void circle(DecoCtx& c, const P& p) const {
    for (int xx = -2; xx <= 2; xx++)
      for (int zz = -2; zz <= 2; zz++)
        if (std::abs(xx) != 2 || std::abs(zz) != 2) block(c, p.offset(xx, 0, zz));
  }
  void place(DecoCtx& c) const override {
    auto ps = c.lowestTrunkOrRoot();
    if (ps.empty()) return;
    int minY = ps[0].y;
    for (auto& p : ps) {
      if (p.y != minY) continue;
      circle(c, p.offset(-1, 0, -1));
      circle(c, p.offset(2, 0, -1));
      circle(c, p.offset(-1, 0, 2));
      circle(c, p.offset(2, 0, 2));
      for (int i = 0; i < 5; i++) {
        int pl = c.r.nextInt(64), xx = pl % 8, zz = pl / 8;
        if (xx == 0 || xx == 7 || zz == 0 || zz == 7) circle(c, p.offset(-3 + xx, 0, -3 + zz));
      }
    }
  }
};
struct AttachedToLeaves : Decorator {
  float probability;
  int exXZ, exY, required;
  const StateProvider* provider;
  std::vector<bs::Dir> dirs;
  void place(DecoCtx& c) const override {
    std::set<P> blacklist;
    auto leaves = c.leaves;
    shuffle(leaves, c.r);
    for (auto& leaf : leaves) {
      bs::Dir d = dirs[size_t(c.r.nextInt(int(dirs.size())))];
      P pl = leaf.rel(d);
      if (blacklist.count(pl) || !(c.r.nextFloat() < probability)) continue;
      bool ok = true;
      for (int i = 1; i <= required && ok; i++) ok = c.air(leaf.rel(d, i));
      if (!ok) continue;
      for (int x = -exXZ; x <= exXZ; x++)
        for (int y = -exY; y <= exY; y++)
          for (int z = -exXZ; z <= exXZ; z++) blacklist.insert(pl.offset(x, y, z));
      c.set(pl, provider->get(c.l, c.r, pl));
    }
  }
};
struct AttachedToLogs : Decorator {
  float probability;
  const StateProvider* provider;
  std::vector<bs::Dir> dirs;
  void place(DecoCtx& c) const override {
    auto logs = c.logs;
    shuffle(logs, c.r);
    for (auto& lp : logs) {
      bs::Dir d = dirs[size_t(c.r.nextInt(int(dirs.size())))];
      P pl = lp.rel(d);
      if (c.r.nextFloat() <= probability && c.air(pl)) c.set(pl, provider->get(c.l, c.r, pl));
    }
  }
};
struct PlaceOnGround : Decorator {
  int tries, radius, height;
  const StateProvider* provider;
  void place(DecoCtx& c) const override {
    auto ps = c.lowestTrunkOrRoot();
    if (ps.empty()) return;
    P o = ps[0];
    int minX = o.x, maxX = o.x, minZ = o.z, maxZ = o.z;
    for (auto& p : ps)
      if (p.y == o.y) minX = std::min(minX, p.x), maxX = std::max(maxX, p.x), minZ = std::min(minZ, p.z), maxZ = std::max(maxZ, p.z);
    for (int i = 0; i < tries; i++) {
      int x = c.r.nextIntBetweenInclusive(minX - radius, maxX + radius);
      int y = c.r.nextIntBetweenInclusive(o.y - height, o.y + height);
      int z = c.r.nextIntBetweenInclusive(minZ - radius, maxZ + radius);
      P p{x, y, z}, a = p.above();
      int as = c.l.get(a);
      if ((R().is_air(as) || is(as, blocks::vine)) && (R().flags(c.l.get(p)) & bs::SOLID_RENDER) &&
          c.l.height(MOTION_BLOCKING_NO_LEAVES, x, z) <= a.y)
        c.set(a, provider->get(c.l, c.r, a));
    }
  }
};
struct CreakingHeart : Decorator {
  float probability;
  void place(DecoCtx& c) const override {
    if (c.logs.empty() || c.r.nextFloat() >= probability) return;
    auto ps = c.logs;
    shuffle(ps, c.r);
    for (auto& p : ps) {
      bool ok = true;
      for (bs::Dir d : ALL_DIRS) ok = ok && in(c.l.get(p.rel(d)), "minecraft:logs");
      if (ok) {
        c.set(p, R().with_bool(R().with(blocks::creaking_heart, "creaking_heart_state", "dormant"), "natural", true));
        return;
      }
    }
  }
};
struct PaleMoss : Decorator {
  float leavesP, trunkP, groundP;
  const Feature* patch;
  static void hanger(DecoCtx& c, P p) {
    while (c.air(p.below()) && !((double)c.r.nextFloat() < 0.5)) c.set(p, R().with_bool(blocks::pale_hanging_moss, "tip", false)), p = p.below();
    c.set(p, R().with_bool(blocks::pale_hanging_moss, "tip", true));
  }
  void place(DecoCtx& c) const override {
    auto logs = c.logs;
    shuffle(logs, c.r);
    if (logs.empty()) return;
    P origin = *std::min_element(logs.begin(), logs.end(), [](const P& a, const P& b) { return a.y < b.y; });
    if (c.r.nextFloat() < groundP && patch) patch->place(c.l, c.r, origin.above());
    for (auto& p : c.logs)
      if (c.r.nextFloat() < trunkP && c.air(p.below())) hanger(c, p.below());
    for (auto& p : c.leaves)
      if (c.r.nextFloat() < leavesP && c.air(p.below())) hanger(c, p.below());
  }
};
struct ShelfMushroom : Decorator {
  float probability;
  static bool replaceable(DecoCtx& c, const P& p) { return R().can_be_replaced(c.l.get(p)) && !c.waterNearby(p); }
  static bool mushroom(DecoCtx& c, const P& p) { return is(c.l.get(p), blocks::shelf_mushroom); }
  static void put(DecoCtx& c, const P& p, bs::Dir f) {
    int age = c.r.nextInt(2);
    c.set(p, R().with(R().with_int(blocks::shelf_mushroom, "age", age), "facing", dir_name(f)));
  }
  void place(DecoCtx& c) const override {
    if (c.r.nextFloat() >= probability || c.logs.empty()) return;
    auto& logs = c.logs;
    if (logs.front().y == logs.back().y) {  // fallen log
      bool xAxis = logs.front().x != logs.back().x;
      bs::Dir dirs[2] = {xAxis ? bs::NORTH : bs::EAST, xAxis ? bs::SOUTH : bs::WEST};
      for (auto& lp : logs)
        for (bs::Dir f : dirs) {
          if (c.r.nextFloat() > 0.25F) continue;
          P m = lp.offset(bs::DX[f], 0, bs::DZ[f]);
          if (!replaceable(c, m)) continue;
          bool adj = false;
          for (bs::Dir d : HORIZONTAL) adj = adj || mushroom(c, m.rel(d)) || mushroom(c, lp.rel(d));
          if (!adj) put(c, m, f);
        }
    } else {
      bs::Dir first = random_horizontal(c.r);
      bs::Dir dirs[2] = {first, clockwise(first)};
      int base = logs.front().y;
      for (auto& lp : logs) {
        int dy = lp.y - base;
        if (dy < 1 || dy > 4) continue;
        for (bs::Dir f : dirs) {
          if (c.r.nextFloat() > 0.25F) continue;
          P m = lp.offset(bs::DX[f], 0, bs::DZ[f]);
          if (!replaceable(c, m) || mushroom(c, m.below())) continue;
          put(c, m, f);
          break;
        }
      }
    }
  }
};

const Decorator* parse_decorator(Loader& L, const Json& j) {
  std::string t = strip_ns(j["type"].asString());
  auto dirs = [&](const Json& a) {
    std::vector<bs::Dir> v;
    for (auto& d : a.arr) v.push_back(dir_by_name(d.str));
    return v;
  };
  if (t == "trunk_vine") return L.make<TrunkVine>();
  if (t == "leave_vine") { auto* d = L.make<LeaveVine>(); d->probability = j["probability"].asFloat(); return d; }
  if (t == "cocoa") { auto* d = L.make<Cocoa>(); d->probability = j["probability"].asFloat(); return d; }
  if (t == "beehive") { auto* d = L.make<Beehive>(); d->probability = j["probability"].asFloat(); return d; }
  if (t == "alter_ground") { auto* d = L.make<AlterGround>(); d->provider = L.provider(j["provider"]); return d; }
  if (t == "attached_to_leaves") {
    auto* d = L.make<AttachedToLeaves>();
    d->probability = j["probability"].asFloat(), d->exXZ = j["exclusion_radius_xz"].asInt(), d->exY = j["exclusion_radius_y"].asInt();
    d->provider = L.provider(j["block_provider"]), d->required = j["required_empty_blocks"].asInt(), d->dirs = dirs(j["directions"]);
    return d;
  }
  if (t == "attached_to_logs") {
    auto* d = L.make<AttachedToLogs>();
    d->probability = j["probability"].asFloat(), d->provider = L.provider(j["block_provider"]), d->dirs = dirs(j["directions"]);
    return d;
  }
  if (t == "place_on_ground") {
    auto* d = L.make<PlaceOnGround>();
    d->tries = j.getInt("tries", 128), d->radius = j.getInt("radius", 2), d->height = j.getInt("height", 1);
    d->provider = L.provider(j["block_state_provider"]);
    return d;
  }
  if (t == "creaking_heart") { auto* d = L.make<CreakingHeart>(); d->probability = j["probability"].asFloat(); return d; }
  if (t == "pale_moss") {
    auto* d = L.make<PaleMoss>();
    d->leavesP = j["leaves_probability"].asFloat(), d->trunkP = j["trunk_probability"].asFloat(), d->groundP = j["ground_probability"].asFloat();
    d->patch = L.feature(Json::parse("\"minecraft:pale_moss_patch\""));
    return d;
  }
  if (t == "shelf_mushroom") { auto* d = L.make<ShelfMushroom>(); d->probability = j["probability"].asFloat(); return d; }
  throw std::runtime_error("tree decorator " + t);
}

// ---------------------------------------------------------------- trunk placers
struct TrunkPlacer {
  int base, randA, randB;
  const bs::Tag* growThrough = nullptr;  // UpwardsBranchingTrunkPlacer.validTreePos
  virtual ~TrunkPlacer() = default;
  int height(RandomSource& r) const {
    int a = r.nextInt(randA + 1);
    return base + a + r.nextInt(randB + 1);
  }
  bool valid(Level& l, const P& p) const { return valid_tree_pos(l, p) || (growThrough && R().in(l.get(p), *growThrough)); }
  bool isFree(Level& l, const P& p) const { return valid(l, p) || in(l.get(p), "minecraft:logs"); }
  virtual std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int h, const P& o) const = 0;
  void below(TreeCtx& c, RandomSource& r, const P& p) const;
  bool log(TreeCtx& c, RandomSource& r, const P& p, const char* axis = nullptr) const;
  void logIfFree(TreeCtx& c, RandomSource& r, const P& p) const {
    if (isFree(c.l, p)) log(c, r, p);
  }
};

// ---------------------------------------------------------------- foliage placers
struct FoliagePlacer {
  IntProvider radius, offset;
  virtual ~FoliagePlacer() = default;
  virtual void create(TreeCtx& c, RandomSource& r, int treeHeight, const Attachment& a, int fh, int leafRadius, int off) const = 0;
  virtual int foliageHeight(RandomSource& r, int treeHeight) const = 0;
  virtual int foliageRadius(RandomSource& r, int) const { return radius.sample(r); }
  virtual bool skip(RandomSource& r, int dx, int y, int dz, int cr, bool dt) const = 0;
  virtual bool skipSigned(RandomSource& r, int dx, int y, int dz, int cr, bool dt) const {
    int mx = dt ? std::min(std::abs(dx), std::abs(dx - 1)) : std::abs(dx);
    int mz = dt ? std::min(std::abs(dz), std::abs(dz - 1)) : std::abs(dz);
    return skip(r, mx, y, mz, cr, dt);
  }
  void row(TreeCtx& c, RandomSource& r, const P& o, int cr, int y, bool dt) const;
  void rowHanging(TreeCtx& c, RandomSource& r, const P& o, int cr, int y, bool dt, float chance, float ext) const;
};

struct FeatureSize {
  int kind = 2, limit = 1, upperLimit = 1, lower = 0, middle = 1, upper = 1;
  std::optional<int> minClipped;
  int at(int treeHeight, int yo) const {
    if (kind == 2) return yo < limit ? lower : upper;
    if (yo < limit) return lower;
    return yo >= treeHeight - upperLimit ? upper : middle;
  }
};

struct RootPlacer {  // MangroveRootPlacer
  IntProvider trunkOffsetY;
  const StateProvider* rootProvider;
  const StateProvider* aboveProvider = nullptr;
  float aboveChance = 0;
  bs::Tag canGrowThrough, muddyIn;
  const StateProvider* muddyProvider;
  int maxWidth, maxLength;
  float skew;
  bool canPlace(Level& l, const P& p) const { return valid_tree_pos(l, p) || R().in(l.get(p), canGrowThrough); }
  static int waterlogged(Level& l, const P& p, int s) {
    return R().has(s, "waterlogged") ? R().with_bool(s, "waterlogged", R().is_water(l.get(p))) : s;
  }
  std::vector<P> potential(const P& pos, bs::Dir d, RandomSource& r, const P& origin) const {
    P below = pos.below(), next = pos.rel(d);
    int width = dist_manhattan(pos, origin);
    if (width > maxWidth - 3 && width <= maxWidth) return r.nextFloat() < skew ? std::vector<P>{below, next.below()} : std::vector<P>{below};
    if (width > maxWidth) return {below};
    if (r.nextFloat() < skew) return {below};
    return r.nextBoolean() ? std::vector<P>{next} : std::vector<P>{below};
  }
  bool simulate(Level& l, RandomSource& r, const P& pos, bs::Dir d, const P& origin, std::vector<P>& out, int layer) const {
    if (layer == maxLength || int(out.size()) > maxLength) return false;
    for (auto& p : potential(pos, d, r, origin))
      if (canPlace(l, p)) {
        out.push_back(p);
        if (!simulate(l, r, p, d, origin, out, layer + 1)) return false;
      }
    return true;
  }
  void placeRoot(TreeCtx& c, RandomSource& r, const P& p) const {
    if (R().in(c.l.get(p), muddyIn)) {
      c.root(p, waterlogged(c.l, p, muddyProvider->get(c.l, r, p)));
      return;
    }
    if (!canPlace(c.l, p)) return;
    c.root(p, waterlogged(c.l, p, rootProvider->get(c.l, r, p)));
    if (aboveProvider) {
      P a = p.above();
      if (r.nextFloat() < aboveChance && R().is_air(c.l.get(a))) c.root(a, waterlogged(c.l, a, aboveProvider->get(c.l, r, a)));
    }
  }
  bool place(TreeCtx& c, RandomSource& r, const P& origin, const P& trunk) const {
    std::vector<P> ps;
    for (P col = origin; col.y < trunk.y; col = col.above())
      if (!canPlace(c.l, col)) return false;
    ps.push_back(trunk.below());
    for (bs::Dir d : HORIZONTAL) {
      std::vector<P> dir;
      if (!simulate(c.l, r, trunk.rel(d), d, trunk, dir, 0)) return false;
      ps.insert(ps.end(), dir.begin(), dir.end());
      ps.push_back(trunk.rel(d));
    }
    for (auto& p : ps) placeRoot(c, r, p);
    return true;
  }
};

struct Tree : Feature {
  const StateProvider *trunkProvider, *foliageProvider, *belowTrunk;
  const TrunkPlacer* trunk;
  const FoliagePlacer* foliage;
  const RootPlacer* roots = nullptr;
  FeatureSize size;
  std::vector<const Decorator*> decorators;
  bool ignoreVines;

  bool doPlace(TreeCtx& c, RandomSource& r, const P& origin) const {
    int treeHeight = trunk->height(r);
    int foliageHeight = foliage->foliageHeight(r, treeHeight);
    int trunkHeight = treeHeight - foliageHeight;
    int leafRadius = foliage->foliageRadius(r, trunkHeight);
    P trunkOrigin = roots ? origin.above(roots->trunkOffsetY.sample(r)) : origin;
    int minY = std::min(origin.y, trunkOrigin.y), maxY = std::max(origin.y, trunkOrigin.y) + treeHeight + 1;
    if (minY < c.l.min_y() + 1 || maxY > c.l.max_y() + 1) return false;
    int clipped = treeHeight;  // getMaxFreeTreeHeight
    for (int y = 0; y <= treeHeight + 1 && clipped == treeHeight; y++) {
      int rr = size.at(treeHeight, y);
      for (int x = -rr; x <= rr && clipped == treeHeight; x++)
        for (int z = -rr; z <= rr; z++) {
          P p = trunkOrigin.offset(x, y, z);
          if (!trunk->isFree(c.l, p) || (!ignoreVines && is(c.l.get(p), blocks::vine))) { clipped = y - 2; break; }
        }
    }
    if (!(clipped >= treeHeight || (size.minClipped && clipped >= *size.minClipped))) return false;
    if (roots && !roots->place(c, r, origin, trunkOrigin)) return false;
    for (auto& a : trunk->place(c, r, clipped, trunkOrigin)) {
      int off = foliage->offset.sample(r);
      foliage->create(c, r, clipped, a, foliageHeight, leafRadius, off);
    }
    return true;
  }

  bool place(Level& l, RandomSource& r, const P& origin) const override {
    TreeCtx c{l, *this, {}, {}, {}, {}};
    if (!doPlace(c, r, origin) || (c.trunks.empty() && c.foliage.empty())) return false;
    if (!decorators.empty()) {
      DecoCtx dc(l, r, c.trunks, c.foliage, c.roots, [&](const P& p, int s) { c.decorations.insert(p), l.set(p, s); });
      for (auto* d : decorators) d->place(dc);
    }
    // BoundingBox.encapsulatingPositions(roots, trunks, foliage, decorations)
    std::vector<P> all;
    for (auto* s : {&c.roots, &c.trunks, &c.foliage, &c.decorations})
      for (auto& p : s->items()) all.push_back(p);
    if (all.empty()) return false;
    P mn = all[0], mx = all[0];
    for (auto& p : all)
      mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)}, mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    int sx = mx.x - mn.x + 1, sy = mx.y - mn.y + 1, sz = mx.z - mn.z + 1;
    std::vector<uint8_t> shape(size_t(sx * sy * sz));
    auto inside = [&](const P& p) { return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y && p.z >= mn.z && p.z <= mx.z; };
    auto cell = [&](int x, int y, int z) -> uint8_t& { return shape[size_t((x * sy + y) * sz + z)]; };
    auto fill = [&](const P& p) { cell(p.x - mn.x, p.y - mn.y, p.z - mn.z) = 1; };
    // updateLeaves
    for (auto* s : {&c.decorations, &c.roots})
      for (auto& p : s->items())
        if (inside(p)) fill(p);
    PosSet toCheck[7];
    for (auto& p : c.trunks.items()) toCheck[0].insert(p);
    int smallest = 0;
    static const bs::Tag& noDecay = R().tag("minecraft:prevents_nearby_leaf_decay");
    for (;;) {
      while (smallest < 7 && toCheck[smallest].empty()) smallest++;
      if (smallest >= 7) break;
      P p = toCheck[smallest].pop_first();
      if (!inside(p)) continue;
      if (smallest != 0) l.set(p, R().with_int(l.get(p), "distance", smallest));
      fill(p);
      for (bs::Dir d : ALL_DIRS) {
        P n = p.rel(d);
        if (!inside(n) || cell(n.x - mn.x, n.y - mn.y, n.z - mn.z)) continue;
        int s = l.get(n);
        int dist = R().in(s, noDecay) ? 0 : R().int_value(s, "distance");
        if (dist < 0) continue;
        int nd = std::min(dist, smallest + 1);
        if (nd < 7) toCheck[nd].insert(n), smallest = std::min(smallest, nd);
      }
    }
    // StructureTemplate.updateShapeAtEdge(level, 3, shape, min)
    update_shape_at_edge(l, mn, sx, sy, sz, [&](int x, int y, int z) { return cell(x, y, z) != 0; });
    return true;
  }
};

void TrunkPlacer::below(TreeCtx& c, RandomSource& r, const P& p) const {
  int s = c.tree.belowTrunk->optional(c.l, r, p);
  if (s >= 0) c.trunk(p, s);
}
bool TrunkPlacer::log(TreeCtx& c, RandomSource& r, const P& p, const char* axis) const {
  if (!valid(c.l, p)) return false;
  int s = c.tree.trunkProvider->get(c.l, r, p);
  if (axis) s = with_axis(s, axis);
  c.trunk(p, s);
  return true;
}

// FoliagePlacer.tryPlaceLeaf
bool try_leaf(TreeCtx& c, RandomSource& r, const P& p) {
  int cur = c.l.get(p);
  if (R().bool_value(cur, "persistent") || !valid_tree_pos(c.l, p)) return false;
  int s = c.tree.foliageProvider->get(c.l, r, p);
  if (R().has(s, "waterlogged")) s = R().with_bool(s, "waterlogged", R().fluid_type(cur) == bs::WATER);
  c.leaf(p, s);
  return true;
}
void FoliagePlacer::row(TreeCtx& c, RandomSource& r, const P& o, int cr, int y, bool dt) const {
  int off = dt ? 1 : 0;
  for (int dx = -cr; dx <= cr + off; dx++)
    for (int dz = -cr; dz <= cr + off; dz++)
      if (!skipSigned(r, dx, y, dz, cr, dt)) try_leaf(c, r, o.offset(dx, y, dz));
}
void FoliagePlacer::rowHanging(TreeCtx& c, RandomSource& r, const P& o, int cr, int y, bool dt, float chance, float ext) const {
  row(c, r, o, cr, y, dt);
  int off = dt ? 1 : 0;
  P logPos = o.below();
  auto tryExt = [&](float ch, const P& p) {
    if (dist_manhattan(p, logPos) >= 7) return false;
    return r.nextFloat() > ch ? false : try_leaf(c, r, p);
  };
  for (bs::Dir along : HORIZONTAL) {
    bs::Dir toEdge = clockwise(along);
    bool positive = toEdge == bs::EAST || toEdge == bs::SOUTH;
    int offToEdge = positive ? cr + off : cr;
    P p = o.offset(0, y - 1, 0).rel(toEdge, offToEdge).rel(along, -cr);
    for (int a = -cr; a < cr + off; a++) {
      bool above = c.leafSet(p.above());
      if (above && tryExt(chance, p)) tryExt(ext, p.below());
      p = p.rel(along);
    }
  }
}

struct Straight : TrunkPlacer {
  std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int h, const P& o) const override {
    below(c, r, o.below());
    for (int y = 0; y < h; y++) log(c, r, o.above(y));
    return {Attachment(o.above(h), 0, false)};
  }
};
struct Forking : TrunkPlacer {
  std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int h, const P& o) const override {
    below(c, r, o.below());
    std::vector<Attachment> at;
    bs::Dir lean = random_horizontal(r);
    int leanHeight = h - r.nextInt(4) - 1;
    int leanSteps = 3 - r.nextInt(3);
    int tx = o.x, tz = o.z;
    std::optional<int> ey;
    for (int yo = 0; yo < h; yo++) {
      int yy = o.y + yo;
      if (yo >= leanHeight && leanSteps > 0) tx += bs::DX[lean], tz += bs::DZ[lean], leanSteps--;
      if (log(c, r, {tx, yy, tz})) ey = yy + 1;
    }
    if (ey) at.push_back(Attachment({tx, *ey, tz}, 1, false));
    tx = o.x, tz = o.z;
    bs::Dir branch = random_horizontal(r);
    if (branch != lean) {
      int branchPos = leanHeight - r.nextInt(2) - 1;
      int steps = 1 + r.nextInt(3);
      ey.reset();
      for (int yo = branchPos; yo < h && steps > 0; steps--) {
        if (yo >= 1) {
          int yy = o.y + yo;
          tx += bs::DX[branch], tz += bs::DZ[branch];
          if (log(c, r, {tx, yy, tz})) ey = yy + 1;
        }
        yo++;
      }
      if (ey) at.push_back(Attachment({tx, *ey, tz}, 0, false));
    }
    return at;
  }
};
struct Giant : TrunkPlacer {
  std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int h, const P& o) const override {
    P b = o.below();
    below(c, r, b), below(c, r, b.offset(1, 0, 0)), below(c, r, b.offset(0, 0, 1)), below(c, r, b.offset(1, 0, 1));
    for (int hh = 0; hh < h; hh++) {
      logIfFree(c, r, o.offset(0, hh, 0));
      if (hh < h - 1) logIfFree(c, r, o.offset(1, hh, 0)), logIfFree(c, r, o.offset(1, hh, 1)), logIfFree(c, r, o.offset(0, hh, 1));
    }
    return {Attachment(o.above(h), 0, true)};
  }
};
struct MegaJungle : Giant {
  std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int h, const P& o) const override {
    auto at = Giant::place(c, r, h, o);
    for (int bh = h - 2 - r.nextInt(4); bh > h / 2; bh -= 2 + r.nextInt(4)) {
      float angle = r.nextFloat() * (float)(M_PI * 2);
      int bx = 0, bz = 0;
      for (int b = 0; b < 5; b++) {
        bx = (int)(1.5F + Mth::cos(angle) * (float)b);
        bz = (int)(1.5F + Mth::sin(angle) * (float)b);
        log(c, r, o.offset(bx, bh - 3 + b / 2, bz));
      }
      at.push_back(Attachment(o.offset(bx, bh, bz), -2, false));
    }
    return at;
  }
};
struct DarkOak : TrunkPlacer {
  std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int h, const P& o) const override {
    std::vector<Attachment> at;
    P b = o.below();
    below(c, r, b), below(c, r, b.offset(1, 0, 0)), below(c, r, b.offset(0, 0, 1)), below(c, r, b.offset(1, 0, 1));
    bs::Dir lean = random_horizontal(r);
    int leanHeight = h - r.nextInt(4);
    int leanSteps = 2 - r.nextInt(3);
    int x = o.x, y = o.y, z = o.z, tx = x, tz = z, ey = y + h - 1;
    for (int dy = 0; dy < h; dy++) {
      if (dy >= leanHeight && leanSteps > 0) tx += bs::DX[lean], tz += bs::DZ[lean], leanSteps--;
      P p{tx, y + dy, tz};
      int s = c.l.get(p);
      if (R().is_air(s) || in(s, "minecraft:leaves")) {
        log(c, r, p), log(c, r, p.offset(1, 0, 0)), log(c, r, p.offset(0, 0, 1)), log(c, r, p.offset(1, 0, 1));
      }
    }
    at.push_back(Attachment({tx, ey, tz}, 0, true));
    for (int ox = -1; ox <= 2; ox++)
      for (int oz = -1; oz <= 2; oz++)
        if ((ox < 0 || ox > 1 || oz < 0 || oz > 1) && r.nextInt(3) <= 0) {
          int len = r.nextInt(3) + 2;
          for (int by = 0; by < len; by++) log(c, r, {x + ox, ey - by - 1, z + oz});
          at.push_back(Attachment({x + ox, ey, z + oz}, 0, false));
        }
    return at;
  }
};
struct Fancy : TrunkPlacer {
  struct Coords {
    Attachment a;
    int branchBase;
  };
  static float treeShape(int height, int y) {
    if ((float)y < (float)height * 0.3F) return -1.0F;
    float radius = (float)height / 2.0F, adj = radius - (float)y;
    float distance = (float)std::sqrt((double)(radius * radius - adj * adj));
    if (adj == 0.0F) distance = radius;
    else if (std::abs(adj) >= radius) return 0.0F;
    return distance * 0.5F;
  }
  bool limb(TreeCtx& c, RandomSource& r, const P& s, const P& e, bool doPlace) const {
    if (!doPlace && s == e) return true;
    P d{e.x - s.x, e.y - s.y, e.z - s.z};
    int steps = std::max(std::abs(d.x), std::max(std::abs(d.y), std::abs(d.z)));
    float dx = (float)d.x / (float)steps, dy = (float)d.y / (float)steps, dz = (float)d.z / (float)steps;
    for (int i = 0; i <= steps; i++) {
      P p = s.offset(Mth::floor(0.5F + (float)i * dx), Mth::floor(0.5F + (float)i * dy), Mth::floor(0.5F + (float)i * dz));
      if (doPlace) {
        int xd = std::abs(p.x - s.x), zd = std::abs(p.z - s.z), m = std::max(xd, zd);
        log(c, r, p, m > 0 ? (xd == m ? "x" : "z") : "y");
      } else if (!isFree(c.l, p)) {
        return false;
      }
    }
    return true;
  }
  static bool trim(int height, int localY) { return (double)localY >= height * 0.2; }
  std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int treeHeight, const P& o) const override {
    int height = treeHeight + 2;
    int trunkHeight = Mth::floor(height * 0.618);
    below(c, r, o.below());
    int clusters = std::min(1, Mth::floor(1.382 + std::pow(1.0 * height / 13.0, 2.0)));
    int trunkTop = o.y + trunkHeight;
    int relY = height - 5;
    std::vector<Coords> coords;
    coords.push_back({Attachment(o.above(relY), 0, false), trunkTop});
    for (; relY >= 0; relY--) {
      float shape = treeShape(height, relY);
      if (shape < 0.0F) continue;
      for (int i = 0; i < clusters; i++) {
        double radius = 1.0 * shape * (r.nextFloat() + 0.328);
        double angle = r.nextFloat() * 2.0F * M_PI;
        double x = radius * std::sin(angle) + 0.5, z = radius * std::cos(angle) + 0.5;
        P cs = o.offset(Mth::floor(x), relY - 1, Mth::floor(z)), ce = cs.above(5);
        if (!limb(c, r, cs, ce, false)) continue;
        int dx = o.x - cs.x, dz = o.z - cs.z;
        double bh = cs.y - std::sqrt((double)(dx * dx + dz * dz)) * 0.381;
        int branchTop = bh > trunkTop ? trunkTop : (int)bh;
        P base{o.x, branchTop, o.z};
        if (limb(c, r, base, cs, false)) coords.push_back({Attachment(cs, 0, false), base.y});
      }
    }
    limb(c, r, o, o.above(trunkHeight), true);
    for (auto& e : coords) {  // makeBranches
      P base{o.x, e.branchBase, o.z};
      if (base != e.a.pos && trim(height, e.branchBase - o.y)) limb(c, r, base, e.a.pos, true);
    }
    std::vector<Attachment> at;
    for (auto& e : coords)
      if (trim(height, e.branchBase - o.y)) at.push_back(e.a);
    return at;
  }
};
struct Bending : TrunkPlacer {
  int minHeightForLeaves;
  IntProvider bendLength;
  std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int h, const P& o) const override {
    bs::Dir d = random_horizontal(r);
    int logHeight = h - 1;
    P p = o;
    below(c, r, p.below());
    std::vector<Attachment> at;
    for (int i = 0; i <= logHeight; i++) {
      if (i + 1 >= logHeight + r.nextInt(2)) p = p.rel(d);
      if (valid_tree_pos(c.l, p)) log(c, r, p);
      if (i >= minHeightForLeaves) at.push_back(Attachment(p, 0, false));
      p = p.above();
    }
    int len = bendLength.sample(r);
    for (int i = 0; i <= len; i++) {
      if (valid_tree_pos(c.l, p)) log(c, r, p);
      at.push_back(Attachment(p, 0, false));
      p = p.rel(d);
    }
    return at;
  }
};
struct UpwardsBranching : TrunkPlacer {
  IntProvider extraSteps, extraLength;
  float probability;
  bs::Tag through;
  std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int h, const P& o) const override {
    std::vector<Attachment> at;
    for (int hp = 0; hp < h; hp++) {
      int cur = o.y + hp;
      P lp{o.x, cur, o.z};
      if (log(c, r, lp) && hp < h - 1 && r.nextFloat() < probability) {
        bs::Dir d = random_horizontal(r);
        int len = extraLength.sample(r);
        int bpos = std::max(0, len - extraLength.sample(r) - 1);
        int steps = extraSteps.sample(r);
        int along = cur + bpos, lx = lp.x, lz = lp.z;
        for (int idx = bpos; idx < h && steps > 0; idx++, steps--) {
          if (idx < 1) continue;
          int ph = cur + idx;
          lx += bs::DX[d], lz += bs::DZ[d];
          along = ph;
          if (log(c, r, {lx, ph, lz})) along = ph + 1;
          at.push_back(Attachment({lx, ph, lz}, 0, false));
        }
        if (along - cur > 1) {
          P f{lx, along, lz};
          at.push_back(Attachment(f, 0, false));
          at.push_back(Attachment(f.below(2), 0, false));
        }
      }
      if (hp == h - 1) at.push_back(Attachment({o.x, cur + 1, o.z}, 0, false));
    }
    return at;
  }
};
struct Cherry : TrunkPlacer {
  IntProvider count, hlen, start, start2, end;
  Attachment branch(TreeCtx& c, RandomSource& r, int h, const P& o, bs::Dir dir, const char* side, int off, bool middleUp) const {
    P lp = o.above(off);
    int endOff = h - 1 + end.sample(r);
    bool extend = middleUp || endOff < off;
    int dist = hlen.sample(r) + (extend ? 1 : 0);
    P endPos = o.rel(dir, dist).above(endOff);
    int steps = extend ? 2 : 1;
    for (int i = 0; i < steps; i++) lp = lp.rel(dir), log(c, r, lp, side);
    bs::Dir vert = endPos.y > lp.y ? bs::UP : bs::DOWN;
    for (;;) {
      int distance = dist_manhattan(lp, endPos);
      if (distance == 0) return Attachment(endPos.above(), 0, false);
      float chance = (float)std::abs(endPos.y - lp.y) / (float)distance;
      bool v = r.nextFloat() < chance;
      lp = lp.rel(v ? vert : dir);
      log(c, r, lp, v ? nullptr : side);
    }
  }
  std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int h, const P& o) const override {
    below(c, r, o.below());
    int first = std::max(0, h - 1 + start.sample(r));
    int second = std::max(0, h - 1 + start2.sample(r));
    if (second >= first) second++;
    int n = count.sample(r);
    bool middle = n == 3, both = n >= 2;
    int th = middle ? h : both ? std::max(first, second) + 1 : first + 1;
    for (int y = 0; y < th; y++) log(c, r, o.above(y));
    std::vector<Attachment> at;
    if (middle) at.push_back(Attachment(o.above(th), 0, false));
    bs::Dir d = random_horizontal(r);
    const char* side = axis_of(d);
    at.push_back(branch(c, r, h, o, d, side, first, first < th - 1));
    if (both) at.push_back(branch(c, r, h, o, bs::opposite(d), side, second, second < th - 1));
    return at;
  }
};
struct Poplar : TrunkPlacer {
  IntProvider above, amount;
  std::vector<Attachment> place(TreeCtx& c, RandomSource& r, int h, const P& o) const override {
    below(c, r, o.below());
    int upTo = h - above.sample(r);
    for (int y = 0; y < h; y++) {
      log(c, r, o.above(y));
      std::vector<bs::Dir> dirs;
      for (bs::Dir d : all_shuffled(r))
        if (d != bs::UP && d != bs::DOWN) dirs.push_back(d);
      if (upTo - 1 == y) {
        int n = amount.sample(r);
        for (int x = 0; x < n; x++) log(c, r, o.above(y).rel(dirs[size_t(x)]), axis_of(dirs[size_t(x)]));
      }
    }
    return {Attachment(o.above(upTo), 0, false)};
  }
};

// ---------------------------------------------------------------- foliage placers
struct Blob : FoliagePlacer {
  int height;
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int off) const override {
    int fho = fh + a.heightOffset;
    for (int yo = off; yo >= off - fho; yo--) row(c, r, a.pos, std::max(lr + a.radiusOffset - 1 - yo / 2, 0), yo, a.doubleTrunk());
  }
  int foliageHeight(RandomSource&, int) const override { return height; }
  bool skip(RandomSource& r, int dx, int y, int dz, int cr, bool) const override {
    return dx == cr && dz == cr && (r.nextInt(2) == 0 || y == 0);
  }
};
struct Bush : Blob {
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int off) const override {
    int fho = fh + a.heightOffset;
    for (int yo = off; yo >= off - fho; yo--) row(c, r, a.pos, lr + a.radiusOffset - 1 - yo, yo, a.doubleTrunk());
  }
  bool skip(RandomSource& r, int dx, int, int dz, int cr, bool) const override { return dx == cr && dz == cr && r.nextInt(2) == 0; }
};
struct FancyFoliage : Blob {
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int off) const override {
    for (int yo = off; yo >= off - fh; yo--) row(c, r, a.pos, lr + (yo != off && yo != off - fh ? 1 : 0), yo, a.doubleTrunk());
  }
  bool skip(RandomSource&, int dx, int, int dz, int cr, bool) const override {
    return Mth::square((float)dx + 0.5F) + Mth::square((float)dz + 0.5F) > (float)(cr * cr);
  }
};
struct Pine : FoliagePlacer {
  IntProvider height;
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int off) const override {
    int cr = 0, fho = fh + a.heightOffset;
    for (int yo = off; yo >= off - fho; yo--) {
      row(c, r, a.pos, cr, yo, a.doubleTrunk());
      if (cr >= 1 && yo == off - fho + 1) cr--;
      else if (cr < lr + a.radiusOffset) cr++;
    }
  }
  int foliageRadius(RandomSource& r, int th) const override {
    int base = radius.sample(r);
    return base + r.nextInt(std::max(th + 1, 1));
  }
  int foliageHeight(RandomSource& r, int) const override { return height.sample(r); }
  bool skip(RandomSource&, int dx, int, int dz, int cr, bool) const override { return dx == cr && dz == cr && cr > 0; }
};
struct Spruce : FoliagePlacer {
  IntProvider trunkHeight;
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int off) const override {
    int cr = r.nextInt(2), maxR = 1, minR = 0, fho = fh + a.heightOffset;
    for (int yo = off; yo >= -fho; yo--) {
      row(c, r, a.pos, cr, yo, a.doubleTrunk());
      if (cr >= maxR) cr = minR, minR = 1, maxR = std::min(maxR + 1, lr + a.radiusOffset);
      else cr++;
    }
  }
  int foliageHeight(RandomSource& r, int th) const override { return std::max(4, th - trunkHeight.sample(r)); }
  bool skip(RandomSource&, int dx, int, int dz, int cr, bool) const override { return dx == cr && dz == cr && cr > 0; }
};
struct MegaPine : FoliagePlacer {
  IntProvider crown;
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int off) const override {
    P fp = a.pos;
    int prev = 0, fho = fh + a.heightOffset;
    for (int yy = fp.y - fho + off; yy <= fp.y + off; yy++) {
      int yo = fp.y - yy;
      int smooth = lr + a.radiusOffset + Mth::floor((float)yo / (float)fho * 3.5F);
      int jagged = yo > 0 && smooth == prev && (yy & 1) == 0 ? smooth + 1 : smooth;
      row(c, r, {fp.x, yy, fp.z}, jagged, 0, a.doubleTrunk());
      prev = smooth;
    }
  }
  int foliageHeight(RandomSource& r, int) const override { return crown.sample(r); }
  bool skip(RandomSource&, int dx, int, int dz, int cr, bool) const override { return dx + dz >= 7 ? true : dx * dx + dz * dz > cr * cr; }
};
struct MegaJungleFoliage : FoliagePlacer {
  int height;
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int off) const override {
    int lh = (a.doubleTrunk() ? fh : 1 + r.nextInt(2)) + a.heightOffset;
    for (int yo = off; yo >= off - lh; yo--) row(c, r, a.pos, lr + a.radiusOffset + 1 - yo, yo, a.doubleTrunk());
  }
  int foliageHeight(RandomSource&, int) const override { return height; }
  bool skip(RandomSource&, int dx, int, int dz, int cr, bool) const override { return dx + dz >= 7 ? true : dx * dx + dz * dz > cr * cr; }
};
struct DarkOakFoliage : FoliagePlacer {
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int, int lr, int off) const override {
    P p = a.pos.above(off);
    bool dt = a.doubleTrunk();
    if (dt) {
      row(c, r, p, lr + 2, -1, dt), row(c, r, p, lr + 3, 0, dt), row(c, r, p, lr + 2, 1, dt);
      if (r.nextBoolean()) row(c, r, p, lr, 2, dt);
    } else {
      row(c, r, p, lr + 2, -1, dt), row(c, r, p, lr + 1, 0, dt);
    }
  }
  int foliageHeight(RandomSource&, int) const override { return 4; }
  bool skipSigned(RandomSource& r, int dx, int y, int dz, int cr, bool dt) const override {
    return y != 0 || !dt || (dx != -cr && dx < cr) || (dz != -cr && dz < cr) ? FoliagePlacer::skipSigned(r, dx, y, dz, cr, dt) : true;
  }
  bool skip(RandomSource&, int dx, int y, int dz, int cr, bool dt) const override {
    if (y == -1 && !dt) return dx == cr && dz == cr;
    return y == 1 ? dx + dz > cr * 2 - 2 : false;
  }
};
struct Acacia : FoliagePlacer {
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int off) const override {
    bool dt = a.doubleTrunk();
    P p = a.pos.above(off);
    int fho = fh + a.heightOffset;
    row(c, r, p, lr + a.radiusOffset, -1 - fho, dt);
    row(c, r, p, lr - 1, -fho, dt);
    row(c, r, p, lr + a.radiusOffset - 1, 0, dt);
  }
  int foliageHeight(RandomSource&, int) const override { return 0; }
  bool skip(RandomSource&, int dx, int y, int dz, int cr, bool) const override {
    return y == 0 ? (dx > 1 || dz > 1) && dx != 0 && dz != 0 : dx == cr && dz == cr && cr > 0;
  }
};
struct RandomSpread : FoliagePlacer {
  IntProvider height;
  int attempts;
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int) const override {
    for (int i = 0; i < attempts; i++) {
      int x1 = r.nextInt(lr);
      int dx = x1 - r.nextInt(lr);
      int y1 = r.nextInt(fh);
      int dy = y1 - r.nextInt(fh);
      int z1 = r.nextInt(lr);
      int dz = z1 - r.nextInt(lr);
      try_leaf(c, r, a.pos.offset(dx, dy, dz));
    }
  }
  int foliageHeight(RandomSource& r, int) const override { return height.sample(r); }
  bool skip(RandomSource&, int, int, int, int, bool) const override { return false; }
};
struct CherryFoliage : FoliagePlacer {
  IntProvider height;
  float wideHole, cornerHole, hanging, hangingExt;
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int off) const override {
    bool dt = a.doubleTrunk();
    P p = a.pos.above(off);
    int cr = lr + a.radiusOffset - 1, fho = fh + a.heightOffset;
    row(c, r, p, cr - 2, fho - 3, dt);
    row(c, r, p, cr - 1, fho - 4, dt);
    for (int y = fho - 5; y >= 0; y--) row(c, r, p, cr, y, dt);
    rowHanging(c, r, p, cr, -1, dt, hanging, hangingExt);
    rowHanging(c, r, p, cr - 1, -2, dt, hanging, hangingExt);
  }
  int foliageHeight(RandomSource& r, int) const override { return height.sample(r); }
  bool skip(RandomSource& r, int dx, int y, int dz, int cr, bool) const override {
    if (y == -1 && (dx == cr || dz == cr) && r.nextFloat() < wideHole) return true;
    bool corner = dx == cr && dz == cr, wide = cr > 2;
    return wide ? corner || (dx + dz > cr * 2 - 2 && r.nextFloat() < cornerHole) : corner && r.nextFloat() < cornerHole;
  }
};
struct PoplarFoliage : FoliagePlacer {
  IntProvider height;
  float sideHole;
  static bool partial(int fh, int y) { return fh - 1 == y || fh - 2 == y; }
  static int corner(int dx, int dz, int cr, bool part, bool flip) {
    bool small = flip ? ((dx > 0 && dz > 0) || (dz < 0 && dx < 0)) : ((dx > 0 && dz < 0) || (dz > 0 && dx < 0));
    return small ? cr - 1 : (part ? cr + 1 : cr);
  }
  static bool rhombus(int cr, int ax, int az, int cut, int extra) { return ax + az <= cr * 2 - (cut + extra); }
  void prow(TreeCtx& c, RandomSource& r, const P& o, int cr, int y, bool dt, int fh, bool flip) const {
    int off = dt ? 1 : 0;
    for (int dx = -cr; dx <= cr + off; dx++)
      for (int dz = -cr; dz <= cr + off; dz++) {
        bool part = partial(fh, y);
        int cut = corner(dx, dz, cr, part, flip), ax = std::abs(dx), az = std::abs(dz);
        bool skipIt;
        if (part && (ax == cr || az == cr)) skipIt = true;
        else skipIt = !rhombus(cr, ax, az, cut, r.nextFloat() <= sideHole ? 1 : 0);
        if (!skipIt) try_leaf(c, r, o.offset(dx, y, dz));
      }
  }
  void create(TreeCtx& c, RandomSource& r, int, const Attachment& a, int fh, int lr, int off) const override {
    bool dt = a.doubleTrunk();
    P p = a.pos.above(off);
    int cr = lr + a.radiusOffset - 1;
    bool flip = r.nextBoolean();
    int fho = fh + a.heightOffset;
    prow(c, r, p, cr - 2, fho - 1, dt, fho, flip);
    prow(c, r, p, cr - 1, fho - 2, dt, fho, flip);
    prow(c, r, p, cr - 1, fho - 3, dt, fho, flip);
    for (int y = fho - 4; y >= 1; y--) prow(c, r, p, cr, y, dt, fho, flip);
    {  // replaceLeavesWithLog
      int y = fho - 4, offd = dt ? 1 : 0;
      for (int dx = -cr; dx <= cr + offd; dx++)
        for (int dz = -cr; dz <= cr + offd; dz++) {
          int az = std::abs(dz), ax = std::abs(dx);
          if (rhombus(cr, ax, az, corner(dx, dz, cr, partial(fho, y), flip), 2) && ((az == 0 && cr - ax >= 4) || (ax == 0 && cr - az >= 4))) {
            P q = p.offset(dx, y, dz);
            if (c.l.get(q) == c.tree.foliageProvider->get(c.l, r, q)) c.leaf(q, with_axis(c.tree.trunkProvider->get(c.l, r, q), az == 0 ? "x" : "z"));
          }
        }
    }
    prow(c, r, p, cr - 1, 0, dt, fho, flip);
    prow(c, r, p, std::clamp(cr - 2, 1, 2), -1, dt, fho, flip);
  }
  int foliageHeight(RandomSource& r, int) const override { return height.sample(r); }
  bool skip(RandomSource&, int, int, int, int, bool) const override { throw std::runtime_error("poplar skip"); }
};

const TrunkPlacer* parse_trunk(Loader& L, const Json& j) {
  std::string t = strip_ns(j["type"].asString());
  TrunkPlacer* p;
  if (t == "straight_trunk_placer") p = L.make<Straight>();
  else if (t == "forking_trunk_placer") p = L.make<Forking>();
  else if (t == "giant_trunk_placer") p = L.make<Giant>();
  else if (t == "mega_jungle_trunk_placer") p = L.make<MegaJungle>();
  else if (t == "dark_oak_trunk_placer") p = L.make<DarkOak>();
  else if (t == "fancy_trunk_placer") p = L.make<Fancy>();
  else if (t == "bending_trunk_placer") {
    auto* b = L.make<Bending>();
    b->minHeightForLeaves = j.getInt("min_height_for_leaves", 1), b->bendLength = IntProvider(j["bend_length"]);
    p = b;
  } else if (t == "upwards_branching_trunk_placer") {
    auto* u = L.make<UpwardsBranching>();
    u->extraSteps = IntProvider(j["extra_branch_steps"]), u->probability = j["place_branch_per_log_probability"].asFloat();
    u->extraLength = IntProvider(j["extra_branch_length"]), u->through = L.block_set(j["can_grow_through"]);
    u->growThrough = &u->through;
    p = u;
  } else if (t == "cherry_trunk_placer") {
    auto* ch = L.make<Cherry>();
    ch->count = IntProvider(j["branch_count"]), ch->hlen = IntProvider(j["branch_horizontal_length"]);
    const Json& st = j["branch_start_offset_from_top"];  // UniformInt.MAP_CODEC: no "type"
    ch->start.t = IntProvider::UNIFORM, ch->start.a = st["min_inclusive"].asInt(), ch->start.b = st["max_inclusive"].asInt();
    ch->start2 = ch->start, ch->start2.b -= 1;  // UniformInt.of(min, max - 1)
    ch->end = IntProvider(j["branch_end_offset_from_top"]);
    p = ch;
  } else if (t == "poplar_trunk_placer") {
    auto* po = L.make<Poplar>();
    po->above = IntProvider(j["trunk_height_above_branches"]), po->amount = IntProvider(j["branch_amount"]);
    p = po;
  } else throw std::runtime_error("trunk placer " + t);
  p->base = j["base_height"].asInt(), p->randA = j["height_rand_a"].asInt(), p->randB = j["height_rand_b"].asInt();
  return p;
}

const FoliagePlacer* parse_foliage(Loader& L, const Json& j) {
  std::string t = strip_ns(j["type"].asString());
  FoliagePlacer* p;
  if (t == "blob_foliage_placer" || t == "bush_foliage_placer" || t == "fancy_foliage_placer") {
    Blob* b = t == "blob_foliage_placer" ? L.make<Blob>() : t == "bush_foliage_placer" ? (Blob*)L.make<Bush>() : (Blob*)L.make<FancyFoliage>();
    b->height = j["height"].asInt();
    p = b;
  } else if (t == "pine_foliage_placer") { auto* x = L.make<Pine>(); x->height = IntProvider(j["height"]); p = x; }
  else if (t == "spruce_foliage_placer") { auto* x = L.make<Spruce>(); x->trunkHeight = IntProvider(j["trunk_height"]); p = x; }
  else if (t == "mega_pine_foliage_placer") { auto* x = L.make<MegaPine>(); x->crown = IntProvider(j["crown_height"]); p = x; }
  else if (t == "jungle_foliage_placer") { auto* x = L.make<MegaJungleFoliage>(); x->height = j["height"].asInt(); p = x; }
  else if (t == "dark_oak_foliage_placer") p = L.make<DarkOakFoliage>();
  else if (t == "acacia_foliage_placer") p = L.make<Acacia>();
  else if (t == "random_spread_foliage_placer") {
    auto* x = L.make<RandomSpread>();
    x->height = IntProvider(j["foliage_height"]), x->attempts = j["leaf_placement_attempts"].asInt();
    p = x;
  } else if (t == "cherry_foliage_placer") {
    auto* x = L.make<CherryFoliage>();
    x->height = IntProvider(j["height"]), x->wideHole = j["wide_bottom_layer_hole_chance"].asFloat();
    x->cornerHole = j["corner_hole_chance"].asFloat(), x->hanging = j["hanging_leaves_chance"].asFloat();
    x->hangingExt = j["hanging_leaves_extension_chance"].asFloat();
    p = x;
  } else if (t == "poplar_foliage_placer") {
    auto* x = L.make<PoplarFoliage>();
    x->height = IntProvider(j["height"]), x->sideHole = j["side_hole_chance"].asFloat();
    p = x;
  } else throw std::runtime_error("foliage placer " + t);
  p->radius = IntProvider(j["radius"]), p->offset = IntProvider(j["offset"]);
  return p;
}

// ---------------------------------------------------------------- fallen tree
struct FallenTree : Feature {
  const StateProvider* trunk;
  IntProvider length;
  std::vector<const Decorator*> stump, logDeco;
  static bool overSolid(Level& l, const P& p) { return R().sturdy(l.get(p.below()), bs::UP); }
  P logBlock(Level& l, RandomSource& r, const P& p, const char* axis) const {
    int s = trunk->get(l, r, p);
    if (axis) s = with_axis(s, axis);
    l.set(p, s);
    l.mark_above_for_post_processing(p);
    return p;
  }
  void decorate(Level& l, RandomSource& r, const PosSet& logs, const std::vector<const Decorator*>& ds) const {
    if (ds.empty()) return;
    PosSet none;
    DecoCtx c(l, r, logs, none, none, [&](const P& p, int s) { l.set(p, s); });
    for (auto* d : ds) d->place(c);
  }
  bool place(Level& l, RandomSource& r, const P& o) const override {
    PosSet st;
    st.insert(logBlock(l, r, o, nullptr));
    decorate(l, r, st, stump);
    bs::Dir d = random_horizontal(r);
    int len = length.sample(r) - 2;
    P p = o.rel(d, 2 + r.nextInt(2));
    p = p.above();  // setGroundHeightForFallenLogStartPos
    for (int i = 0; i < 6; i++) {
      if (valid_tree_pos(l, p) && overSolid(l, p)) break;
      p = p.below();
    }
    int gap = 0;  // canPlaceEntireFallenLog
    P q = p;
    for (int i = 0; i < len; i++) {
      if (!valid_tree_pos(l, q)) return true;
      if (!overSolid(l, q)) {
        if (++gap > 2) return true;
      } else {
        gap = 0;
      }
      q = q.rel(d);
    }
    PosSet logs;
    for (int i = 0; i < len; i++) logs.insert(logBlock(l, r, p, axis_of(d))), p = p.rel(d);
    decorate(l, r, logs, logDeco);
    return true;
  }
};
}  // namespace

void register_tree_features(ParserMap& m) {
  m["tree"] = [](Loader& L, const Json& j) {
    auto* t = L.make<Tree>();
    t->trunkProvider = L.provider(j["trunk_provider"]);
    t->trunk = parse_trunk(L, j["trunk_placer"]);
    t->foliageProvider = L.provider(j["foliage_provider"]);
    t->foliage = parse_foliage(L, j["foliage_placer"]);
    if (j.has("root_placer")) {
      const Json& rj = j["root_placer"];
      auto* rp = L.make<RootPlacer>();
      rp->trunkOffsetY = IntProvider(rj["trunk_offset_y"]);
      rp->rootProvider = L.provider(rj["root_provider"]);
      if (rj.has("above_root_placement")) {
        rp->aboveProvider = L.provider(rj["above_root_placement"]["above_root_provider"]);
        rp->aboveChance = rj["above_root_placement"]["above_root_placement_chance"].asFloat();
      }
      const Json& mp = rj["mangrove_root_placement"];
      rp->canGrowThrough = L.block_set(mp["can_grow_through"]), rp->muddyIn = L.block_set(mp["muddy_roots_in"]);
      rp->muddyProvider = L.provider(mp["muddy_roots_provider"]);
      rp->maxWidth = mp["max_root_width"].asInt(), rp->maxLength = mp["max_root_length"].asInt();
      rp->skew = mp["random_skew_chance"].asFloat();
      t->roots = rp;
    }
    const Json& ms = j["minimum_size"];
    bool three = strip_ns(ms["type"].asString()) == "three_layers_feature_size";
    t->size.kind = three ? 3 : 2;
    t->size.limit = ms.getInt("limit", 1), t->size.upperLimit = ms.getInt("upper_limit", 1);
    t->size.lower = ms.getInt("lower_size", 0), t->size.middle = ms.getInt("middle_size", 1), t->size.upper = ms.getInt("upper_size", 1);
    if (ms.has("min_clipped_height")) t->size.minClipped = ms["min_clipped_height"].asInt();
    for (auto& d : j["decorators"].arr) t->decorators.push_back(parse_decorator(L, d));
    t->ignoreVines = j.has("ignore_vines") && j["ignore_vines"].asBool();
    t->belowTrunk = L.provider(j["below_trunk_provider"]);
    return t;
  };
  m["fallen_tree"] = [](Loader& L, const Json& j) {
    auto* f = L.make<FallenTree>();
    f->trunk = L.provider(j["trunk_provider"]);
    f->length = IntProvider(j["log_length"]);
    for (auto& d : j["stump_decorators"].arr) f->stump.push_back(parse_decorator(L, d));
    for (auto& d : j["log_decorators"].arr) f->logDeco.push_back(parse_decorator(L, d));
    return f;
  };
}

}  // namespace worldgen::feat
