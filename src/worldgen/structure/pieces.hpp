// Shared helpers for the hand-ported legacy structure pieces.
#pragma once
#include <string>

#include "gen/ids.hpp"
#include "worldgen/structure/template.hpp"

namespace structure {

// BlockState value with Java-like setValue chaining: BS(blocks::oak_stairs).with("facing", Dir::NORTH).
struct BS {
  int id;
  BS(int s) : id(s) {}  // NOLINT: implicit like a BlockState
  operator int() const { return id; }
  BS with(std::string_view k, std::string_view v) const { return Blocks::get().with(id, k, v); }
  BS with(std::string_view k, const char* v) const { return with(k, std::string_view(v)); }
  BS with(std::string_view k, bool v) const { return with(k, v ? "true" : "false"); }
  BS with(std::string_view k, int v) const { return with(k, std::to_string(v)); }
  BS with(std::string_view k, Dir d) const { return with(k, dirName(d)); }
  BS rotate(Rotation r) const { return Blocks::get().rotate(id, r); }
  bool is(int block) const { return Blocks::get().blockOf(id) == Blocks::get().blockOf(block); }
  bool isAir() const { return Blocks::get().isAir(id); }
};

// ScatteredFeaturePiece
struct ScatteredPiece : Piece {
  int width, height, depth, heightPosition = -1;
  ScatteredPiece(int west, int floor, int north, int w, int h, int d, Dir dir)
      : Piece(0, makeBoundingBox(west, floor, north, dir, w, h, d)), width(w), height(h), depth(d) {
    setOrientation(dir);
  }
  bool updateAverageGroundHeight(Level& level, const BoundingBox& chunkBB, int offset);
  bool updateHeightPositionToLowestGroundHeight(Level& level, int offset);
  void saveState(std::vector<int>& out) const override {
    Piece::saveState(out);
    out.push_back(heightPosition);
  }
  void loadState(const int*& p) override {
    Piece::loadState(p);
    heightPosition = *p++;
  }
};

// TemplateStructurePiece
struct TemplatePiece : Piece {
  std::string templateName;
  const Template* tmpl;
  PlaceSettings settings;
  BlockPos templatePosition;
  TemplatePiece(int genDepth, const std::string& name, PlaceSettings s, BlockPos pos)
      : Piece(genDepth, templateBoundingBox(Template::get(name), s, pos)), templateName(fullId(name)),
        tmpl(&Template::get(name)), settings(std::move(s)), templatePosition(pos) {
    setOrientation(Dir::NORTH);
  }
  void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int cx, int cz, BlockPos ref) override;
  virtual void handleDataMarker(const std::string& marker, BlockPos pos, Level& level, RandomSource& random, const BoundingBox& chunkBB) {
    (void)marker, (void)pos, (void)level, (void)random, (void)chunkBB;
  }
  void move(int dx, int dy, int dz) override {
    Piece::move(dx, dy, dz);
    templatePosition = templatePosition.offset(dx, dy, dz);
  }
  bool isTemplate() const override { return true; }
  void saveState(std::vector<int>& out) const override {
    Piece::saveState(out);
    out.insert(out.end(), {templatePosition.x, templatePosition.y, templatePosition.z});
  }
  void loadState(const int*& p) override {
    Piece::loadState(p);
    templatePosition = {p[0], p[1], p[2]};
    p += 3;
  }
};

// RandomizableContainer.setBlockEntityLootTable: consumes random.nextLong() when the block entity is a container.
inline void setLootTable(Level& level, RandomSource& random, BlockPos pos, const char* table) {
  if (Blocks::get().flags(level.getBlock(pos.x, pos.y, pos.z)) & BS_RANDOMIZABLE) level.setLootTable(pos, table, random.nextLong());
}

inline std::unique_ptr<RandomSource> positionalRandom(int64_t seed, BlockPos pos) {  // createThreadLocalInstance(seed).forkPositional().at(pos)
  SingleThreadedRandomSource base(seed);
  return base.forkPositional()->at(pos.x, pos.y, pos.z);
}

}  // namespace structure
