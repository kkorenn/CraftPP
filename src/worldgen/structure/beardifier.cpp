// Beardifier (levelgen/Beardifier): terrain adaptation density added to final_density around structures.
#include <cmath>

#include "worldgen/density.hpp"
#include "worldgen/structure/jigsaw.hpp"

namespace structure {

namespace {
const float* beardKernel() {
  static const std::vector<float> k = [] {
    std::vector<float> v(24 * 24 * 24);
    for (int zi = 0; zi < 24; zi++)
      for (int xi = 0; xi < 24; xi++)
        for (int yi = 0; yi < 24; yi++) {
          double dx = xi - 12, dy = (yi - 12) + 0.5, dz = zi - 12;
          v[zi * 24 * 24 + xi * 24 + yi] = (float)std::pow(M_E, -(dx * dx + dy * dy + dz * dz) / 16.0);
        }
    return v;
  }();
  return k.data();
}

double fastInvSqrt(double x) {  // Mth.fastInvSqrt
  double xhalf = 0.5 * x;
  int64_t i;
  std::memcpy(&i, &x, 8);
  i = 6910469410427058090LL - (i >> 1);
  std::memcpy(&x, &i, 8);
  return x * (1.5 - xhalf * x * x);
}

float buryContribution(float dx, float dy, float dz) {
  float d = dx * dx + dy * dy + dz * dz;
  return d >= 36.0f ? 0.0f : 1.0f - (float)std::sqrt((double)d) / 6.0f;
}

float beardContribution(int dx, int dy, int dz, int yToGround) {
  int xi = dx + 12, yi = dy + 12, zi = dz + 12;
  if (xi < 0 || xi >= 24 || yi < 0 || yi >= 24 || zi < 0 || zi >= 24) return 0.0f;
  float dyo = (float)yToGround + 0.5f;
  float d = (float)dx * (float)dx + dyo * dyo + (float)dz * (float)dz;
  float value = -dyo * (float)fastInvSqrt((double)(d / 2.0f)) / 2.0f;
  return value * beardKernel()[zi * 24 * 24 + xi * 24 + yi];
}
}  // namespace

struct BeardifierData : df::Sampler {
  struct Rigid { BoundingBox box; TerrainAdjustment adj; int groundLevelDelta; };
  std::vector<Rigid> rigids;
  std::vector<JigsawJunction> junctions;
  BoundingBox affected;

  float unchecked(int x, int y, int z) const {
    float v = 0.0f;
    for (auto& r : rigids) {
      const BoundingBox& b = r.box;
      int dx = std::max(0, std::max(b.minX - x, x - b.maxX));
      int dz = std::max(0, std::max(b.minZ - z, z - b.maxZ));
      int groundY = b.minY + r.groundLevelDelta;
      int dyToGround = y - groundY;
      int dy = 0;
      switch (r.adj) {
        case TerrainAdjustment::BURY: case TerrainAdjustment::BEARD_THIN: dy = dyToGround; break;
        case TerrainAdjustment::BEARD_BOX: dy = std::max(0, std::max(groundY - y, y - b.maxY)); break;
        case TerrainAdjustment::ENCAPSULATE: dy = std::max(0, std::max(b.minY - y, y - b.maxY)); break;
        default: break;
      }
      switch (r.adj) {
        case TerrainAdjustment::BURY: v += buryContribution((float)dx, (float)dy / 2.0f, (float)dz); break;
        case TerrainAdjustment::BEARD_THIN: case TerrainAdjustment::BEARD_BOX:
          v += beardContribution(dx, dy, dz, dyToGround) * 0.8f;
          break;
        case TerrainAdjustment::ENCAPSULATE:
          v += buryContribution((float)dx / 2.0f, (float)dy / 2.0f, (float)dz / 2.0f) * 0.8f;
          break;
        default: break;
      }
    }
    for (auto& j : junctions) {
      int dx = x - j.sourceX, dy = y - j.sourceGroundY, dz = z - j.sourceZ;
      v += beardContribution(dx, dy, dz, dy) * 0.4f;
    }
    return v;
  }
  float sampleValue(df::SamplerContext&, int x, int y, int z) const override {
    return affected.isInside(x, y, z) ? unchecked(x, y, z) : 0.0f;
  }
  void sampleVolume(df::SamplerContext&, DensityBuffer& out, const DensityVolume& v) const override {
    out.fill(0.0f);
    BoundingBox vb(v.minBlockX, v.minBlockY, v.minBlockZ, v.minBlockX + v.sizeX * v.stepBlockX - 1,
                   v.minBlockY + v.sizeY * v.stepBlockY - 1, v.minBlockZ + v.sizeZ * v.stepBlockZ - 1);
    if (!affected.intersects(vb)) return;
    auto fd = [](int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; };
    int minX = fd(std::max(0, affected.minX - v.minBlockX), v.stepBlockX);
    int minY = fd(std::max(0, affected.minY - v.minBlockY), v.stepBlockY);
    int minZ = fd(std::max(0, affected.minZ - v.minBlockZ), v.stepBlockZ);
    int maxX = std::min(v.sizeX - 1, fd(affected.maxX - v.minBlockX, v.stepBlockX));
    int maxY = std::min(v.sizeY - 1, fd(affected.maxY - v.minBlockY, v.stepBlockY));
    int maxZ = std::min(v.sizeZ - 1, fd(affected.maxZ - v.minBlockZ, v.stepBlockZ));
    for (int z = minZ; z <= maxZ; z++)
      for (int x = minX; x <= maxX; x++)
        for (int y = minY; y <= maxY; y++)
          out.set(y + (x + z * v.sizeX) * v.sizeY, unchecked(v.blockX(x), v.blockY(y), v.blockZ(z)));
  }
};

// Beardifier.forStructuresInChunk
std::shared_ptr<const df::Sampler> StructureManager::beardifier(int cx, int cz) {
  auto data = std::make_shared<BeardifierData>();
  std::optional<BoundingBox> any;
  auto include = [&](const BoundingBox& b) { any = any ? BoundingBox(*any).encapsulate(b) : b; };
  int x0 = cx * 16, z0 = cz * 16;
  // ponytail: vanilla iterates the chunk's reference map (a HashMap: identity order); registry order here.
  for (auto& [s, keys] : references(cx, cz)) {
    if (s->adaptation == TerrainAdjustment::NONE) continue;
    for (int64_t k : keys)
      for (auto& start : starts(keyX(k), keyZ(k))) {
        if (start->structure != s) continue;
        for (auto& piece : start->pieces) {
          if (!piece->isCloseToChunk(cx, cz, 12)) continue;
          if (piece->isPoolElement()) {
            auto* pp = static_cast<PoolElementPiece*>(piece.get());
            if (!pp->element->terrainMatching) {
              data->rigids.push_back({pp->boundingBox, s->adaptation, pp->groundLevelDelta});
              include(pp->boundingBox);
            }
            for (auto& j : pp->junctions)
              if (j.sourceX > x0 - 12 && j.sourceZ > z0 - 12 && j.sourceX < x0 + 15 + 12 && j.sourceZ < z0 + 15 + 12) {
                data->junctions.push_back(j);
                include(BoundingBox(BlockPos{j.sourceX, j.sourceGroundY, j.sourceZ}));
              }
          } else {
            data->rigids.push_back({piece->boundingBox, s->adaptation, 0});
            include(piece->boundingBox);
          }
        }
      }
  }
  if (!any) return nullptr;
  data->affected = any->inflatedBy(24);
  return data;
}

}  // namespace structure
