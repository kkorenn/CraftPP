// WoodlandMansionPieces (26.3): the grid layout and the template piece placer. Placer/grid bodies are
// mechanically translated from vanilla (one-off script); edit here directly.
#include <string>

#include "worldgen/structure/legacy.hpp"
#include "worldgen/structure/pieces.hpp"

#pragma GCC diagnostic ignored "-Wunused-parameter"  // translated bodies keep vanilla's signatures

namespace structure {
namespace {

const Dir DIR_NULL = (Dir)255;  // Java null Direction

BlockPos rotatePos(BlockPos p, Rotation r) {  // BlockPos.rotate
  switch (r) {
    case Rotation::CLOCKWISE_90: return {-p.z, p.y, p.x};
    case Rotation::CLOCKWISE_180: return {-p.x, p.y, -p.z};
    case Rotation::COUNTERCLOCKWISE_90: return {p.z, p.y, -p.x};
    default: return p;
  }
}

struct SimpleGrid {
  std::vector<int> grid;
  int width = 0, height = 0, valueIfOutside = 0;
  SimpleGrid() = default;
  SimpleGrid(int w, int h, int outside) : grid((size_t)w * h, 0), width(w), height(h), valueIfOutside(outside) {}
  void set(int x, int y, int v) {
    if (x >= 0 && x < width && y >= 0 && y < height) grid[(size_t)x * height + y] = v;
  }
  void set(int x0, int y0, int x1, int y1, int v) {
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) set(x, y, v);
  }
  int get(int x, int y) const { return x >= 0 && x < width && y >= 0 && y < height ? grid[(size_t)x * height + y] : valueIfOutside; }
  void setif(int x, int y, int ifValue, int v) {
    if (get(x, y) == ifValue) set(x, y, v);
  }
  bool edgesTo(int x, int y, int v) const { return get(x - 1, y) == v || get(x + 1, y) == v || get(x, y + 1) == v || get(x, y - 1) == v; }
};

struct GridPos { int x, y; };

bool isHouse(const SimpleGrid& g, int x, int y) {
  int v = g.get(x, y);
  return v == 1 || v == 2 || v == 3 || v == 4;
}

struct MansionGrid {
  RandomSource& random;
  SimpleGrid baseGrid, thirdFloorGrid;
  SimpleGrid floorRooms[3];
  int entranceX = 7, entranceY = 4;

  explicit MansionGrid(RandomSource& r) : random(r) {
    baseGrid = SimpleGrid(11, 11, 5);
    baseGrid.set(entranceX, entranceY, entranceX + 1, entranceY + 1, 3);
    baseGrid.set(entranceX - 1, entranceY, entranceX - 1, entranceY + 1, 2);
    baseGrid.set(entranceX + 2, entranceY - 2, entranceX + 3, entranceY + 3, 5);
    baseGrid.set(entranceX + 1, entranceY - 2, entranceX + 1, entranceY - 1, 1);
    baseGrid.set(entranceX + 1, entranceY + 2, entranceX + 1, entranceY + 3, 1);
    baseGrid.set(entranceX - 1, entranceY - 1, 1);
    baseGrid.set(entranceX - 1, entranceY + 2, 1);
    baseGrid.set(0, 0, 11, 1, 5);
    baseGrid.set(0, 9, 11, 11, 5);
    recursiveCorridor(baseGrid, entranceX, entranceY - 2, Dir::WEST, 6);
    recursiveCorridor(baseGrid, entranceX, entranceY + 3, Dir::WEST, 6);
    recursiveCorridor(baseGrid, entranceX - 2, entranceY - 1, Dir::WEST, 3);
    recursiveCorridor(baseGrid, entranceX - 2, entranceY + 2, Dir::WEST, 3);
    while (cleanEdges(baseGrid)) {
    }
    for (auto& f : floorRooms) f = SimpleGrid(11, 11, 5);
    identifyRooms(baseGrid, floorRooms[0]);
    identifyRooms(baseGrid, floorRooms[1]);
    floorRooms[0].set(entranceX + 1, entranceY, entranceX + 1, entranceY + 1, 8388608);
    floorRooms[1].set(entranceX + 1, entranceY, entranceX + 1, entranceY + 1, 8388608);
    thirdFloorGrid = SimpleGrid(baseGrid.width, baseGrid.height, 5);
    setupThirdFloor();
    identifyRooms(thirdFloorGrid, floorRooms[2]);
  }

  static bool cleanEdges(SimpleGrid& grid) {
    bool touched = false;
    for (int y = 0; y < grid.height; y++)
      for (int x = 0; x < grid.width; x++) {
        if (grid.get(x, y) != 0) continue;
        int direct = isHouse(grid, x + 1, y) + isHouse(grid, x - 1, y) + isHouse(grid, x, y + 1) + isHouse(grid, x, y - 1);
        if (direct >= 3) {
          grid.set(x, y, 2);
          touched = true;
        } else if (direct == 2) {
          int diag = isHouse(grid, x + 1, y + 1) + isHouse(grid, x - 1, y + 1) + isHouse(grid, x + 1, y - 1) + isHouse(grid, x - 1, y - 1);
          if (diag <= 1) {
            grid.set(x, y, 2);
            touched = true;
          }
        }
      }
    return touched;
  }
      bool isRoomId(SimpleGrid& grid, int x, int y, int floor, int roomId) {
         return (floorRooms[floor].get(x, y) & 65535) == roomId;
      }

      Dir get1x2RoomDirection(SimpleGrid& grid, int x, int y, int floorNum, int roomId) {
         for (Dir direction : HORIZONTAL) {
            if (isRoomId(grid, x + stepX(direction), y + stepZ(direction), floorNum, roomId)) {
               return direction;
            }
         }

         return DIR_NULL;
      }

