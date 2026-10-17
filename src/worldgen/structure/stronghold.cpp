// StrongholdPieces (26.3). The piece-weight table that vanilla keeps in statics lives in the StartPiece here.
#include <cstdlib>

#include "worldgen/structure/legacy.hpp"
#include "worldgen/structure/pieces.hpp"

namespace structure {
namespace {

const int CAVE_AIR = blocks::cave_air;
enum Kind { STRAIGHT, PRISON_HALL, LEFT_TURN, RIGHT_TURN, ROOM_CROSSING, STRAIGHT_STAIRS_DOWN, STAIRS_DOWN, FIVE_CROSSING,
            CHEST_CORRIDOR, LIBRARY, PORTAL_ROOM, KIND_NONE };
struct PieceWeight {
  Kind kind;
  int weight, placeCount, maxPlaceCount;
  bool doPlace(int depth) const {
    bool ok = maxPlaceCount == 0 || placeCount < maxPlaceCount;
    if (kind == LIBRARY) return ok && depth > 4;
    if (kind == PORTAL_ROOM) return ok && depth > 5;
    return ok;
  }
  bool isValid() const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
};
enum Door { OPENING, WOOD_DOOR, GRATES, IRON_DOOR };

struct SmoothStoneSelector : Piece::BlockSelector {
  void select(RandomSource& random, int, int, int, bool isEdge) override {
    if (!isEdge) {
      next = blocks::cave_air;
      return;
    }
    float s = random.nextFloat();
    next = s < 0.2f ? blocks::cracked_stone_bricks : s < 0.5f ? blocks::mossy_stone_bricks : s < 0.55f ? blocks::infested_stone_bricks
                                                                                                       : blocks::stone_bricks;
  }
};

struct StartPiece;

struct SHPiece : Piece {
  Door entryDoor = OPENING;
  SHPiece(int depth, const BoundingBox& bb) : Piece(depth, bb) {}
  static bool isOkBox(const BoundingBox& b) { return b.minY > 10; }
  static Door randomSmallDoor(RandomSource& random) {
    switch (random.nextInt(5)) {
      case 2: return WOOD_DOOR;
      case 3: return GRATES;
      case 4: return IRON_DOOR;
      default: return OPENING;
    }
  }
  void generateSmallDoor(Level& level, RandomSource&, const BoundingBox& bb, Door door, int x, int y, int z) {
    switch (door) {
      case OPENING: generateBox(level, bb, x, y, z, x + 3 - 1, y + 3 - 1, z, CAVE_AIR, CAVE_AIR, false); break;
      case WOOD_DOOR: case IRON_DOOR: {
        int sb = blocks::stone_bricks;
        placeBlock(level, sb, x, y, z, bb);
        placeBlock(level, sb, x, y + 1, z, bb);
        placeBlock(level, sb, x, y + 2, z, bb);
        placeBlock(level, sb, x + 1, y + 2, z, bb);
        placeBlock(level, sb, x + 2, y + 2, z, bb);
        placeBlock(level, sb, x + 2, y + 1, z, bb);
        placeBlock(level, sb, x + 2, y, z, bb);
        int d = door == WOOD_DOOR ? blocks::oak_door : blocks::iron_door;
        placeBlock(level, d, x + 1, y, z, bb);
        placeBlock(level, BS(d).with("half", "upper"), x + 1, y + 1, z, bb);
        if (door == IRON_DOOR) {
          placeBlock(level, BS(blocks::stone_button).with("facing", Dir::NORTH), x + 2, y + 1, z + 1, bb);
          placeBlock(level, BS(blocks::stone_button).with("facing", Dir::SOUTH), x + 2, y + 1, z - 1, bb);
        }
        break;
      }
      case GRATES:
        placeBlock(level, blocks::cave_air, x + 1, y, z, bb);
        placeBlock(level, blocks::cave_air, x + 1, y + 1, z, bb);
        placeBlock(level, BS(blocks::iron_bars).with("west", true), x, y, z, bb);
        placeBlock(level, BS(blocks::iron_bars).with("west", true), x, y + 1, z, bb);
        placeBlock(level, BS(blocks::iron_bars).with("east", true).with("west", true), x, y + 2, z, bb);
        placeBlock(level, BS(blocks::iron_bars).with("east", true).with("west", true), x + 1, y + 2, z, bb);
        placeBlock(level, BS(blocks::iron_bars).with("east", true).with("west", true), x + 2, y + 2, z, bb);
        placeBlock(level, BS(blocks::iron_bars).with("east", true), x + 2, y + 1, z, bb);
        placeBlock(level, BS(blocks::iron_bars).with("east", true), x + 2, y, z, bb);
        break;
    }
  }
  Piece* childForward(StartPiece* start, Builder& b, RandomSource& r, int xOff, int yOff);
  Piece* childLeft(StartPiece* start, Builder& b, RandomSource& r, int yOff, int zOff);
  Piece* childRight(StartPiece* start, Builder& b, RandomSource& r, int yOff, int zOff);
};

PieceP createPiece(Kind k, Builder& b, RandomSource& r, int x, int y, int z, Dir d, int depth);

struct StartPiece;
Piece* generateAndAddPiece(StartPiece* start, Builder& builder, RandomSource& random, int x, int y, int z, Dir d, int depth);

// ------------------------------------------------------------------------------------------- pieces
#define SH_CTOR(Name) Name(int depth, RandomSource& random, const BoundingBox& bb, Dir d) : SHPiece(depth, bb) { \
    setOrientation(d);                                                                                       \
    entryDoor = randomSmallDoor(random);                                                                     \
    init(random);                                                                                            \
  }

struct ChestCorridor : SHPiece {
  bool hasPlacedChest = false;
  void init(RandomSource&) {}
  SH_CTOR(ChestCorridor)
  void saveState(std::vector<int>& out) const override { Piece::saveState(out), out.push_back(hasPlacedChest); }
  void loadState(const int*& p) override { Piece::loadState(p), hasPlacedChest = *p++; }
  void addChildren(Piece* s, Builder& b, RandomSource& r) override;
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    SmoothStoneSelector sel;
    generateBox(level, bb, 0, 0, 0, 4, 4, 6, true, random, sel);
    generateSmallDoor(level, random, bb, entryDoor, 1, 1, 0);
    generateSmallDoor(level, random, bb, OPENING, 1, 1, 6);
    generateBox(level, bb, 3, 1, 2, 3, 1, 4, blocks::stone_bricks, blocks::stone_bricks, false);
    placeBlock(level, blocks::stone_brick_slab, 3, 1, 1, bb);
    placeBlock(level, blocks::stone_brick_slab, 3, 1, 5, bb);
    placeBlock(level, blocks::stone_brick_slab, 3, 2, 2, bb);
    placeBlock(level, blocks::stone_brick_slab, 3, 2, 4, bb);
    for (int z = 2; z <= 4; z++) placeBlock(level, blocks::stone_brick_slab, 2, 1, z, bb);
    if (!hasPlacedChest && bb.isInside(worldPos(3, 2, 3))) {
      hasPlacedChest = true;
      createChest(level, bb, random, 3, 2, 3, "minecraft:chests/stronghold_corridor");
    }
  }
};

