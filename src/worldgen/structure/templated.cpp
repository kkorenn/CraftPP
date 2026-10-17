// Template-based legacy pieces: TemplateStructurePiece, igloo, shipwreck, ocean ruins, ruined portal, nether fossil.
#include <algorithm>
#include <map>
#include <mutex>

#include "worldgen/structure/legacy.hpp"
#include "worldgen/structure/pieces.hpp"

namespace structure {

void TemplatePiece::postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int, int, BlockPos ref) {
  settings.boundingBox = chunkBB;
  boundingBox = templateBoundingBox(*tmpl, settings, templatePosition);
  if (!placeInWorld(*tmpl, level, templatePosition, ref, settings, random)) return;
  const Blocks& b = Blocks::get();
  for (auto& m : filterBlocks(*tmpl, templatePosition, settings, "structure_block"))
    if (m.nbt >= 0 && m.nbt != NBT_SYNTHETIC && tmpl->nbts[m.nbt].mode == "DATA")
      handleDataMarker(tmpl->nbts[m.nbt].metadata, m.pos, level, random, chunkBB);
  for (auto& j : filterBlocks(*tmpl, templatePosition, settings, "jigsaw"))
    if (j.nbt >= 0 && j.nbt != NBT_SYNTHETIC) {
      int s = b.parse(tmpl->nbts[j.nbt].finalState);
      level.set(j.pos, s < 0 ? blocks::air : s);
    }
}

static bool hasWater(int state) {  // getFluidState().is(FluidTags.WATER)
  const Blocks& b = Blocks::get();
  return (b.flags(state) & BS_HAS_FLUID) && !b.is(state, "lava") && !b.is(state, "flowing_lava");
}

// ------------------------------------------------------------------------------------------- igloo
namespace {
struct IglooSpec { const char* id; BlockPos pivot, offset; };
const IglooSpec IGLOO_TOP{"minecraft:igloo/top", {3, 5, 5}, {0, 0, 0}};
const IglooSpec IGLOO_MIDDLE{"minecraft:igloo/middle", {1, 3, 1}, {2, -3, 4}};
const IglooSpec IGLOO_BOTTOM{"minecraft:igloo/bottom", {3, 6, 7}, {0, -3, -2}};

PlaceSettings iglooSettings(Rotation r, const IglooSpec& s) {
  PlaceSettings p;
  p.rotation = r;
  p.pivot = s.pivot;
  static const ProcessorP IGNORE = blockIgnore({"structure_block"});
  p.processors.push_back(IGNORE);
  p.applyWaterlogging = false;
  return p;
}

struct IglooPiece : TemplatePiece {
  const IglooSpec* spec;
  IglooPiece(const IglooSpec& s, BlockPos pos, Rotation r, int depth)
      : TemplatePiece(0, s.id, iglooSettings(r, s), pos.offset(s.offset).below(depth)), spec(&s) {}
  void handleDataMarker(const std::string& marker, BlockPos pos, Level& level, RandomSource& random, const BoundingBox&) override {
    if (marker != "chest") return;
    level.set(pos, blocks::air);
    if (Blocks::get().is(level.get(pos.below()), "chest")) level.setLootTable(pos.below(), "minecraft:chests/igloo_chest", random.nextLong());
  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int cx, int cz, BlockPos ref) override {
    PlaceSettings s = iglooSettings(settings.rotation, *spec);
    BlockPos entrance = templatePosition.offset(relativePosition(s, {3 - spec->offset.x, 0, -spec->offset.z}));
    int height = level.getHeight(Heightmap::WORLD_SURFACE_WG, entrance.x, entrance.z);
    BlockPos old = templatePosition;
    templatePosition = templatePosition.offset(0, height - 90 - 1, 0);
    TemplatePiece::postProcess(level, random, chunkBB, cx, cz, ref);
    if (spec == &IGLOO_TOP) {
      BlockPos trap = templatePosition.offset(relativePosition(s, {3, 0, 5}));
      BS below = level.get(trap.below());
      if (!below.isAir() && !below.is(blocks::ladder)) level.set(trap, blocks::snow_block);
    }
    templatePosition = old;
  }
};
}  // namespace

