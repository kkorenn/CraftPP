// ponytail: placeholder pieces for the big legacy structures still being ported (stronghold,
// fortress, monument, mansion, end city): one marker piece at the start so starts, references and /locate work.
#include "worldgen/structure/legacy.hpp"

namespace structure {
namespace {
struct Marker : Piece {
  explicit Marker(BlockPos p) : Piece(0, BoundingBox(p)) {}
  void postProcess(Level&, RandomSource&, const BoundingBox&, int, int, BlockPos) override {}
};
}  // namespace

StrongholdResult generateStronghold(Builder& b, RandomSource&, int x, int z) {
  b.add(std::make_unique<Marker>(BlockPos{x, 64, z}));
  return {true};
}
void generateFortress(Builder& b, RandomSource&, int x, int z) { b.add(std::make_unique<Marker>(BlockPos{x, 64, z})); }
PieceP makeMonumentBuilding(RandomSource&, int west, int north, Dir) { return std::make_unique<Marker>(BlockPos{west, 39, north}); }
void generateMansion(BlockPos pos, Rotation, Builder& b, RandomSource&) { b.add(std::make_unique<Marker>(pos)); }
void generateEndCity(BlockPos pos, Rotation, Builder& b, RandomSource&) { b.add(std::make_unique<Marker>(pos)); }

}  // namespace structure
