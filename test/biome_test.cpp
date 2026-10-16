#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "gen/ids.hpp"
#include "worldgen/biome_source.hpp"

using namespace worldgen;

static TargetPoint center_of(const ParameterPoint& p) {
    auto c = [&](int d) { return (p.space[d].min + p.space[d].max) / 2; };
    return {c(0), c(1), c(2), c(3), c(4), c(5)};
}

int main() {
    BiomeSource src;
    assert(src.values().size() == 7594);
    for (auto& [p, b] : src.values()) {
        assert(b >= 0 && b < biomes::COUNT);
        for (auto& q : p.space) assert(q.min <= q.max);
    }
    // First entry: mushroom_fields, continentalness [-1.2,-1.05], depth 0.
    assert(src.values()[0].second == biomes::mushroom_fields);
    assert(src.values()[0].first.space[2].min == -12000 && src.values()[0].first.space[2].max == -10500);

    assert(quantize_coord(-0.455F) == -4550 && quantize_coord(0.0F) == 0 && quantize_coord(-1e30F) == INT64_MIN);
    assert(quantize_coord(0.00019999F) == 1 && quantize_coord(-0.00019999F) == -1);  // truncation toward zero

    // Hand-checkable: deep in the mushroom continentalness band at the surface.
    assert(src.find(target(0, 0, -1.1F, 0, 0, 0)) == biomes::mushroom_fields);
    assert(src.find(target(-0.9F, 0, -0.9F, 0, 0, 0)) == biomes::deep_frozen_ocean);

    // A point-center whose zero-distance match is unique must return that biome.
    int unique_checked = 0;
    for (auto& [p, b] : src.values()) {
        TargetPoint t = center_of(p);
        int64_t tv[7] = {t.temperature, t.humidity, t.continentalness, t.erosion, t.depth, t.weirdness, 0};
        int zero = 0, zb = -1;
        for (auto& [q, qb] : src.values()) {
            bool in = q.space[6].min == 0;
            for (int d = 0; d < 6 && in; d++) in = q.space[d].min <= tv[d] && tv[d] <= q.space[d].max;
            if (in) zero++, zb = qb;
        }
        if (zero == 1 && p.space[6].min == 0) {
            assert(zb == b && src.find(t) == b);
            unique_checked++;
        }
    }
    assert(unique_checked > 0);

    // Vanilla parity: same LCG target sequence fed to Climate.ParameterList.findValue on the real
    // 26.3 server jar (single thread, so lastResult cache state matches); FNV-1a over the ids.
    // Odd steps use a coarse 500-grid to force many equal-distance ties.
    int64_t s = 12345;
    uint32_t h = 0x811c9dc5;
    for (int i = 0; i < 200000; i++) {
        int64_t v[6];
        for (int d = 0; d < 6; d++) {
            s = (int64_t)((uint64_t)s * 6364136223846793005ULL + 1442695040888963407ULL);
            int r = (int)((uint64_t)s >> 33);
            v[d] = (i & 1) == 0 ? (r % 24001) - 12000 : ((r % 49) - 24) * 500LL;
        }
        uint32_t id = (uint32_t)src.find({v[0], v[1], v[2], v[3], v[4], v[5]});
        for (int k = 0; k < 4; k++) h = (h ^ ((id >> (8 * k)) & 0xff)) * 0x01000193u;
    }
    assert(h == 0x9e413aa8u);
    std::printf("biome_test OK: %zu entries, %d unique centers checked, vanilla parity hash matches\n",
                src.values().size(), unique_checked);
}
