// Structure types (Structure.findGenerationPoint per structure_type) and the makeStructure factory.
#include "worldgen/json.hpp"
#include "worldgen/structure/jigsaw.hpp"
#include "worldgen/structure/legacy.hpp"
#include "worldgen/structure/pieces.hpp"

namespace structure {
namespace {

using GenFn = std::function<void(Builder&)>;

struct BuriedTreasure : Structure {
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    int cx = ctx.cx, cz = ctx.cz;
    return ctx.onTopOfChunkCenter(Heightmap::OCEAN_FLOOR_WG, [cx, cz](Builder& b) { b.add(makeBuriedTreasure({cx * 16 + 9, 90, cz * 16 + 9})); });
  }
};

// SinglePieceStructure (desert pyramid 21x21, jungle temple 12x15)
struct SinglePiece : Structure {
  int width, depth;
  PieceP (*make)(RandomSource&, int, int);
  bool desert;
  SinglePiece(int w, int d, PieceP (*m)(RandomSource&, int, int), bool desert) : width(w), depth(d), make(m), desert(desert) {}
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    if (!ctx.couldValidBiomeExistOnTopOfChunkCenter()) return std::nullopt;
    if (ctx.lowestY(ctx.cx * 16, ctx.cz * 16, width, depth) < SEA_LEVEL) return std::nullopt;
    RandomSource* r = &ctx.random;
    int x = ctx.cx * 16, z = ctx.cz * 16;
    auto m = make;
    return ctx.onTopOfChunkCenter(Heightmap::WORLD_SURFACE_WG, [=](Builder& b) { b.add(m(*r, x, z)); }, false);
  }
  void afterPlace(Level& level, RandomSource&, const BoundingBox& chunkBB, int, int, const std::vector<PieceP>& pieces) const override {
    if (desert) desertPyramidAfterPlace(level, chunkBB, pieces);
  }
};

struct SwampHut : Structure {
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    RandomSource* r = &ctx.random;
    int x = ctx.cx * 16, z = ctx.cz * 16;
    return ctx.onTopOfChunkCenter(Heightmap::WORLD_SURFACE_WG, [=](Builder& b) { b.add(makeSwampHut(*r, x, z)); });
  }
};

struct Igloo : Structure {
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    RandomSource* r = &ctx.random;
    int x = ctx.cx * 16, z = ctx.cz * 16;
    return ctx.onTopOfChunkCenter(Heightmap::WORLD_SURFACE_WG, [=](Builder& b) {
      Rotation rot = getRandomRotation(*r);
      addIglooPieces({x, 90, z}, rot, b, *r);
    });
  }
};

struct Shipwreck : Structure {
  bool beached;
  explicit Shipwreck(bool b) : beached(b) {}
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    GenContext* c = &ctx;
    bool be = beached;
    return ctx.onTopOfChunkCenter(be ? Heightmap::WORLD_SURFACE_WG : Heightmap::OCEAN_FLOOR_WG, [c, be](Builder& b) {
      Rotation rot = getRandomRotation(c->random);
      ShipwreckHandle* p = addShipwreckPiece({c->cx * 16, 90, c->cz * 16}, rot, b, c->random, be);
      if (p->tooBig()) {
        BoundingBox bb = p->box();
        int h = be ? p->beachedPosition(c->lowestY(bb.minX, bb.minZ, bb.xSpan(), bb.zSpan()), c->random)
                   : c->meanFirstOccupiedHeight(bb.minX, bb.xSpan(), bb.minZ, bb.zSpan());
        p->adjustHeight(h);
      }
    });
  }
};

struct OceanRuin : Structure {
  bool cold;
  float large, cluster;
  explicit OceanRuin(const Json& j)
      : cold(j["biome_temp"].asString() == "cold"), large(j["large_probability"].asFloat()), cluster(j["cluster_probability"].asFloat()) {}
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    GenContext* c = &ctx;
    bool co = cold;
    float l = large, cl = cluster;
    return ctx.onTopOfChunkCenter(Heightmap::OCEAN_FLOOR_WG, [=](Builder& b) {
      Rotation rot = getRandomRotation(c->random);
      addOceanRuinPieces({c->cx * 16, 90, c->cz * 16}, rot, b, c->random, co, l, cl);
    });
  }
};