void addIglooPieces(BlockPos pos, Rotation r, Builder& builder, RandomSource& random) {
  if (random.nextDouble() < 0.5) {
    int depth = random.nextInt(8) + 4;
    builder.add(std::make_unique<IglooPiece>(IGLOO_BOTTOM, pos, r, depth * 3));
    for (int i = 0; i < depth - 1; i++) builder.add(std::make_unique<IglooPiece>(IGLOO_MIDDLE, pos, r, i * 3));
  }
  builder.add(std::make_unique<IglooPiece>(IGLOO_TOP, pos, r, 0));
}

// ------------------------------------------------------------------------------------------- shipwreck
namespace {
const char* SHIPWRECK_BEACHED[] = {"with_mast", "sideways_full", "sideways_fronthalf", "sideways_backhalf", "rightsideup_full",
                                   "rightsideup_fronthalf", "rightsideup_backhalf", "with_mast_degraded",
                                   "rightsideup_full_degraded", "rightsideup_fronthalf_degraded", "rightsideup_backhalf_degraded"};
const char* SHIPWRECK_OCEAN[] = {"with_mast", "upsidedown_full", "upsidedown_fronthalf", "upsidedown_backhalf", "sideways_full",
                                 "sideways_fronthalf", "sideways_backhalf", "rightsideup_full", "rightsideup_fronthalf",
                                 "rightsideup_backhalf", "with_mast_degraded", "upsidedown_full_degraded",
                                 "upsidedown_fronthalf_degraded", "upsidedown_backhalf_degraded", "sideways_full_degraded",
                                 "sideways_fronthalf_degraded", "sideways_backhalf_degraded", "rightsideup_full_degraded",
                                 "rightsideup_fronthalf_degraded", "rightsideup_backhalf_degraded"};

PlaceSettings shipwreckSettings(Rotation r) {
  PlaceSettings p;
  p.rotation = r;
  p.pivot = {4, 0, 15};
  static const ProcessorP IGNORE = blockIgnore({"air", "structure_block"});
  p.processors.push_back(IGNORE);
  return p;
}

struct ShipwreckPiece : TemplatePiece, ShipwreckHandle {
  bool beached, heightAdjusted = false;
  ShipwreckPiece(const std::string& id, BlockPos pos, Rotation r, bool beached)
      : TemplatePiece(0, id, shipwreckSettings(r), pos), beached(beached) {}
  void handleDataMarker(const std::string& marker, BlockPos pos, Level& level, RandomSource& random, const BoundingBox&) override {
    if (marker == "map_chest") setLootTable(level, random, pos.below(), "minecraft:chests/shipwreck_map");
    else if (marker == "treasure_chest") setLootTable(level, random, pos.below(), "minecraft:chests/shipwreck_treasure");
    else if (marker == "supply_chest") setLootTable(level, random, pos.below(), "minecraft:chests/shipwreck_supply");
  }
  bool tooBig() const override { return tmpl->size.x > 32 || tmpl->size.y > 32; }
  int beachedPosition(int minY, RandomSource& random) const override { return minY - tmpl->size.y / 2 - random.nextInt(3); }
  void adjustHeight(int h) override {
    heightAdjusted = true;
    templatePosition = {templatePosition.x, h, templatePosition.z};
  }
  const BoundingBox& box() const override { return boundingBox; }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int cx, int cz, BlockPos ref) override {
    if (!heightAdjusted && !tooBig()) {
      int minY = MAX_Y + 1, mean = 0;
      BlockPos size = tmpl->size;
      Heightmap h = beached ? Heightmap::WORLD_SURFACE_WG : Heightmap::OCEAN_FLOOR_WG;
      int base = size.x * size.z;
      if (base == 0) {
        mean = level.getHeight(h, templatePosition.x, templatePosition.z);
      } else {
        for (int z = templatePosition.z; z <= templatePosition.z + size.z - 1; z++)
          for (int x = templatePosition.x; x <= templatePosition.x + size.x - 1; x++) {
            int hm = level.getHeight(h, x, z);
            mean += hm;
            minY = std::min(minY, hm);
          }
        mean /= base;
      }
      adjustHeight(beached ? beachedPosition(minY, random) : mean);
    }
    TemplatePiece::postProcess(level, random, chunkBB, cx, cz, ref);
  }
  void saveState(std::vector<int>& out) const override {
    TemplatePiece::saveState(out);
    out.push_back(heightAdjusted);
  }
  void loadState(const int*& p) override {
    TemplatePiece::loadState(p);
    heightAdjusted = *p++;
  }
};
}  // namespace

