// Piece generators of the legacy (code-defined) structures, used by structures.cpp.
#pragma once
#include "worldgen/structure/structure.hpp"

namespace structure {

PieceP makeSwampHut(RandomSource& r, int x, int z);
PieceP makeJungleTemple(RandomSource& r, int x, int z);
PieceP makeDesertPyramid(RandomSource& r, int x, int z);
PieceP makeBuriedTreasure(BlockPos p);
void desertPyramidAfterPlace(Level& level, const BoundingBox& chunkBB, const std::vector<PieceP>& pieces);

void addIglooPieces(BlockPos pos, Rotation r, Builder& builder, RandomSource& random);
struct ShipwreckHandle {
  virtual ~ShipwreckHandle() = default;
  virtual bool tooBig() const = 0;
  virtual int beachedPosition(int minY, RandomSource& random) const = 0;
  virtual void adjustHeight(int h) = 0;
  virtual const BoundingBox& box() const = 0;
};
ShipwreckHandle* addShipwreckPiece(BlockPos pos, Rotation r, Builder& builder, RandomSource& random, bool beached);
void addOceanRuinPieces(BlockPos pos, Rotation r, Builder& builder, RandomSource& random, bool cold, float largeProbability,
                        float clusterProbability);
namespace RuinedPortal {
enum Placement { ON_LAND_SURFACE, PARTLY_BURIED, ON_OCEAN_FLOOR, IN_MOUNTAIN, UNDERGROUND, IN_NETHER };
struct Properties { bool cold; float mossiness; bool airPocket, overgrown, vines, replaceWithBlackstone; };
}  // namespace RuinedPortal
PieceP makeRuinedPortalPiece(BlockPos pos, RuinedPortal::Placement vp, const RuinedPortal::Properties& p, const std::string& id,
                             Rotation r, Mirror m, BlockPos pivot);
void addNetherFossilPieces(Builder& builder, RandomSource& random, BlockPos pos);

// Big legacy structures (each in its own file).
void generateMineshaft(Builder& builder, RandomSource& random, int x, int z, bool mesa);  // MineShaftRoom + children
struct StrongholdResult { bool hasPortalRoom; };
StrongholdResult generateStronghold(Builder& builder, RandomSource& random, int x, int z);
void generateFortress(Builder& builder, RandomSource& random, int x, int z);
PieceP makeMonumentBuilding(RandomSource& random, int west, int north, Dir orientation);
void generateMansion(BlockPos pos, Rotation r, Builder& builder, RandomSource& random);
void generateEndCity(BlockPos pos, Rotation r, Builder& builder, RandomSource& random);

}  // namespace structure