      void recursiveCorridor(SimpleGrid& grid, int x, int y, Dir heading, int depth) {
         if (depth > 0) {
            grid.set(x, y, 1);
            grid.setif(x + stepX(heading), y + stepZ(heading), 0, 1);

            for (int attempts = 0; attempts < 8; attempts++) {
               Dir nextDir = from2d(random.nextInt(4));
               if (nextDir != opposite(heading) && (nextDir != Dir::EAST || !random.nextBoolean())) {
                  int nx = x + stepX(heading);
                  int ny = y + stepZ(heading);
                  if (grid.get(nx + stepX(nextDir), ny + stepZ(nextDir)) == 0
                     && grid.get(nx + stepX(nextDir) * 2, ny + stepZ(nextDir) * 2) == 0) {
                     recursiveCorridor(grid, x + stepX(heading) + stepX(nextDir), y + stepZ(heading) + stepZ(nextDir), nextDir, depth - 1);
                     break;
                  }
               }
            }

            Dir cw = clockWise(heading);
            Dir ccw = counterClockWise(heading);
            grid.setif(x + stepX(cw), y + stepZ(cw), 0, 2);
            grid.setif(x + stepX(ccw), y + stepZ(ccw), 0, 2);
            grid.setif(x + stepX(heading) + stepX(cw), y + stepZ(heading) + stepZ(cw), 0, 2);
            grid.setif(x + stepX(heading) + stepX(ccw), y + stepZ(heading) + stepZ(ccw), 0, 2);
            grid.setif(x + stepX(heading) * 2, y + stepZ(heading) * 2, 0, 2);
            grid.setif(x + stepX(cw) * 2, y + stepZ(cw) * 2, 0, 2);
            grid.setif(x + stepX(ccw) * 2, y + stepZ(ccw) * 2, 0, 2);
         }
      }

      void setupThirdFloor() {
         std::vector<GridPos> potentialRooms;
         SimpleGrid& floor = floorRooms[1];

         for (int y = 0; y < thirdFloorGrid.height; y++) {
            for (int x = 0; x < thirdFloorGrid.width; x++) {
               int roomData = floor.get(x, y);
               int roomType = roomData & 983040;
               if (roomType == 131072 && (roomData & 2097152) == 2097152) {
                  potentialRooms.push_back(GridPos{x, y});
               }
            }
         }

         if (potentialRooms.empty()) {
            thirdFloorGrid.set(0, 0, thirdFloorGrid.width, thirdFloorGrid.height, 5);
         } else {
            GridPos roomPos = potentialRooms[random.nextInt((int)potentialRooms.size())];
            int roomData = floor.get(roomPos.x, roomPos.y);
            floor.set(roomPos.x, roomPos.y, roomData | 4194304);
            Dir roomDir = get1x2RoomDirection(baseGrid, roomPos.x, roomPos.y, 1, roomData & 65535);
            int roomEndX = roomPos.x + stepX(roomDir);
            int roomEndY = roomPos.y + stepZ(roomDir);

            for (int y = 0; y < thirdFloorGrid.height; y++) {
               for (int xx = 0; xx < thirdFloorGrid.width; xx++) {
                  if (!isHouse(baseGrid, xx, y)) {
                     thirdFloorGrid.set(xx, y, 5);
                  } else if (xx == roomPos.x && y == roomPos.y) {
                     thirdFloorGrid.set(xx, y, 3);
                  } else if (xx == roomEndX && y == roomEndY) {
                     thirdFloorGrid.set(xx, y, 3);
                     floorRooms[2].set(xx, y, 8388608);
                  }
               }
            }

            std::vector<Dir> potentialCorridors;

            for (Dir direction : HORIZONTAL) {
               if (thirdFloorGrid.get(roomEndX + stepX(direction), roomEndY + stepZ(direction)) == 0) {
                  potentialCorridors.push_back(direction);
               }
            }

            if (potentialCorridors.empty()) {
               thirdFloorGrid.set(0, 0, thirdFloorGrid.width, thirdFloorGrid.height, 5);
               floor.set(roomPos.x, roomPos.y, roomData);
            } else {
               Dir corridorDir = potentialCorridors[random.nextInt((int)potentialCorridors.size())];
               recursiveCorridor(thirdFloorGrid, roomEndX + stepX(corridorDir), roomEndY + stepZ(corridorDir), corridorDir, 4);

               while (cleanEdges(thirdFloorGrid)) {
               }
            }
         }
      }

      void identifyRooms(SimpleGrid& fromGrid, SimpleGrid& roomGrid) {
         std::vector<GridPos> roomPos;

         for (int y = 0; y < fromGrid.height; y++) {
            for (int x = 0; x < fromGrid.width; x++) {
               if (fromGrid.get(x, y) == 2) {
                  roomPos.push_back(GridPos{x, y});
               }
            }
         }

         shuffle(roomPos, random);
         int roomId = 10;

         for (GridPos pos : roomPos) {
            int xx = pos.x;
            int y = pos.y;
            if (roomGrid.get(xx, y) == 0) {
               int x0 = xx;
               int x1 = xx;
               int y0 = y;
               int y1 = y;
               int type = 65536;
               if (roomGrid.get(xx + 1, y) == 0
                  && roomGrid.get(xx, y + 1) == 0
                  && roomGrid.get(xx + 1, y + 1) == 0
                  && fromGrid.get(xx + 1, y) == 2
                  && fromGrid.get(xx, y + 1) == 2
                  && fromGrid.get(xx + 1, y + 1) == 2) {
                  x1 = xx + 1;
                  y1 = y + 1;
                  type = 262144;
               } else if (roomGrid.get(xx - 1, y) == 0
                  && roomGrid.get(xx, y + 1) == 0
                  && roomGrid.get(xx - 1, y + 1) == 0
                  && fromGrid.get(xx - 1, y) == 2
                  && fromGrid.get(xx, y + 1) == 2
                  && fromGrid.get(xx - 1, y + 1) == 2) {
                  x0 = xx - 1;
                  y1 = y + 1;
                  type = 262144;
               } else if (roomGrid.get(xx - 1, y) == 0
                  && roomGrid.get(xx, y - 1) == 0
                  && roomGrid.get(xx - 1, y - 1) == 0
                  && fromGrid.get(xx - 1, y) == 2
                  && fromGrid.get(xx, y - 1) == 2
                  && fromGrid.get(xx - 1, y - 1) == 2) {
                  x0 = xx - 1;
                  y0 = y - 1;
                  type = 262144;
               } else if (roomGrid.get(xx + 1, y) == 0 && fromGrid.get(xx + 1, y) == 2) {
                  x1 = xx + 1;
                  type = 131072;
               } else if (roomGrid.get(xx, y + 1) == 0 && fromGrid.get(xx, y + 1) == 2) {
                  y1 = y + 1;
                  type = 131072;
               } else if (roomGrid.get(xx - 1, y) == 0 && fromGrid.get(xx - 1, y) == 2) {
                  x0 = xx - 1;
                  type = 131072;
               } else if (roomGrid.get(xx, y - 1) == 0 && fromGrid.get(xx, y - 1) == 2) {
                  y0 = y - 1;
                  type = 131072;
               }

               int doorX = random.nextBoolean() ? x0 : x1;
               int doorY = random.nextBoolean() ? y0 : y1;
               int doorFlag = 2097152;
               if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                  doorX = doorX == x0 ? x1 : x0;
                  doorY = doorY == y0 ? y1 : y0;
                  if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                     doorY = doorY == y0 ? y1 : y0;
                     if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                        doorX = doorX == x0 ? x1 : x0;
                        doorY = doorY == y0 ? y1 : y0;
                        if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                           doorFlag = 0;
                           doorX = x0;
                           doorY = y0;
                        }
                     }
                  }
               }