struct FillerCorridor : SHPiece {
  int steps;
  FillerCorridor(int depth, const BoundingBox& bb, Dir d) : SHPiece(depth, bb) {
    setOrientation(d);
    steps = d != Dir::NORTH && d != Dir::SOUTH ? bb.xSpan() : bb.zSpan();
  }
  static std::optional<BoundingBox> findPieceBox(Builder& builder, int x, int y, int z, Dir d) {
    BoundingBox box = BoundingBox::orientBox(x, y, z, -1, -1, 0, 5, 5, 4, d);
    Piece* c = builder.findCollisionPiece(box);
    if (!c) return std::nullopt;
    if (c->boundingBox.minY == box.minY)
      for (int depth = 2; depth >= 1; depth--) {
        box = BoundingBox::orientBox(x, y, z, -1, -1, 0, 5, 5, depth, d);
        if (!c->boundingBox.intersects(box)) return BoundingBox::orientBox(x, y, z, -1, -1, 0, 5, 5, depth + 1, d);
      }
    return std::nullopt;
  }
  void postProcess(Level& level, RandomSource&, const BoundingBox& bb, int, int, BlockPos) override {
    for (int i = 0; i < steps; i++) {
      for (int x = 0; x <= 4; x++) placeBlock(level, blocks::stone_bricks, x, 0, i, bb);
      for (int y = 1; y <= 3; y++) {
        placeBlock(level, blocks::stone_bricks, 0, y, i, bb);
        placeBlock(level, blocks::cave_air, 1, y, i, bb);
        placeBlock(level, blocks::cave_air, 2, y, i, bb);
        placeBlock(level, blocks::cave_air, 3, y, i, bb);
        placeBlock(level, blocks::stone_bricks, 4, y, i, bb);
      }
      for (int x = 0; x <= 4; x++) placeBlock(level, blocks::stone_bricks, x, 4, i, bb);
    }
  }
};

struct FiveCrossing : SHPiece {
  bool leftLow, leftHigh, rightLow, rightHigh;
  void init(RandomSource& r) {
    leftLow = r.nextBoolean();
    leftHigh = r.nextBoolean();
    rightLow = r.nextBoolean();
    rightHigh = r.nextInt(3) > 0;
  }
  SH_CTOR(FiveCrossing)
  void addChildren(Piece* s, Builder& b, RandomSource& r) override;
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    SmoothStoneSelector sel;
    int slab = blocks::smooth_stone_slab;
    generateBox(level, bb, 0, 0, 0, 9, 8, 10, true, random, sel);
    generateSmallDoor(level, random, bb, entryDoor, 4, 3, 0);
    if (leftLow) generateBox(level, bb, 0, 3, 1, 0, 5, 3, CAVE_AIR, CAVE_AIR, false);
    if (rightLow) generateBox(level, bb, 9, 3, 1, 9, 5, 3, CAVE_AIR, CAVE_AIR, false);
    if (leftHigh) generateBox(level, bb, 0, 5, 7, 0, 7, 9, CAVE_AIR, CAVE_AIR, false);
    if (rightHigh) generateBox(level, bb, 9, 5, 7, 9, 7, 9, CAVE_AIR, CAVE_AIR, false);
    generateBox(level, bb, 5, 1, 10, 7, 3, 10, CAVE_AIR, CAVE_AIR, false);
    generateBox(level, bb, 1, 2, 1, 8, 2, 6, false, random, sel);
    generateBox(level, bb, 4, 1, 5, 4, 4, 9, false, random, sel);
    generateBox(level, bb, 8, 1, 5, 8, 4, 9, false, random, sel);
    generateBox(level, bb, 1, 4, 7, 3, 4, 9, false, random, sel);
    generateBox(level, bb, 1, 3, 5, 3, 3, 6, false, random, sel);
    generateBox(level, bb, 1, 3, 4, 3, 3, 4, slab, slab, false);
    generateBox(level, bb, 1, 4, 6, 3, 4, 6, slab, slab, false);
    generateBox(level, bb, 5, 1, 7, 7, 1, 8, false, random, sel);
    generateBox(level, bb, 5, 1, 9, 7, 1, 9, slab, slab, false);
    generateBox(level, bb, 5, 2, 7, 7, 2, 7, slab, slab, false);
    generateBox(level, bb, 4, 5, 7, 4, 5, 9, slab, slab, false);
    generateBox(level, bb, 8, 5, 7, 8, 5, 9, slab, slab, false);
    BS dbl = BS(slab).with("type", "double");
    generateBox(level, bb, 5, 5, 7, 7, 5, 9, dbl, dbl, false);
    placeBlock(level, BS(blocks::wall_torch).with("facing", Dir::SOUTH), 6, 5, 6, bb);
  }
};