ShipwreckHandle* addShipwreckPiece(BlockPos pos, Rotation r, Builder& builder, RandomSource& random, bool beached) {
  std::string id = std::string("minecraft:shipwreck/") +
                   (beached ? SHIPWRECK_BEACHED[random.nextInt(11)] : SHIPWRECK_OCEAN[random.nextInt(20)]);
  auto p = std::make_unique<ShipwreckPiece>(id, pos, r, beached);
  ShipwreckPiece* raw = p.get();
  builder.add(std::move(p));
  return raw;
}

// ------------------------------------------------------------------------------------------- ocean ruins
namespace {
ProcessorP archyProcessor(bool cold) {
  // CappedProcessor(RuleProcessor([block_match candidate -> suspicious, append_loot]), constant 5)
  Json rule = Json::parse(std::string(R"({"processor_type":"minecraft:capped","limit":5,"delegate":{"processor_type":"minecraft:rule","rules":[{"input_predicate":{"predicate_type":"minecraft:block_match","block":")") +
                          (cold ? "minecraft:gravel" : "minecraft:sand") +
                          R"("},"location_predicate":{"predicate_type":"minecraft:always_true"},"output_state":")" +
                          (cold ? "minecraft:suspicious_gravel" : "minecraft:suspicious_sand") +
                          R"(","block_entity_modifier":{"type":"minecraft:append_loot","loot_table":"x"}}]}})");
  Json list;
  list.type = Json::Array;
  list.arr.push_back(rule);
  return processorList(list)[0];
}

PlaceSettings oceanRuinSettings(Rotation r, float integrity, bool cold) {
  static const ProcessorP WARM = archyProcessor(false), COLD = archyProcessor(true);
  static const ProcessorP IGNORE = blockIgnore({"air", "structure_block"});
  PlaceSettings p;
  p.rotation = r;
  p.processors = {blockRot(integrity), IGNORE, cold ? COLD : WARM};
  return p;
}