               for (int ry = y0; ry <= y1; ry++) {
                  for (int rx = x0; rx <= x1; rx++) {
                     if (rx == doorX && ry == doorY) {
                        roomGrid.set(rx, ry, 1048576 | doorFlag | type | roomId);
                     } else {
                        roomGrid.set(rx, ry, type | roomId);
                     }
                  }
               }

               roomId++;
            }
         }
      }
};

struct FloorRooms {
  virtual ~FloorRooms() = default;
  virtual std::string get_1x1(RandomSource& r) const = 0;
  virtual std::string get_1x1Secret(RandomSource& r) const { return "1x1_as" + std::to_string(r.nextInt(4) + 1); }
  virtual std::string get_1x2SideEntrance(RandomSource& r, bool stairs) const = 0;
  virtual std::string get_1x2FrontEntrance(RandomSource& r, bool stairs) const = 0;
  virtual std::string get_1x2Secret(RandomSource& r) const = 0;
  virtual std::string get_2x2(RandomSource& r) const = 0;
  std::string get_2x2Secret(RandomSource&) const { return "2x2_s1"; }
};
struct FirstFloor : FloorRooms {
  std::string get_1x1(RandomSource& r) const override { return "1x1_a" + std::to_string(r.nextInt(5) + 1); }
  std::string get_1x2SideEntrance(RandomSource& r, bool) const override { return "1x2_a" + std::to_string(r.nextInt(9) + 1); }
  std::string get_1x2FrontEntrance(RandomSource& r, bool) const override { return "1x2_b" + std::to_string(r.nextInt(5) + 1); }
  std::string get_1x2Secret(RandomSource& r) const override { return "1x2_s" + std::to_string(r.nextInt(2) + 1); }
  std::string get_2x2(RandomSource& r) const override { return "2x2_a" + std::to_string(r.nextInt(4) + 1); }
};
struct SecondFloor : FloorRooms {  // also ThirdFloorRoomCollection
  std::string get_1x1(RandomSource& r) const override { return "1x1_b" + std::to_string(r.nextInt(5) + 1); }
  std::string get_1x2SideEntrance(RandomSource& r, bool stairs) const override {
    return stairs ? "1x2_c_stairs" : "1x2_c" + std::to_string(r.nextInt(4) + 1);
  }
  std::string get_1x2FrontEntrance(RandomSource& r, bool stairs) const override {
    return stairs ? "1x2_d_stairs" : "1x2_d" + std::to_string(r.nextInt(5) + 1);
  }
  std::string get_1x2Secret(RandomSource& r) const override { return "1x2_se" + std::to_string(r.nextInt(1) + 1); }
  std::string get_2x2(RandomSource& r) const override { return "2x2_b" + std::to_string(r.nextInt(5) + 1); }
};

PlaceSettings mansionSettings(Mirror m, Rotation r) {
  PlaceSettings s;
  s.ignoreEntities = true;
  s.rotation = r;
  s.mirror = m;
  static const ProcessorP IGNORE = blockIgnore({"structure_block"});
  s.processors.push_back(IGNORE);
  return s;
}

struct MansionPiece : TemplatePiece {
  MansionPiece(const std::string& name, BlockPos pos, Rotation r, Mirror m)
      : TemplatePiece(0, "minecraft:woodland_mansion/" + name, mansionSettings(m, r), pos) {}
  void handleDataMarker(const std::string& marker, BlockPos pos, Level& level, RandomSource& random, const BoundingBox& bb) override {
    if (marker.rfind("Chest", 0) == 0) {
      Rotation rot = settings.rotation;
      BS chest = blocks::chest;
      if (marker == "ChestWest") chest = chest.with("facing", rotate(rot, Dir::WEST));
      else if (marker == "ChestEast") chest = chest.with("facing", rotate(rot, Dir::EAST));
      else if (marker == "ChestSouth") chest = chest.with("facing", rotate(rot, Dir::SOUTH));
      else if (marker == "ChestNorth") chest = chest.with("facing", rotate(rot, Dir::NORTH));
      createChest(level, bb, random, pos, chest);
      return;
    }
    int mobs;
    if (marker == "Mage" || marker == "Warrior") mobs = 1;
    else if (marker == "Group of Allays") mobs = level.random().nextInt(3) + 1;
    else return;
    // ponytail: evokers/vindicators/allays are not spawned; the marker block is still cleared like vanilla
    if (mobs > 0) level.set(pos, blocks::air);
  }
};

PieceP mp(const std::string& name, BlockPos pos, Rotation r, Mirror m = Mirror::NONE) {
  return std::make_unique<MansionPiece>(name, pos, r, m);
}

struct PlacementData {
  Rotation rotation;
  BlockPos position;
  std::string wallType;
};

