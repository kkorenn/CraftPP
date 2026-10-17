// Scattered single-piece structures: swamp hut, desert pyramid, jungle temple, buried treasure.
// Bodies are mechanically translated from vanilla 26.3 (StructurePiece helper calls kept 1:1).
#include <set>

#include "worldgen/structure/pieces.hpp"
#include "worldgen/structure/legacy.hpp"

namespace structure {

bool ScatteredPiece::updateAverageGroundHeight(Level& level, const BoundingBox& chunkBB, int offset) {
  if (heightPosition >= 0) return true;
  int total = 0, count = 0;
  for (int z = boundingBox.minZ; z <= boundingBox.maxZ; z++)
    for (int x = boundingBox.minX; x <= boundingBox.maxX; x++)
      if (chunkBB.isInside(x, 64, z)) {
        total += level.getHeight(Heightmap::MOTION_BLOCKING_NO_LEAVES, x, z);
        count++;
      }
  if (count == 0) return false;
  heightPosition = total / count;
  boundingBox.move(0, heightPosition - boundingBox.minY + offset, 0);
  return true;
}

bool ScatteredPiece::updateHeightPositionToLowestGroundHeight(Level& level, int offset) {
  if (heightPosition >= 0) return true;
  int lowest = MAX_Y + 1;
  bool found = false;
  for (int z = boundingBox.minZ; z <= boundingBox.maxZ; z++)
    for (int x = boundingBox.minX; x <= boundingBox.maxX; x++) {
      lowest = std::min(lowest, level.getHeight(Heightmap::MOTION_BLOCKING_NO_LEAVES, x, z));
      found = true;
    }
  if (!found) return false;
  heightPosition = lowest;
  boundingBox.move(0, heightPosition - boundingBox.minY + offset, 0);
  return true;
}

namespace {

struct SwampHutPiece : ScatteredPiece {
  bool spawnedWitch = false, spawnedCat = false;
  explicit SwampHutPiece(RandomSource& random, int west, int north) : ScatteredPiece(west, 64, north, 7, 7, 9, randomHorizontal(random)) {}
  void saveState(std::vector<int>& out) const override { ScatteredPiece::saveState(out), out.push_back(spawnedWitch | spawnedCat << 1); }
  void loadState(const int*& p) override { ScatteredPiece::loadState(p), spawnedWitch = *p & 1, spawnedCat = *p++ >> 1 & 1; }
   void postProcess(Level& level, RandomSource&, const BoundingBox& chunkBB, int, int, BlockPos) override {
      if (updateAverageGroundHeight(level, chunkBB, 0)) {
         generateBox(level, chunkBB, 1, 1, 1, 5, 1, 7, BS(blocks::spruce_planks), BS(blocks::spruce_planks), false);
         generateBox(level, chunkBB, 1, 4, 2, 5, 4, 7, BS(blocks::spruce_planks), BS(blocks::spruce_planks), false);
         generateBox(level, chunkBB, 2, 1, 0, 4, 1, 0, BS(blocks::spruce_planks), BS(blocks::spruce_planks), false);
         generateBox(level, chunkBB, 2, 2, 2, 3, 3, 2, BS(blocks::spruce_planks), BS(blocks::spruce_planks), false);
         generateBox(level, chunkBB, 1, 2, 3, 1, 3, 6, BS(blocks::spruce_planks), BS(blocks::spruce_planks), false);
         generateBox(level, chunkBB, 5, 2, 3, 5, 3, 6, BS(blocks::spruce_planks), BS(blocks::spruce_planks), false);
         generateBox(level, chunkBB, 2, 2, 7, 4, 3, 7, BS(blocks::spruce_planks), BS(blocks::spruce_planks), false);
         generateBox(level, chunkBB, 1, 0, 2, 1, 3, 2, BS(blocks::oak_log), BS(blocks::oak_log), false);
         generateBox(level, chunkBB, 5, 0, 2, 5, 3, 2, BS(blocks::oak_log), BS(blocks::oak_log), false);
         generateBox(level, chunkBB, 1, 0, 7, 1, 3, 7, BS(blocks::oak_log), BS(blocks::oak_log), false);
         generateBox(level, chunkBB, 5, 0, 7, 5, 3, 7, BS(blocks::oak_log), BS(blocks::oak_log), false);
         placeBlock(level, BS(blocks::oak_fence), 2, 3, 2, chunkBB);
         placeBlock(level, BS(blocks::oak_fence), 3, 3, 7, chunkBB);
         placeBlock(level, BS(blocks::air), 1, 3, 4, chunkBB);
         placeBlock(level, BS(blocks::air), 5, 3, 4, chunkBB);
         placeBlock(level, BS(blocks::air), 5, 3, 5, chunkBB);
         placeBlock(level, BS(blocks::potted_red_mushroom), 1, 3, 5, chunkBB);
         placeBlock(level, BS(blocks::crafting_table), 3, 2, 6, chunkBB);
         placeBlock(level, BS(blocks::cauldron), 4, 2, 6, chunkBB);
         placeBlock(level, BS(blocks::oak_fence), 1, 2, 1, chunkBB);
         placeBlock(level, BS(blocks::oak_fence), 5, 2, 1, chunkBB);
         BS northStairs = BS(blocks::spruce_stairs).with("facing", Dir::NORTH);
         BS eastStairs = BS(blocks::spruce_stairs).with("facing", Dir::EAST);
         BS westStairs = BS(blocks::spruce_stairs).with("facing", Dir::WEST);
         BS southStairs = BS(blocks::spruce_stairs).with("facing", Dir::SOUTH);
         generateBox(level, chunkBB, 0, 4, 1, 6, 4, 1, northStairs, northStairs, false);
         generateBox(level, chunkBB, 0, 4, 2, 0, 4, 7, eastStairs, eastStairs, false);
         generateBox(level, chunkBB, 6, 4, 2, 6, 4, 7, westStairs, westStairs, false);
         generateBox(level, chunkBB, 0, 4, 8, 6, 4, 8, southStairs, southStairs, false);
         placeBlock(level, northStairs.with("shape", "outer_right"), 0, 4, 1, chunkBB);
         placeBlock(level, northStairs.with("shape", "outer_left"), 6, 4, 1, chunkBB);
         placeBlock(level, southStairs.with("shape", "outer_left"), 0, 4, 8, chunkBB);
         placeBlock(level, southStairs.with("shape", "outer_right"), 6, 4, 8, chunkBB);

         for (int z = 2; z <= 7; z += 5) {
            for (int x = 1; x <= 5; x += 4) {
               fillColumnDown(level, BS(blocks::oak_log), x, -1, z, chunkBB);
            }
         }

         BlockPos pos = worldPos(2, 2, 5);
         if (!spawnedWitch && chunkBB.isInside(pos)) spawnedWitch = true, level.addMob(entity_types::witch, pos, true);
         if (!spawnedCat && chunkBB.isInside(pos)) spawnedCat = true, level.addMob(entity_types::cat, pos, true);
      }
   }
};

struct MossStoneSelector : Piece::BlockSelector {
  void select(RandomSource& random, int, int, int, bool) override {
    next = random.nextFloat() < 0.4F ? blocks::cobblestone : blocks::mossy_cobblestone;
  }
};

struct JungleTemplePiece : ScatteredPiece {
  bool placedMainChest = false, placedHiddenChest = false, placedTrap1 = false, placedTrap2 = false;
  explicit JungleTemplePiece(RandomSource& random, int west, int north) : ScatteredPiece(west, 64, north, 12, 10, 15, randomHorizontal(random)) {}
  void saveState(std::vector<int>& out) const override {
    ScatteredPiece::saveState(out);
    out.insert(out.end(), {placedMainChest, placedHiddenChest, placedTrap1, placedTrap2});
  }
  void loadState(const int*& p) override {
    ScatteredPiece::loadState(p);
    placedMainChest = p[0], placedHiddenChest = p[1], placedTrap1 = p[2], placedTrap2 = p[3];
    p += 4;
  }
   void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int, int, BlockPos) override {
      MossStoneSelector stoneSelector;
      if (updateAverageGroundHeight(level, chunkBB, 0)) {
         generateBox(level, chunkBB, 0, -4, 0, width - 1, 0, depth - 1, false, random, stoneSelector);
         generateBox(level, chunkBB, 2, 1, 2, 9, 2, 2, false, random, stoneSelector);
         generateBox(level, chunkBB, 2, 1, 12, 9, 2, 12, false, random, stoneSelector);
         generateBox(level, chunkBB, 2, 1, 3, 2, 2, 11, false, random, stoneSelector);
         generateBox(level, chunkBB, 9, 1, 3, 9, 2, 11, false, random, stoneSelector);
         generateBox(level, chunkBB, 1, 3, 1, 10, 6, 1, false, random, stoneSelector);
         generateBox(level, chunkBB, 1, 3, 13, 10, 6, 13, false, random, stoneSelector);
         generateBox(level, chunkBB, 1, 3, 2, 1, 6, 12, false, random, stoneSelector);
         generateBox(level, chunkBB, 10, 3, 2, 10, 6, 12, false, random, stoneSelector);
         generateBox(level, chunkBB, 2, 3, 2, 9, 3, 12, false, random, stoneSelector);
         generateBox(level, chunkBB, 2, 6, 2, 9, 6, 12, false, random, stoneSelector);
         generateBox(level, chunkBB, 3, 7, 3, 8, 7, 11, false, random, stoneSelector);
         generateBox(level, chunkBB, 4, 8, 4, 7, 8, 10, false, random, stoneSelector);
         generateAirBox(level, chunkBB, 3, 1, 3, 8, 2, 11);
         generateAirBox(level, chunkBB, 4, 3, 6, 7, 3, 9);
         generateAirBox(level, chunkBB, 2, 4, 2, 9, 5, 12);
         generateAirBox(level, chunkBB, 4, 6, 5, 7, 6, 9);
         generateAirBox(level, chunkBB, 5, 7, 6, 6, 7, 8);
         generateAirBox(level, chunkBB, 5, 1, 2, 6, 2, 2);
         generateAirBox(level, chunkBB, 5, 2, 12, 6, 2, 12);
         generateAirBox(level, chunkBB, 5, 5, 1, 6, 5, 1);
         generateAirBox(level, chunkBB, 5, 5, 13, 6, 5, 13);
         placeBlock(level, BS(blocks::air), 1, 5, 5, chunkBB);
         placeBlock(level, BS(blocks::air), 10, 5, 5, chunkBB);
         placeBlock(level, BS(blocks::air), 1, 5, 9, chunkBB);
         placeBlock(level, BS(blocks::air), 10, 5, 9, chunkBB);

         for (int z = 0; z <= 14; z += 14) {
            generateBox(level, chunkBB, 2, 4, z, 2, 5, z, false, random, stoneSelector);
            generateBox(level, chunkBB, 4, 4, z, 4, 5, z, false, random, stoneSelector);
            generateBox(level, chunkBB, 7, 4, z, 7, 5, z, false, random, stoneSelector);
            generateBox(level, chunkBB, 9, 4, z, 9, 5, z, false, random, stoneSelector);
         }

         generateBox(level, chunkBB, 5, 6, 0, 6, 6, 0, false, random, stoneSelector);

         for (int x = 0; x <= 11; x += 11) {
            for (int z = 2; z <= 12; z += 2) {
               generateBox(level, chunkBB, x, 4, z, x, 5, z, false, random, stoneSelector);
            }

            generateBox(level, chunkBB, x, 6, 5, x, 6, 5, false, random, stoneSelector);
            generateBox(level, chunkBB, x, 6, 9, x, 6, 9, false, random, stoneSelector);
         }

         generateBox(level, chunkBB, 2, 7, 2, 2, 9, 2, false, random, stoneSelector);
         generateBox(level, chunkBB, 9, 7, 2, 9, 9, 2, false, random, stoneSelector);
         generateBox(level, chunkBB, 2, 7, 12, 2, 9, 12, false, random, stoneSelector);
         generateBox(level, chunkBB, 9, 7, 12, 9, 9, 12, false, random, stoneSelector);
         generateBox(level, chunkBB, 4, 9, 4, 4, 9, 4, false, random, stoneSelector);
         generateBox(level, chunkBB, 7, 9, 4, 7, 9, 4, false, random, stoneSelector);
         generateBox(level, chunkBB, 4, 9, 10, 4, 9, 10, false, random, stoneSelector);
         generateBox(level, chunkBB, 7, 9, 10, 7, 9, 10, false, random, stoneSelector);
         generateBox(level, chunkBB, 5, 9, 7, 6, 9, 7, false, random, stoneSelector);
         BS eastStairs = BS(blocks::cobblestone_stairs).with("facing", Dir::EAST);
         BS westStairs = BS(blocks::cobblestone_stairs).with("facing", Dir::WEST);
         BS southStairs = BS(blocks::cobblestone_stairs).with("facing", Dir::SOUTH);
         BS northStairs = BS(blocks::cobblestone_stairs).with("facing", Dir::NORTH);
         placeBlock(level, northStairs, 5, 9, 6, chunkBB);
         placeBlock(level, northStairs, 6, 9, 6, chunkBB);
         placeBlock(level, southStairs, 5, 9, 8, chunkBB);
         placeBlock(level, southStairs, 6, 9, 8, chunkBB);
         placeBlock(level, northStairs, 4, 0, 0, chunkBB);
         placeBlock(level, northStairs, 5, 0, 0, chunkBB);
         placeBlock(level, northStairs, 6, 0, 0, chunkBB);
         placeBlock(level, northStairs, 7, 0, 0, chunkBB);
         placeBlock(level, northStairs, 4, 1, 8, chunkBB);
         placeBlock(level, northStairs, 4, 2, 9, chunkBB);
         placeBlock(level, northStairs, 4, 3, 10, chunkBB);
         placeBlock(level, northStairs, 7, 1, 8, chunkBB);
         placeBlock(level, northStairs, 7, 2, 9, chunkBB);
         placeBlock(level, northStairs, 7, 3, 10, chunkBB);
         generateBox(level, chunkBB, 4, 1, 9, 4, 1, 9, false, random, stoneSelector);
         generateBox(level, chunkBB, 7, 1, 9, 7, 1, 9, false, random, stoneSelector);
         generateBox(level, chunkBB, 4, 1, 10, 7, 2, 10, false, random, stoneSelector);
         generateBox(level, chunkBB, 5, 4, 5, 6, 4, 5, false, random, stoneSelector);
         placeBlock(level, eastStairs, 4, 4, 5, chunkBB);
         placeBlock(level, westStairs, 7, 4, 5, chunkBB);

         for (int i = 0; i < 4; i++) {
            placeBlock(level, southStairs, 5, 0 - i, 6 + i, chunkBB);
            placeBlock(level, southStairs, 6, 0 - i, 6 + i, chunkBB);
            generateAirBox(level, chunkBB, 5, 0 - i, 7 + i, 6, 0 - i, 9 + i);
         }

         generateAirBox(level, chunkBB, 1, -3, 12, 10, -1, 13);
         generateAirBox(level, chunkBB, 1, -3, 1, 3, -1, 13);
         generateAirBox(level, chunkBB, 1, -3, 1, 9, -1, 5);

         for (int z = 1; z <= 13; z += 2) {
            generateBox(level, chunkBB, 1, -3, z, 1, -2, z, false, random, stoneSelector);
         }

         for (int z = 2; z <= 12; z += 2) {
            generateBox(level, chunkBB, 1, -1, z, 3, -1, z, false, random, stoneSelector);
         }

         generateBox(level, chunkBB, 2, -2, 1, 5, -2, 1, false, random, stoneSelector);
         generateBox(level, chunkBB, 7, -2, 1, 9, -2, 1, false, random, stoneSelector);
         generateBox(level, chunkBB, 6, -3, 1, 6, -3, 1, false, random, stoneSelector);
         generateBox(level, chunkBB, 6, -1, 1, 6, -1, 1, false, random, stoneSelector);
         placeBlock(
            level,
            BS(blocks::tripwire_hook).with("facing", Dir::EAST).with("attached", true),
            1,
            -3,
            8,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::tripwire_hook).with("facing", Dir::WEST).with("attached", true),
            4,
            -3,
            8,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::tripwire).with("east", true).with("west", true).with("attached", true),
            2,
            -3,
            8,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::tripwire).with("east", true).with("west", true).with("attached", true),
            3,
            -3,
            8,
            chunkBB
         );
         BS redstoneWireNS = BS(blocks::redstone_wire)
            .with("north", "side")
            .with("south", "side");
         placeBlock(level, redstoneWireNS, 5, -3, 7, chunkBB);
         placeBlock(level, redstoneWireNS, 5, -3, 6, chunkBB);
         placeBlock(level, redstoneWireNS, 5, -3, 5, chunkBB);
         placeBlock(level, redstoneWireNS, 5, -3, 4, chunkBB);
         placeBlock(level, redstoneWireNS, 5, -3, 3, chunkBB);
         placeBlock(level, redstoneWireNS, 5, -3, 2, chunkBB);
         placeBlock(
            level,
            BS(blocks::redstone_wire).with("north", "side").with("west", "side"),
            5,
            -3,
            1,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::redstone_wire).with("east", "side").with("west", "side"),
            4,
            -3,
            1,
            chunkBB
         );
         placeBlock(level, BS(blocks::mossy_cobblestone), 3, -3, 1, chunkBB);
         if (!placedTrap1) {
            placedTrap1 = createDispenser(level, chunkBB, random, 3, -2, 1, Dir::NORTH);
         }

         placeBlock(level, BS(blocks::vine).with("south", true), 3, -2, 2, chunkBB);
         placeBlock(
            level,
            BS(blocks::tripwire_hook).with("facing", Dir::NORTH).with("attached", true),
            7,
            -3,
            1,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::tripwire_hook).with("facing", Dir::SOUTH).with("attached", true),
            7,
            -3,
            5,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::tripwire).with("north", true).with("south", true).with("attached", true),
            7,
            -3,
            2,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::tripwire).with("north", true).with("south", true).with("attached", true),
            7,
            -3,
            3,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::tripwire).with("north", true).with("south", true).with("attached", true),
            7,
            -3,
            4,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::redstone_wire).with("east", "side").with("west", "side"),
            8,
            -3,
            6,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::redstone_wire).with("west", "side").with("south", "side"),
            9,
            -3,
            6,
            chunkBB
         );
         placeBlock(
            level,
            BS(blocks::redstone_wire).with("north", "side").with("south", "up"),
            9,
            -3,
            5,
            chunkBB
         );
         placeBlock(level, BS(blocks::mossy_cobblestone), 9, -3, 4, chunkBB);
         placeBlock(level, redstoneWireNS, 9, -2, 4, chunkBB);
         if (!placedTrap2) {
            placedTrap2 = createDispenser(level, chunkBB, random, 9, -2, 3, Dir::WEST);
         }

         placeBlock(level, BS(blocks::vine).with("east", true), 8, -1, 3, chunkBB);
         placeBlock(level, BS(blocks::vine).with("east", true), 8, -2, 3, chunkBB);
         if (!placedMainChest) {
            placedMainChest = createChest(level, chunkBB, random, 8, -3, 3);
         }

         placeBlock(level, BS(blocks::mossy_cobblestone), 9, -3, 2, chunkBB);
         placeBlock(level, BS(blocks::mossy_cobblestone), 8, -3, 1, chunkBB);
         placeBlock(level, BS(blocks::mossy_cobblestone), 4, -3, 5, chunkBB);
         placeBlock(level, BS(blocks::mossy_cobblestone), 5, -2, 5, chunkBB);
         placeBlock(level, BS(blocks::mossy_cobblestone), 5, -1, 5, chunkBB);
         placeBlock(level, BS(blocks::mossy_cobblestone), 6, -3, 5, chunkBB);
         placeBlock(level, BS(blocks::mossy_cobblestone), 7, -2, 5, chunkBB);
         placeBlock(level, BS(blocks::mossy_cobblestone), 7, -1, 5, chunkBB);
         placeBlock(level, BS(blocks::mossy_cobblestone), 8, -3, 5, chunkBB);
         generateBox(level, chunkBB, 9, -1, 1, 9, -1, 5, false, random, stoneSelector);
         generateAirBox(level, chunkBB, 8, -3, 8, 10, -1, 10);
         placeBlock(level, BS(blocks::chiseled_stone_bricks), 8, -2, 11, chunkBB);
         placeBlock(level, BS(blocks::chiseled_stone_bricks), 9, -2, 11, chunkBB);
         placeBlock(level, BS(blocks::chiseled_stone_bricks), 10, -2, 11, chunkBB);
         BS lever = BS(blocks::lever).with("facing", Dir::NORTH).with("face", "wall");
         placeBlock(level, lever, 8, -2, 12, chunkBB);
         placeBlock(level, lever, 9, -2, 12, chunkBB);
         placeBlock(level, lever, 10, -2, 12, chunkBB);
         generateBox(level, chunkBB, 8, -3, 8, 8, -3, 10, false, random, stoneSelector);
         generateBox(level, chunkBB, 10, -3, 8, 10, -3, 10, false, random, stoneSelector);
         placeBlock(level, BS(blocks::mossy_cobblestone), 10, -2, 9, chunkBB);
         placeBlock(level, redstoneWireNS, 8, -2, 9, chunkBB);
         placeBlock(level, redstoneWireNS, 8, -2, 10, chunkBB);
         placeBlock(
            level,
            BS(blocks::redstone_wire)
               .with("north", "side")
               .with("south", "side")
               .with("east", "side")
               .with("west", "side"),
            10,
            -1,
            9,
            chunkBB
         );
         placeBlock(level, BS(blocks::sticky_piston).with("facing", Dir::UP), 9, -2, 8, chunkBB);
         placeBlock(level, BS(blocks::sticky_piston).with("facing", Dir::WEST), 10, -2, 8, chunkBB);
         placeBlock(level, BS(blocks::sticky_piston).with("facing", Dir::WEST), 10, -1, 8, chunkBB);
         placeBlock(level, BS(blocks::repeater).with("facing", Dir::NORTH), 10, -2, 10, chunkBB);
         if (!placedHiddenChest) {
            placedHiddenChest = createChest(level, chunkBB, random, 9, -3, 10);
         }
      }
   }
};

}  // namespace