struct OceanRuinPiece : TemplatePiece {
  bool cold, large;
  OceanRuinPiece(const std::string& id, BlockPos pos, Rotation r, float integrity, bool cold, bool large)
      : TemplatePiece(0, id, oceanRuinSettings(r, integrity, cold), pos), cold(cold), large(large) {}
  void handleDataMarker(const std::string& marker, BlockPos pos, Level& level, RandomSource& random, const BoundingBox&) override {
    if (marker == "chest") {
      level.set(pos, BS(blocks::chest).with("waterlogged", hasWater(level.get(pos))));
      level.setLootTable(pos, large ? "minecraft:chests/underwater_ruin_big" : "minecraft:chests/underwater_ruin_small", random.nextLong());
    } else if (marker == "drowned") {
      level.addMob(entity_types::drowned, pos, true);
      level.set(pos, pos.y > SEA_LEVEL ? blocks::air : blocks::water);
    }
  }
  int getHeight(BlockPos pos, Level& level, BlockPos corner) {
    const Blocks& b = Blocks::get();
    int newY = pos.y, minY = 512, topY = newY - 1, area = 0;
    for (int z = std::min(pos.z, corner.z); z <= std::max(pos.z, corner.z); z++)
      for (int x = std::min(pos.x, corner.x); x <= std::max(pos.x, corner.x); x++) {
        int floorY = pos.y - 1;
        int st = level.getBlock(x, floorY, z);
        while ((b.isAir(st) || hasWater(st) || b.inTag(st, "ice")) && floorY > MIN_Y + 1) st = level.getBlock(x, --floorY, z);
        minY = std::min(minY, floorY);
        if (floorY < topY - 2) area++;
      }
    int width = std::abs(pos.x - corner.x);
    if (topY - minY > 2 && area > width - 2) newY = minY + 1;
    return newY;
  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int cx, int cz, BlockPos ref) override {
    int h = level.getHeight(Heightmap::OCEAN_FLOOR_WG, templatePosition.x, templatePosition.z);
    templatePosition = {templatePosition.x, h, templatePosition.z};
    BlockPos corner = transform({tmpl->size.x - 1, 0, tmpl->size.z - 1}, Mirror::NONE, settings.rotation, {}).offset(templatePosition);
    templatePosition = {templatePosition.x, getHeight(templatePosition, level, corner), templatePosition.z};
    TemplatePiece::postProcess(level, random, chunkBB, cx, cz, ref);
  }
};

void addOceanRuinPiece(BlockPos pos, Rotation r, Builder& builder, RandomSource& random, bool cold, bool large, float integrity) {
  auto id = [](const char* kind, int n) { return std::string("minecraft:underwater_ruin/") + kind + std::to_string(n); };
  static const int BIG[4] = {1, 2, 3, 8}, BIG_WARM[4] = {4, 5, 6, 7};
  if (!cold) {
    std::string loc = large ? id("big_warm_", BIG_WARM[random.nextInt(4)]) : id("warm_", random.nextInt(8) + 1);
    builder.add(std::make_unique<OceanRuinPiece>(loc, pos, r, integrity, cold, large));
    return;
  }
  int idx = random.nextInt(large ? 4 : 8);
  int n = large ? BIG[idx] : idx + 1;
  const char* pre = large ? "big_" : "";
  builder.add(std::make_unique<OceanRuinPiece>(id((std::string(pre) + "brick_").c_str(), n), pos, r, integrity, cold, large));
  builder.add(std::make_unique<OceanRuinPiece>(id((std::string(pre) + "cracked_").c_str(), n), pos, r, 0.7f, cold, large));
  builder.add(std::make_unique<OceanRuinPiece>(id((std::string(pre) + "mossy_").c_str(), n), pos, r, 0.5f, cold, large));
}
}  // namespace

