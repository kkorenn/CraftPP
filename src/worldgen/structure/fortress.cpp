// NetherFortressPieces (26.3). Piece bodies are a mechanical port of the vanilla source; the static piece-weight
// tables live in the StartPiece (vanilla resets the shared statics there).
#include <algorithm>
#include <cstdlib>

#include "worldgen/structure/legacy.hpp"
#include "worldgen/structure/pieces.hpp"

namespace structure {
namespace {

enum Kind { BRIDGE_STRAIGHT, BRIDGE_CROSSING, ROOM_CROSSING, STAIRS_ROOM, MONSTER_THRONE, CASTLE_ENTRANCE, SMALL_CORRIDOR,
            SMALL_CORRIDOR_CROSSING, SMALL_CORRIDOR_RIGHT, SMALL_CORRIDOR_LEFT, CORRIDOR_STAIRS, CORRIDOR_T_BALCONY, STALK_ROOM };
struct PieceWeight {
  Kind kind;
  int weight, maxPlaceCount;
  bool allowInRow;
  int placeCount = 0;
  bool doPlace() const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
  bool isValid() const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
};

struct StartPiece;
struct NBPiece : Piece {
  NBPiece(int depth, const BoundingBox& bb) : Piece(depth, bb) {}
  static bool isOkBox(const BoundingBox& b) { return b.minY > 10; }
  Piece* generateAndAddPiece(StartPiece* start, Builder& builder, RandomSource& random, int x, int y, int z, Dir d, int depth, bool castle);
  Piece* generateChildForward(Piece* start, Builder& b, RandomSource& r, int xOff, int yOff, bool castle);
  Piece* generateChildLeft(Piece* start, Builder& b, RandomSource& r, int yOff, int zOff, bool castle);
  Piece* generateChildRight(Piece* start, Builder& b, RandomSource& r, int yOff, int zOff, bool castle);
};

struct BridgeCrossing : NBPiece {
  BridgeCrossing(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  static constexpr int BOX[6] = {-8, -3, 0, 19, 10, 19};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildForward(start, builder, random, 8, 3, false);
   generateChildLeft(start, builder, random, 3, 8, false);
   generateChildRight(start, builder, random, 3, 8, false);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 7, 3, 0, 11, 4, 18, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 3, 7, 18, 4, 11, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 8, 5, 0, 10, 7, 18, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 0, 5, 8, 18, 7, 10, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 7, 5, 0, 7, 5, 7, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 7, 5, 11, 7, 5, 18, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 11, 5, 0, 11, 5, 7, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 11, 5, 11, 11, 5, 18, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 5, 7, 7, 5, 7, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 11, 5, 7, 18, 5, 7, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 5, 11, 7, 5, 11, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 11, 5, 11, 18, 5, 11, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 7, 2, 0, 11, 2, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 7, 2, 13, 11, 2, 18, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 7, 0, 0, 11, 1, 3, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 7, 0, 15, 11, 1, 18, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   for (int x = 7; x <= 11; x++) {
      for (int z = 0; z <= 2; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, 18 - z, bb);
      }
   }
   generateBox(level, bb, 0, 2, 7, 5, 2, 11, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 13, 2, 7, 18, 2, 11, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 0, 7, 3, 1, 11, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 15, 0, 7, 18, 1, 11, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   for (int x = 0; x <= 2; x++) {
      for (int z = 7; z <= 11; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
         fillColumnDown(level, BS(blocks::nether_bricks), 18 - x, -1, z, bb);
      }
   }

  }
};

struct BridgeEndFiller : NBPiece {
  int selfSeed = 0;
  BridgeEndFiller(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    selfSeed = r.nextInt();
  }
  void saveState(std::vector<int>& out) const override { Piece::saveState(out), out.push_back(selfSeed); }
  void loadState(const int*& p) override { Piece::loadState(p), selfSeed = *p++; }
  static constexpr int BOX[6] = {-1, -3, 0, 5, 10, 8};
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   SingleThreadedRandomSource selfRandom(selfSeed);
   for (int x = 0; x <= 4; x++) {
      for (int y = 3; y <= 4; y++) {
         int z = selfRandom.nextInt(8);
         generateBox(level, bb, x, y, 0, x, y, z, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
      }
   }
   int z = selfRandom.nextInt(8);
   generateBox(level, bb, 0, 5, 0, 0, 5, z, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   z = selfRandom.nextInt(8);
   generateBox(level, bb, 4, 5, 0, 4, 5, z, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   for (int x = 0; x <= 4; x++) {
      int zx = selfRandom.nextInt(5);
      generateBox(level, bb, x, 2, 0, x, 2, zx, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   }
   for (int x = 0; x <= 4; x++) {
      for (int y = 0; y <= 1; y++) {
         int zx = selfRandom.nextInt(3);
         generateBox(level, bb, x, y, 0, x, y, zx, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
      }
   }

  }
};

struct BridgeStraight : NBPiece {
  BridgeStraight(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  static constexpr int BOX[6] = {-1, -3, 0, 5, 10, 19};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildForward(start, builder, random, 1, 3, false);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 0, 3, 0, 4, 4, 18, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 5, 0, 3, 7, 18, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 0, 5, 0, 0, 5, 18, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 4, 5, 0, 4, 5, 18, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 0, 4, 2, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 13, 4, 2, 18, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 0, 0, 4, 1, 3, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 0, 15, 4, 1, 18, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   for (int x = 0; x <= 4; x++) {
      for (int z = 0; z <= 2; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, 18 - z, bb);
      }
   }
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   BS nseFence = nsFence.with("east", true);
   BS nswFence = nsFence.with("west", true);
   generateBox(level, bb, 0, 1, 1, 0, 4, 1, nseFence, nseFence, false);
   generateBox(level, bb, 0, 3, 4, 0, 4, 4, nseFence, nseFence, false);
   generateBox(level, bb, 0, 3, 14, 0, 4, 14, nseFence, nseFence, false);
   generateBox(level, bb, 0, 1, 17, 0, 4, 17, nseFence, nseFence, false);
   generateBox(level, bb, 4, 1, 1, 4, 4, 1, nswFence, nswFence, false);
   generateBox(level, bb, 4, 3, 4, 4, 4, 4, nswFence, nswFence, false);
   generateBox(level, bb, 4, 3, 14, 4, 4, 14, nswFence, nswFence, false);
   generateBox(level, bb, 4, 1, 17, 4, 4, 17, nswFence, nswFence, false);

  }
};

struct CastleCorridorStairsPiece : NBPiece {
  CastleCorridorStairsPiece(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  static constexpr int BOX[6] = {-1, -7, 0, 5, 14, 10};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildForward(start, builder, random, 1, 0, true);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   BS stairs = BS(blocks::nether_brick_stairs).with("facing", Dir::SOUTH);
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   for (int step = 0; step <= 9; step++) {
      int floor = std::max(1, 7 - step);
      int roof = std::min(std::max(floor + 5, 14 - step), 13);
      int z = step;
      generateBox(
         level, bb, 0, 0, step, 4, floor, step, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false
      );
      generateBox(level, bb, 1, floor + 1, step, 3, roof - 1, step, BS(blocks::air), BS(blocks::air), false);
      if (step <= 6) {
         placeBlock(level, stairs, 1, floor + 1, step, bb);
         placeBlock(level, stairs, 2, floor + 1, step, bb);
         placeBlock(level, stairs, 3, floor + 1, step, bb);
      }
      generateBox(
         level, bb, 0, roof, step, 4, roof, step, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false
      );
      generateBox(
         level, bb, 0, floor + 1, step, 0, roof - 1, step, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false
      );
      generateBox(
         level, bb, 4, floor + 1, step, 4, roof - 1, step, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false
      );
      if ((step & 1) == 0) {
         generateBox(level, bb, 0, floor + 2, step, 0, floor + 3, step, nsFence, nsFence, false);
         generateBox(level, bb, 4, floor + 2, step, 4, floor + 3, step, nsFence, nsFence, false);
      }
      for (int x = 0; x <= 4; x++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
      }
   }

  }
};

struct CastleCorridorTBalconyPiece : NBPiece {
  CastleCorridorTBalconyPiece(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  static constexpr int BOX[6] = {-3, 0, 0, 9, 7, 9};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   int zOff = 1;
   std::optional<Dir> orientation = this->orientation();
   if (orientation == Dir::WEST || orientation == Dir::NORTH) {
      zOff = 5;
   }
   generateChildLeft(start, builder, random, 0, zOff, random.nextInt(8) > 0);
   generateChildRight(start, builder, random, 0, zOff, random.nextInt(8) > 0);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   BS weFence = BS(blocks::nether_brick_fence).with("west", true).with("east", true);
   generateBox(level, bb, 0, 0, 0, 8, 1, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 0, 8, 5, 8, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 0, 6, 0, 8, 6, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 0, 2, 5, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 6, 2, 0, 8, 5, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 3, 0, 1, 4, 0, weFence, weFence, false);
   generateBox(level, bb, 7, 3, 0, 7, 4, 0, weFence, weFence, false);
   generateBox(level, bb, 0, 2, 4, 8, 2, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 1, 4, 2, 2, 4, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 6, 1, 4, 7, 2, 4, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 1, 3, 8, 7, 3, 8, weFence, weFence, false);
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("east", true).with("south", true), 0, 3, 8, bb
   );
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("west", true).with("south", true), 8, 3, 8, bb
   );
   generateBox(level, bb, 0, 3, 6, 0, 3, 7, nsFence, nsFence, false);
   generateBox(level, bb, 8, 3, 6, 8, 3, 7, nsFence, nsFence, false);
   generateBox(level, bb, 0, 3, 4, 0, 5, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 8, 3, 4, 8, 5, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 3, 5, 2, 5, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 6, 3, 5, 7, 5, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 4, 5, 1, 5, 5, weFence, weFence, false);
   generateBox(level, bb, 7, 4, 5, 7, 5, 5, weFence, weFence, false);
   for (int z = 0; z <= 5; z++) {
      for (int x = 0; x <= 8; x++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
      }
   }

  }
};

struct CastleEntrance : NBPiece {
  CastleEntrance(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  static constexpr int BOX[6] = {-5, -3, 0, 13, 14, 13};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildForward(start, builder, random, 5, 3, true);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 0, 3, 0, 12, 4, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 5, 0, 12, 13, 12, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 0, 5, 0, 1, 12, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 11, 5, 0, 12, 12, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 5, 11, 4, 12, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 8, 5, 11, 10, 12, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 5, 9, 11, 7, 12, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 5, 0, 4, 12, 1, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 8, 5, 0, 10, 12, 1, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 5, 9, 0, 7, 12, 1, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 11, 2, 10, 12, 10, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 5, 8, 0, 7, 8, 0, BS(blocks::nether_brick_fence), BS(blocks::nether_brick_fence), false);
   BS weFence = BS(blocks::nether_brick_fence).with("west", true).with("east", true);
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   for (int i = 1; i <= 11; i += 2) {
      generateBox(level, bb, i, 10, 0, i, 11, 0, weFence, weFence, false);
      generateBox(level, bb, i, 10, 12, i, 11, 12, weFence, weFence, false);
      generateBox(level, bb, 0, 10, i, 0, 11, i, nsFence, nsFence, false);
      generateBox(level, bb, 12, 10, i, 12, 11, i, nsFence, nsFence, false);
      placeBlock(level, BS(blocks::nether_bricks), i, 13, 0, bb);
      placeBlock(level, BS(blocks::nether_bricks), i, 13, 12, bb);
      placeBlock(level, BS(blocks::nether_bricks), 0, 13, i, bb);
      placeBlock(level, BS(blocks::nether_bricks), 12, 13, i, bb);
      if (i != 11) {
         placeBlock(level, weFence, i + 1, 13, 0, bb);
         placeBlock(level, weFence, i + 1, 13, 12, bb);
         placeBlock(level, nsFence, 0, 13, i + 1, bb);
         placeBlock(level, nsFence, 12, 13, i + 1, bb);
      }
   }
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("north", true).with("east", true), 0, 13, 0, bb
   );
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("south", true).with("east", true), 0, 13, 12, bb
   );
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("south", true).with("west", true), 12, 13, 12, bb
   );
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("north", true).with("west", true), 12, 13, 0, bb
   );
   for (int z = 3; z <= 9; z += 2) {
      generateBox(level, bb, 1, 7, z, 1, 8, z, nsFence.with("west", true), nsFence.with("west", true), false);
      generateBox(level, bb, 11, 7, z, 11, 8, z, nsFence.with("east", true), nsFence.with("east", true), false);
   }
   generateBox(level, bb, 4, 2, 0, 8, 2, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 4, 12, 2, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 4, 0, 0, 8, 1, 3, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 4, 0, 9, 8, 1, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 0, 4, 3, 1, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 9, 0, 4, 12, 1, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   for (int x = 4; x <= 8; x++) {
      for (int z = 0; z <= 2; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, 12 - z, bb);
      }
   }
   for (int x = 0; x <= 2; x++) {
      for (int z = 4; z <= 8; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
         fillColumnDown(level, BS(blocks::nether_bricks), 12 - x, -1, z, bb);
      }
   }
   generateBox(level, bb, 5, 5, 5, 7, 5, 7, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 6, 1, 6, 6, 4, 6, BS(blocks::air), BS(blocks::air), false);
   placeBlock(level, BS(blocks::nether_bricks), 6, 0, 6, bb);
   placeBlock(level, BS(blocks::lava), 6, 5, 6, bb);
   BlockPos pos = worldPos(6, 5, 6);
   (void)pos;  // ponytail: the lava tick vanilla schedules here is not kept (no worldgen fluid ticks)

  }
};