struct MansionPiecePlacer {
  RandomSource& random;
  int startX = 0, startY = 0;
  explicit MansionPiecePlacer(RandomSource& r) : random(r) {}
      void createMansion(
         BlockPos origin,
         Rotation rotation,
         std::vector<PieceP>& pieces,
         MansionGrid& mansion
      ) {
         PlacementData data;
         data.position = origin;
         data.rotation = rotation;
         data.wallType = "wall_flat";
         PlacementData secondData;
         entrance(pieces, data);
         secondData.position = data.position.above(8);
         secondData.rotation = data.rotation;
         secondData.wallType = "wall_window";

         SimpleGrid& baseGrid = mansion.baseGrid;
         SimpleGrid& thirdGrid = mansion.thirdFloorGrid;
         startX = mansion.entranceX + 1;
         startY = mansion.entranceY + 1;
         int endX = mansion.entranceX + 1;
         int endY = mansion.entranceY;
         traverseOuterWalls(pieces, data, baseGrid, Dir::SOUTH, startX, startY, endX, endY);
         traverseOuterWalls(pieces, secondData, baseGrid, Dir::SOUTH, startX, startY, endX, endY);
         PlacementData thirdData;
         thirdData.position = data.position.above(19);
         thirdData.rotation = data.rotation;
         thirdData.wallType = "wall_window";
         bool done = false;

         for (int y = 0; y < thirdGrid.height && !done; y++) {
            for (int x = thirdGrid.width - 1; x >= 0 && !done; x--) {
               if (isHouse(thirdGrid, x, y)) {
                  thirdData.position = thirdData.position.relative(rotate(rotation, Dir::SOUTH), 8 + (y - startY) * 8);
                  thirdData.position = thirdData.position.relative(rotate(rotation, Dir::EAST), (x - startX) * 8);
                  traverseWallPiece(pieces, thirdData);
                  traverseOuterWalls(pieces, thirdData, thirdGrid, Dir::SOUTH, x, y, x, y);
                  done = true;
               }
            }
         }

         createRoof(pieces, origin.above(16), rotation, baseGrid, &thirdGrid);
         createRoof(pieces, origin.above(27), rotation, thirdGrid, nullptr);

         static const FirstFloor first;
         static const SecondFloor second;
         const FloorRooms* roomCollections[] = {&first, &second, &second};

         for (int floorNum = 0; floorNum < 3; floorNum++) {
            BlockPos floorOrigin = origin.above(8 * floorNum + (floorNum == 2 ? 3 : 0));
            SimpleGrid& rooms = mansion.floorRooms[floorNum];
            SimpleGrid& grid = floorNum == 2 ? thirdGrid : baseGrid;
            std::string southPiece = floorNum == 0 ? "carpet_south_1" : "carpet_south_2";
            std::string westPiece = floorNum == 0 ? "carpet_west_1" : "carpet_west_2";

            for (int y = 0; y < grid.height; y++) {
               for (int xx = 0; xx < grid.width; xx++) {
                  if (grid.get(xx, y) == 1) {
                     BlockPos pos = floorOrigin.relative(rotate(rotation, Dir::SOUTH), 8 + (y - startY) * 8);
                     pos = pos.relative(rotate(rotation, Dir::EAST), (xx - startX) * 8);
                     pieces.push_back(mp("corridor_floor", pos, rotation));
                     if (grid.get(xx, y - 1) == 1 || (rooms.get(xx, y - 1) & 8388608) == 8388608) {
                        pieces.push_back(
                           mp("carpet_north", pos.relative(rotate(rotation, Dir::EAST), 1).above(), rotation
                           )
                        );
                     }

                     if (grid.get(xx + 1, y) == 1 || (rooms.get(xx + 1, y) & 8388608) == 8388608) {
                        pieces.push_back(
                           mp("carpet_east",
                              pos.relative(rotate(rotation, Dir::SOUTH), 1).relative(rotate(rotation, Dir::EAST), 5).above(),
                              rotation
                           )
                        );
                     }

                     if (grid.get(xx, y + 1) == 1 || (rooms.get(xx, y + 1) & 8388608) == 8388608) {
                        pieces.push_back(
                           mp(southPiece,
                              pos.relative(rotate(rotation, Dir::SOUTH), 5).relative(rotate(rotation, Dir::WEST), 1),
                              rotation
                           )
                        );
                     }

                     if (grid.get(xx - 1, y) == 1 || (rooms.get(xx - 1, y) & 8388608) == 8388608) {
                        pieces.push_back(
                           mp(westPiece,
                              pos.relative(rotate(rotation, Dir::WEST), 1).relative(rotate(rotation, Dir::NORTH), 1),
                              rotation
                           )
                        );
                     }
                  }
               }
            }

            std::string wallPiece = floorNum == 0 ? "indoors_wall_1" : "indoors_wall_2";
            std::string doorPiece = floorNum == 0 ? "indoors_door_1" : "indoors_door_2";
            std::vector<Dir> doorDirs;

            for (int y = 0; y < grid.height; y++) {
               for (int xxx = 0; xxx < grid.width; xxx++) {
                  bool thirdFloorStartRoom = floorNum == 2 && grid.get(xxx, y) == 3;
                  if (grid.get(xxx, y) == 2 || thirdFloorStartRoom) {
                     int roomData = rooms.get(xxx, y);
                     int roomType = roomData & 983040;
                     int roomId = roomData & 65535;
                     thirdFloorStartRoom = thirdFloorStartRoom && (roomData & 8388608) == 8388608;
                     doorDirs.clear();
                     if ((roomData & 2097152) == 2097152) {
                        for (Dir direction : HORIZONTAL) {
                           if (grid.get(xxx + stepX(direction), y + stepZ(direction)) == 1) {
                              doorDirs.push_back(direction);
                           }
                        }
                     }

                     Dir doorDir = DIR_NULL;
                     if (!doorDirs.empty()) {
                        doorDir = doorDirs[random.nextInt((int)doorDirs.size())];
                     } else if ((roomData & 1048576) == 1048576) {
                        doorDir = Dir::UP;
                     }

                     BlockPos roomPos = floorOrigin.relative(rotate(rotation, Dir::SOUTH), 8 + (y - startY) * 8);
                     roomPos = roomPos.relative(rotate(rotation, Dir::EAST), -1 + (xxx - startX) * 8);
                     if (isHouse(grid, xxx - 1, y) && !mansion.isRoomId(grid, xxx - 1, y, floorNum, roomId)) {
                        pieces.push_back(
                           mp(doorDir == Dir::WEST ? doorPiece : wallPiece, roomPos, rotation
                           )
                        );
                     }

                     if (grid.get(xxx + 1, y) == 1 && !thirdFloorStartRoom) {
                        BlockPos posx = roomPos.relative(rotate(rotation, Dir::EAST), 8);
                        pieces.push_back(
                           mp(doorDir == Dir::EAST ? doorPiece : wallPiece, posx, rotation
                           )
                        );
                     }

                     if (isHouse(grid, xxx, y + 1) && !mansion.isRoomId(grid, xxx, y + 1, floorNum, roomId)) {
                        BlockPos posx = roomPos.relative(rotate(rotation, Dir::SOUTH), 7);
                        posx = posx.relative(rotate(rotation, Dir::EAST), 7);
                        pieces.push_back(
                           mp(doorDir == Dir::SOUTH ? doorPiece : wallPiece,
                              posx,
                              rotationAdd(rotation, Rotation::CLOCKWISE_90)
                           )
                        );
                     }

                     if (grid.get(xxx, y - 1) == 1 && !thirdFloorStartRoom) {
                        BlockPos posx = roomPos.relative(rotate(rotation, Dir::NORTH), 1);
                        posx = posx.relative(rotate(rotation, Dir::EAST), 7);
                        pieces.push_back(
                           mp(doorDir == Dir::NORTH ? doorPiece : wallPiece,
                              posx,
                              rotationAdd(rotation, Rotation::CLOCKWISE_90)
                           )
                        );
                     }

                     if (roomType == 65536) {
                        addRoom1x1(pieces, roomPos, rotation, doorDir, *roomCollections[floorNum]);
                     } else if (roomType == 131072 && doorDir != DIR_NULL) {
                        Dir roomDir = mansion.get1x2RoomDirection(grid, xxx, y, floorNum, roomId);
                        bool isStairsRoom = (roomData & 4194304) == 4194304;
                        addRoom1x2(pieces, roomPos, rotation, roomDir, doorDir, *roomCollections[floorNum], isStairsRoom);
                     } else if (roomType == 262144 && doorDir != DIR_NULL && doorDir != Dir::UP) {
                        Dir roomDir = clockWise(doorDir);
                        if (!mansion.isRoomId(grid, xxx + stepX(roomDir), y + stepZ(roomDir), floorNum, roomId)) {
                           roomDir = opposite(roomDir);
                        }

                        addRoom2x2(pieces, roomPos, rotation, roomDir, doorDir, *roomCollections[floorNum]);
                     } else if (roomType == 262144 && doorDir == Dir::UP) {
                        addRoom2x2Secret(pieces, roomPos, rotation, *roomCollections[floorNum]);
                     }
                  }
               }
            }
         }
      }