void addOceanRuinPieces(BlockPos position, Rotation rotation, Builder& builder, RandomSource& random, bool cold,
                        float largeProbability, float clusterProbability) {
  bool large = random.nextFloat() <= largeProbability;
  float baseIntegrity = large ? 0.9f : 0.8f;
  addOceanRuinPiece(position, rotation, builder, random, cold, large, baseIntegrity);
  if (!large || !(random.nextFloat() <= clusterProbability)) return;
  // addClusterRuins
  BlockPos parentPos{position.x, 90, position.z};
  BlockPos parentCorner = transform({15, 0, 15}, Mirror::NONE, rotation, {}).offset(parentPos);
  BoundingBox parentBB = BoundingBox::fromCorners(parentPos, parentCorner);
  BlockPos o{std::min(parentPos.x, parentCorner.x), parentPos.y, std::min(parentPos.z, parentCorner.z)};
  auto ni = [&](int a, int b) { return a >= b ? a : random.nextInt(b - a + 1) + a; };  // Mth.nextInt
  // (draws sequenced explicitly: C++ leaves argument evaluation order unspecified)
  std::vector<BlockPos> all;
  { int dx = -16 + ni(1, 8); int dz = 16 + ni(1, 7); all.push_back(o.offset(dx, 0, dz)); }
  { int dx = -16 + ni(1, 8); int dz = ni(1, 7); all.push_back(o.offset(dx, 0, dz)); }
  { int dx = -16 + ni(1, 8); int dz = -16 + ni(4, 8); all.push_back(o.offset(dx, 0, dz)); }
  { int dx = ni(1, 7); int dz = 16 + ni(1, 7); all.push_back(o.offset(dx, 0, dz)); }
  { int dx = ni(1, 7); int dz = -16 + ni(4, 6); all.push_back(o.offset(dx, 0, dz)); }
  { int dx = 16 + ni(1, 7); int dz = 16 + ni(3, 8); all.push_back(o.offset(dx, 0, dz)); }
  { int dx = 16 + ni(1, 7); int dz = ni(1, 7); all.push_back(o.offset(dx, 0, dz)); }
  { int dx = 16 + ni(1, 7); int dz = -16 + ni(4, 8); all.push_back(o.offset(dx, 0, dz)); }
  int ruins = ni(4, 8);
  for (int i = 0; i < ruins; i++) {
    if (all.empty()) continue;
    int idx = random.nextInt((int)all.size());
    BlockPos pos = all[idx];
    all.erase(all.begin() + idx);
    Rotation next = getRandomRotation(random);
    BlockPos nextCorner = transform({5, 0, 6}, Mirror::NONE, next, {}).offset(pos);
    if (!BoundingBox::fromCorners(pos, nextCorner).intersects(parentBB))
      addOceanRuinPiece(pos, next, builder, random, cold, false, 0.8f);
  }
}

// ------------------------------------------------------------------------------------------- ruined portal
namespace {
PlaceSettings portalSettings(Mirror m, Rotation r, RuinedPortal::Placement vp, BlockPos pivot, const RuinedPortal::Properties& p) {
  PlaceSettings s;
  s.rotation = r;
  s.mirror = m;
  s.pivot = pivot;
  static const ProcessorP STRUCTURE = blockIgnore({"structure_block"}), STRUCTURE_AIR = blockIgnore({"air", "structure_block"});
  s.processors.push_back(p.airPocket ? STRUCTURE : STRUCTURE_AIR);
  auto randomRule = [](const char* src, float prob, const char* dst) {
    return std::string(R"({"input_predicate":{"predicate_type":"minecraft:random_block_match","block":")") + src +
           R"(","probability":)" + std::to_string(prob) + R"(},"location_predicate":{"predicate_type":"minecraft:always_true"},"output_state":")" +
           dst + "\"}";
  };
  auto rule = [](const char* src, const char* dst) {
    return std::string(R"({"input_predicate":{"predicate_type":"minecraft:block_match","block":")") + src +
           R"("},"location_predicate":{"predicate_type":"minecraft:always_true"},"output_state":")" + dst + "\"}";
  };
  std::vector<std::string> rules{randomRule("minecraft:gold_block", 0.3f, "minecraft:air")};
  if (vp == RuinedPortal::ON_OCEAN_FLOOR) rules.push_back(rule("minecraft:lava", "minecraft:magma_block"));
  else if (p.cold) rules.push_back(rule("minecraft:lava", "minecraft:netherrack"));
  else rules.push_back(randomRule("minecraft:lava", 0.2f, "minecraft:magma_block"));
  if (!p.cold) rules.push_back(randomRule("minecraft:netherrack", 0.07f, "minecraft:magma_block"));
  std::string j = R"([{"processor_type":"minecraft:rule","rules":[)";
  for (size_t i = 0; i < rules.size(); i++) j += (i ? "," : "") + rules[i];
  j += "]},{\"processor_type\":\"minecraft:protected_blocks\",\"value\":\"#minecraft:features_cannot_replace\"}]";
  static std::mutex cacheLock;
  static std::map<std::string, const ProcessorList*> cache;  // 3 distinct rule sets
  const ProcessorList* listp;
  {
    std::lock_guard lock(cacheLock);
    auto& slot = cache[j];
    if (!slot) slot = &processorList(Json::parse(j));
    listp = slot;
  }
  const ProcessorList& list = *listp;
  s.processors.push_back(list[0]);
  s.processors.push_back(blockAge(p.mossiness));
  s.processors.push_back(list[1]);
  s.processors.push_back(lavaSubmerged());
  if (p.replaceWithBlackstone) s.processors.push_back(blackstoneReplace());
  return s;
}