struct Turn : SHPiece {
  bool left;
  Turn(bool left, int depth, RandomSource& random, const BoundingBox& bb, Dir d) : SHPiece(depth, bb), left(left) {
    setOrientation(d);
    entryDoor = randomSmallDoor(random);
  }
  void addChildren(Piece* s, Builder& b, RandomSource& r) override;
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    SmoothStoneSelector sel;
    generateBox(level, bb, 0, 0, 0, 4, 4, 4, true, random, sel);
    generateSmallDoor(level, random, bb, entryDoor, 1, 1, 0);
    Dir o = *orientation();
    bool openAt4 = (o != Dir::NORTH && o != Dir::EAST) == left;  // LeftTurn opens x=4 when not N/E
    if (openAt4) generateBox(level, bb, 4, 1, 1, 4, 3, 3, CAVE_AIR, CAVE_AIR, false);
    else generateBox(level, bb, 0, 1, 1, 0, 3, 3, CAVE_AIR, CAVE_AIR, false);
  }
};

struct Library : SHPiece {
  bool isTall;
  void init(RandomSource&) { isTall = boundingBox.ySpan() > 6; }
  SH_CTOR(Library)
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    SmoothStoneSelector sel;
    int h = isTall ? 11 : 6;
    int planks = blocks::oak_planks, shelf = blocks::bookshelf;
    generateBox(level, bb, 0, 0, 0, 13, h - 1, 14, true, random, sel);
    generateSmallDoor(level, random, bb, entryDoor, 4, 1, 0);
    generateMaybeBox(level, bb, random, 0.07f, 2, 1, 1, 11, 4, 13, blocks::cobweb, blocks::cobweb, false, false);
    for (int d = 1; d <= 13; d++) {
      if ((d - 1) % 4 == 0) {
        generateBox(level, bb, 1, 1, d, 1, 4, d, planks, planks, false);
        generateBox(level, bb, 12, 1, d, 12, 4, d, planks, planks, false);
        placeBlock(level, BS(blocks::wall_torch).with("facing", Dir::EAST), 2, 3, d, bb);
        placeBlock(level, BS(blocks::wall_torch).with("facing", Dir::WEST), 11, 3, d, bb);
        if (isTall) {
          generateBox(level, bb, 1, 6, d, 1, 9, d, planks, planks, false);
          generateBox(level, bb, 12, 6, d, 12, 9, d, planks, planks, false);
        }
      } else {
        generateBox(level, bb, 1, 1, d, 1, 4, d, shelf, shelf, false);
        generateBox(level, bb, 12, 1, d, 12, 4, d, shelf, shelf, false);
        if (isTall) {
          generateBox(level, bb, 1, 6, d, 1, 9, d, shelf, shelf, false);
          generateBox(level, bb, 12, 6, d, 12, 9, d, shelf, shelf, false);
        }
      }
    }
    for (int dx = 3; dx < 12; dx += 2) {
      generateBox(level, bb, 3, 1, dx, 4, 3, dx, shelf, shelf, false);
      generateBox(level, bb, 6, 1, dx, 7, 3, dx, shelf, shelf, false);
      generateBox(level, bb, 9, 1, dx, 10, 3, dx, shelf, shelf, false);
    }
    if (isTall) {
      generateBox(level, bb, 1, 5, 1, 3, 5, 13, planks, planks, false);
      generateBox(level, bb, 10, 5, 1, 12, 5, 13, planks, planks, false);
      generateBox(level, bb, 4, 5, 1, 9, 5, 2, planks, planks, false);
      generateBox(level, bb, 4, 5, 12, 9, 5, 13, planks, planks, false);
      placeBlock(level, planks, 9, 5, 11, bb);
      placeBlock(level, planks, 8, 5, 11, bb);
      placeBlock(level, planks, 9, 5, 10, bb);
      BS fence = blocks::oak_fence;
      BS we = fence.with("west", true).with("east", true), ns = fence.with("north", true).with("south", true);
      generateBox(level, bb, 3, 6, 3, 3, 6, 11, ns, ns, false);
      generateBox(level, bb, 10, 6, 3, 10, 6, 9, ns, ns, false);
      generateBox(level, bb, 4, 6, 2, 9, 6, 2, we, we, false);
      generateBox(level, bb, 4, 6, 12, 7, 6, 12, we, we, false);
      placeBlock(level, fence.with("north", true).with("east", true), 3, 6, 2, bb);
      placeBlock(level, fence.with("south", true).with("east", true), 3, 6, 12, bb);
      placeBlock(level, fence.with("north", true).with("west", true), 10, 6, 2, bb);
      for (int i = 0; i <= 2; i++) {
        placeBlock(level, fence.with("south", true).with("west", true), 8 + i, 6, 12 - i, bb);
        if (i != 2) placeBlock(level, fence.with("north", true).with("east", true), 8 + i, 6, 11 - i, bb);
      }
      BS ladder = BS(blocks::ladder).with("facing", Dir::SOUTH);
      for (int y = 1; y <= 7; y++) placeBlock(level, ladder, 10, y, 13, bb);
      BS e = fence.with("east", true), w = fence.with("west", true);
      placeBlock(level, e, 6, 9, 7, bb);
      placeBlock(level, w, 7, 9, 7, bb);
      placeBlock(level, e, 6, 8, 7, bb);
      placeBlock(level, w, 7, 8, 7, bb);
      BS nswe = ns.with("west", true).with("east", true);
      placeBlock(level, nswe, 6, 7, 7, bb);
      placeBlock(level, nswe, 7, 7, 7, bb);
      placeBlock(level, e, 5, 7, 7, bb);
      placeBlock(level, w, 8, 7, 7, bb);
      placeBlock(level, e.with("north", true), 6, 7, 6, bb);
      placeBlock(level, e.with("south", true), 6, 7, 8, bb);
      placeBlock(level, w.with("north", true), 7, 7, 6, bb);
      placeBlock(level, w.with("south", true), 7, 7, 8, bb);
      int torch = blocks::torch;
      placeBlock(level, torch, 5, 8, 7, bb);
      placeBlock(level, torch, 8, 8, 7, bb);
      placeBlock(level, torch, 6, 8, 6, bb);
      placeBlock(level, torch, 6, 8, 8, bb);
      placeBlock(level, torch, 7, 8, 6, bb);
      placeBlock(level, torch, 7, 8, 8, bb);
    }
    createChest(level, bb, random, 3, 3, 5, "minecraft:chests/stronghold_library");
    if (isTall) {
      placeBlock(level, CAVE_AIR, 12, 9, 1, bb);
      createChest(level, bb, random, 12, 8, 1, "minecraft:chests/stronghold_library");
    }
  }
};

