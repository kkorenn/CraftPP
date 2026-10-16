// MineshaftPieces (26.3): room, corridors, crossings, stairs.
#include <cstdlib>

#include "worldgen/structure/legacy.hpp"
#include "worldgen/structure/pieces.hpp"

namespace structure {
namespace {

struct MineType { int wood, planks, fence; };
const MineType NORMAL{blocks::oak_log, blocks::oak_planks, blocks::oak_fence};
const MineType MESA{blocks::dark_oak_log, blocks::dark_oak_planks, blocks::dark_oak_fence};
const int CAVE_AIR = blocks::cave_air;

bool sturdy(int s, Dir d) { return bs::R().sturdy(s, bs::Dir(d)); }
bool liquid(int s) { return Blocks::get().flags(s) & BS_LIQUID; }
bool isFallingBlock(int s) {
  static const std::vector<int> list = [] {
    std::vector<int> v;
    for (const char* n : {"sand", "red_sand", "gravel", "anvil", "chipped_anvil", "damaged_anvil", "dragon_egg"})
      v.push_back(Blocks::get().blockIndex(n));
    for (const char* c : {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray", "light_gray", "cyan",
                          "purple", "blue", "brown", "green", "red", "black"})
      v.push_back(Blocks::get().blockIndex(std::string(c) + "_concrete_powder"));
    return v;
  }();
  int b = Blocks::get().blockOf(s);
  return std::find(list.begin(), list.end(), b) != list.end();
}

struct MinePiece : Piece {
  const MineType* type;
  MinePiece(int depth, const MineType* t, const BoundingBox& bb) : Piece(depth, bb), type(t) {}
  bool canBeReplaced(Level& level, int x, int y, int z, const BoundingBox& chunkBB) override {
    BS s = getBlock(level, x, y, z, chunkBB);
    return !s.is(type->planks) && !s.is(type->wood) && !s.is(type->fence) && !s.is(blocks::iron_chain);
  }
  bool isSupportingBox(Level& level, const BoundingBox& chunkBB, int x0, int x1, int y1, int z0) {
    for (int x = x0; x <= x1; x++)
      if (BS(getBlock(level, x, y1 + 1, z0, chunkBB)).isAir()) return false;
    return true;
  }
  bool isInInvalidLocation(Level& level, const BoundingBox& chunkBB) {
    int x0 = std::max(boundingBox.minX - 1, chunkBB.minX), y0 = std::max(boundingBox.minY - 1, chunkBB.minY);
    int z0 = std::max(boundingBox.minZ - 1, chunkBB.minZ), x1 = std::min(boundingBox.maxX + 1, chunkBB.maxX);
    int y1 = std::min(boundingBox.maxY + 1, chunkBB.maxY), z1 = std::min(boundingBox.maxZ + 1, chunkBB.maxZ);
    static const std::vector<uint8_t> blocking = biomeSet(Json::parse("\"#minecraft:mineshaft_blocking\""));
    if (blocking[level.biomeAt((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)]) return true;
    for (int x = x0; x <= x1; x++)
      for (int z = z0; z <= z1; z++)
        if (liquid(level.getBlock(x, y0, z)) || liquid(level.getBlock(x, y1, z))) return true;
    for (int x = x0; x <= x1; x++)
      for (int y = y0; y <= y1; y++)
        if (liquid(level.getBlock(x, y, z0)) || liquid(level.getBlock(x, y, z1))) return true;
    for (int z = z0; z <= z1; z++)
      for (int y = y0; y <= y1; y++)
        if (liquid(level.getBlock(x0, y, z)) || liquid(level.getBlock(x1, y, z))) return true;
    return false;
  }
  void setPlanksBlock(Level& level, const BoundingBox& chunkBB, int planks, int x, int y, int z) {
    if (!isInterior(level, x, y, z, chunkBB)) return;
    BlockPos p = worldPos(x, y, z);
    if (!sturdy(level.get(p), Dir::UP)) level.set(p, planks);
  }
};

PieceP createRandomShaftPiece(Builder& b, RandomSource& r, int x, int y, int z, Dir d, int depth, const MineType* t);

MinePiece* generateAndAddPiece(Piece* start, Builder& builder, RandomSource& random, int x, int y, int z, Dir d, int depth) {
  if (depth > 8) return nullptr;
  if (std::abs(x - start->boundingBox.minX) > 80 || std::abs(z - start->boundingBox.minZ) > 80) return nullptr;
  const MineType* t = static_cast<MinePiece*>(start)->type;
  PieceP p = createRandomShaftPiece(builder, random, x, y, z, d, depth + 1, t);
  if (!p) return nullptr;
  Piece* raw = p.get();
  builder.add(std::move(p));
  raw->addChildren(start, builder, random);
  return static_cast<MinePiece*>(raw);
}

// ------------------------------------------------------------------------------------------- corridor
struct Corridor : MinePiece {
  bool hasRails, spiderCorridor, hasPlacedSpider = false;
  int numSections;
  Corridor(int depth, RandomSource& random, const BoundingBox& bb, Dir d, const MineType* t) : MinePiece(depth, t, bb) {
    setOrientation(d);
    hasRails = random.nextInt(3) == 0;
    spiderCorridor = !hasRails && random.nextInt(23) == 0;
    numSections = isZAxis(*orientation()) ? bb.zSpan() / 5 : bb.xSpan() / 5;
  }
  void saveState(std::vector<int>& out) const override {
    Piece::saveState(out);
    out.push_back(hasPlacedSpider);
  }
  void loadState(const int*& p) override {
    Piece::loadState(p);
    hasPlacedSpider = *p++;
  }
  static std::optional<BoundingBox> findCorridorSize(Builder& builder, RandomSource& random, int x, int y, int z, Dir d) {
    for (int len = random.nextInt(3) + 2; len > 0; len--) {
      int bl = len * 5;
      BoundingBox box;
      switch (d) {
        case Dir::SOUTH: box = BoundingBox(0, 0, 0, 2, 2, bl - 1); break;
        case Dir::WEST: box = BoundingBox(-(bl - 1), 0, 0, 0, 2, 2); break;
        case Dir::EAST: box = BoundingBox(0, 0, 0, bl - 1, 2, 2); break;
        default: box = BoundingBox(0, 0, -(bl - 1), 2, 2, 0);
      }
      box.move(x, y, z);
      if (!builder.findCollisionPiece(box)) return box;
    }
    return std::nullopt;
  }
  void addChildren(Piece* start, Builder& builder, RandomSource& random) override {
    int depth = genDepth;
    int end = random.nextInt(4);
    Dir o = *orientation();
    const BoundingBox& b = boundingBox;
    auto add = [&](int x, int y, int z, Dir d, int dep) { generateAndAddPiece(start, builder, random, x, y, z, d, dep); };
    switch (o) {
      case Dir::SOUTH:
        if (end <= 1) add(b.minX, b.minY - 1 + random.nextInt(3), b.maxZ + 1, o, depth);
        else if (end == 2) add(b.minX - 1, b.minY - 1 + random.nextInt(3), b.maxZ - 3, Dir::WEST, depth);
        else add(b.maxX + 1, b.minY - 1 + random.nextInt(3), b.maxZ - 3, Dir::EAST, depth);
        break;
      case Dir::WEST:
        if (end <= 1) add(b.minX - 1, b.minY - 1 + random.nextInt(3), b.minZ, o, depth);
        else if (end == 2) add(b.minX, b.minY - 1 + random.nextInt(3), b.minZ - 1, Dir::NORTH, depth);
        else add(b.minX, b.minY - 1 + random.nextInt(3), b.maxZ + 1, Dir::SOUTH, depth);
        break;
      case Dir::EAST:
        if (end <= 1) add(b.maxX + 1, b.minY - 1 + random.nextInt(3), b.minZ, o, depth);
        else if (end == 2) add(b.maxX - 3, b.minY - 1 + random.nextInt(3), b.minZ - 1, Dir::NORTH, depth);
        else add(b.maxX - 3, b.minY - 1 + random.nextInt(3), b.maxZ + 1, Dir::SOUTH, depth);
        break;
      default:
        if (end <= 1) add(b.minX, b.minY - 1 + random.nextInt(3), b.minZ - 1, o, depth);
        else if (end == 2) add(b.minX - 1, b.minY - 1 + random.nextInt(3), b.minZ, Dir::WEST, depth);
        else add(b.maxX + 1, b.minY - 1 + random.nextInt(3), b.minZ, Dir::EAST, depth);
    }
    if (depth < 8) {
      if (o != Dir::NORTH && o != Dir::SOUTH) {
        for (int x = b.minX + 3; x + 3 <= b.maxX; x += 5) {
          int s = random.nextInt(5);
          if (s == 0) add(x, b.minY, b.minZ - 1, Dir::NORTH, depth + 1);
          else if (s == 1) add(x, b.minY, b.maxZ + 1, Dir::SOUTH, depth + 1);
        }
      } else {
        for (int z = b.minZ + 3; z + 3 <= b.maxZ; z += 5) {
          int s = random.nextInt(5);
          if (s == 0) add(b.minX - 1, b.minY, z, Dir::WEST, depth + 1);
          else if (s == 1) add(b.maxX + 1, b.minY, z, Dir::EAST, depth + 1);
        }
      }
    }
  }
  bool createMinecartChest(Level& level, const BoundingBox& chunkBB, RandomSource& random, int x, int y, int z) {
    BlockPos p = worldPos(x, y, z);
    if (!chunkBB.isInside(p) || !BS(level.get(p)).isAir() || BS(level.get(p.below())).isAir()) return false;
    BS state = BS(blocks::rail).with("shape", random.nextBoolean() ? "north_south" : "east_west");
    placeBlock(level, state, x, y, z, chunkBB);
    random.nextLong();  // ponytail: chest minecart entity not spawned; its loot seed is still drawn
    return true;
  }
  void fillPillarDownOrChainUp(Level& level, int pillar, int x, int y, int z, const BoundingBox& chunkBB) {
    BlockPos p = worldPos(x, y, z);
    if (!chunkBB.isInside(p)) return;
    int worldY = p.y;
    bool checkBelow = true, checkAbove = true;
    for (int d = 1; checkBelow || checkAbove; d++) {
      if (checkBelow) {
        p.y = worldY - d;
        int below = level.get(p);
        bool emptyBelow = isReplaceableByStructures(below) && !BS(below).is(blocks::lava);
        if (!emptyBelow && sturdy(below, Dir::UP)) {
          for (int yy = worldY - d + 1; yy < worldY; yy++) level.setBlock(p.x, yy, p.z, pillar);
          return;
        }
        checkBelow = d <= 20 && emptyBelow && p.y > MIN_Y + 1;
      }
      if (checkAbove) {
        p.y = worldY + d;
        int above = level.get(p);
        bool emptyAbove = isReplaceableByStructures(above);
        if (!emptyAbove && bs::R().sturdy(above, bs::DOWN, bs::CENTER) && !isFallingBlock(above)) {
          level.setBlock(p.x, worldY + 1, p.z, type->fence);
          for (int yy = worldY + 2; yy < worldY + d; yy++) level.setBlock(p.x, yy, p.z, blocks::iron_chain);
          return;
        }
        checkAbove = d <= 50 && emptyAbove && p.y < MAX_Y;
      }
    }
  }
  void placeDoubleLowerOrUpperSupport(Level& level, const BoundingBox& chunkBB, int x, int y, int z) {
    if (BS(getBlock(level, x, y, z, chunkBB)).is(type->planks)) fillPillarDownOrChainUp(level, type->wood, x, y, z, chunkBB);
    if (BS(getBlock(level, x + 2, y, z, chunkBB)).is(type->planks)) fillPillarDownOrChainUp(level, type->wood, x + 2, y, z, chunkBB);
  }
  void placeSupport(Level& level, const BoundingBox& chunkBB, int x0, int y0, int z, int y1, int x1, RandomSource& random) {
    if (!isSupportingBox(level, chunkBB, x0, x1, y1, z)) return;
    generateBox(level, chunkBB, x0, y0, z, x0, y1 - 1, z, BS(type->fence).with("west", true), CAVE_AIR, false);
    generateBox(level, chunkBB, x1, y0, z, x1, y1 - 1, z, BS(type->fence).with("east", true), CAVE_AIR, false);
    if (random.nextInt(4) == 0) {
      generateBox(level, chunkBB, x0, y1, z, x0, y1, z, type->planks, CAVE_AIR, false);
      generateBox(level, chunkBB, x1, y1, z, x1, y1, z, type->planks, CAVE_AIR, false);
    } else {
      generateBox(level, chunkBB, x0, y1, z, x1, y1, z, type->planks, CAVE_AIR, false);
      maybeGenerateBlock(level, chunkBB, random, 0.05f, x0 + 1, y1, z - 1, BS(blocks::wall_torch).with("facing", Dir::SOUTH));
      maybeGenerateBlock(level, chunkBB, random, 0.05f, x0 + 1, y1, z + 1, BS(blocks::wall_torch).with("facing", Dir::NORTH));
    }
  }
  bool hasSturdyNeighbours(Level& level, const BoundingBox& chunkBB, int x, int y, int z, int count) {
    BlockPos p = worldPos(x, y, z);
    int n = 0;
    for (Dir d : ALL_DIRS) {
      BlockPos q = p.relative(d);
      if (chunkBB.isInside(q) && sturdy(level.get(q), opposite(d)) && ++n >= count) return true;
    }
    return false;
  }
  void maybePlaceCobWeb(Level& level, const BoundingBox& chunkBB, RandomSource& random, float p, int x, int y, int z) {
    if (isInterior(level, x, y, z, chunkBB) && random.nextFloat() < p && hasSturdyNeighbours(level, chunkBB, x, y, z, 2))
      placeBlock(level, blocks::cobweb, x, y, z, chunkBB);
  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int, int, BlockPos) override {
    if (isInInvalidLocation(level, chunkBB)) return;
    int length = numSections * 5 - 1;
    generateBox(level, chunkBB, 0, 0, 0, 2, 1, length, CAVE_AIR, CAVE_AIR, false);
    generateMaybeBox(level, chunkBB, random, 0.8f, 0, 2, 0, 2, 2, length, CAVE_AIR, CAVE_AIR, false, false);
    if (spiderCorridor) generateMaybeBox(level, chunkBB, random, 0.6f, 0, 0, 0, 2, 1, length, blocks::cobweb, CAVE_AIR, false, true);
    for (int section = 0; section < numSections; section++) {
      int z = 2 + section * 5;
      placeSupport(level, chunkBB, 0, 0, z, 2, 2, random);
      maybePlaceCobWeb(level, chunkBB, random, 0.1f, 0, 2, z - 1);
      maybePlaceCobWeb(level, chunkBB, random, 0.1f, 2, 2, z - 1);
      maybePlaceCobWeb(level, chunkBB, random, 0.1f, 0, 2, z + 1);
      maybePlaceCobWeb(level, chunkBB, random, 0.1f, 2, 2, z + 1);
      maybePlaceCobWeb(level, chunkBB, random, 0.05f, 0, 2, z - 2);
      maybePlaceCobWeb(level, chunkBB, random, 0.05f, 2, 2, z - 2);
      maybePlaceCobWeb(level, chunkBB, random, 0.05f, 0, 2, z + 2);
      maybePlaceCobWeb(level, chunkBB, random, 0.05f, 2, 2, z + 2);
      if (random.nextInt(100) == 0) createMinecartChest(level, chunkBB, random, 2, 0, z - 1);
      if (random.nextInt(100) == 0) createMinecartChest(level, chunkBB, random, 0, 0, z + 1);
      if (spiderCorridor && !hasPlacedSpider) {
        int newZ = z - 1 + random.nextInt(3);
        BlockPos p = worldPos(1, 0, newZ);
        if (chunkBB.isInside(p) && isInterior(level, 1, 0, newZ, chunkBB)) {
          hasPlacedSpider = true;
          level.set(p, blocks::spawner);  // setEntityId draws no random (empty spawn potentials)
        }
      }
    }
    for (int x = 0; x <= 2; x++)
      for (int z = 0; z <= length; z++) setPlanksBlock(level, chunkBB, type->planks, x, -1, z);
    placeDoubleLowerOrUpperSupport(level, chunkBB, 0, -1, 2);
    if (numSections > 1) placeDoubleLowerOrUpperSupport(level, chunkBB, 0, -1, length - 2);
    if (hasRails) {
      BS rail = BS(blocks::rail).with("shape", "north_south");
      for (int z = 0; z <= length; z++) {
        int floor = getBlock(level, 1, -1, z, chunkBB);
        if (!Blocks::get().isAir(floor) && (Blocks::get().flags(floor) & BS_SOLID_RENDER)) {
          float p = isInterior(level, 1, 0, z, chunkBB) ? 0.7f : 0.9f;
          maybeGenerateBlock(level, chunkBB, random, p, 1, 0, z, rail);
        }
      }
    }
  }
};

// ------------------------------------------------------------------------------------------- crossing
struct Crossing : MinePiece {
  Dir direction;
  bool twoFloored;
  Crossing(int depth, const BoundingBox& bb, Dir d, const MineType* t) : MinePiece(depth, t, bb), direction(d), twoFloored(bb.ySpan() > 3) {}
  static std::optional<BoundingBox> find(Builder& builder, RandomSource& random, int x, int y, int z, Dir d) {
    int y1 = random.nextInt(4) == 0 ? 6 : 2;
    BoundingBox box;
    switch (d) {
      case Dir::SOUTH: box = BoundingBox(-1, 0, 0, 3, y1, 4); break;
      case Dir::WEST: box = BoundingBox(-4, 0, -1, 0, y1, 3); break;
      case Dir::EAST: box = BoundingBox(0, 0, -1, 4, y1, 3); break;
      default: box = BoundingBox(-1, 0, -4, 3, y1, 0);
    }
    box.move(x, y, z);
    if (builder.findCollisionPiece(box)) return std::nullopt;
    return box;
  }
  void addChildren(Piece* start, Builder& builder, RandomSource& random) override {
    int depth = genDepth;
    const BoundingBox& b = boundingBox;
    auto add = [&](int x, int y, int z, Dir d) { generateAndAddPiece(start, builder, random, x, y, z, d, depth); };
    switch (direction) {
      case Dir::SOUTH:
        add(b.minX + 1, b.minY, b.maxZ + 1, Dir::SOUTH);
        add(b.minX - 1, b.minY, b.minZ + 1, Dir::WEST);
        add(b.maxX + 1, b.minY, b.minZ + 1, Dir::EAST);
        break;
      case Dir::WEST:
        add(b.minX + 1, b.minY, b.minZ - 1, Dir::NORTH);
        add(b.minX + 1, b.minY, b.maxZ + 1, Dir::SOUTH);
        add(b.minX - 1, b.minY, b.minZ + 1, Dir::WEST);
        break;
      case Dir::EAST:
        add(b.minX + 1, b.minY, b.minZ - 1, Dir::NORTH);
        add(b.minX + 1, b.minY, b.maxZ + 1, Dir::SOUTH);
        add(b.maxX + 1, b.minY, b.minZ + 1, Dir::EAST);
        break;
      default:
        add(b.minX + 1, b.minY, b.minZ - 1, Dir::NORTH);
        add(b.minX - 1, b.minY, b.minZ + 1, Dir::WEST);
        add(b.maxX + 1, b.minY, b.minZ + 1, Dir::EAST);
    }
    if (twoFloored) {
      if (random.nextBoolean()) add(b.minX + 1, b.minY + 3 + 1, b.minZ - 1, Dir::NORTH);
      if (random.nextBoolean()) add(b.minX - 1, b.minY + 3 + 1, b.minZ + 1, Dir::WEST);
      if (random.nextBoolean()) add(b.maxX + 1, b.minY + 3 + 1, b.minZ + 1, Dir::EAST);
      if (random.nextBoolean()) add(b.minX + 1, b.minY + 3 + 1, b.maxZ + 1, Dir::SOUTH);
    }
  }
  void placeSupportPillar(Level& level, const BoundingBox& chunkBB, int x, int y0, int z, int y1) {
    if (!BS(getBlock(level, x, y1 + 1, z, chunkBB)).isAir()) generateBox(level, chunkBB, x, y0, z, x, y1, z, type->planks, CAVE_AIR, false);
  }
  void postProcess(Level& level, RandomSource&, const BoundingBox& chunkBB, int, int, BlockPos) override {
    if (isInInvalidLocation(level, chunkBB)) return;
    const BoundingBox& b = boundingBox;
    if (twoFloored) {
      generateBox(level, chunkBB, b.minX + 1, b.minY, b.minZ, b.maxX - 1, b.minY + 3 - 1, b.maxZ, CAVE_AIR, CAVE_AIR, false);
      generateBox(level, chunkBB, b.minX, b.minY, b.minZ + 1, b.maxX, b.minY + 3 - 1, b.maxZ - 1, CAVE_AIR, CAVE_AIR, false);
      generateBox(level, chunkBB, b.minX + 1, b.maxY - 2, b.minZ, b.maxX - 1, b.maxY, b.maxZ, CAVE_AIR, CAVE_AIR, false);
      generateBox(level, chunkBB, b.minX, b.maxY - 2, b.minZ + 1, b.maxX, b.maxY, b.maxZ - 1, CAVE_AIR, CAVE_AIR, false);
      generateBox(level, chunkBB, b.minX + 1, b.minY + 3, b.minZ + 1, b.maxX - 1, b.minY + 3, b.maxZ - 1, CAVE_AIR, CAVE_AIR, false);
    } else {
      generateBox(level, chunkBB, b.minX + 1, b.minY, b.minZ, b.maxX - 1, b.maxY, b.maxZ, CAVE_AIR, CAVE_AIR, false);
      generateBox(level, chunkBB, b.minX, b.minY, b.minZ + 1, b.maxX, b.maxY, b.maxZ - 1, CAVE_AIR, CAVE_AIR, false);
    }
    placeSupportPillar(level, chunkBB, b.minX + 1, b.minY, b.minZ + 1, b.maxY);
    placeSupportPillar(level, chunkBB, b.minX + 1, b.minY, b.maxZ - 1, b.maxY);
    placeSupportPillar(level, chunkBB, b.maxX - 1, b.minY, b.minZ + 1, b.maxY);
    placeSupportPillar(level, chunkBB, b.maxX - 1, b.minY, b.maxZ - 1, b.maxY);
    int y = b.minY - 1;
    for (int x = b.minX; x <= b.maxX; x++)
      for (int z = b.minZ; z <= b.maxZ; z++) setPlanksBlock(level, chunkBB, type->planks, x, y, z);
  }
};

// ------------------------------------------------------------------------------------------- stairs
struct Stairs : MinePiece {
  Stairs(int depth, const BoundingBox& bb, Dir d, const MineType* t) : MinePiece(depth, t, bb) { setOrientation(d); }
  static std::optional<BoundingBox> find(Builder& builder, int x, int y, int z, Dir d) {
    BoundingBox box;
    switch (d) {
      case Dir::SOUTH: box = BoundingBox(0, -5, 0, 2, 2, 8); break;
      case Dir::WEST: box = BoundingBox(-8, -5, 0, 0, 2, 2); break;
      case Dir::EAST: box = BoundingBox(0, -5, 0, 8, 2, 2); break;
      default: box = BoundingBox(0, -5, -8, 2, 2, 0);
    }
    box.move(x, y, z);
    if (builder.findCollisionPiece(box)) return std::nullopt;
    return box;
  }
  void addChildren(Piece* start, Builder& builder, RandomSource& random) override {
    const BoundingBox& b = boundingBox;
    switch (*orientation()) {
      case Dir::SOUTH: generateAndAddPiece(start, builder, random, b.minX, b.minY, b.maxZ + 1, Dir::SOUTH, genDepth); break;
      case Dir::WEST: generateAndAddPiece(start, builder, random, b.minX - 1, b.minY, b.minZ, Dir::WEST, genDepth); break;
      case Dir::EAST: generateAndAddPiece(start, builder, random, b.maxX + 1, b.minY, b.minZ, Dir::EAST, genDepth); break;
      default: generateAndAddPiece(start, builder, random, b.minX, b.minY, b.minZ - 1, Dir::NORTH, genDepth);
    }
  }
  void postProcess(Level& level, RandomSource&, const BoundingBox& chunkBB, int, int, BlockPos) override {
    if (isInInvalidLocation(level, chunkBB)) return;
    generateBox(level, chunkBB, 0, 5, 0, 2, 7, 1, CAVE_AIR, CAVE_AIR, false);
    generateBox(level, chunkBB, 0, 0, 7, 2, 2, 8, CAVE_AIR, CAVE_AIR, false);
    for (int i = 0; i < 5; i++) generateBox(level, chunkBB, 0, 5 - i - (i < 4 ? 1 : 0), 2 + i, 2, 7 - i, 2 + i, CAVE_AIR, CAVE_AIR, false);
  }
};

// ------------------------------------------------------------------------------------------- room
struct Room : MinePiece {
  std::vector<BoundingBox> entrances;
  Room(int depth, RandomSource& random, int west, int north, const MineType* t) : MinePiece(depth, t, {}) {
    int x1 = west + 7 + random.nextInt(6);  // sequenced like Java's left-to-right argument evaluation
    int y1 = 54 + random.nextInt(6);
    int z1 = north + 7 + random.nextInt(6);
    boundingBox = BoundingBox(west, 50, north, x1, y1, z1);
  }
  void addChildren(Piece* start, Builder& builder, RandomSource& random) override {
    const BoundingBox& b = boundingBox;
    int heightSpace = b.ySpan() - 3 - 1;
    if (heightSpace <= 0) heightSpace = 1;
    auto side = [&](int span, auto at, Dir d, auto entrance) {
      int pos = 0;
      while (pos < span) {
        pos += random.nextInt(span);
        if (pos + 3 > span) break;
        auto [x, z] = at(pos);
        MinePiece* child = generateAndAddPiece(start, builder, random, x, b.minY + random.nextInt(heightSpace) + 1, z, d, genDepth);
        if (child) entrances.push_back(entrance(child->boundingBox));
        pos += 4;
      }
    };
    // Argument order matters (random draws): x/z first, then y, as in the Java call.
    side(b.xSpan(), [&](int p) { return std::pair{b.minX + p, b.minZ - 1}; }, Dir::NORTH,
         [&](const BoundingBox& c) { return BoundingBox(c.minX, c.minY, b.minZ, c.maxX, c.maxY, b.minZ + 1); });
    side(b.xSpan(), [&](int p) { return std::pair{b.minX + p, b.maxZ + 1}; }, Dir::SOUTH,
         [&](const BoundingBox& c) { return BoundingBox(c.minX, c.minY, b.maxZ - 1, c.maxX, c.maxY, b.maxZ); });
    side(b.zSpan(), [&](int p) { return std::pair{b.minX - 1, b.minZ + p}; }, Dir::WEST,
         [&](const BoundingBox& c) { return BoundingBox(b.minX, c.minY, c.minZ, b.minX + 1, c.maxY, c.maxZ); });
    side(b.zSpan(), [&](int p) { return std::pair{b.maxX + 1, b.minZ + p}; }, Dir::EAST,
         [&](const BoundingBox& c) { return BoundingBox(b.maxX - 1, c.minY, c.minZ, b.maxX, c.maxY, c.maxZ); });
  }
  void postProcess(Level& level, RandomSource&, const BoundingBox& chunkBB, int, int, BlockPos) override {
    if (isInInvalidLocation(level, chunkBB)) return;
    const BoundingBox& b = boundingBox;
    generateBox(level, chunkBB, b.minX, b.minY + 1, b.minZ, b.maxX, std::min(b.minY + 3, b.maxY), b.maxZ, CAVE_AIR, CAVE_AIR, false);
    for (auto& e : entrances) generateBox(level, chunkBB, e.minX, e.maxY - 2, e.minZ, e.maxX, e.maxY, e.maxZ, CAVE_AIR, CAVE_AIR, false);
    generateUpperHalfSphere(level, chunkBB, b.minX, b.minY + 4, b.minZ, b.maxX, b.maxY, b.maxZ, CAVE_AIR, false);
  }
  void move(int dx, int dy, int dz) override {
    Piece::move(dx, dy, dz);
    for (auto& e : entrances) e.move(dx, dy, dz);
  }
};

PieceP createRandomShaftPiece(Builder& b, RandomSource& r, int x, int y, int z, Dir d, int depth, const MineType* t) {
  int sel = r.nextInt(100);
  if (sel >= 80) {
    if (auto box = Crossing::find(b, r, x, y, z, d)) return std::make_unique<Crossing>(depth, *box, d, t);
  } else if (sel >= 70) {
    if (auto box = Stairs::find(b, x, y, z, d)) return std::make_unique<Stairs>(depth, *box, d, t);
  } else {
    if (auto box = Corridor::findCorridorSize(b, r, x, y, z, d)) return std::make_unique<Corridor>(depth, r, *box, d, t);
  }
  return nullptr;
}
}  // namespace

void generateMineshaft(Builder& builder, RandomSource& random, int x, int z, bool mesa) {
  auto room = std::make_unique<Room>(0, random, x, z, mesa ? &MESA : &NORMAL);
  Room* r = room.get();
  builder.add(std::move(room));
  r->addChildren(r, builder, random);
}

}  // namespace structure