      void traverseOuterWalls(
         std::vector<PieceP>& pieces,
         PlacementData& data,
         SimpleGrid& grid,
         Dir gridDirection,
         int startX,
         int startY,
         int endX,
         int endY
      ) {
         int gridX = startX;
         int gridY = startY;
         Dir startDirection = gridDirection;

         do {
            if (!isHouse(grid, gridX + stepX(gridDirection), gridY + stepZ(gridDirection))) {
               traverseTurn(pieces, data);
               gridDirection = clockWise(gridDirection);
               if (gridX != endX || gridY != endY || startDirection != gridDirection) {
                  traverseWallPiece(pieces, data);
               }
            } else if (isHouse(grid, gridX + stepX(gridDirection), gridY + stepZ(gridDirection))
               && isHouse(
                  grid,
                  gridX + stepX(gridDirection) + stepX(counterClockWise(gridDirection)),
                  gridY + stepZ(gridDirection) + stepZ(counterClockWise(gridDirection))
               )) {
               traverseInnerTurn(pieces, data);
               gridX += stepX(gridDirection);
               gridY += stepZ(gridDirection);
               gridDirection = counterClockWise(gridDirection);
            } else {
               gridX += stepX(gridDirection);
               gridY += stepZ(gridDirection);
               if (gridX != endX || gridY != endY || startDirection != gridDirection) {
                  traverseWallPiece(pieces, data);
               }
            }
         } while (gridX != endX || gridY != endY || startDirection != gridDirection);
      }