struct PortalRoom : SHPiece {
  bool hasPlacedSpawner = false;
  PortalRoom(int depth, const BoundingBox& bb, Dir d) : SHPiece(depth, bb) { setOrientation(d); }
  void saveState(std::vector<int>& out) const override { Piece::saveState(out), out.push_back(hasPlacedSpawner); }
  void loadState(const int*& p) override { Piece::loadState(p), hasPlacedSpawner = *p++; }
  void addChildren(Piece* s, Builder& b, RandomSource& r) override;
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    SmoothStoneSelector sel;
    generateBox(level, bb, 0, 0, 0, 10, 7, 15, false, random, sel);
    generateSmallDoor(level, random, bb, GRATES, 4, 1, 0);
    generateBox(level, bb, 1, 6, 1, 1, 6, 14, false, random, sel);
    generateBox(level, bb, 9, 6, 1, 9, 6, 14, false, random, sel);
    generateBox(level, bb, 2, 6, 1, 8, 6, 2, false, random, sel);
    generateBox(level, bb, 2, 6, 14, 8, 6, 14, false, random, sel);
    generateBox(level, bb, 1, 1, 1, 2, 1, 4, false, random, sel);
    generateBox(level, bb, 8, 1, 1, 9, 1, 4, false, random, sel);
    generateBox(level, bb, 1, 1, 1, 1, 1, 3, blocks::lava, blocks::lava, false);
    generateBox(level, bb, 9, 1, 1, 9, 1, 3, blocks::lava, blocks::lava, false);
    generateBox(level, bb, 3, 1, 8, 7, 1, 12, false, random, sel);
    generateBox(level, bb, 4, 1, 9, 6, 1, 11, blocks::lava, blocks::lava, false);
    BS ns = BS(blocks::iron_bars).with("north", true).with("south", true);
    BS we = BS(blocks::iron_bars).with("west", true).with("east", true);
    for (int z = 3; z < 14; z += 2) {
      generateBox(level, bb, 0, 3, z, 0, 4, z, ns, ns, false);
      generateBox(level, bb, 10, 3, z, 10, 4, z, ns, ns, false);
    }
    for (int x = 2; x < 9; x += 2) generateBox(level, bb, x, 3, 15, x, 4, 15, we, we, false);
    BS stairs = BS(blocks::stone_brick_stairs).with("facing", Dir::NORTH);
    generateBox(level, bb, 4, 1, 5, 6, 1, 7, false, random, sel);
    generateBox(level, bb, 4, 2, 6, 6, 2, 7, false, random, sel);
    generateBox(level, bb, 4, 3, 7, 6, 3, 7, false, random, sel);
    for (int x = 4; x <= 6; x++) {
      placeBlock(level, stairs, x, 1, 4, bb);
      placeBlock(level, stairs, x, 2, 5, bb);
      placeBlock(level, stairs, x, 3, 6, bb);
    }
    BS frame = blocks::end_portal_frame;
    BS n = frame.with("facing", Dir::NORTH), s = frame.with("facing", Dir::SOUTH), e = frame.with("facing", Dir::EAST),
       w = frame.with("facing", Dir::WEST);
    bool allEyes = true, eyes[12];
    for (bool& eye : eyes) {
      eye = random.nextFloat() > 0.9f;
      allEyes &= eye;
    }
    placeBlock(level, n.with("eye", eyes[0]), 4, 3, 8, bb);
    placeBlock(level, n.with("eye", eyes[1]), 5, 3, 8, bb);
    placeBlock(level, n.with("eye", eyes[2]), 6, 3, 8, bb);
    placeBlock(level, s.with("eye", eyes[3]), 4, 3, 12, bb);
    placeBlock(level, s.with("eye", eyes[4]), 5, 3, 12, bb);
    placeBlock(level, s.with("eye", eyes[5]), 6, 3, 12, bb);
    placeBlock(level, e.with("eye", eyes[6]), 3, 3, 9, bb);
    placeBlock(level, e.with("eye", eyes[7]), 3, 3, 10, bb);
    placeBlock(level, e.with("eye", eyes[8]), 3, 3, 11, bb);
    placeBlock(level, w.with("eye", eyes[9]), 7, 3, 9, bb);
    placeBlock(level, w.with("eye", eyes[10]), 7, 3, 10, bb);
    placeBlock(level, w.with("eye", eyes[11]), 7, 3, 11, bb);
    if (allEyes)
      for (int z = 9; z <= 11; z++)
        for (int x = 4; x <= 6; x++) placeBlock(level, blocks::end_portal, x, 3, z, bb);
    if (!hasPlacedSpawner) {
      BlockPos p = worldPos(5, 3, 6);
      if (bb.isInside(p)) {
        hasPlacedSpawner = true;
        level.set(p, blocks::spawner);  // setEntityId draws no random (empty spawn potentials)
      }
    }
  }
};

