// Port of net.minecraft.world.level.biome.Climate (ParameterList + RTree) and
// MultiNoiseBiomeSource.getNoiseBiome for the overworld preset. Bit-exact with vanilla 26.3.
//
// Wiring (vanilla MultiNoiseBiomeSource / Climate.Sampler / ChunkGenerator.doCreateBiomes):
//  - Biomes live on a quart grid (4x4x4 blocks). quart = block >> 2 (QuartPos.fromBlock),
//    block = quart << 2 (QuartPos.toBlock). No offset: the sample for quart (qx,qy,qz) is taken
//    at block (qx<<2, qy<<2, qz<<2), i.e. the min corner of the cell.
//  - Climate.Sampler.sample evaluates six noise-router density functions there (all return FLOAT in 26.x):
//      temperature     = router.temperature  (overworld/temperature)
//      humidity        = router.vegetation   (overworld/vegetation)
//      continentalness = router.continents   (overworld/continents)
//      erosion         = router.erosion      (overworld/erosion)
//      depth           = router.depth        (overworld/depth)
//      weirdness       = router.ridges       (overworld/ridges)
//    then Climate.target(...) = quantize_coord() of each float -> TargetPoint -> BiomeSource::find.
//  - Chunk biome fill (createResolverForChunk + ChunkAccess.fillBiomesFromNoise): quart volume
//    starting at (chunkMinX>>2, minY>>2, chunkMinZ>>2), size 4 x height/4 x 4, step 4 blocks.
//    Call order (matters for the per-thread last-result cache on ties):
//      for sectionY = minSection..maxSection: for x in 0..3: for y in 0..3: for z in 0..3:
//        find(sample(quartMinX+x, (sectionY<<2)+y, quartMinZ+z))
//    Vanilla samples the six functions for the whole volume first (sampleVolume) and then
//    resolves in this order; only the find() order affects the cache.
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace worldgen {

// Climate.quantizeCoord(float): (long)(coord * 10000.0F), with Java's saturating float->long cast.
inline int64_t quantize_coord(float coord) {
    float v = coord * 10000.0F;
    if (v != v) return 0;
    if (v >= 9223372036854775808.0F) return INT64_MAX;
    if (v < -9223372036854775808.0F) return INT64_MIN;
    return (int64_t)v;
}

struct TargetPoint { int64_t temperature, humidity, continentalness, erosion, depth, weirdness; };

inline TargetPoint target(float t, float h, float c, float e, float d, float w) {
    return {quantize_coord(t), quantize_coord(h), quantize_coord(c), quantize_coord(e), quantize_coord(d), quantize_coord(w)};
}

struct Parameter { int64_t min, max; };

// Climate.ParameterPoint; space[6] is the offset as a point parameter {offset, offset}.
struct ParameterPoint { Parameter space[7]; };

class BiomeSource {
public:
    BiomeSource();  // overworld preset (OverworldBiomeBuilder order), children per node 19
    explicit BiomeSource(std::vector<std::pair<ParameterPoint, int>> values, int childrenPerNode = 19);
    int find(const TargetPoint& target) const;             // ParameterList.findValue (RTree + thread-local lastResult)
    const std::vector<std::pair<ParameterPoint, int>>& values() const { return values_; }

private:
    struct Node { Parameter space[7]; std::vector<int> children; int value = -1; };  // leaf <=> children empty
    int build(std::vector<int> children);
    int search(int node, const int64_t* target, int candidate) const;
    std::vector<std::pair<ParameterPoint, int>> values_;
    std::vector<Node> nodes_;  // nodes_[i] for i < values_.size() are the leaves, in list order
    int root_, childrenPerNode_;
    uint64_t id_;
};

}  // namespace worldgen
