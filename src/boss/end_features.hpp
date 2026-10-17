#pragma once
// End fight structures: EndSpikeFeature (pillars, cages, crystals), EndPodiumFeature (exit portal),
// EndGatewayFeature, and the seed-derived spike / gateway lists. Pure functions over a block accessor so the
// End chunk generator (dimensions/features branches) and the runtime fight share them.
#include "worldgen/random.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace endgen {

struct EndSpike {
    int x, z, radius, height;
    bool guarded;
};

// IntArrayList Util.toShuffledList(IntStream.range(0, n), random)
template <size_t N> inline std::array<int, N> shuffled_range(RandomSource& r) {
    std::array<int, N> a{};
    for (size_t i = 0; i < N; i++) a[i] = int(i);
    for (int i = int(N); i > 1; i--) std::swap(a[size_t(i - 1)], a[size_t(r.nextInt(i))]);
    return a;
}

// EndSpikeFeature.getSpikesForLevel + SpikeCacheLoader.load.
inline std::array<EndSpike, 10> spikes_for_seed(int64_t seed) {
    SingleThreadedRandomSource r(seed);
    int64_t key = r.nextLong() & 65535;
    SingleThreadedRandomSource kr(key);
    auto sizes = shuffled_range<10>(kr);
    std::array<EndSpike, 10> out{};
    for (int i = 0; i < 10; i++) {
        int x = Mth::floor(42.0 * std::cos(2.0 * (-M_PI + (M_PI / 10) * i)));
        int z = Mth::floor(42.0 * std::sin(2.0 * (-M_PI + (M_PI / 10) * i)));
        int size = sizes[size_t(i)];
        out[size_t(i)] = {x, z, 2 + size / 3, 76 + size * 3, size == 1 || size == 2};
    }
    return out;
}

// EnderDragonFight.init: the 20 gateway slots, shuffled once per world; spawnNewGateway pops from the back.
inline std::vector<int> gateways_for_seed(int64_t seed) {
    SingleThreadedRandomSource r(seed);
    auto a = shuffled_range<20>(r);
    return {a.begin(), a.end()};
}
inline void gateway_pos(int slot, int& x, int& y, int& z) {
    x = Mth::floor(96.0 * std::cos(2.0 * (-M_PI + (M_PI / 20) * slot)));
    z = Mth::floor(96.0 * std::sin(2.0 * (-M_PI + (M_PI / 20) * slot)));
    y = 75;
}

// Crystals placed by the end_spike worldgen feature (worker threads); the boss tick adds the entities.
struct PendingCrystal { double x, y, z; float yaw; };
inline std::mutex pending_mtx;
inline std::vector<PendingCrystal> pending_crystals;

// Block states the features place (filled in from the generated tables by the caller; worldgen tests can
// leave them at 0).
struct States {
    int air, obsidian, bedrock, end_stone, end_portal, end_gateway, fire, wall_torch_n, wall_torch_e, wall_torch_s,
        wall_torch_w;
    int iron_bars[16];  // index: north | south << 1 | west << 2 | east << 3
};

// EndSpikeFeature.placeSpike: blocks only. Returns the crystal spawn (x+0.5, height+1, z+0.5); the caller
// spawns the EndCrystal (beam target / invulnerable) and should draw yaw = random.nextFloat() * 360 first.
template <class Set>
inline void place_spike(const EndSpike& s, const States& st, int min_y, Set&& set) {
    int r = s.radius;
    for (int z = s.z - r; z <= s.z + r; z++)
        for (int y = min_y; y <= s.height + 10; y++)
            for (int x = s.x - r; x <= s.x + r; x++) {
                double dx = x - s.x, dz = z - s.z;  // distToLowCornerSqr(cx, y, cz)
                if (dx * dx + dz * dz <= r * r + 1 && y < s.height) set(x, y, z, st.obsidian);
                else if (y > 65) set(x, y, z, st.air);
            }
    if (s.guarded)
        for (int dx = -2; dx <= 2; dx++)
            for (int dz = -2; dz <= 2; dz++)
                for (int dy = 0; dy <= 3; dy++) {
                    bool xs = std::abs(dx) == 2, zs = std::abs(dz) == 2, top = dy == 3;
                    if (!(xs || zs || top)) continue;
                    bool xe = dx == -2 || dx == 2 || top, ze = dz == -2 || dz == 2 || top;
                    int idx = (xe && dz != -2) | (xe && dz != 2) << 1 | (ze && dx != -2) << 2 | (ze && dx != 2) << 3;
                    set(s.x + dx, s.height + dy, s.z + dz, st.iron_bars[idx]);
                }
    set(s.x, s.height, s.z, st.bedrock);  // crystal blockPosition().below()
    set(s.x, s.height + 1, s.z, st.fire);
}

// EndPodiumFeature.place. `active` places the end portal; `drop` is called before replacing a block when
// active (dropPreviousAndSetBlock: destroyBlock with drops).
template <class Get, class Set, class Drop>
inline void place_podium(int ox, int oy, int oz, bool active, const States& st, Get&& get, Set&& set, Drop&& drop) {
    auto drop_set = [&](int x, int y, int z, int state) {
        if (get(x, y, z) != state) {
            drop(x, y, z);
            set(x, y, z, state);
        }
    };
    for (int z = oz - 4; z <= oz + 4; z++)
        for (int y = oy - 1; y <= oy + 32; y++)
            for (int x = ox - 4; x <= ox + 4; x++) {
                double d2 = double(x - ox) * (x - ox) + double(y - oy) * (y - oy) + double(z - oz) * (z - oz);
                bool inside_rim = d2 < 2.5 * 2.5;  // Vec3i.closerThan: distSqr < r * r
                if (!inside_rim && !(d2 < 3.5 * 3.5)) continue;
                if (y < oy) {
                    if (inside_rim) set(x, y, z, st.bedrock);
                    else if (active) drop_set(x, y, z, st.end_stone);
                    else set(x, y, z, st.end_stone);
                } else if (y > oy) {
                    if (active) drop_set(x, y, z, st.air);
                    else set(x, y, z, st.air);
                } else if (!inside_rim) set(x, y, z, st.bedrock);
                else if (active) drop_set(x, y, z, st.end_portal);
                else set(x, y, z, st.air);
            }
    for (int y = 0; y < 4; y++) set(ox, oy + y, oz, st.bedrock);
    set(ox, oy + 2, oz - 1, st.wall_torch_n);  // Direction.Plane.HORIZONTAL: north, east, south, west
    set(ox + 1, oy + 2, oz, st.wall_torch_e);
    set(ox, oy + 2, oz + 1, st.wall_torch_s);
    set(ox - 1, oy + 2, oz, st.wall_torch_w);
}

// EndGatewayFeature.place (the block entity's exit is the caller's business).
template <class Set> inline void place_gateway(int ox, int oy, int oz, const States& st, Set&& set) {
    for (int z = oz - 1; z <= oz + 1; z++)
        for (int y = oy - 2; y <= oy + 2; y++)
            for (int x = ox - 1; x <= ox + 1; x++) {
                bool sx = x == ox, sy = y == oy, sz = z == oz, end = std::abs(y - oy) == 2;
                if (sx && sy && sz) set(x, y, z, st.end_gateway);
                else if (sy) set(x, y, z, st.air);
                else if (end && sx && sz) set(x, y, z, st.bedrock);
                else if ((sx || sz) && !end) set(x, y, z, st.bedrock);
                else set(x, y, z, st.air);
            }
}

}  // namespace endgen