struct CastleSmallCorridorCrossingPiece : NBPiece {
  CastleSmallCorridorCrossingPiece(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  static constexpr int BOX[6] = {-1, 0, 0, 5, 7, 5};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildForward(start, builder, random, 1, 0, true);
   generateChildLeft(start, builder, random, 0, 1, true);
   generateChildRight(start, builder, random, 0, 1, true);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 0, 0, 0, 4, 1, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 0, 4, 5, 4, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 0, 2, 0, 0, 5, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 4, 2, 0, 4, 5, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 4, 0, 5, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 4, 2, 4, 4, 5, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 6, 0, 4, 6, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   for (int x = 0; x <= 4; x++) {
      for (int z = 0; z <= 4; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
      }
   }

  }
};

struct CastleSmallCorridorLeftTurnPiece : NBPiece {
  bool isNeedingChest = false;
  CastleSmallCorridorLeftTurnPiece(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    isNeedingChest = r.nextInt(3) == 0;
  }
  void saveState(std::vector<int>& out) const override { Piece::saveState(out), out.push_back(isNeedingChest); }
  void loadState(const int*& p) override { Piece::loadState(p), isNeedingChest = *p++; }
  static constexpr int BOX[6] = {-1, 0, 0, 5, 7, 5};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildLeft(start, builder, random, 0, 1, true);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 0, 0, 0, 4, 1, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 0, 4, 5, 4, BS(blocks::air), BS(blocks::air), false);
   BS weFence = BS(blocks::nether_brick_fence).with("west", true).with("east", true);
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   generateBox(level, bb, 4, 2, 0, 4, 5, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 4, 3, 1, 4, 4, 1, nsFence, nsFence, false);
   generateBox(level, bb, 4, 3, 3, 4, 4, 3, nsFence, nsFence, false);
   generateBox(level, bb, 0, 2, 0, 0, 5, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 4, 3, 5, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 3, 4, 1, 4, 4, weFence, weFence, false);
   generateBox(level, bb, 3, 3, 4, 3, 4, 4, weFence, weFence, false);
   if (isNeedingChest && bb.isInside(worldPos(3, 2, 3))) {
      isNeedingChest = false;
      createChest(level, bb, random, 3, 2, 3);
   }
   generateBox(level, bb, 0, 6, 0, 4, 6, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   for (int x = 0; x <= 4; x++) {
      for (int z = 0; z <= 4; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
      }
   }

  }
};