struct DesertPyramidPiece : ScatteredPiece {
  bool hasPlacedChest[4] = {false, false, false, false};
  std::vector<BlockPos> potentialSuspiciousSandWorldPositions;
  BlockPos randomCollapsedRoofPos;
  explicit DesertPyramidPiece(RandomSource& random, int west, int north) : ScatteredPiece(west, 64, north, 21, 15, 21, randomHorizontal(random)) {}
  void saveState(std::vector<int>& out) const override {
    ScatteredPiece::saveState(out);
    out.insert(out.end(), {hasPlacedChest[0], hasPlacedChest[1], hasPlacedChest[2], hasPlacedChest[3]});
  }
  void loadState(const int*& p) override {
    ScatteredPiece::loadState(p);
    for (int i = 0; i < 4; i++) hasPlacedChest[i] = p[i];
    p += 4;
  }
   void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int, int, BlockPos) override {
      if (updateHeightPositionToLowestGroundHeight(level, -random.nextInt(3))) {
         generateBox(
            level, chunkBB, 0, -4, 0, width - 1, 0, depth - 1, BS(blocks::sandstone), BS(blocks::sandstone), false
         );

         for (int pos = 1; pos <= 9; pos++) {
            generateBox(
               level,
               chunkBB,
               pos,
               pos,
               pos,
               width - 1 - pos,
               pos,
               depth - 1 - pos,
               BS(blocks::sandstone),
               BS(blocks::sandstone),
               false
            );
            generateBox(
               level,
               chunkBB,
               pos + 1,
               pos,
               pos + 1,
               width - 2 - pos,
               pos,
               depth - 2 - pos,
               BS(blocks::air),
               BS(blocks::air),
               false
            );
         }

         for (int x = 0; x < width; x++) {
            for (int z = 0; z < depth; z++) {

               fillColumnDown(level, BS(blocks::sandstone), x, -5, z, chunkBB);
            }
         }

         BS northStairs = BS(blocks::sandstone_stairs).with("facing", Dir::NORTH);
         BS southStairs = BS(blocks::sandstone_stairs).with("facing", Dir::SOUTH);
         BS eastStairs = BS(blocks::sandstone_stairs).with("facing", Dir::EAST);
         BS westStairs = BS(blocks::sandstone_stairs).with("facing", Dir::WEST);
         generateBox(level, chunkBB, 0, 0, 0, 4, 9, 4, BS(blocks::sandstone), BS(blocks::air), false);
         generateBox(level, chunkBB, 1, 10, 1, 3, 10, 3, BS(blocks::sandstone), BS(blocks::sandstone), false);
         placeBlock(level, northStairs, 2, 10, 0, chunkBB);
         placeBlock(level, southStairs, 2, 10, 4, chunkBB);
         placeBlock(level, eastStairs, 0, 10, 2, chunkBB);
         placeBlock(level, westStairs, 4, 10, 2, chunkBB);
         generateBox(
            level, chunkBB, width - 5, 0, 0, width - 1, 9, 4, BS(blocks::sandstone), BS(blocks::air), false
         );
         generateBox(
            level, chunkBB, width - 4, 10, 1, width - 2, 10, 3, BS(blocks::sandstone), BS(blocks::sandstone), false
         );
         placeBlock(level, northStairs, width - 3, 10, 0, chunkBB);
         placeBlock(level, southStairs, width - 3, 10, 4, chunkBB);
         placeBlock(level, eastStairs, width - 5, 10, 2, chunkBB);
         placeBlock(level, westStairs, width - 1, 10, 2, chunkBB);
         generateBox(level, chunkBB, 8, 0, 0, 12, 4, 4, BS(blocks::sandstone), BS(blocks::air), false);
         generateBox(level, chunkBB, 9, 1, 0, 11, 3, 4, BS(blocks::air), BS(blocks::air), false);
         placeBlock(level, BS(blocks::cut_sandstone), 9, 1, 1, chunkBB);
         placeBlock(level, BS(blocks::cut_sandstone), 9, 2, 1, chunkBB);
         placeBlock(level, BS(blocks::cut_sandstone), 9, 3, 1, chunkBB);
         placeBlock(level, BS(blocks::cut_sandstone), 10, 3, 1, chunkBB);
         placeBlock(level, BS(blocks::cut_sandstone), 11, 3, 1, chunkBB);
         placeBlock(level, BS(blocks::cut_sandstone), 11, 2, 1, chunkBB);
         placeBlock(level, BS(blocks::cut_sandstone), 11, 1, 1, chunkBB);
         generateBox(level, chunkBB, 4, 1, 1, 8, 3, 3, BS(blocks::sandstone), BS(blocks::air), false);
         generateBox(level, chunkBB, 4, 1, 2, 8, 2, 2, BS(blocks::air), BS(blocks::air), false);
         generateBox(level, chunkBB, 12, 1, 1, 16, 3, 3, BS(blocks::sandstone), BS(blocks::air), false);
         generateBox(level, chunkBB, 12, 1, 2, 16, 2, 2, BS(blocks::air), BS(blocks::air), false);
         generateBox(
            level, chunkBB, 5, 4, 5, width - 6, 4, depth - 6, BS(blocks::sandstone), BS(blocks::sandstone), false
         );
         generateBox(level, chunkBB, 9, 4, 9, 11, 4, 11, BS(blocks::air), BS(blocks::air), false);
         generateBox(level, chunkBB, 8, 1, 8, 8, 3, 8, BS(blocks::cut_sandstone), BS(blocks::cut_sandstone), false);
         generateBox(level, chunkBB, 12, 1, 8, 12, 3, 8, BS(blocks::cut_sandstone), BS(blocks::cut_sandstone), false);
         generateBox(level, chunkBB, 8, 1, 12, 8, 3, 12, BS(blocks::cut_sandstone), BS(blocks::cut_sandstone), false);
         generateBox(level, chunkBB, 12, 1, 12, 12, 3, 12, BS(blocks::cut_sandstone), BS(blocks::cut_sandstone), false);
         generateBox(level, chunkBB, 1, 1, 5, 4, 4, 11, BS(blocks::sandstone), BS(blocks::sandstone), false);
         generateBox(
            level, chunkBB, width - 5, 1, 5, width - 2, 4, 11, BS(blocks::sandstone), BS(blocks::sandstone), false
         );
         generateBox(level, chunkBB, 6, 7, 9, 6, 7, 11, BS(blocks::sandstone), BS(blocks::sandstone), false);
         generateBox(
            level, chunkBB, width - 7, 7, 9, width - 7, 7, 11, BS(blocks::sandstone), BS(blocks::sandstone), false
         );
         generateBox(level, chunkBB, 5, 5, 9, 5, 7, 11, BS(blocks::cut_sandstone), BS(blocks::cut_sandstone), false);
         generateBox(
            level,
            chunkBB,
            width - 6,
            5,
            9,
            width - 6,
            7,
            11,
            BS(blocks::cut_sandstone),
            BS(blocks::cut_sandstone),
            false
         );
         placeBlock(level, BS(blocks::air), 5, 5, 10, chunkBB);
         placeBlock(level, BS(blocks::air), 5, 6, 10, chunkBB);
         placeBlock(level, BS(blocks::air), 6, 6, 10, chunkBB);
         placeBlock(level, BS(blocks::air), width - 6, 5, 10, chunkBB);
         placeBlock(level, BS(blocks::air), width - 6, 6, 10, chunkBB);
         placeBlock(level, BS(blocks::air), width - 7, 6, 10, chunkBB);
         generateBox(level, chunkBB, 2, 4, 4, 2, 6, 4, BS(blocks::air), BS(blocks::air), false);
         generateBox(level, chunkBB, width - 3, 4, 4, width - 3, 6, 4, BS(blocks::air), BS(blocks::air), false);
         placeBlock(level, northStairs, 2, 4, 5, chunkBB);
         placeBlock(level, northStairs, 2, 3, 4, chunkBB);
         placeBlock(level, northStairs, width - 3, 4, 5, chunkBB);
         placeBlock(level, northStairs, width - 3, 3, 4, chunkBB);
         generateBox(level, chunkBB, 1, 1, 3, 2, 2, 3, BS(blocks::sandstone), BS(blocks::sandstone), false);
         generateBox(
            level, chunkBB, width - 3, 1, 3, width - 2, 2, 3, BS(blocks::sandstone), BS(blocks::sandstone), false
         );
         placeBlock(level, BS(blocks::sandstone), 1, 1, 2, chunkBB);
         placeBlock(level, BS(blocks::sandstone), width - 2, 1, 2, chunkBB);
         placeBlock(level, BS(blocks::sandstone_slab), 1, 2, 2, chunkBB);
         placeBlock(level, BS(blocks::sandstone_slab), width - 2, 2, 2, chunkBB);
         placeBlock(level, westStairs, 2, 1, 2, chunkBB);
         placeBlock(level, eastStairs, width - 3, 1, 2, chunkBB);
         generateBox(level, chunkBB, 4, 3, 5, 4, 3, 17, BS(blocks::sandstone), BS(blocks::sandstone), false);
         generateBox(
            level, chunkBB, width - 5, 3, 5, width - 5, 3, 17, BS(blocks::sandstone), BS(blocks::sandstone), false
         );
         generateBox(level, chunkBB, 3, 1, 5, 4, 2, 16, BS(blocks::air), BS(blocks::air), false);
         generateBox(level, chunkBB, width - 6, 1, 5, width - 5, 2, 16, BS(blocks::air), BS(blocks::air), false);

         for (int z = 5; z <= 17; z += 2) {
            placeBlock(level, BS(blocks::cut_sandstone), 4, 1, z, chunkBB);
            placeBlock(level, BS(blocks::chiseled_sandstone), 4, 2, z, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), width - 5, 1, z, chunkBB);
            placeBlock(level, BS(blocks::chiseled_sandstone), width - 5, 2, z, chunkBB);
         }

         placeBlock(level, BS(blocks::orange_terracotta), 10, 0, 7, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 10, 0, 8, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 9, 0, 9, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 11, 0, 9, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 8, 0, 10, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 12, 0, 10, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 7, 0, 10, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 13, 0, 10, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 9, 0, 11, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 11, 0, 11, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 10, 0, 12, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 10, 0, 13, chunkBB);
         placeBlock(level, BS(blocks::blue_terracotta), 10, 0, 10, chunkBB);

         for (int x = 0; x <= width - 1; x += width - 1) {
            placeBlock(level, BS(blocks::cut_sandstone), x, 2, 1, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 2, 2, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x, 2, 3, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x, 3, 1, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 3, 2, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x, 3, 3, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 4, 1, chunkBB);
            placeBlock(level, BS(blocks::chiseled_sandstone), x, 4, 2, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 4, 3, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x, 5, 1, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 5, 2, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x, 5, 3, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 6, 1, chunkBB);
            placeBlock(level, BS(blocks::chiseled_sandstone), x, 6, 2, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 6, 3, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 7, 1, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 7, 2, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 7, 3, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x, 8, 1, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x, 8, 2, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x, 8, 3, chunkBB);
         }

         for (int x = 2; x <= width - 3; x += width - 3 - 2) {
            placeBlock(level, BS(blocks::cut_sandstone), x - 1, 2, 0, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 2, 0, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x + 1, 2, 0, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x - 1, 3, 0, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 3, 0, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x + 1, 3, 0, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x - 1, 4, 0, chunkBB);
            placeBlock(level, BS(blocks::chiseled_sandstone), x, 4, 0, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x + 1, 4, 0, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x - 1, 5, 0, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 5, 0, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x + 1, 5, 0, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x - 1, 6, 0, chunkBB);
            placeBlock(level, BS(blocks::chiseled_sandstone), x, 6, 0, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x + 1, 6, 0, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x - 1, 7, 0, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x, 7, 0, chunkBB);
            placeBlock(level, BS(blocks::orange_terracotta), x + 1, 7, 0, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x - 1, 8, 0, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x, 8, 0, chunkBB);
            placeBlock(level, BS(blocks::cut_sandstone), x + 1, 8, 0, chunkBB);
         }

         generateBox(level, chunkBB, 8, 4, 0, 12, 6, 0, BS(blocks::cut_sandstone), BS(blocks::cut_sandstone), false);
         placeBlock(level, BS(blocks::air), 8, 6, 0, chunkBB);
         placeBlock(level, BS(blocks::air), 12, 6, 0, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 9, 5, 0, chunkBB);
         placeBlock(level, BS(blocks::chiseled_sandstone), 10, 5, 0, chunkBB);
         placeBlock(level, BS(blocks::orange_terracotta), 11, 5, 0, chunkBB);
         generateBox(level, chunkBB, 8, -14, 8, 12, -11, 12, BS(blocks::cut_sandstone), BS(blocks::cut_sandstone), false);
         generateBox(
            level, chunkBB, 8, -10, 8, 12, -10, 12, BS(blocks::chiseled_sandstone), BS(blocks::chiseled_sandstone), false
         );
         generateBox(level, chunkBB, 8, -9, 8, 12, -9, 12, BS(blocks::cut_sandstone), BS(blocks::cut_sandstone), false);
         generateBox(level, chunkBB, 8, -8, 8, 12, -1, 12, BS(blocks::sandstone), BS(blocks::sandstone), false);
         generateBox(level, chunkBB, 9, -11, 9, 11, -1, 11, BS(blocks::air), BS(blocks::air), false);
         placeBlock(level, BS(blocks::stone_pressure_plate), 10, -11, 10, chunkBB);
         generateBox(level, chunkBB, 9, -13, 9, 11, -13, 11, BS(blocks::tnt), BS(blocks::air), false);
         placeBlock(level, BS(blocks::air), 8, -11, 10, chunkBB);
         placeBlock(level, BS(blocks::air), 8, -10, 10, chunkBB);
         placeBlock(level, BS(blocks::chiseled_sandstone), 7, -10, 10, chunkBB);
         placeBlock(level, BS(blocks::cut_sandstone), 7, -11, 10, chunkBB);
         placeBlock(level, BS(blocks::air), 12, -11, 10, chunkBB);
         placeBlock(level, BS(blocks::air), 12, -10, 10, chunkBB);
         placeBlock(level, BS(blocks::chiseled_sandstone), 13, -10, 10, chunkBB);
         placeBlock(level, BS(blocks::cut_sandstone), 13, -11, 10, chunkBB);
         placeBlock(level, BS(blocks::air), 10, -11, 8, chunkBB);
         placeBlock(level, BS(blocks::air), 10, -10, 8, chunkBB);
         placeBlock(level, BS(blocks::chiseled_sandstone), 10, -10, 7, chunkBB);
         placeBlock(level, BS(blocks::cut_sandstone), 10, -11, 7, chunkBB);
         placeBlock(level, BS(blocks::air), 10, -11, 12, chunkBB);
         placeBlock(level, BS(blocks::air), 10, -10, 12, chunkBB);
         placeBlock(level, BS(blocks::chiseled_sandstone), 10, -10, 13, chunkBB);
         placeBlock(level, BS(blocks::cut_sandstone), 10, -11, 13, chunkBB);

         for (Dir direction : HORIZONTAL) {
            if (!hasPlacedChest[data2d(direction)]) {
               int xo = stepX(direction) * 2;
               int zo = stepZ(direction) * 2;
               hasPlacedChest[data2d(direction)] = createChest(
                  level, chunkBB, random, 10 + xo, -11, 10 + zo
               );
            }
         }

         addCellar(level, chunkBB);
      }
   }

   void addCellar(Level& level, const BoundingBox& chunkBB) {
      BlockPos roomCenter = BlockPos{16, -4, 13};
      addCellarStairs(roomCenter, level, chunkBB);
      addCellarRoom(roomCenter, level, chunkBB);
   }

   void addCellarStairs(BlockPos roomCenter, Level& level, const BoundingBox& chunkBB) {
      int x = roomCenter.x;
      int y = roomCenter.y;
      int z = roomCenter.z;
      BS sandStoneStairs = BS(blocks::sandstone_stairs);
      placeBlock(level, sandStoneStairs.rotate(Rotation::COUNTERCLOCKWISE_90), 13, -1, 17, chunkBB);
      placeBlock(level, sandStoneStairs.rotate(Rotation::COUNTERCLOCKWISE_90), 14, -2, 17, chunkBB);
      placeBlock(level, sandStoneStairs.rotate(Rotation::COUNTERCLOCKWISE_90), 15, -3, 17, chunkBB);
      BS sand = BS(blocks::sand);
      BS sandStone = BS(blocks::sandstone);
      bool variant = level.random().nextBoolean();
      placeBlock(level, sand, x - 4, y + 4, z + 4, chunkBB);
      placeBlock(level, sand, x - 3, y + 4, z + 4, chunkBB);
      placeBlock(level, sand, x - 2, y + 4, z + 4, chunkBB);
      placeBlock(level, sand, x - 1, y + 4, z + 4, chunkBB);
      placeBlock(level, sand, x, y + 4, z + 4, chunkBB);
      placeBlock(level, sand, x - 2, y + 3, z + 4, chunkBB);
      placeBlock(level, variant ? sand : sandStone, x - 1, y + 3, z + 4, chunkBB);
      placeBlock(level, !variant ? sand : sandStone, x, y + 3, z + 4, chunkBB);
      placeBlock(level, sand, x - 1, y + 2, z + 4, chunkBB);
      placeBlock(level, sandStone, x, y + 2, z + 4, chunkBB);
      placeBlock(level, sand, x, y + 1, z + 4, chunkBB);
   }

   void addCellarRoom(BlockPos roomCenter, Level& level, const BoundingBox& chunkBB) {
      int x = roomCenter.x;
      int y = roomCenter.y;
      int z = roomCenter.z;
      BS cutSandStone = BS(blocks::cut_sandstone);
      BS hieroglyphsSandStone = BS(blocks::chiseled_sandstone);
      generateBox(level, chunkBB, x - 3, y + 1, z - 3, x - 3, y + 1, z + 2, cutSandStone, cutSandStone, true);
      generateBox(level, chunkBB, x + 3, y + 1, z - 3, x + 3, y + 1, z + 2, cutSandStone, cutSandStone, true);
      generateBox(level, chunkBB, x - 3, y + 1, z - 3, x + 3, y + 1, z - 2, cutSandStone, cutSandStone, true);
      generateBox(level, chunkBB, x - 3, y + 1, z + 3, x + 3, y + 1, z + 3, cutSandStone, cutSandStone, true);
      generateBox(level, chunkBB, x - 3, y + 2, z - 3, x - 3, y + 2, z + 2, hieroglyphsSandStone, hieroglyphsSandStone, true);
      generateBox(level, chunkBB, x + 3, y + 2, z - 3, x + 3, y + 2, z + 2, hieroglyphsSandStone, hieroglyphsSandStone, true);
      generateBox(level, chunkBB, x - 3, y + 2, z - 3, x + 3, y + 2, z - 2, hieroglyphsSandStone, hieroglyphsSandStone, true);
      generateBox(level, chunkBB, x - 3, y + 2, z + 3, x + 3, y + 2, z + 3, hieroglyphsSandStone, hieroglyphsSandStone, true);
      generateBox(level, chunkBB, x - 3, -1, z - 3, x - 3, -1, z + 2, cutSandStone, cutSandStone, true);
      generateBox(level, chunkBB, x + 3, -1, z - 3, x + 3, -1, z + 2, cutSandStone, cutSandStone, true);
      generateBox(level, chunkBB, x - 3, -1, z - 3, x + 3, -1, z - 2, cutSandStone, cutSandStone, true);
      generateBox(level, chunkBB, x - 3, -1, z + 3, x + 3, -1, z + 3, cutSandStone, cutSandStone, true);
      placeSandBox(x - 2, y + 1, z - 2, x + 2, y + 3, z + 2);
      placeCollapsedRoof(level, chunkBB, x - 2, y + 4, z - 2, x + 2, z + 2);
      BS orangeTeracotta = BS(blocks::orange_terracotta);
      BS blueTeracotta = BS(blocks::blue_terracotta);
      placeBlock(level, blueTeracotta, x, y, z, chunkBB);
      placeBlock(level, orangeTeracotta, x + 1, y, z - 1, chunkBB);
      placeBlock(level, orangeTeracotta, x + 1, y, z + 1, chunkBB);
      placeBlock(level, orangeTeracotta, x - 1, y, z - 1, chunkBB);
      placeBlock(level, orangeTeracotta, x - 1, y, z + 1, chunkBB);
      placeBlock(level, orangeTeracotta, x + 2, y, z, chunkBB);
      placeBlock(level, orangeTeracotta, x - 2, y, z, chunkBB);
      placeBlock(level, orangeTeracotta, x, y, z + 2, chunkBB);
      placeBlock(level, orangeTeracotta, x, y, z - 2, chunkBB);
      placeBlock(level, orangeTeracotta, x + 3, y, z, chunkBB);
      placeSand(x + 3, y + 1, z);
      placeSand(x + 3, y + 2, z);
      placeBlock(level, cutSandStone, x + 4, y + 1, z, chunkBB);
      placeBlock(level, hieroglyphsSandStone, x + 4, y + 2, z, chunkBB);
      placeBlock(level, orangeTeracotta, x - 3, y, z, chunkBB);
      placeSand(x - 3, y + 1, z);
      placeSand(x - 3, y + 2, z);
      placeBlock(level, cutSandStone, x - 4, y + 1, z, chunkBB);
      placeBlock(level, hieroglyphsSandStone, x - 4, y + 2, z, chunkBB);
      placeBlock(level, orangeTeracotta, x, y, z + 3, chunkBB);
      placeSand(x, y + 1, z + 3);
      placeSand(x, y + 2, z + 3);
      placeBlock(level, orangeTeracotta, x, y, z - 3, chunkBB);
      placeSand(x, y + 1, z - 3);
      placeSand(x, y + 2, z - 3);
      placeBlock(level, cutSandStone, x, y + 1, z - 4, chunkBB);
      placeBlock(level, hieroglyphsSandStone, x, -2, z - 4, chunkBB);
   }

   void placeSand(int x, int y, int z) {
      potentialSuspiciousSandWorldPositions.push_back(worldPos(x, y, z));
   }

   void placeSandBox(int x0, int y0, int z0, int x1, int y1, int z1) {
      for (int y = y0; y <= y1; y++) {
         for (int x = x0; x <= x1; x++) {
            for (int z = z0; z <= z1; z++) {
               placeSand(x, y, z);
            }
         }
      }
   }

   void placeCollapsedRoofPiece(Level& level, int x, int y, int z, const BoundingBox& chunkBB) {
      if (level.random().nextFloat() < 0.33F) {
         BS blockState = BS(blocks::sandstone);
         placeBlock(level, blockState, x, y, z, chunkBB);
      } else {
         BS blockState = BS(blocks::sand);
         placeBlock(level, blockState, x, y, z, chunkBB);
      }
   }

   void placeCollapsedRoof(Level& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int z1) {
      for (int x = x0; x <= x1; x++) {
         for (int z = z0; z <= z1; z++) {
            placeCollapsedRoofPiece(level, x, y0, z, chunkBB);
         }
      }

      auto random = positionalRandom(level.seed(), worldPos(x0, y0, z0));
      int roofPosX = random->nextIntBetweenInclusive(x0, x1);
      int roofPosZ = random->nextIntBetweenInclusive(z0, z1);
      randomCollapsedRoofPos = BlockPos{worldX(roofPosX, roofPosZ), worldY(y0), worldZ(roofPosX, roofPosZ)};
   }
};