struct RuinedPortalStructure : Structure {
  struct Setup {
    RuinedPortal::Placement placement;
    float airPocket, mossiness;
    bool overgrown, vines, canBeCold, blackstone;
    float weight;
  };
  std::vector<Setup> setups;
  explicit RuinedPortalStructure(const Json& j) {
    for (auto& s : j["setups"].arr) {
      std::string p = s["placement"].asString();
      static const char* names[] = {"on_land_surface", "partly_buried", "on_ocean_floor", "in_mountain", "underground", "in_nether"};
      Setup x{};
      for (int i = 0; i < 6; i++)
        if (p == names[i]) x.placement = (RuinedPortal::Placement)i;
      x.airPocket = s["air_pocket_probability"].asFloat(), x.mossiness = s["mossiness"].asFloat();
      x.overgrown = s["overgrown"].asBool(), x.vines = s["vines"].asBool(), x.canBeCold = s["can_be_cold"].asBool();
      x.blackstone = s["replace_with_blackstone"].asBool(), x.weight = s["weight"].asFloat();
      setups.push_back(x);
    }
  }
  static int randomWithin(RandomSource& r, int minPreferred, int max) {
    return minPreferred < max ? randomBetweenInclusive(r, minPreferred, max) : max;
  }
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    RandomSource& random = ctx.random;
    const Setup* chosen = nullptr;
    if (setups.size() > 1) {
      float total = 0;
      for (auto& s : setups) total += s.weight;
      float pick = random.nextFloat();
      for (auto& s : setups) {
        pick -= s.weight / total;
        if (pick < 0.0f) {
          chosen = &s;
          break;
        }
      }
    } else {
      chosen = &setups[0];
    }
    if (!chosen) return std::nullopt;  // vanilla throws
    Setup setup = *chosen;
    bool airPocket = setup.airPocket == 0.0f ? false : setup.airPocket == 1.0f ? true : random.nextFloat() < setup.airPocket;
    std::string loc;
    if (random.nextFloat() < 0.05f) loc = "minecraft:ruined_portal/giant_portal_" + std::to_string(random.nextInt(3) + 1);
    else loc = "minecraft:ruined_portal/portal_" + std::to_string(random.nextInt(10) + 1);
    const Template& t = Template::get(loc);
    Rotation rotation = getRandomRotation(random);
    Mirror mirror = random.nextFloat() < 0.5f ? Mirror::NONE : Mirror::FRONT_BACK;
    BlockPos pivot{t.size.x / 2, 0, t.size.z / 2};
    BlockPos base{ctx.cx * 16, 0, ctx.cz * 16};
    PlaceSettings s;
    s.rotation = rotation, s.mirror = mirror, s.pivot = pivot;
    BoundingBox bb = templateBoundingBox(t, s, base);
    BlockPos center = bb.center();
    Heightmap hm = setup.placement == RuinedPortal::ON_OCEAN_FLOOR ? Heightmap::OCEAN_FLOOR_WG : Heightmap::WORLD_SURFACE_WG;
    int surfaceY = ctx.gen.baseHeight(center.x, center.z, hm) - 1;
    // findSuitableY
    int minY = ctx.gen.minBuildHeight() + 15, ySpan = bb.ySpan(), newY;
    switch (setup.placement) {
      case RuinedPortal::IN_NETHER:
        if (airPocket) newY = randomBetweenInclusive(random, 32, 100);
        else if (random.nextFloat() < 0.5f) newY = randomBetweenInclusive(random, 27, 29);
        else newY = randomBetweenInclusive(random, 29, 100);
        break;
      case RuinedPortal::IN_MOUNTAIN: newY = randomWithin(random, 70, surfaceY - ySpan); break;
      case RuinedPortal::UNDERGROUND: newY = randomWithin(random, minY, surfaceY - ySpan); break;
      case RuinedPortal::PARTLY_BURIED: newY = surfaceY - ySpan + randomBetweenInclusive(random, 2, 8); break;
      default: newY = surfaceY;
    }
    std::vector<int> cols[4];
    BlockPos corners[4] = {{bb.minX, 0, bb.minZ}, {bb.maxX, 0, bb.minZ}, {bb.minX, 0, bb.maxZ}, {bb.maxX, 0, bb.maxZ}};
    for (int i = 0; i < 4; i++) ctx.gen.baseColumn(corners[i].x, corners[i].z, cols[i]);
    bool ocean = setup.placement == RuinedPortal::ON_OCEAN_FLOOR;
    const bs::Tag& motion = Blocks::get().tag("blocks_motion_in_heightmap");
    int projectedY;
    for (projectedY = newY; projectedY > minY; projectedY--) {
      int solid = 0;
      bool done = false;
      for (auto& c : cols) {
        int idx = projectedY - ctx.gen.minBuildHeight();
        int st = idx >= 0 && idx < int(c.size()) ? c[idx] : blocks::air;  // NoiseColumn.getBlock: air outside
        bool opaque = ocean ? motion.has(Blocks::get().blockOf(st)) : !Blocks::get().isAir(st);
        if (opaque && ++solid == 3) {
          done = true;
          break;
        }
      }
      if (done) break;
    }
    BlockPos origin{base.x, projectedY, base.z};
    Generator* gen = &ctx.gen;
    return Stub{origin, [=](Builder& b) {
                  bool cold = false;
                  if (setup.canBeCold) {
                    int biome = gen->noiseBiome(origin.x >> 2, origin.y >> 2, origin.z >> 2);
                    cold = gen->biomeTemperature(biome, origin) < 0.15f;
                  }
                  RuinedPortal::Properties p{cold, setup.mossiness, airPocket, setup.overgrown, setup.vines, setup.blackstone};
                  b.add(makeRuinedPortalPiece(origin, setup.placement, p, loc, rotation, mirror, pivot));
                },
                nullptr};
  }
};