struct PrisonHall : SHPiece {
  void init(RandomSource&) {}
  SH_CTOR(PrisonHall)
  void addChildren(Piece* s, Builder& b, RandomSource& r) override;
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    SmoothStoneSelector sel;
    generateBox(level, bb, 0, 0, 0, 8, 4, 10, true, random, sel);
    generateSmallDoor(level, random, bb, entryDoor, 1, 1, 0);
    generateBox(level, bb, 1, 1, 10, 3, 3, 10, CAVE_AIR, CAVE_AIR, false);
    generateBox(level, bb, 4, 1, 1, 4, 3, 1, false, random, sel);
    generateBox(level, bb, 4, 1, 3, 4, 3, 3, false, random, sel);
    generateBox(level, bb, 4, 1, 7, 4, 3, 7, false, random, sel);
    generateBox(level, bb, 4, 1, 9, 4, 3, 9, false, random, sel);
    BS bars = blocks::iron_bars;
    for (int y = 1; y <= 3; y++) {
      placeBlock(level, bars.with("north", true).with("south", true), 4, y, 4, bb);
      placeBlock(level, bars.with("north", true).with("south", true).with("east", true), 4, y, 5, bb);
      placeBlock(level, bars.with("north", true).with("south", true), 4, y, 6, bb);
      placeBlock(level, bars.with("west", true).with("east", true), 5, y, 5, bb);
      placeBlock(level, bars.with("west", true).with("east", true), 6, y, 5, bb);
      placeBlock(level, bars.with("west", true).with("east", true), 7, y, 5, bb);
    }
    placeBlock(level, bars.with("north", true).with("south", true), 4, 3, 2, bb);
    placeBlock(level, bars.with("north", true).with("south", true), 4, 3, 8, bb);
    BS bottom = BS(blocks::iron_door).with("facing", Dir::WEST), top = bottom.with("half", "upper");
    placeBlock(level, bottom, 4, 1, 2, bb);
    placeBlock(level, top, 4, 2, 2, bb);
    placeBlock(level, bottom, 4, 1, 8, bb);
    placeBlock(level, top, 4, 2, 8, bb);
  }
};

struct RoomCrossing : SHPiece {
  int type;
  void init(RandomSource& r) { type = r.nextInt(5); }
  SH_CTOR(RoomCrossing)
  void addChildren(Piece* s, Builder& b, RandomSource& r) override;
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    SmoothStoneSelector sel;
    generateBox(level, bb, 0, 0, 0, 10, 6, 10, true, random, sel);
    generateSmallDoor(level, random, bb, entryDoor, 4, 1, 0);
    generateBox(level, bb, 4, 1, 10, 6, 3, 10, CAVE_AIR, CAVE_AIR, false);
    generateBox(level, bb, 0, 1, 4, 0, 3, 6, CAVE_AIR, CAVE_AIR, false);
    generateBox(level, bb, 10, 1, 4, 10, 3, 6, CAVE_AIR, CAVE_AIR, false);
    int sb = blocks::stone_bricks, slab = blocks::smooth_stone_slab, cob = blocks::cobblestone, planks = blocks::oak_planks;
    switch (type) {
      case 0:
        placeBlock(level, sb, 5, 1, 5, bb);
        placeBlock(level, sb, 5, 2, 5, bb);
        placeBlock(level, sb, 5, 3, 5, bb);
        placeBlock(level, BS(blocks::wall_torch).with("facing", Dir::WEST), 4, 3, 5, bb);
        placeBlock(level, BS(blocks::wall_torch).with("facing", Dir::EAST), 6, 3, 5, bb);
        placeBlock(level, BS(blocks::wall_torch).with("facing", Dir::SOUTH), 5, 3, 4, bb);
        placeBlock(level, BS(blocks::wall_torch).with("facing", Dir::NORTH), 5, 3, 6, bb);
        placeBlock(level, slab, 4, 1, 4, bb);
        placeBlock(level, slab, 4, 1, 5, bb);
        placeBlock(level, slab, 4, 1, 6, bb);
        placeBlock(level, slab, 6, 1, 4, bb);
        placeBlock(level, slab, 6, 1, 5, bb);
        placeBlock(level, slab, 6, 1, 6, bb);
        placeBlock(level, slab, 5, 1, 4, bb);
        placeBlock(level, slab, 5, 1, 6, bb);
        break;
      case 1:
        for (int i = 0; i < 5; i++) {
          placeBlock(level, sb, 3, 1, 3 + i, bb);
          placeBlock(level, sb, 7, 1, 3 + i, bb);
          placeBlock(level, sb, 3 + i, 1, 3, bb);
          placeBlock(level, sb, 3 + i, 1, 7, bb);
        }
        placeBlock(level, sb, 5, 1, 5, bb);
        placeBlock(level, sb, 5, 2, 5, bb);
        placeBlock(level, sb, 5, 3, 5, bb);
        placeBlock(level, blocks::water, 5, 4, 5, bb);
        break;
      case 2: {
        for (int z = 1; z <= 9; z++) {
          placeBlock(level, cob, 1, 3, z, bb);
          placeBlock(level, cob, 9, 3, z, bb);
        }
        for (int x = 1; x <= 9; x++) {
          placeBlock(level, cob, x, 3, 1, bb);
          placeBlock(level, cob, x, 3, 9, bb);
        }
        placeBlock(level, cob, 5, 1, 4, bb);
        placeBlock(level, cob, 5, 1, 6, bb);
        placeBlock(level, cob, 5, 3, 4, bb);
        placeBlock(level, cob, 5, 3, 6, bb);
        placeBlock(level, cob, 4, 1, 5, bb);
        placeBlock(level, cob, 6, 1, 5, bb);
        placeBlock(level, cob, 4, 3, 5, bb);
        placeBlock(level, cob, 6, 3, 5, bb);
        for (int y = 1; y <= 3; y++) {
          placeBlock(level, cob, 4, y, 4, bb);
          placeBlock(level, cob, 6, y, 4, bb);
          placeBlock(level, cob, 4, y, 6, bb);
          placeBlock(level, cob, 6, y, 6, bb);
        }
        placeBlock(level, blocks::wall_torch, 5, 3, 5, bb);
        for (int z = 2; z <= 8; z++) {
          placeBlock(level, planks, 2, 3, z, bb);
          placeBlock(level, planks, 3, 3, z, bb);
          if (z <= 3 || z >= 7) {
            placeBlock(level, planks, 4, 3, z, bb);
            placeBlock(level, planks, 5, 3, z, bb);
            placeBlock(level, planks, 6, 3, z, bb);
          }
          placeBlock(level, planks, 7, 3, z, bb);
          placeBlock(level, planks, 8, 3, z, bb);
        }
        BS ladder = BS(blocks::ladder).with("facing", Dir::WEST);
        placeBlock(level, ladder, 9, 1, 3, bb);
        placeBlock(level, ladder, 9, 2, 3, bb);
        placeBlock(level, ladder, 9, 3, 3, bb);
        createChest(level, bb, random, 3, 4, 8, "minecraft:chests/stronghold_crossing");
        break;
      }
      default: break;
    }
  }
};