struct RuinedPortalPiece : TemplatePiece {
  RuinedPortal::Placement vp;
  RuinedPortal::Properties props;
  RuinedPortalPiece(BlockPos pos, RuinedPortal::Placement vp, const RuinedPortal::Properties& p, const std::string& id, Rotation r,
                    Mirror m, BlockPos pivot)
      : TemplatePiece(0, id, portalSettings(m, r, vp, pivot, p), pos), vp(vp), props(p) {}

  static Heightmap heightmapFor(RuinedPortal::Placement vp) {
    return vp == RuinedPortal::ON_OCEAN_FLOOR ? Heightmap::OCEAN_FLOOR_WG : Heightmap::WORLD_SURFACE_WG;
  }
  bool canReplace(int state) const {
    const Blocks& b = Blocks::get();
    return !b.is(state, "air") && !b.is(state, "obsidian") && !b.inTag(state, "features_cannot_replace") &&
           (vp == RuinedPortal::IN_NETHER || !b.is(state, "lava"));
  }
  void placeNetherrackOrMagma(RandomSource& random, Level& level, BlockPos pos) {
    if (!canReplace(level.get(pos))) return;
    if (!props.cold && random.nextFloat() < 0.07f) level.set(pos, blocks::magma_block);
    else level.set(pos, blocks::netherrack);
  }
  void addDripColumn(RandomSource& random, Level& level, BlockPos pos) {
    placeNetherrackOrMagma(random, level, pos);
    int cap = 8;
    while (cap > 0 && random.nextFloat() < 0.5f) {
      pos = pos.below();
      cap--;
      placeNetherrackOrMagma(random, level, pos);
    }
  }
  void maybeAddLeavesAbove(RandomSource& random, Level& level, BlockPos pos) {
    if (random.nextFloat() < 0.5f && BS(level.get(pos)).is(blocks::netherrack) && BS(level.get(pos.above())).isAir())
      level.set(pos.above(), BS(blocks::jungle_leaves).with("persistent", true));
  }
  void maybeAddVines(RandomSource& random, Level& level, BlockPos pos) {
    BS state = level.get(pos);
    if (state.isAir() || state.is(blocks::vine)) return;
    Dir d = randomHorizontal(random);
    BlockPos n = pos.relative(d);
    if (!BS(level.get(n)).isAir()) return;
    if (Blocks::get().faceFull(state, (int)d)) level.set(n, BS(blocks::vine).with(dirName(opposite(d)), true));
  }
  void spreadNetherrack(RandomSource& random, Level& level) {
    bool follow = vp == RuinedPortal::ON_LAND_SURFACE || vp == RuinedPortal::ON_OCEAN_FLOOR;
    BlockPos c = boundingBox.center();
    static const float P[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.9f, 0.9f, 0.8f, 0.7f, 0.6f, 0.4f, 0.2f};
    int maxDistance = 14;
    int averageWidth = (boundingBox.xSpan() + boundingBox.zSpan()) / 2;
    int adj = random.nextInt(std::max(1, 8 - averageWidth / 2));
    for (int x = c.x - maxDistance; x <= c.x + maxDistance; x++)
      for (int z = c.z - maxDistance; z <= c.z + maxDistance; z++) {
        int distance = std::abs(x - c.x) + std::abs(z - c.z);
        int ad = std::max(0, distance + adj);
        if (ad >= maxDistance) continue;
        if (!(random.nextDouble() < (double)P[ad])) continue;
        int surfaceY = level.getHeight(heightmapFor(vp), x, z) - 1;
        int y = follow ? surfaceY : std::min(boundingBox.minY, surfaceY);
        BlockPos pos{x, y, z};
        if (std::abs(y - boundingBox.minY) <= 3 && canReplace(level.get(pos))) {
          placeNetherrackOrMagma(random, level, pos);
          if (props.overgrown) maybeAddLeavesAbove(random, level, pos);
          addDripColumn(random, level, pos.below());
        }
      }
  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int cx, int cz, BlockPos ref) override {
    BoundingBox bb = templateBoundingBox(*tmpl, settings, templatePosition);
    if (!chunkBB.isInside(bb.center())) return;
    const_cast<BoundingBox&>(chunkBB).encapsulate(bb);  // vanilla mutates the writable-area box
    TemplatePiece::postProcess(level, random, chunkBB, cx, cz, ref);
    spreadNetherrack(random, level);
    for (int x = boundingBox.minX + 1; x < boundingBox.maxX; x++)
      for (int z = boundingBox.minZ + 1; z < boundingBox.maxZ; z++) {
        BlockPos p{x, boundingBox.minY, z};
        if (BS(level.get(p)).is(blocks::netherrack)) addDripColumn(random, level, p.below());
      }
    if (props.vines || props.overgrown)
      for (int z = bb.minZ; z <= bb.maxZ; z++)  // BlockPos.betweenClosedStream: x fastest, then y, then z
        for (int y = bb.minY; y <= bb.maxY; y++)
          for (int x = bb.minX; x <= bb.maxX; x++) {
            if (props.vines) maybeAddVines(random, level, {x, y, z});
            if (props.overgrown) maybeAddLeavesAbove(random, level, {x, y, z});
          }
  }
};
}  // namespace