struct CastleSmallCorridorPiece : NBPiece {
  CastleSmallCorridorPiece(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  static constexpr int BOX[6] = {-1, 0, 0, 5, 7, 5};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildForward(start, builder, random, 1, 0, true);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 0, 0, 0, 4, 1, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 0, 4, 5, 4, BS(blocks::air), BS(blocks::air), false);
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   generateBox(level, bb, 0, 2, 0, 0, 5, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 4, 2, 0, 4, 5, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 3, 1, 0, 4, 1, nsFence, nsFence, false);
   generateBox(level, bb, 0, 3, 3, 0, 4, 3, nsFence, nsFence, false);
   generateBox(level, bb, 4, 3, 1, 4, 4, 1, nsFence, nsFence, false);
   generateBox(level, bb, 4, 3, 3, 4, 4, 3, nsFence, nsFence, false);
   generateBox(level, bb, 0, 6, 0, 4, 6, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   for (int x = 0; x <= 4; x++) {
      for (int z = 0; z <= 4; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
      }
   }

  }
};

struct CastleSmallCorridorRightTurnPiece : NBPiece {
  bool isNeedingChest = false;
  CastleSmallCorridorRightTurnPiece(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    isNeedingChest = r.nextInt(3) == 0;
  }
  void saveState(std::vector<int>& out) const override { Piece::saveState(out), out.push_back(isNeedingChest); }
  void loadState(const int*& p) override { Piece::loadState(p), isNeedingChest = *p++; }
  static constexpr int BOX[6] = {-1, 0, 0, 5, 7, 5};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildRight(start, builder, random, 0, 1, true);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 0, 0, 0, 4, 1, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 0, 4, 5, 4, BS(blocks::air), BS(blocks::air), false);
   BS weFence = BS(blocks::nether_brick_fence).with("west", true).with("east", true);
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   generateBox(level, bb, 0, 2, 0, 0, 5, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 3, 1, 0, 4, 1, nsFence, nsFence, false);
   generateBox(level, bb, 0, 3, 3, 0, 4, 3, nsFence, nsFence, false);
   generateBox(level, bb, 4, 2, 0, 4, 5, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 2, 4, 4, 5, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 3, 4, 1, 4, 4, weFence, weFence, false);
   generateBox(level, bb, 3, 3, 4, 3, 4, 4, weFence, weFence, false);
   if (isNeedingChest && bb.isInside(worldPos(1, 2, 3))) {
      isNeedingChest = false;
      createChest(level, bb, random, 1, 2, 3);
   }
   generateBox(level, bb, 0, 6, 0, 4, 6, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   for (int x = 0; x <= 4; x++) {
      for (int z = 0; z <= 4; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
      }
   }

  }
};