struct NetherFossil : Structure {
  Json height;
  explicit NetherFossil(const Json& j) : height(j["height"]) {}
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    RandomSource& random = ctx.random;
    int x = ctx.cx * 16 + random.nextInt(16), z = ctx.cz * 16 + random.nextInt(16);
    int y = sampleHeight(height, random, ctx.gen.minBuildHeight(), ctx.gen.generationHeight());
    std::vector<int> col;
    ctx.gen.baseColumn(x, z, col);
    const Blocks& b = Blocks::get();
    auto at = [&](int yy) { int i = yy - ctx.gen.minBuildHeight(); return i >= 0 && i < int(col.size()) ? col[i] : blocks::air; };
    while (y > ctx.gen.seaLevel()) {
      int current = at(y), below = at(--y);
      if (b.isAir(current) && (b.is(below, "soul_sand") || (b.flags(below) & BS_STURDY_UP))) break;
    }
    if (y <= ctx.gen.seaLevel()) return std::nullopt;
    BlockPos pos{x, y, z};
    RandomSource* r = &ctx.random;
    return Stub{pos, [=](Builder& bl) { addNetherFossilPieces(bl, *r, pos); }, nullptr};
  }
};

struct Mineshaft : Structure {
  bool mesa;
  explicit Mineshaft(bool m) : mesa(m) {}
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    ctx.random.nextDouble();
    BlockPos start{ctx.cx * 16 + 8, 50, ctx.cz * 16};
    auto b = std::make_unique<Builder>();
    generateMineshaft(*b, ctx.random, ctx.cx * 16 + 2, ctx.cz * 16 + 2, mesa);
    int dy;
    if (mesa) {
      BlockPos c = b->boundingBox().center();
      int surface = ctx.gen.baseHeight(c.x, c.z, Heightmap::WORLD_SURFACE_WG);
      int target = surface <= SEA_LEVEL ? SEA_LEVEL : randomBetweenInclusive(ctx.random, SEA_LEVEL, surface);
      dy = target - c.y;
      b->offsetPiecesVertically(dy);
    } else {
      dy = b->moveBelowSeaLevel(SEA_LEVEL, MIN_Y, ctx.random, 10);
    }
    return Stub{start.offset(0, dy, 0), nullptr, std::move(b)};
  }
};

struct Stronghold : Structure {
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    GenContext* c = &ctx;
    return Stub{{ctx.cx * 16, 0, ctx.cz * 16}, [c](Builder& b) {
                  int tries = 0;
                  StrongholdResult r;
                  do {
                    b.clear();
                    c->random.setLargeFeatureSeed(c->seed + tries++, c->cx, c->cz);
                    r = generateStronghold(b, c->random, c->cx * 16 + 2, c->cz * 16 + 2);
                    b.moveBelowSeaLevel(SEA_LEVEL, MIN_Y, c->random, 10);
                  } while (b.empty() || !r.hasPortalRoom);
                },
                nullptr};
  }
};

struct Fortress : Structure {
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    GenContext* c = &ctx;
    return Stub{{ctx.cx * 16, 64, ctx.cz * 16}, [c](Builder& b) {
                  generateFortress(b, c->random, c->cx * 16 + 2, c->cz * 16 + 2);
                  b.moveInsideHeights(c->random, 48, 70);
                },
                nullptr};
  }
};

struct Monument : Structure {
  std::vector<uint8_t> surrounding = biomeSet(Json::parse("\"#minecraft:required_ocean_monument_surrounding\""));
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    int x = ctx.cx * 16 + 9, z = ctx.cz * 16 + 9, y = SEA_LEVEL, r = 29;
    for (int qz = (z - r) >> 2; qz <= (z + r) >> 2; qz++)  // BiomeResolver.getBiomesWithin
      for (int qx = (x - r) >> 2; qx <= (x + r) >> 2; qx++)
        for (int qy = (y - r) >> 2; qy <= (y + r) >> 2; qy++)
          if (!surrounding[ctx.gen.noiseBiome(qx, qy, qz)]) return std::nullopt;
    GenContext* c = &ctx;
    return ctx.onTopOfChunkCenter(Heightmap::OCEAN_FLOOR_WG, [c](Builder& b) {
      Dir o = randomHorizontal(c->random);
      b.add(makeMonumentBuilding(c->random, c->cx * 16 - 29, c->cz * 16 - 29, o));
    });
  }
};