PieceP makeRuinedPortalPiece(BlockPos pos, RuinedPortal::Placement vp, const RuinedPortal::Properties& p, const std::string& id,
                             Rotation r, Mirror m, BlockPos pivot) {
  return std::make_unique<RuinedPortalPiece>(pos, vp, p, id, r, m, pivot);
}

// ------------------------------------------------------------------------------------------- nether fossil
namespace {
struct NetherFossilPiece : TemplatePiece {
  NetherFossilPiece(const std::string& id, BlockPos pos, Rotation r) : TemplatePiece(0, id, settingsFor(r), pos) {}
  static PlaceSettings settingsFor(Rotation r) {
    PlaceSettings p;
    p.rotation = r;
    static const ProcessorP IGNORE = blockIgnore({"air", "structure_block"});
    p.processors.push_back(IGNORE);
    return p;
  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int cx, int cz, BlockPos ref) override {
    BoundingBox fossil = templateBoundingBox(*tmpl, settings, templatePosition);
    const_cast<BoundingBox&>(chunkBB).encapsulate(fossil);
    TemplatePiece::postProcess(level, random, chunkBB, cx, cz, ref);
    auto r = positionalRandom(level.seed(), fossil.center());  // placeDriedGhast
    if (r->nextFloat() < 0.5f) {
      int x = fossil.minX + r->nextInt(fossil.xSpan());
      int y = fossil.minY;
      int z = fossil.minZ + r->nextInt(fossil.zSpan());
      BlockPos p{x, y, z};
      if (BS(level.get(p)).isAir() && chunkBB.isInside(p)) level.set(p, BS(blocks::dried_ghast).rotate(getRandomRotation(*r)));
    }
  }
};
}  // namespace

void addNetherFossilPieces(Builder& builder, RandomSource& random, BlockPos pos) {
  Rotation r = getRandomRotation(random);
  builder.add(std::make_unique<NetherFossilPiece>("minecraft:nether_fossils/fossil_" + std::to_string(random.nextInt(14) + 1), pos, r));
}

}  // namespace structure