struct CastleStalkRoom : NBPiece {
  CastleStalkRoom(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  static constexpr int BOX[6] = {-5, -3, 0, 13, 14, 13};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildForward(start, builder, random, 5, 3, true);
   generateChildForward(start, builder, random, 5, 11, true);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 0, 3, 0, 12, 4, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 5, 0, 12, 13, 12, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 0, 5, 0, 1, 12, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 11, 5, 0, 12, 12, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 5, 11, 4, 12, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 8, 5, 11, 10, 12, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 5, 9, 11, 7, 12, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 5, 0, 4, 12, 1, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 8, 5, 0, 10, 12, 1, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 5, 9, 0, 7, 12, 1, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 11, 2, 10, 12, 10, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   BS weFence = BS(blocks::nether_brick_fence).with("west", true).with("east", true);
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   BS nswFence = nsFence.with("west", true);
   BS nseFence = nsFence.with("east", true);
   for (int i = 1; i <= 11; i += 2) {
      generateBox(level, bb, i, 10, 0, i, 11, 0, weFence, weFence, false);
      generateBox(level, bb, i, 10, 12, i, 11, 12, weFence, weFence, false);
      generateBox(level, bb, 0, 10, i, 0, 11, i, nsFence, nsFence, false);
      generateBox(level, bb, 12, 10, i, 12, 11, i, nsFence, nsFence, false);
      placeBlock(level, BS(blocks::nether_bricks), i, 13, 0, bb);
      placeBlock(level, BS(blocks::nether_bricks), i, 13, 12, bb);
      placeBlock(level, BS(blocks::nether_bricks), 0, 13, i, bb);
      placeBlock(level, BS(blocks::nether_bricks), 12, 13, i, bb);
      if (i != 11) {
         placeBlock(level, weFence, i + 1, 13, 0, bb);
         placeBlock(level, weFence, i + 1, 13, 12, bb);
         placeBlock(level, nsFence, 0, 13, i + 1, bb);
         placeBlock(level, nsFence, 12, 13, i + 1, bb);
      }
   }
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("north", true).with("east", true), 0, 13, 0, bb
   );
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("south", true).with("east", true), 0, 13, 12, bb
   );
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("south", true).with("west", true), 12, 13, 12, bb
   );
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("north", true).with("west", true), 12, 13, 0, bb
   );
   for (int z = 3; z <= 9; z += 2) {
      generateBox(level, bb, 1, 7, z, 1, 8, z, nswFence, nswFence, false);
      generateBox(level, bb, 11, 7, z, 11, 8, z, nseFence, nseFence, false);
   }
   BS stairs = BS(blocks::nether_brick_stairs).with("facing", Dir::NORTH);
   for (int ix = 0; ix <= 6; ix++) {
      int z = ix + 4;
      for (int x = 5; x <= 7; x++) {
         placeBlock(level, stairs, x, 5 + ix, z, bb);
      }
      if (z >= 5 && z <= 8) {
         generateBox(
            level, bb, 5, 5, z, 7, ix + 4, z, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false
         );
      } else if (z >= 9 && z <= 10) {
         generateBox(
            level, bb, 5, 8, z, 7, ix + 4, z, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false
         );
      }
      if (ix >= 1) {
         generateBox(level, bb, 5, 6 + ix, z, 7, 9 + ix, z, BS(blocks::air), BS(blocks::air), false);
      }
   }
   for (int x = 5; x <= 7; x++) {
      placeBlock(level, stairs, x, 12, 11, bb);
   }
   generateBox(level, bb, 5, 6, 7, 5, 7, 7, nseFence, nseFence, false);
   generateBox(level, bb, 7, 6, 7, 7, 7, 7, nswFence, nswFence, false);
   generateBox(level, bb, 5, 13, 12, 7, 13, 12, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 2, 5, 2, 3, 5, 3, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 5, 9, 3, 5, 10, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 5, 4, 2, 5, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 9, 5, 2, 10, 5, 3, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 9, 5, 9, 10, 5, 10, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 10, 5, 4, 10, 5, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   BS eastStairs = stairs.with("facing", Dir::EAST);
   BS westStairs = stairs.with("facing", Dir::WEST);
   placeBlock(level, westStairs, 4, 5, 2, bb);
   placeBlock(level, westStairs, 4, 5, 3, bb);
   placeBlock(level, westStairs, 4, 5, 9, bb);
   placeBlock(level, westStairs, 4, 5, 10, bb);
   placeBlock(level, eastStairs, 8, 5, 2, bb);
   placeBlock(level, eastStairs, 8, 5, 3, bb);
   placeBlock(level, eastStairs, 8, 5, 9, bb);
   placeBlock(level, eastStairs, 8, 5, 10, bb);
   generateBox(level, bb, 3, 4, 4, 4, 4, 8, BS(blocks::soul_sand), BS(blocks::soul_sand), false);
   generateBox(level, bb, 8, 4, 4, 9, 4, 8, BS(blocks::soul_sand), BS(blocks::soul_sand), false);
   generateBox(level, bb, 3, 5, 4, 4, 5, 8, BS(blocks::nether_wart), BS(blocks::nether_wart), false);
   generateBox(level, bb, 8, 5, 4, 9, 5, 8, BS(blocks::nether_wart), BS(blocks::nether_wart), false);
   generateBox(level, bb, 4, 2, 0, 8, 2, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 4, 12, 2, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 4, 0, 0, 8, 1, 3, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 4, 0, 9, 8, 1, 12, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 0, 4, 3, 1, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 9, 0, 4, 12, 1, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   for (int x = 4; x <= 8; x++) {
      for (int z = 0; z <= 2; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, 12 - z, bb);
      }
   }
   for (int x = 0; x <= 2; x++) {
      for (int z = 4; z <= 8; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
         fillColumnDown(level, BS(blocks::nether_bricks), 12 - x, -1, z, bb);
      }
   }

  }
};

