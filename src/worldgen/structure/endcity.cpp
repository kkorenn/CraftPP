// EndCityPieces (26.3): template pieces chained by section generators (house tower, tower, bridge, fat tower).
#include "worldgen/structure/legacy.hpp"
#include "worldgen/structure/pieces.hpp"

namespace structure {
namespace {

PlaceSettings endCitySettings(bool overwrite, Rotation r) {
  static const ProcessorP STRUCTURE = blockIgnore({"structure_block"}), STRUCTURE_AIR = blockIgnore({"air", "structure_block"});
  PlaceSettings s;
  s.ignoreEntities = true;
  s.processors.push_back(overwrite ? STRUCTURE : STRUCTURE_AIR);
  s.rotation = r;
  return s;
}

struct EndCityPiece : TemplatePiece {
  EndCityPiece(const std::string& name, BlockPos pos, Rotation r, bool overwrite)
      : TemplatePiece(0, "minecraft:end_city/" + name, endCitySettings(overwrite, r), pos) {}
  void handleDataMarker(const std::string& marker, BlockPos pos, Level& level, RandomSource& random, const BoundingBox& chunkBB) override {
    if (marker.rfind("Chest", 0) == 0) {
      if (chunkBB.isInside(pos.below())) setLootTable(level, random, pos.below());
    } else if (chunkBB.isInside(pos) && marker.rfind("Sentry", 0) == 0) {
      level.addMob(entity_types::shulker, pos, false, false, false);  // vanilla: setPos + addFreshEntity only
    }
    // ponytail: the "Elytra" item frame is not placed (no item frame entity yet).
  }
};

using List = std::vector<PieceP>;
EndCityPiece* addHelper(List& pieces, std::unique_ptr<EndCityPiece> p) {
  EndCityPiece* raw = p.get();
  pieces.push_back(std::move(p));
  return raw;
}

std::unique_ptr<EndCityPiece> addPiece(EndCityPiece* parent, BlockPos offset, const std::string& name, Rotation r, bool overwrite) {
  auto child = std::make_unique<EndCityPiece>(name, parent->templatePosition, r, overwrite);
  // StructureTemplate.calculateConnectedPosition(parent settings, offset, child settings, ZERO)
  BlockPos o = relativePosition(parent->settings, offset).subtract(relativePosition(child->settings, {0, 0, 0}));
  child->move(o.x, o.y, o.z);
  return child;
}

enum Gen { HOUSE_TOWER, TOWER, TOWER_BRIDGE, FAT_TOWER };
struct Ctx {
  RandomSource& random;
  bool shipCreated = false;  // TOWER_BRIDGE_GENERATOR state (vanilla static, reset per city)
};

bool recursiveChildren(Ctx& c, Gen g, int depth, EndCityPiece* parent, std::optional<BlockPos> offset, List& pieces);

bool generate(Ctx& c, Gen g, int depth, EndCityPiece* parent, std::optional<BlockPos> offset, List& pieces) {
  RandomSource& random = c.random;
  Rotation rotation = parent->settings.rotation;
  switch (g) {
    case HOUSE_TOWER: {
      if (depth > 8) return false;
      EndCityPiece* last = addHelper(pieces, addPiece(parent, *offset, "base_floor", rotation, true));
      int floors = random.nextInt(3);
      if (floors == 0) {
        addHelper(pieces, addPiece(last, {-1, 4, -1}, "base_roof", rotation, true));
      } else if (floors == 1) {
        last = addHelper(pieces, addPiece(last, {-1, 0, -1}, "second_floor_2", rotation, false));
        last = addHelper(pieces, addPiece(last, {-1, 8, -1}, "second_roof", rotation, false));
        recursiveChildren(c, TOWER, depth + 1, last, std::nullopt, pieces);
      } else {
        last = addHelper(pieces, addPiece(last, {-1, 0, -1}, "second_floor_2", rotation, false));
        last = addHelper(pieces, addPiece(last, {-1, 4, -1}, "third_floor_2", rotation, false));
        last = addHelper(pieces, addPiece(last, {-1, 8, -1}, "third_roof", rotation, true));
        recursiveChildren(c, TOWER, depth + 1, last, std::nullopt, pieces);
      }
      return true;
    }
    case TOWER: {
      static const std::pair<Rotation, BlockPos> BRIDGES[4] = {{Rotation::NONE, {1, -1, 0}}, {Rotation::CLOCKWISE_90, {6, -1, 1}},
                                                               {Rotation::COUNTERCLOCKWISE_90, {0, -1, 5}}, {Rotation::CLOCKWISE_180, {5, -1, 6}}};
      int bx = 3 + random.nextInt(2), bz = 3 + random.nextInt(2);
      EndCityPiece* last = addHelper(pieces, addPiece(parent, {bx, -3, bz}, "tower_base", rotation, true));
      last = addHelper(pieces, addPiece(last, {0, 7, 0}, "tower_piece", rotation, true));
      EndCityPiece* bridge = random.nextInt(3) == 0 ? last : nullptr;
      int height = 1 + random.nextInt(3);
      for (int i = 0; i < height; i++) {
        last = addHelper(pieces, addPiece(last, {0, 4, 0}, "tower_piece", rotation, true));
        if (i < height - 1 && random.nextBoolean()) bridge = last;
      }
      if (bridge) {
        for (auto& [r, off] : BRIDGES)
          if (random.nextBoolean()) {
            EndCityPiece* start = addHelper(pieces, addPiece(bridge, off, "bridge_end", rotationAdd(rotation, r), true));
            recursiveChildren(c, TOWER_BRIDGE, depth + 1, start, std::nullopt, pieces);
          }
        addHelper(pieces, addPiece(last, {-1, 4, -1}, "tower_top", rotation, true));
      } else {
        if (depth != 7) return recursiveChildren(c, FAT_TOWER, depth + 1, last, std::nullopt, pieces);
        addHelper(pieces, addPiece(last, {-1, 4, -1}, "tower_top", rotation, true));
      }
      return true;
    }
    case TOWER_BRIDGE: {
      int length = random.nextInt(4) + 1;
      EndCityPiece* last = addHelper(pieces, addPiece(parent, {0, 0, -4}, "bridge_piece", rotation, true));
      last->genDepth = -1;
      int nextY = 0;
      for (int i = 0; i < length; i++) {
        if (random.nextBoolean()) {
          last = addHelper(pieces, addPiece(last, {0, nextY, -4}, "bridge_piece", rotation, true));
          nextY = 0;
        } else {
          if (random.nextBoolean()) last = addHelper(pieces, addPiece(last, {0, nextY, -4}, "bridge_steep_stairs", rotation, true));
          else last = addHelper(pieces, addPiece(last, {0, nextY, -8}, "bridge_gentle_stairs", rotation, true));
          nextY = 4;
        }
      }
      if (!c.shipCreated && random.nextInt(10 - depth) == 0) {
        int sx = -8 + random.nextInt(8), sz = -70 + random.nextInt(10);
        addHelper(pieces, addPiece(last, {sx, nextY, sz}, "ship", rotation, true));
        c.shipCreated = true;
      } else if (!recursiveChildren(c, HOUSE_TOWER, depth + 1, last, BlockPos{-3, nextY + 1, -11}, pieces)) {
        return false;
      }
      last = addHelper(pieces, addPiece(last, {4, nextY, 0}, "bridge_end", rotationAdd(rotation, Rotation::CLOCKWISE_180), true));
      last->genDepth = -1;
      return true;
    }
    case FAT_TOWER: {
      static const std::pair<Rotation, BlockPos> BRIDGES[4] = {{Rotation::NONE, {4, -1, 0}}, {Rotation::CLOCKWISE_90, {12, -1, 4}},
                                                               {Rotation::COUNTERCLOCKWISE_90, {0, -1, 8}}, {Rotation::CLOCKWISE_180, {8, -1, 12}}};
      EndCityPiece* last = addHelper(pieces, addPiece(parent, {-3, 4, -3}, "fat_tower_base", rotation, true));
      last = addHelper(pieces, addPiece(last, {0, 4, 0}, "fat_tower_middle", rotation, true));
      for (int i = 0; i < 2 && random.nextInt(3) != 0; i++) {
        last = addHelper(pieces, addPiece(last, {0, 8, 0}, "fat_tower_middle", rotation, true));
        for (auto& [r, off] : BRIDGES)
          if (random.nextBoolean()) {
            EndCityPiece* start = addHelper(pieces, addPiece(last, off, "bridge_end", rotationAdd(rotation, r), true));
            recursiveChildren(c, TOWER_BRIDGE, depth + 1, start, std::nullopt, pieces);
          }
      }
      addHelper(pieces, addPiece(last, {-2, 8, -2}, "fat_tower_top", rotation, true));
      return true;
    }
  }
  return false;
}

bool recursiveChildren(Ctx& c, Gen g, int depth, EndCityPiece* parent, std::optional<BlockPos> offset, List& pieces) {
  if (depth > 8) return false;
  List children;
  if (!generate(c, g, depth, parent, offset, children)) return false;
  int tag = c.random.nextInt();
  for (auto& child : children) {
    child->genDepth = tag;
    for (auto& p : pieces)  // StructurePiece.findCollisionPiece(pieces, box)
      if (p->boundingBox.intersects(child->boundingBox)) {
        if (p->genDepth != parent->genDepth) return false;
        break;
      }
  }
  for (auto& child : children) pieces.push_back(std::move(child));
  return true;
}

}  // namespace

void generateEndCity(BlockPos pos, Rotation r, Builder& builder, RandomSource& random) {  // startHouseTower
  Ctx c{random};
  List& pieces = builder.pieces;
  EndCityPiece* last = addHelper(pieces, std::make_unique<EndCityPiece>("base_floor", pos, r, true));
  last = addHelper(pieces, addPiece(last, {-1, 0, -1}, "second_floor_1", r, false));
  last = addHelper(pieces, addPiece(last, {-1, 4, -1}, "third_floor_1", r, false));
  last = addHelper(pieces, addPiece(last, {-1, 8, -1}, "third_roof", r, true));
  recursiveChildren(c, TOWER, 1, last, std::nullopt, pieces);
}

}  // namespace structure