struct StairsDown : SHPiece {
  bool isSource;
  StairsDown(int depth, int west, int north, Dir d) : SHPiece(depth, makeBoundingBox(west, 64, north, d, 5, 11, 5)), isSource(true) {
    setOrientation(d);
    entryDoor = OPENING;
  }
  StairsDown(int depth, RandomSource& random, const BoundingBox& bb, Dir d) : SHPiece(depth, bb), isSource(false) {
    setOrientation(d);
    entryDoor = randomSmallDoor(random);
  }
  void addChildren(Piece* s, Builder& b, RandomSource& r) override;
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    SmoothStoneSelector sel;
    int sb = blocks::stone_bricks, slab = blocks::smooth_stone_slab;
    generateBox(level, bb, 0, 0, 0, 4, 10, 4, true, random, sel);
    generateSmallDoor(level, random, bb, entryDoor, 1, 7, 0);
    generateSmallDoor(level, random, bb, OPENING, 1, 1, 4);
    placeBlock(level, sb, 2, 6, 1, bb);
    placeBlock(level, sb, 1, 5, 1, bb);
    placeBlock(level, slab, 1, 6, 1, bb);
    placeBlock(level, sb, 1, 5, 2, bb);
    placeBlock(level, sb, 1, 4, 3, bb);
    placeBlock(level, slab, 1, 5, 3, bb);
    placeBlock(level, sb, 2, 4, 3, bb);
    placeBlock(level, sb, 3, 3, 3, bb);
    placeBlock(level, slab, 3, 4, 3, bb);
    placeBlock(level, sb, 3, 3, 2, bb);
    placeBlock(level, sb, 3, 2, 1, bb);
    placeBlock(level, slab, 3, 3, 1, bb);
    placeBlock(level, sb, 2, 2, 1, bb);
    placeBlock(level, sb, 1, 1, 1, bb);
    placeBlock(level, slab, 1, 2, 1, bb);
    placeBlock(level, sb, 1, 1, 2, bb);
    placeBlock(level, slab, 1, 1, 3, bb);
  }
};

struct StartPiece : StairsDown {
  std::vector<PieceWeight> weights, current;  // STRONGHOLD_PIECE_WEIGHTS / currentPieces
  Kind imposed = KIND_NONE;
  int totalWeight = 0;
  int previousPiece = -1;  // index into weights
  PortalRoom* portalRoom = nullptr;
  std::vector<Piece*> pendingChildren;
  StartPiece(RandomSource& random, int west, int north) : StairsDown(0, west, north, randomHorizontal(random)) {
    weights = {{STRAIGHT, 40, 0, 0},       {PRISON_HALL, 5, 0, 5},    {LEFT_TURN, 20, 0, 0},     {RIGHT_TURN, 20, 0, 0},
               {ROOM_CROSSING, 10, 0, 6},  {STRAIGHT_STAIRS_DOWN, 5, 0, 5}, {STAIRS_DOWN, 5, 0, 5}, {FIVE_CROSSING, 5, 0, 4},
               {CHEST_CORRIDOR, 5, 0, 4},  {LIBRARY, 10, 0, 2},       {PORTAL_ROOM, 20, 0, 1}};
    current = weights;
  }
};

struct StraightStairsDown : SHPiece {
  void init(RandomSource&) {}
  SH_CTOR(StraightStairsDown)
  void addChildren(Piece* s, Builder& b, RandomSource& r) override;
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    SmoothStoneSelector sel;
    generateBox(level, bb, 0, 0, 0, 4, 10, 7, true, random, sel);
    generateSmallDoor(level, random, bb, entryDoor, 1, 7, 0);
    generateSmallDoor(level, random, bb, OPENING, 1, 1, 7);
    BS stairs = BS(blocks::cobblestone_stairs).with("facing", Dir::SOUTH);
    for (int i = 0; i < 6; i++) {
      for (int x = 1; x <= 3; x++) placeBlock(level, stairs, x, 6 - i, 1 + i, bb);
      if (i < 5)
        for (int x = 1; x <= 3; x++) placeBlock(level, blocks::stone_bricks, x, 5 - i, 1 + i, bb);
    }
  }
};