struct BuriedTreasurePiece : Piece {
  explicit BuriedTreasurePiece(BlockPos offset) : Piece(0, BoundingBox(offset)) {}
      void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int, int, BlockPos) override {
         int y = level.getHeight(Heightmap::OCEAN_FLOOR_WG, boundingBox.minX, boundingBox.minZ);
         BlockPos pos = BlockPos{boundingBox.minX, y, boundingBox.minZ};

         while (pos.y > MIN_Y) {
            BS currentState = level.get(pos);
            BS belowState = level.get(pos.below());
            if (belowState.is(blocks::sandstone)
               || belowState.is(blocks::stone)
               || belowState.is(blocks::andesite)
               || belowState.is(blocks::granite)
               || belowState.is(blocks::diorite)) {
               BS softState = !currentState.isAir() && !isLiquid(currentState) ? currentState : BS(blocks::sand);

               for (Dir direction : ALL_DIRS) {
                  BlockPos relativePos = pos.relative(direction);
                  BS relativeState = level.get(relativePos);
                  if (relativeState.isAir() || isLiquid(relativeState)) {
                     BlockPos belowRelativePos = relativePos.below();
                     BS belowRelativeState = level.get(belowRelativePos);
                     if ((belowRelativeState.isAir() || isLiquid(belowRelativeState)) && direction != Dir::UP) {
                        level.set(relativePos, belowState);
                     } else {
                        level.set(relativePos, softState);
                     }
                  }
               }

               boundingBox = BoundingBox(pos);
               createChest(level, chunkBB, random, pos, -1);
               return;
            }

            pos = pos.offset(0, -1, 0);
         }
      }

      static bool isLiquid(BS blockState) {
         return blockState.is(blocks::water) || blockState.is(blocks::lava);
      }
};