      void createRoof(
         std::vector<PieceP>& pieces,
         BlockPos roofOrigin,
         Rotation rotation,
         SimpleGrid& grid,
         SimpleGrid* aboveGrid
      ) {
         for (int y = 0; y < grid.height; y++) {
            for (int x = 0; x < grid.width; x++) {
               BlockPos position = roofOrigin.relative(rotate(rotation, Dir::SOUTH), 8 + (y - startY) * 8);
               position = position.relative(rotate(rotation, Dir::EAST), (x - startX) * 8);
               bool isAbove = aboveGrid != nullptr && isHouse(*aboveGrid, x, y);
               if (isHouse(grid, x, y) && !isAbove) {
                  pieces.push_back(mp("roof", position.above(3), rotation));
                  if (!isHouse(grid, x + 1, y)) {
                     BlockPos p2 = position.relative(rotate(rotation, Dir::EAST), 6);
                     pieces.push_back(mp("roof_front", p2, rotation));
                  }

                  if (!isHouse(grid, x - 1, y)) {
                     BlockPos p2 = position.relative(rotate(rotation, Dir::EAST), 0);
                     p2 = p2.relative(rotate(rotation, Dir::SOUTH), 7);
                     pieces.push_back(
                        mp("roof_front", p2, rotationAdd(rotation, Rotation::CLOCKWISE_180)
                        )
                     );
                  }

                  if (!isHouse(grid, x, y - 1)) {
                     BlockPos p2 = position.relative(rotate(rotation, Dir::WEST), 1);
                     pieces.push_back(
                        mp("roof_front", p2, rotationAdd(rotation, Rotation::COUNTERCLOCKWISE_90)
                        )
                     );
                  }

                  if (!isHouse(grid, x, y + 1)) {
                     BlockPos p2 = position.relative(rotate(rotation, Dir::EAST), 6);
                     p2 = p2.relative(rotate(rotation, Dir::SOUTH), 6);
                     pieces.push_back(
                        mp("roof_front", p2, rotationAdd(rotation, Rotation::CLOCKWISE_90)
                        )
                     );
                  }
               }
            }
         }

         if (aboveGrid != nullptr) {
            for (int y = 0; y < grid.height; y++) {
               for (int xx = 0; xx < grid.width; xx++) {
                  BlockPos var17 = roofOrigin.relative(rotate(rotation, Dir::SOUTH), 8 + (y - startY) * 8);
                  var17 = var17.relative(rotate(rotation, Dir::EAST), (xx - startX) * 8);
                  bool isAbove = isHouse(*aboveGrid, xx, y);
                  if (isHouse(grid, xx, y) && isAbove) {
                     if (!isHouse(grid, xx + 1, y)) {
                        BlockPos p2 = var17.relative(rotate(rotation, Dir::EAST), 7);
                        pieces.push_back(mp("small_wall", p2, rotation));
                     }

                     if (!isHouse(grid, xx - 1, y)) {
                        BlockPos p2 = var17.relative(rotate(rotation, Dir::WEST), 1);
                        p2 = p2.relative(rotate(rotation, Dir::SOUTH), 6);
                        pieces.push_back(
                           mp("small_wall", p2, rotationAdd(rotation, Rotation::CLOCKWISE_180)
                           )
                        );
                     }

                     if (!isHouse(grid, xx, y - 1)) {
                        BlockPos p2 = var17.relative(rotate(rotation, Dir::WEST), 0);
                        p2 = p2.relative(rotate(rotation, Dir::NORTH), 1);
                        pieces.push_back(
                           mp("small_wall", p2, rotationAdd(rotation, Rotation::COUNTERCLOCKWISE_90)
                           )
                        );
                     }

                     if (!isHouse(grid, xx, y + 1)) {
                        BlockPos p2 = var17.relative(rotate(rotation, Dir::EAST), 6);
                        p2 = p2.relative(rotate(rotation, Dir::SOUTH), 7);
                        pieces.push_back(
                           mp("small_wall", p2, rotationAdd(rotation, Rotation::CLOCKWISE_90)
                           )
                        );
                     }

                     if (!isHouse(grid, xx + 1, y)) {
                        if (!isHouse(grid, xx, y - 1)) {
                           BlockPos p2 = var17.relative(rotate(rotation, Dir::EAST), 7);
                           p2 = p2.relative(rotate(rotation, Dir::NORTH), 2);
                           pieces.push_back(mp("small_wall_corner", p2, rotation));
                        }

                        if (!isHouse(grid, xx, y + 1)) {
                           BlockPos p2 = var17.relative(rotate(rotation, Dir::EAST), 8);
                           p2 = p2.relative(rotate(rotation, Dir::SOUTH), 7);
                           pieces.push_back(
                              mp("small_wall_corner", p2, rotationAdd(rotation, Rotation::CLOCKWISE_90)
                              )
                           );
                        }
                     }

                     if (!isHouse(grid, xx - 1, y)) {
                        if (!isHouse(grid, xx, y - 1)) {
                           BlockPos p2 = var17.relative(rotate(rotation, Dir::WEST), 2);
                           p2 = p2.relative(rotate(rotation, Dir::NORTH), 1);
                           pieces.push_back(
                              mp("small_wall_corner", p2, rotationAdd(rotation, Rotation::COUNTERCLOCKWISE_90)
                              )
                           );
                        }

                        if (!isHouse(grid, xx, y + 1)) {
                           BlockPos p2 = var17.relative(rotate(rotation, Dir::WEST), 1);
                           p2 = p2.relative(rotate(rotation, Dir::SOUTH), 8);
                           pieces.push_back(
                              mp("small_wall_corner", p2, rotationAdd(rotation, Rotation::CLOCKWISE_180)
                              )
                           );
                        }
                     }
                  }
               }
            }
         }

         for (int y = 0; y < grid.height; y++) {
            for (int xxx = 0; xxx < grid.width; xxx++) {
               BlockPos var19 = roofOrigin.relative(rotate(rotation, Dir::SOUTH), 8 + (y - startY) * 8);
               var19 = var19.relative(rotate(rotation, Dir::EAST), (xxx - startX) * 8);
               bool isAbove = aboveGrid != nullptr && isHouse(*aboveGrid, xxx, y);
               if (isHouse(grid, xxx, y) && !isAbove) {
                  if (!isHouse(grid, xxx + 1, y)) {
                     BlockPos p2 = var19.relative(rotate(rotation, Dir::EAST), 6);
                     if (!isHouse(grid, xxx, y + 1)) {
                        BlockPos p3 = p2.relative(rotate(rotation, Dir::SOUTH), 6);
                        pieces.push_back(mp("roof_corner", p3, rotation));
                     } else if (isHouse(grid, xxx + 1, y + 1)) {
                        BlockPos p3 = p2.relative(rotate(rotation, Dir::SOUTH), 5);
                        pieces.push_back(mp("roof_inner_corner", p3, rotation));
                     }

                     if (!isHouse(grid, xxx, y - 1)) {
                        pieces.push_back(
                           mp("roof_corner", p2, rotationAdd(rotation, Rotation::COUNTERCLOCKWISE_90)
                           )
                        );
                     } else if (isHouse(grid, xxx + 1, y - 1)) {
                        BlockPos p3 = var19.relative(rotate(rotation, Dir::EAST), 9);
                        p3 = p3.relative(rotate(rotation, Dir::NORTH), 2);
                        pieces.push_back(
                           mp("roof_inner_corner", p3, rotationAdd(rotation, Rotation::CLOCKWISE_90)
                           )
                        );
                     }
                  }

                  if (!isHouse(grid, xxx - 1, y)) {
                     BlockPos p2x = var19.relative(rotate(rotation, Dir::EAST), 0);
                     p2x = p2x.relative(rotate(rotation, Dir::SOUTH), 0);
                     if (!isHouse(grid, xxx, y + 1)) {
                        BlockPos p3 = p2x.relative(rotate(rotation, Dir::SOUTH), 6);
                        pieces.push_back(
                           mp("roof_corner", p3, rotationAdd(rotation, Rotation::CLOCKWISE_90)
                           )
                        );
                     } else if (isHouse(grid, xxx - 1, y + 1)) {
                        BlockPos p3 = p2x.relative(rotate(rotation, Dir::SOUTH), 8);
                        p3 = p3.relative(rotate(rotation, Dir::WEST), 3);
                        pieces.push_back(
                           mp("roof_inner_corner", p3, rotationAdd(rotation, Rotation::COUNTERCLOCKWISE_90)
                           )
                        );
                     }

                     if (!isHouse(grid, xxx, y - 1)) {
                        pieces.push_back(
                           mp("roof_corner", p2x, rotationAdd(rotation, Rotation::CLOCKWISE_180)
                           )
                        );
                     } else if (isHouse(grid, xxx - 1, y - 1)) {
                        BlockPos p3 = p2x.relative(rotate(rotation, Dir::SOUTH), 1);
                        pieces.push_back(
                           mp("roof_inner_corner", p3, rotationAdd(rotation, Rotation::CLOCKWISE_180)
                           )
                        );
                     }
                  }
               }
            }
         }
      }