// Structure.getLowestYIn5by5Box
BlockPos lowestYIn5by5Box(GenContext& ctx, int x, int z, Rotation r) {
  int ox = 5, oz = 5;
  if (r == Rotation::CLOCKWISE_90) ox = -5;
  else if (r == Rotation::CLOCKWISE_180) ox = -5, oz = -5;
  else if (r == Rotation::COUNTERCLOCKWISE_90) oz = -5;
  return {x, ctx.lowestY(x, z, ox, oz), z};
}

struct Mansion : Structure {
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    int x = ctx.cx * 16 + 7, z = ctx.cz * 16 + 7;
    if (!ctx.couldValidBiomeExistInTerrainColumn(x, z)) return std::nullopt;
    Rotation r = getRandomRotation(ctx.random);
    BlockPos start = lowestYIn5by5Box(ctx, x, z, r);
    if (start.y < 60) return std::nullopt;
    RandomSource* rnd = &ctx.random;
    return Stub{start, [=](Builder& b) { generateMansion(start, r, b, *rnd); }, nullptr};
  }
  void afterPlace(Level& level, RandomSource&, const BoundingBox& chunkBB, int, int, const std::vector<PieceP>& pieces) const override {
    BoundingBox bb = pieces[0]->boundingBox;
    for (auto& p : pieces) bb.encapsulate(p->boundingBox);
    int yStart = bb.minY;
    const Blocks& b = Blocks::get();
    for (int x = chunkBB.minX; x <= chunkBB.maxX; x++)
      for (int z = chunkBB.minZ; z <= chunkBB.maxZ; z++) {
        if (level.isEmpty(x, yStart, z) || !bb.isInside(x, yStart, z)) continue;
        bool inside = false;
        for (auto& p : pieces)
          if (p->boundingBox.isInside(x, yStart, z)) inside = true;
        if (!inside) continue;
        for (int y = yStart - 1; y > MIN_Y; y--) {
          int s = level.getBlock(x, y, z);
          if (!b.isAir(s) && !(b.flags(s) & BS_LIQUID)) break;
          level.setBlock(x, y, z, blocks::cobblestone);
        }
      }
  }
};

struct EndCity : Structure {
  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    int x = ctx.cx * 16 + 7, z = ctx.cz * 16 + 7;
    if (!ctx.couldValidBiomeExistInTerrainColumn(x, z)) return std::nullopt;
    Rotation r = getRandomRotation(ctx.random);
    BlockPos start = lowestYIn5by5Box(ctx, x, z, r);
    if (start.y < 60) return std::nullopt;
    RandomSource* rnd = &ctx.random;
    return Stub{start, [=](Builder& b) { generateEndCity(start, r, b, *rnd); }, nullptr};
  }
};

}  // namespace

std::unique_ptr<Structure> makeStructure(const std::string& type, const Json& j) {
  if (type == "jigsaw") return makeJigsawStructure(j);
  if (type == "buried_treasure") return std::make_unique<BuriedTreasure>();
  if (type == "desert_pyramid") return std::make_unique<SinglePiece>(21, 21, makeDesertPyramid, true);
  if (type == "jungle_temple") return std::make_unique<SinglePiece>(12, 15, makeJungleTemple, false);
  if (type == "swamp_hut") return std::make_unique<SwampHut>();
  if (type == "igloo") return std::make_unique<Igloo>();
  if (type == "shipwreck") return std::make_unique<Shipwreck>(j.find("is_beached") && j["is_beached"].asBool());
  if (type == "ocean_ruin") return std::make_unique<OceanRuin>(j);
  if (type == "ruined_portal") return std::make_unique<RuinedPortalStructure>(j);
  if (type == "nether_fossil") return std::make_unique<NetherFossil>(j);
  if (type == "mineshaft") return std::make_unique<Mineshaft>(j["mineshaft_type"].asString() == "mesa");
  if (type == "stronghold") return std::make_unique<Stronghold>();
  if (type == "fortress") return std::make_unique<Fortress>();
  if (type == "ocean_monument") return std::make_unique<Monument>();
  if (type == "woodland_mansion") return std::make_unique<Mansion>();
  if (type == "end_city") return std::make_unique<EndCity>();
  return nullptr;
}

}  // namespace structure