struct MonsterThrone : NBPiece {
  bool hasPlacedSpawner = false;
  MonsterThrone(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  void saveState(std::vector<int>& out) const override { Piece::saveState(out), out.push_back(hasPlacedSpawner); }
  void loadState(const int*& p) override { Piece::loadState(p), hasPlacedSpawner = *p++; }
  static constexpr int BOX[6] = {-2, 0, 0, 7, 8, 9};
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 0, 2, 0, 6, 7, 7, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 1, 0, 0, 5, 1, 7, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 2, 1, 5, 2, 7, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 3, 2, 5, 3, 7, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 4, 3, 5, 4, 7, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 2, 0, 1, 4, 2, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 5, 2, 0, 5, 4, 2, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 5, 2, 1, 5, 3, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 5, 5, 2, 5, 5, 3, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 5, 3, 0, 5, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 6, 5, 3, 6, 5, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 5, 8, 5, 5, 8, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   BS weFence = BS(blocks::nether_brick_fence).with("west", true).with("east", true);
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   placeBlock(level, BS(blocks::nether_brick_fence).with("west", true), 1, 6, 3, bb);
   placeBlock(level, BS(blocks::nether_brick_fence).with("east", true), 5, 6, 3, bb);
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("east", true).with("north", true), 0, 6, 3, bb
   );
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("west", true).with("north", true), 6, 6, 3, bb
   );
   generateBox(level, bb, 0, 6, 4, 0, 6, 7, nsFence, nsFence, false);
   generateBox(level, bb, 6, 6, 4, 6, 6, 7, nsFence, nsFence, false);
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("east", true).with("south", true), 0, 6, 8, bb
   );
   placeBlock(
      level, BS(blocks::nether_brick_fence).with("west", true).with("south", true), 6, 6, 8, bb
   );
   generateBox(level, bb, 1, 6, 8, 5, 6, 8, weFence, weFence, false);
   placeBlock(level, BS(blocks::nether_brick_fence).with("east", true), 1, 7, 8, bb);
   generateBox(level, bb, 2, 7, 8, 4, 7, 8, weFence, weFence, false);
   placeBlock(level, BS(blocks::nether_brick_fence).with("west", true), 5, 7, 8, bb);
   placeBlock(level, BS(blocks::nether_brick_fence).with("east", true), 2, 8, 8, bb);
   placeBlock(level, weFence, 3, 8, 8, bb);
   placeBlock(level, BS(blocks::nether_brick_fence).with("west", true), 4, 8, 8, bb);
   if (!hasPlacedSpawner) {
      BlockPos pos = worldPos(3, 5, 5);
      if (bb.isInside(pos)) {
         hasPlacedSpawner = true;
         level.set(pos, blocks::spawner);  // setEntityId draws no random (empty spawn potentials)
      }
   }
   for (int x = 0; x <= 6; x++) {
      for (int z = 0; z <= 6; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
      }
   }

  }
};

