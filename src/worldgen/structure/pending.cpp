// ponytail: placeholder pieces for the big legacy structures still being ported (
// end city): one marker piece at the start so starts, references and /locate work.
#include "worldgen/structure/legacy.hpp"

namespace structure {
namespace {
struct Marker : Piece {
  explicit Marker(BlockPos p) : Piece(0, BoundingBox(p)) {}
  void postProcess(Level&, RandomSource&, const BoundingBox&, int, int, BlockPos) override {}
};
}  // namespace

void generateEndCity(BlockPos pos, Rotation, Builder& b, RandomSource&) { b.add(std::make_unique<Marker>(pos)); }

}  // namespace structure