      void entrance(std::vector<PieceP>& pieces, PlacementData& data) {
         Dir west = rotate(data.rotation, Dir::WEST);
         pieces.push_back(mp("entrance", data.position.relative(west, 9), data.rotation));
         data.position = data.position.relative(rotate(data.rotation, Dir::SOUTH), 16);
      }

      void traverseWallPiece(std::vector<PieceP>& pieces, PlacementData& data) {
         pieces.push_back(
            mp(data.wallType, data.position.relative(rotate(data.rotation, Dir::EAST), 7), data.rotation
            )
         );
         data.position = data.position.relative(rotate(data.rotation, Dir::SOUTH), 8);
      }

      void traverseTurn(std::vector<PieceP>& pieces, PlacementData& data) {
         data.position = data.position.relative(rotate(data.rotation, Dir::SOUTH), -1);
         pieces.push_back(mp("wall_corner", data.position, data.rotation));
         data.position = data.position.relative(rotate(data.rotation, Dir::SOUTH), -7);
         data.position = data.position.relative(rotate(data.rotation, Dir::WEST), -6);
         data.rotation = rotationAdd(data.rotation, Rotation::CLOCKWISE_90);
      }

      void traverseInnerTurn(std::vector<PieceP>& pieces, PlacementData& data) {
         data.position = data.position.relative(rotate(data.rotation, Dir::SOUTH), 6);
         data.position = data.position.relative(rotate(data.rotation, Dir::EAST), 8);
         data.rotation = rotationAdd(data.rotation, Rotation::COUNTERCLOCKWISE_90);
      }

      void addRoom1x1(
         std::vector<PieceP>& pieces,
         BlockPos roomPos,
         Rotation rotation,
         Dir doorDir,
         const FloorRooms& rooms
      ) {
         Rotation pieceRot = Rotation::NONE;
         std::string roomType = rooms.get_1x1(random);
         if (doorDir != Dir::EAST) {
            if (doorDir == Dir::NORTH) {
               pieceRot = rotationAdd(pieceRot, Rotation::COUNTERCLOCKWISE_90);
            } else if (doorDir == Dir::WEST) {
               pieceRot = rotationAdd(pieceRot, Rotation::CLOCKWISE_180);
            } else if (doorDir == Dir::SOUTH) {
               pieceRot = rotationAdd(pieceRot, Rotation::CLOCKWISE_90);
            } else {
               roomType = rooms.get_1x1Secret(random);
            }
         }

         BlockPos orientation = zeroPositionWithTransform(BlockPos{1, 0, 0}, Mirror::NONE, pieceRot, 7, 7);
         pieceRot = rotationAdd(pieceRot, rotation);
         orientation = rotatePos(orientation, rotation);
         BlockPos pos = roomPos.offset(orientation.x, 0, orientation.z);
         pieces.push_back(mp(roomType, pos, pieceRot));
      }