struct Straight : SHPiece {
  bool leftChild, rightChild;
  void init(RandomSource& r) {
    leftChild = r.nextInt(2) == 0;
    rightChild = r.nextInt(2) == 0;
  }
  SH_CTOR(Straight)
  void addChildren(Piece* s, Builder& b, RandomSource& r) override;
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    SmoothStoneSelector sel;
    generateBox(level, bb, 0, 0, 0, 4, 4, 6, true, random, sel);
    generateSmallDoor(level, random, bb, entryDoor, 1, 1, 0);
    generateSmallDoor(level, random, bb, OPENING, 1, 1, 6);
    BS e = BS(blocks::wall_torch).with("facing", Dir::EAST), w = BS(blocks::wall_torch).with("facing", Dir::WEST);
    maybeGenerateBlock(level, bb, random, 0.1f, 1, 2, 1, e);
    maybeGenerateBlock(level, bb, random, 0.1f, 3, 2, 1, w);
    maybeGenerateBlock(level, bb, random, 0.1f, 1, 2, 5, e);
    maybeGenerateBlock(level, bb, random, 0.1f, 3, 2, 5, w);
    if (leftChild) generateBox(level, bb, 0, 1, 2, 0, 3, 4, CAVE_AIR, CAVE_AIR, false);
    if (rightChild) generateBox(level, bb, 4, 1, 2, 4, 3, 4, CAVE_AIR, CAVE_AIR, false);
  }
};

// ------------------------------------------------------------------------------------------- children
Piece* SHPiece::childForward(StartPiece* s, Builder& b, RandomSource& r, int xOff, int yOff) {
  const BoundingBox& bb = boundingBox;
  switch (*orientation()) {
    case Dir::NORTH: return generateAndAddPiece(s, b, r, bb.minX + xOff, bb.minY + yOff, bb.minZ - 1, Dir::NORTH, genDepth);
    case Dir::SOUTH: return generateAndAddPiece(s, b, r, bb.minX + xOff, bb.minY + yOff, bb.maxZ + 1, Dir::SOUTH, genDepth);
    case Dir::WEST: return generateAndAddPiece(s, b, r, bb.minX - 1, bb.minY + yOff, bb.minZ + xOff, Dir::WEST, genDepth);
    case Dir::EAST: return generateAndAddPiece(s, b, r, bb.maxX + 1, bb.minY + yOff, bb.minZ + xOff, Dir::EAST, genDepth);
    default: return nullptr;
  }
}
Piece* SHPiece::childLeft(StartPiece* s, Builder& b, RandomSource& r, int yOff, int zOff) {
  const BoundingBox& bb = boundingBox;
  switch (*orientation()) {
    case Dir::NORTH: case Dir::SOUTH: return generateAndAddPiece(s, b, r, bb.minX - 1, bb.minY + yOff, bb.minZ + zOff, Dir::WEST, genDepth);
    case Dir::WEST: case Dir::EAST: return generateAndAddPiece(s, b, r, bb.minX + zOff, bb.minY + yOff, bb.minZ - 1, Dir::NORTH, genDepth);
    default: return nullptr;
  }
}
Piece* SHPiece::childRight(StartPiece* s, Builder& b, RandomSource& r, int yOff, int zOff) {
  const BoundingBox& bb = boundingBox;
  switch (*orientation()) {
    case Dir::NORTH: case Dir::SOUTH: return generateAndAddPiece(s, b, r, bb.maxX + 1, bb.minY + yOff, bb.minZ + zOff, Dir::EAST, genDepth);
    case Dir::WEST: case Dir::EAST: return generateAndAddPiece(s, b, r, bb.minX + zOff, bb.minY + yOff, bb.maxZ + 1, Dir::SOUTH, genDepth);
    default: return nullptr;
  }
}

#define START static_cast<StartPiece*>(s)
void ChestCorridor::addChildren(Piece* s, Builder& b, RandomSource& r) { childForward(START, b, r, 1, 1); }
void PrisonHall::addChildren(Piece* s, Builder& b, RandomSource& r) { childForward(START, b, r, 1, 1); }
void StraightStairsDown::addChildren(Piece* s, Builder& b, RandomSource& r) { childForward(START, b, r, 1, 1); }
void FiveCrossing::addChildren(Piece* s, Builder& b, RandomSource& r) {
  int a = 3, c = 5;
  Dir o = *orientation();
  if (o == Dir::WEST || o == Dir::NORTH) a = 8 - a, c = 8 - c;
  childForward(START, b, r, 5, 1);
  if (leftLow) childLeft(START, b, r, a, 1);
  if (leftHigh) childLeft(START, b, r, c, 7);
  if (rightLow) childRight(START, b, r, a, 1);
  if (rightHigh) childRight(START, b, r, c, 7);
}
void Turn::addChildren(Piece* s, Builder& b, RandomSource& r) {
  Dir o = *orientation();
  bool notNE = o != Dir::NORTH && o != Dir::EAST;
  if (notNE == left) childRight(START, b, r, 1, 1);
  else childLeft(START, b, r, 1, 1);
}
void PortalRoom::addChildren(Piece* s, Builder&, RandomSource&) {
  if (s) START->portalRoom = this;
}
void RoomCrossing::addChildren(Piece* s, Builder& b, RandomSource& r) {
  childForward(START, b, r, 4, 1);
  childLeft(START, b, r, 1, 4);
  childRight(START, b, r, 1, 4);
}
void StairsDown::addChildren(Piece* s, Builder& b, RandomSource& r) {
  if (isSource) START->imposed = FIVE_CROSSING;
  childForward(START, b, r, 1, 1);
}
void Straight::addChildren(Piece* s, Builder& b, RandomSource& r) {
  childForward(START, b, r, 1, 1);
  if (leftChild) childLeft(START, b, r, 1, 2);
  if (rightChild) childRight(START, b, r, 1, 2);
}
#undef START