struct RoomCrossing : NBPiece {
  RoomCrossing(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  static constexpr int BOX[6] = {-2, 0, 0, 7, 9, 7};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildForward(start, builder, random, 2, 0, false);
   generateChildLeft(start, builder, random, 0, 2, false);
   generateChildRight(start, builder, random, 0, 2, false);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 0, 0, 0, 6, 1, 6, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 0, 6, 7, 6, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 0, 2, 0, 1, 6, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 6, 1, 6, 6, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 5, 2, 0, 6, 6, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 5, 2, 6, 6, 6, 6, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 0, 0, 6, 1, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 5, 0, 6, 6, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 6, 2, 0, 6, 6, 1, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 6, 2, 5, 6, 6, 6, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   BS weFence = BS(blocks::nether_brick_fence).with("west", true).with("east", true);
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   generateBox(level, bb, 2, 6, 0, 4, 6, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 5, 0, 4, 5, 0, weFence, weFence, false);
   generateBox(level, bb, 2, 6, 6, 4, 6, 6, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 5, 6, 4, 5, 6, weFence, weFence, false);
   generateBox(level, bb, 0, 6, 2, 0, 6, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 5, 2, 0, 5, 4, nsFence, nsFence, false);
   generateBox(level, bb, 6, 6, 2, 6, 6, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 6, 5, 2, 6, 5, 4, nsFence, nsFence, false);
   for (int x = 0; x <= 6; x++) {
      for (int z = 0; z <= 6; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
      }
   }

  }
};