      void addRoom1x2(
         std::vector<PieceP>& pieces,
         BlockPos roomPos,
         Rotation rotation,
         Dir roomDir,
         Dir doorDir,
         const FloorRooms& rooms,
         bool isStairsRoom
      ) {
         if (doorDir == Dir::EAST && roomDir == Dir::SOUTH) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 1);
            pieces.push_back(
               mp(rooms.get_1x2SideEntrance(random, isStairsRoom), pos, rotation)
            );
         } else if (doorDir == Dir::EAST && roomDir == Dir::NORTH) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 1);
            pos = pos.relative(rotate(rotation, Dir::SOUTH), 6);
            pieces.push_back(
               mp(rooms.get_1x2SideEntrance(random, isStairsRoom), pos, rotation, Mirror::LEFT_RIGHT
               )
            );
         } else if (doorDir == Dir::WEST && roomDir == Dir::NORTH) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 7);
            pos = pos.relative(rotate(rotation, Dir::SOUTH), 6);
            pieces.push_back(
               mp(rooms.get_1x2SideEntrance(random, isStairsRoom), pos, rotationAdd(rotation, Rotation::CLOCKWISE_180)
               )
            );
         } else if (doorDir == Dir::WEST && roomDir == Dir::SOUTH) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 7);
            pieces.push_back(
               mp(rooms.get_1x2SideEntrance(random, isStairsRoom), pos, rotation, Mirror::FRONT_BACK
               )
            );
         } else if (doorDir == Dir::SOUTH && roomDir == Dir::EAST) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 1);
            pieces.push_back(
               mp(rooms.get_1x2SideEntrance(random, isStairsRoom),
                  pos,
                  rotationAdd(rotation, Rotation::CLOCKWISE_90),
                  Mirror::LEFT_RIGHT
               )
            );
         } else if (doorDir == Dir::SOUTH && roomDir == Dir::WEST) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 7);
            pieces.push_back(
               mp(rooms.get_1x2SideEntrance(random, isStairsRoom), pos, rotationAdd(rotation, Rotation::CLOCKWISE_90)
               )
            );
         } else if (doorDir == Dir::NORTH && roomDir == Dir::WEST) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 7);
            pos = pos.relative(rotate(rotation, Dir::SOUTH), 6);
            pieces.push_back(
               mp(rooms.get_1x2SideEntrance(random, isStairsRoom),
                  pos,
                  rotationAdd(rotation, Rotation::CLOCKWISE_90),
                  Mirror::FRONT_BACK
               )
            );
         } else if (doorDir == Dir::NORTH && roomDir == Dir::EAST) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 1);
            pos = pos.relative(rotate(rotation, Dir::SOUTH), 6);
            pieces.push_back(
               mp(rooms.get_1x2SideEntrance(random, isStairsRoom), pos, rotationAdd(rotation, Rotation::COUNTERCLOCKWISE_90)
               )
            );
         } else if (doorDir == Dir::SOUTH && roomDir == Dir::NORTH) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 1);
            pos = pos.relative(rotate(rotation, Dir::NORTH), 8);
            pieces.push_back(
               mp(rooms.get_1x2FrontEntrance(random, isStairsRoom), pos, rotation
               )
            );
         } else if (doorDir == Dir::NORTH && roomDir == Dir::SOUTH) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 7);
            pos = pos.relative(rotate(rotation, Dir::SOUTH), 14);
            pieces.push_back(
               mp(rooms.get_1x2FrontEntrance(random, isStairsRoom), pos, rotationAdd(rotation, Rotation::CLOCKWISE_180)
               )
            );
         } else if (doorDir == Dir::WEST && roomDir == Dir::EAST) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 15);
            pieces.push_back(
               mp(rooms.get_1x2FrontEntrance(random, isStairsRoom), pos, rotationAdd(rotation, Rotation::CLOCKWISE_90)
               )
            );
         } else if (doorDir == Dir::EAST && roomDir == Dir::WEST) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::WEST), 7);
            pos = pos.relative(rotate(rotation, Dir::SOUTH), 6);
            pieces.push_back(
               mp(rooms.get_1x2FrontEntrance(random, isStairsRoom), pos, rotationAdd(rotation, Rotation::COUNTERCLOCKWISE_90)
               )
            );
         } else if (doorDir == Dir::UP && roomDir == Dir::EAST) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 15);
            pieces.push_back(
               mp(rooms.get_1x2Secret(random), pos, rotationAdd(rotation, Rotation::CLOCKWISE_90)
               )
            );
         } else if (doorDir == Dir::UP && roomDir == Dir::SOUTH) {
            BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 1);
            pos = pos.relative(rotate(rotation, Dir::NORTH), 0);
            pieces.push_back(mp(rooms.get_1x2Secret(random), pos, rotation));
         }
      }

      void addRoom2x2(
         std::vector<PieceP>& pieces,
         BlockPos roomPos,
         Rotation rotation,
         Dir roomDir,
         Dir doorDir,
         const FloorRooms& rooms
      ) {
         int east = 0;
         int south = 0;
         Rotation rot = rotation;
         Mirror mirror = Mirror::NONE;
         if (doorDir == Dir::EAST && roomDir == Dir::SOUTH) {
            east = -7;
         } else if (doorDir == Dir::EAST && roomDir == Dir::NORTH) {
            east = -7;
            south = 6;
            mirror = Mirror::LEFT_RIGHT;
         } else if (doorDir == Dir::NORTH && roomDir == Dir::EAST) {
            east = 1;
            south = 14;
            rot = rotationAdd(rotation, Rotation::COUNTERCLOCKWISE_90);
         } else if (doorDir == Dir::NORTH && roomDir == Dir::WEST) {
            east = 7;
            south = 14;
            rot = rotationAdd(rotation, Rotation::COUNTERCLOCKWISE_90);
            mirror = Mirror::LEFT_RIGHT;
         } else if (doorDir == Dir::SOUTH && roomDir == Dir::WEST) {
            east = 7;
            south = -8;
            rot = rotationAdd(rotation, Rotation::CLOCKWISE_90);
         } else if (doorDir == Dir::SOUTH && roomDir == Dir::EAST) {
            east = 1;
            south = -8;
            rot = rotationAdd(rotation, Rotation::CLOCKWISE_90);
            mirror = Mirror::LEFT_RIGHT;
         } else if (doorDir == Dir::WEST && roomDir == Dir::NORTH) {
            east = 15;
            south = 6;
            rot = rotationAdd(rotation, Rotation::CLOCKWISE_180);
         } else if (doorDir == Dir::WEST && roomDir == Dir::SOUTH) {
            east = 15;
            mirror = Mirror::FRONT_BACK;
         }

         BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), east);
         pos = pos.relative(rotate(rotation, Dir::SOUTH), south);
         pieces.push_back(mp(rooms.get_2x2(random), pos, rot, mirror));
      }

      void addRoom2x2Secret(
         std::vector<PieceP>& pieces,
         BlockPos roomPos,
         Rotation rotation,
         const FloorRooms& rooms
      ) {
         BlockPos pos = roomPos.relative(rotate(rotation, Dir::EAST), 1);
         pieces.push_back(mp(rooms.get_2x2Secret(random), pos, rotation, Mirror::NONE));
      }
};

}  // namespace

void generateMansion(BlockPos pos, Rotation r, Builder& builder, RandomSource& random) {
  MansionGrid grid(random);
  MansionPiecePlacer placer(random);
  std::vector<PieceP> pieces;
  placer.createMansion(pos, r, pieces, grid);
  for (auto& p : pieces) builder.add(std::move(p));
}

}  // namespace structure