PieceP createPiece(Kind k, Builder& b, RandomSource& r, int x, int y, int z, Dir d, int depth) {
  auto make = [&](int ox, int oy, int oz, int w, int h, int dp, auto ctor) -> PieceP {
    BoundingBox box = BoundingBox::orientBox(x, y, z, ox, oy, oz, w, h, dp, d);
    if (!SHPiece::isOkBox(box) || b.findCollisionPiece(box)) return nullptr;
    return ctor(box);
  };
  switch (k) {
    case STRAIGHT: return make(-1, -1, 0, 5, 5, 7, [&](auto& box) { return std::make_unique<Straight>(depth, r, box, d); });
    case PRISON_HALL: return make(-1, -1, 0, 9, 5, 11, [&](auto& box) { return std::make_unique<PrisonHall>(depth, r, box, d); });
    case LEFT_TURN: return make(-1, -1, 0, 5, 5, 5, [&](auto& box) { return std::make_unique<Turn>(true, depth, r, box, d); });
    case RIGHT_TURN: return make(-1, -1, 0, 5, 5, 5, [&](auto& box) { return std::make_unique<Turn>(false, depth, r, box, d); });
    case ROOM_CROSSING: return make(-4, -1, 0, 11, 7, 11, [&](auto& box) { return std::make_unique<RoomCrossing>(depth, r, box, d); });
    case STRAIGHT_STAIRS_DOWN:
      return make(-1, -7, 0, 5, 11, 8, [&](auto& box) { return std::make_unique<StraightStairsDown>(depth, r, box, d); });
    case STAIRS_DOWN: return make(-1, -7, 0, 5, 11, 5, [&](auto& box) { return std::make_unique<StairsDown>(depth, r, box, d); });
    case FIVE_CROSSING: return make(-4, -3, 0, 10, 9, 11, [&](auto& box) { return std::make_unique<FiveCrossing>(depth, r, box, d); });
    case CHEST_CORRIDOR: return make(-1, -1, 0, 5, 5, 7, [&](auto& box) { return std::make_unique<ChestCorridor>(depth, r, box, d); });
    case LIBRARY: {
      BoundingBox box = BoundingBox::orientBox(x, y, z, -4, -1, 0, 14, 11, 15, d);
      if (!SHPiece::isOkBox(box) || b.findCollisionPiece(box)) {
        box = BoundingBox::orientBox(x, y, z, -4, -1, 0, 14, 6, 15, d);
        if (!SHPiece::isOkBox(box) || b.findCollisionPiece(box)) return nullptr;
      }
      return std::make_unique<Library>(depth, r, box, d);
    }
    case PORTAL_ROOM: return make(-4, -1, 0, 11, 8, 16, [&](auto& box) { return std::make_unique<PortalRoom>(depth, box, d); });
    default: return nullptr;
  }
}

PieceP generatePieceFromSmallDoor(StartPiece* start, Builder& builder, RandomSource& random, int x, int y, int z, Dir d, int depth) {
  // updatePieceWeight
  bool any = false;
  start->totalWeight = 0;
  for (auto& p : start->current) {
    if (p.maxPlaceCount > 0 && p.placeCount < p.maxPlaceCount) any = true;
    start->totalWeight += p.weight;
  }
  if (!any) return nullptr;
  if (start->imposed != KIND_NONE) {
    PieceP p = createPiece(start->imposed, builder, random, x, y, z, d, depth);
    start->imposed = KIND_NONE;
    if (p) return p;
  }
  for (int attempts = 0; attempts < 5;) {
    attempts++;
    int sel = random.nextInt(start->totalWeight);
    for (size_t i = 0; i < start->current.size(); i++) {
      PieceWeight& w = start->current[i];
      sel -= w.weight;
      if (sel >= 0) continue;
      if (!w.doPlace(depth) || (int)w.kind == start->previousPiece) break;
      PieceP p = createPiece(w.kind, builder, random, x, y, z, d, depth);
      if (p) {
        w.placeCount++;
        start->previousPiece = (int)w.kind;
        if (!w.isValid()) start->current.erase(start->current.begin() + (long)i);
        return p;
      }
    }
  }
  auto box = FillerCorridor::findPieceBox(builder, x, y, z, d);
  return box && box->minY > 1 ? std::make_unique<FillerCorridor>(depth, *box, d) : nullptr;
}

Piece* generateAndAddPiece(StartPiece* start, Builder& builder, RandomSource& random, int x, int y, int z, Dir d, int depth) {
  if (depth > 50) return nullptr;
  if (std::abs(x - start->boundingBox.minX) > 112 || std::abs(z - start->boundingBox.minZ) > 112) return nullptr;
  PieceP p = generatePieceFromSmallDoor(start, builder, random, x, y, z, d, depth + 1);
  if (!p) return nullptr;
  Piece* raw = p.get();
  builder.add(std::move(p));
  start->pendingChildren.push_back(raw);
  return raw;
}
}  // namespace

StrongholdResult generateStronghold(Builder& builder, RandomSource& random, int x, int z) {
  auto start = std::make_unique<StartPiece>(random, x, z);
  StartPiece* s = start.get();
  builder.add(std::move(start));
  s->addChildren(s, builder, random);
  while (!s->pendingChildren.empty()) {
    int pos = random.nextInt((int)s->pendingChildren.size());
    Piece* p = s->pendingChildren[pos];
    s->pendingChildren.erase(s->pendingChildren.begin() + pos);
    p->addChildren(s, builder, random);
  }
  return {s->portalRoom != nullptr};
}

}  // namespace structure
