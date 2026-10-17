// Template pools and jigsaw pieces (see jigsaw.cpp).
#pragma once
#include <climits>
#include <string>
#include <vector>

#include "worldgen/structure/template.hpp"

namespace structure {

struct PoolElement {
  enum Kind { EMPTY, SINGLE, LEGACY, LIST, FEATURE } kind = EMPTY;
  bool terrainMatching = false;           // projection
  std::string location;                   // template id (single/legacy) or placed feature id
  const ProcessorList* processors = nullptr;
  int overrideLiquid = -1;                // -1 none, 0 ignore, 1 apply waterlogging
  std::vector<const PoolElement*> list;
  bool isEmpty() const { return kind == EMPTY; }
  int groundLevelDelta() const { return 1; }
  const Template& tmpl() const;
  std::vector<JigsawInfo> shuffledJigsaws(BlockPos pos, Rotation r, RandomSource& random) const;
  BoundingBox boundingBox(BlockPos pos, Rotation r) const;
  bool place(Level& level, BlockPos pos, BlockPos ref, Rotation r, const BoundingBox& chunkBB, RandomSource& random,
             bool waterlog) const;
};

struct Pool {  // StructureTemplatePool
  std::string id, fallback;
  std::vector<const PoolElement*> templates;  // expanded by weight
  int maxSize() const;
  static const Pool* get(const std::string& id);  // nullptr when the pool does not exist
 private:
  mutable int maxSize_ = INT_MIN;
};

struct JigsawJunction {
  int sourceX, sourceGroundY, sourceZ, deltaY;
  bool destTerrainMatching;
};

struct PoolElementPiece : Piece {  // PoolElementStructurePiece
  const PoolElement* element;
  BlockPos position;
  int groundLevelDelta;
  Rotation rot;
  bool waterlog;
  std::vector<JigsawJunction> junctions;
  PoolElementPiece(const PoolElement* e, BlockPos pos, int gld, Rotation r, const BoundingBox& bb, bool waterlog)
      : Piece(0, bb), element(e), position(pos), groundLevelDelta(gld), rot(r), waterlog(waterlog) {}
  void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int cx, int cz, BlockPos ref) override;
  void move(int dx, int dy, int dz) override {
    Piece::move(dx, dy, dz);
    position = position.offset(dx, dy, dz);
  }
  bool isPoolElement() const override { return true; }
};

int resolveAnchor(const Json& j, int minY = MIN_Y, int height = GEN_HEIGHT);
int sampleHeight(const Json& j, RandomSource& r, int minY = MIN_Y, int height = GEN_HEIGHT);
std::unique_ptr<Structure> makeJigsawStructure(const Json& j);

}  // namespace structure