PieceP makeSwampHut(RandomSource& r, int x, int z) { return std::make_unique<SwampHutPiece>(r, x, z); }
PieceP makeJungleTemple(RandomSource& r, int x, int z) { return std::make_unique<JungleTemplePiece>(r, x, z); }
PieceP makeDesertPyramid(RandomSource& r, int x, int z) { return std::make_unique<DesertPyramidPiece>(r, x, z); }
PieceP makeBuriedTreasure(BlockPos p) { return std::make_unique<BuriedTreasurePiece>(p); }

// DesertPyramidStructure.afterPlace
void desertPyramidAfterPlace(Level& level, const BoundingBox& chunkBB, const std::vector<PieceP>& pieces) {
  std::set<BlockPos> unique;  // SortedArraySet by Vec3i.compareTo
  auto placeSuspicious = [&](BlockPos p) {
    if (chunkBB.isInside(p)) level.set(p, blocks::suspicious_sand);
  };
  for (auto& p : pieces)
    if (auto* d = dynamic_cast<DesertPyramidPiece*>(p.get())) {
      unique.insert(d->potentialSuspiciousSandWorldPositions.begin(), d->potentialSuspiciousSandWorldPositions.end());
      placeSuspicious(d->randomCollapsedRoofPos);
    }
  std::vector<BlockPos> shuffled(unique.begin(), unique.end());
  BoundingBox bb = pieces[0]->boundingBox;
  for (auto& p : pieces) bb.encapsulate(p->boundingBox);
  auto random = positionalRandom(level.seed(), bb.center());
  shuffle(shuffled, *random);
  int toPlace = std::min((int)unique.size(), random->nextInt(5, 8));
  for (BlockPos p : shuffled) {
    if (toPlace > 0) {
      toPlace--;
      placeSuspicious(p);
    } else if (chunkBB.isInside(p)) {
      level.set(p, blocks::sand);
    }
  }
}

}  // namespace structure