struct StairsRoom : NBPiece {
  StairsRoom(int depth, RandomSource& r, const BoundingBox& bb, Dir d) : NBPiece(depth, bb) {
    setOrientation(d);
    (void)r;
  }
  static constexpr int BOX[6] = {-2, 0, 0, 7, 11, 7};
  void addChildren(Piece* s, Builder& builder, RandomSource& random) override {
    Piece* start = s;
   generateChildRight(start, builder, random, 6, 2, false);

  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& bb, int, int, BlockPos) override {
    (void)random;
   generateBox(level, bb, 0, 0, 0, 6, 1, 6, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 0, 6, 10, 6, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 0, 2, 0, 1, 8, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 5, 2, 0, 6, 8, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 0, 2, 1, 0, 8, 6, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 6, 2, 1, 6, 8, 6, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 2, 6, 5, 8, 6, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   BS weFence = BS(blocks::nether_brick_fence).with("west", true).with("east", true);
   BS nsFence = BS(blocks::nether_brick_fence).with("north", true).with("south", true);
   generateBox(level, bb, 0, 3, 2, 0, 5, 4, nsFence, nsFence, false);
   generateBox(level, bb, 6, 3, 2, 6, 5, 2, nsFence, nsFence, false);
   generateBox(level, bb, 6, 3, 4, 6, 5, 4, nsFence, nsFence, false);
   placeBlock(level, BS(blocks::nether_bricks), 5, 2, 5, bb);
   generateBox(level, bb, 4, 2, 5, 4, 3, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 3, 2, 5, 3, 4, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 2, 5, 2, 5, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 2, 5, 1, 6, 5, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 1, 7, 1, 5, 7, 4, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 6, 8, 2, 6, 8, 4, BS(blocks::air), BS(blocks::air), false);
   generateBox(level, bb, 2, 6, 0, 4, 8, 0, BS(blocks::nether_bricks), BS(blocks::nether_bricks), false);
   generateBox(level, bb, 2, 5, 0, 4, 5, 0, weFence, weFence, false);
   for (int x = 0; x <= 6; x++) {
      for (int z = 0; z <= 6; z++) {
         fillColumnDown(level, BS(blocks::nether_bricks), x, -1, z, bb);
      }
   }

  }
};

struct StartPiece : BridgeCrossing {
  PieceWeight* previousPiece = nullptr;
  std::vector<PieceWeight> bridgeWeights, castleWeights;      // owned tables (vanilla statics)
  std::vector<PieceWeight*> availableBridgePieces, availableCastlePieces;
  std::vector<Piece*> pendingChildren;
  StartPiece(RandomSource& random, int west, int north, Dir d)
      : BridgeCrossing(0, random, makeBoundingBox(west, 64, north, d, 19, 10, 19), d) {
    bridgeWeights = {{BRIDGE_STRAIGHT, 30, 0, true}, {BRIDGE_CROSSING, 10, 4, false}, {ROOM_CROSSING, 10, 4, false},
                     {STAIRS_ROOM, 10, 3, false},    {MONSTER_THRONE, 5, 2, false},  {CASTLE_ENTRANCE, 5, 1, false}};
    castleWeights = {{SMALL_CORRIDOR, 25, 0, true},       {SMALL_CORRIDOR_CROSSING, 15, 5, false}, {SMALL_CORRIDOR_RIGHT, 5, 10, false},
                     {SMALL_CORRIDOR_LEFT, 5, 10, false}, {CORRIDOR_STAIRS, 10, 3, true},          {CORRIDOR_T_BALCONY, 7, 2, false},
                     {STALK_ROOM, 5, 2, false}};
    for (auto& w : bridgeWeights) availableBridgePieces.push_back(&w);
    for (auto& w : castleWeights) availableCastlePieces.push_back(&w);
  }
};

template <class T>
PieceP make(Builder& b, RandomSource& r, int x, int y, int z, Dir d, int depth) {
  BoundingBox box = BoundingBox::orientBox(x, y, z, T::BOX[0], T::BOX[1], T::BOX[2], T::BOX[3], T::BOX[4], T::BOX[5], d);
  if (!NBPiece::isOkBox(box) || b.findCollisionPiece(box)) return nullptr;
  return std::make_unique<T>(depth, r, box, d);
}

PieceP createPiece(Kind k, Builder& b, RandomSource& r, int x, int y, int z, Dir d, int depth) {  // findAndCreateBridgePieceFactory
  switch (k) {
    case BRIDGE_STRAIGHT: return make<BridgeStraight>(b, r, x, y, z, d, depth);
    case BRIDGE_CROSSING: return make<BridgeCrossing>(b, r, x, y, z, d, depth);
    case ROOM_CROSSING: return make<RoomCrossing>(b, r, x, y, z, d, depth);
    case STAIRS_ROOM: return make<StairsRoom>(b, r, x, y, z, d, depth);
    case MONSTER_THRONE: return make<MonsterThrone>(b, r, x, y, z, d, depth);
    case CASTLE_ENTRANCE: return make<CastleEntrance>(b, r, x, y, z, d, depth);
    case SMALL_CORRIDOR: return make<CastleSmallCorridorPiece>(b, r, x, y, z, d, depth);
    case SMALL_CORRIDOR_CROSSING: return make<CastleSmallCorridorCrossingPiece>(b, r, x, y, z, d, depth);
    case SMALL_CORRIDOR_RIGHT: return make<CastleSmallCorridorRightTurnPiece>(b, r, x, y, z, d, depth);
    case SMALL_CORRIDOR_LEFT: return make<CastleSmallCorridorLeftTurnPiece>(b, r, x, y, z, d, depth);
    case CORRIDOR_STAIRS: return make<CastleCorridorStairsPiece>(b, r, x, y, z, d, depth);
    case CORRIDOR_T_BALCONY: return make<CastleCorridorTBalconyPiece>(b, r, x, y, z, d, depth);
    case STALK_ROOM: return make<CastleStalkRoom>(b, r, x, y, z, d, depth);
  }
  return nullptr;
}

PieceP generatePiece(StartPiece* start, std::vector<PieceWeight*>& pieces, Builder& b, RandomSource& random, int x, int y, int z,
                     Dir d, int depth) {
  bool any = false;
  int total = 0;
  for (auto* p : pieces) {
    if (p->maxPlaceCount > 0 && p->placeCount < p->maxPlaceCount) any = true;
    total += p->weight;
  }
  if (!any) total = -1;
  bool doStuff = total > 0 && depth <= 30;
  for (int attempts = 0; attempts < 5 && doStuff;) {
    attempts++;
    int sel = random.nextInt(total);
    for (size_t i = 0; i < pieces.size(); i++) {
      PieceWeight* p = pieces[i];
      sel -= p->weight;
      if (sel < 0) {
        if (!p->doPlace() || (p == start->previousPiece && !p->allowInRow)) break;
        PieceP piece = createPiece(p->kind, b, random, x, y, z, d, depth);
        if (piece) {
          p->placeCount++;
          start->previousPiece = p;
          if (!p->isValid()) pieces.erase(pieces.begin() + long(i));
          return piece;
        }
      }
    }
  }
  return make<BridgeEndFiller>(b, random, x, y, z, d, depth);
}

Piece* NBPiece::generateAndAddPiece(StartPiece* start, Builder& b, RandomSource& random, int x, int y, int z, Dir d, int depth, bool castle) {
  if (std::abs(x - start->boundingBox.minX) > 112 || std::abs(z - start->boundingBox.minZ) > 112) {
    make<BridgeEndFiller>(b, random, x, y, z, d, depth);  // vanilla returns it without adding it
    return nullptr;
  }
  PieceP p = generatePiece(start, castle ? start->availableCastlePieces : start->availableBridgePieces, b, random, x, y, z, d, depth + 1);
  if (!p) return nullptr;
  Piece* raw = p.get();
  b.add(std::move(p));
  start->pendingChildren.push_back(raw);
  return raw;
}

Piece* NBPiece::generateChildForward(Piece* sp, Builder& b, RandomSource& r, int xOff, int yOff, bool castle) {
  if (!orientation()) return nullptr;
  auto* s = static_cast<StartPiece*>(sp);
  const BoundingBox& bb = boundingBox;
  switch (*orientation()) {
    case Dir::NORTH: return generateAndAddPiece(s, b, r, bb.minX + xOff, bb.minY + yOff, bb.minZ - 1, Dir::NORTH, genDepth, castle);
    case Dir::SOUTH: return generateAndAddPiece(s, b, r, bb.minX + xOff, bb.minY + yOff, bb.maxZ + 1, Dir::SOUTH, genDepth, castle);
    case Dir::WEST: return generateAndAddPiece(s, b, r, bb.minX - 1, bb.minY + yOff, bb.minZ + xOff, Dir::WEST, genDepth, castle);
    case Dir::EAST: return generateAndAddPiece(s, b, r, bb.maxX + 1, bb.minY + yOff, bb.minZ + xOff, Dir::EAST, genDepth, castle);
    default: return nullptr;
  }
}

Piece* NBPiece::generateChildLeft(Piece* sp, Builder& b, RandomSource& r, int yOff, int zOff, bool castle) {
  if (!orientation()) return nullptr;
  auto* s = static_cast<StartPiece*>(sp);
  const BoundingBox& bb = boundingBox;
  switch (*orientation()) {
    case Dir::NORTH: case Dir::SOUTH: return generateAndAddPiece(s, b, r, bb.minX - 1, bb.minY + yOff, bb.minZ + zOff, Dir::WEST, genDepth, castle);
    case Dir::WEST: case Dir::EAST: return generateAndAddPiece(s, b, r, bb.minX + zOff, bb.minY + yOff, bb.minZ - 1, Dir::NORTH, genDepth, castle);
    default: return nullptr;
  }
}

Piece* NBPiece::generateChildRight(Piece* sp, Builder& b, RandomSource& r, int yOff, int zOff, bool castle) {
  if (!orientation()) return nullptr;
  auto* s = static_cast<StartPiece*>(sp);
  const BoundingBox& bb = boundingBox;
  switch (*orientation()) {
    case Dir::NORTH: case Dir::SOUTH: return generateAndAddPiece(s, b, r, bb.maxX + 1, bb.minY + yOff, bb.minZ + zOff, Dir::EAST, genDepth, castle);
    case Dir::WEST: case Dir::EAST: return generateAndAddPiece(s, b, r, bb.minX + zOff, bb.minY + yOff, bb.maxZ + 1, Dir::SOUTH, genDepth, castle);
    default: return nullptr;
  }
}

}  // namespace

void generateFortress(Builder& builder, RandomSource& random, int x, int z) {
  Dir d = randomHorizontal(random);
  auto start = std::make_unique<StartPiece>(random, x, z, d);
  StartPiece* s = start.get();
  builder.add(std::move(start));
  s->addChildren(s, builder, random);
  while (!s->pendingChildren.empty()) {
    int pos = random.nextInt((int)s->pendingChildren.size());
    Piece* p = s->pendingChildren[size_t(pos)];
    s->pendingChildren.erase(s->pendingChildren.begin() + pos);
    p->addChildren(s, builder, random);
  }
}

}  // namespace structure
