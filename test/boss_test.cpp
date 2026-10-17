// Boss fight unit checks: seed-derived End spikes and gateways against java.util.Random-based references
// (SingleThreadedRandomSource is the same LCG), plus podium/gateway shapes.
#include "boss/end_features.hpp"
#include "boss/emth.hpp"
#include "boss/pattern.hpp"

#include <cassert>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <tuple>

static std::string describe(int64_t seed) {
    std::ostringstream o;
    o << seed << " spikes";
    for (auto& s : endgen::spikes_for_seed(seed)) o << ' ' << s.x << ',' << s.z << ',' << s.radius << ',' << s.height << ',' << s.guarded;
    o << " gateways";
    for (int g : endgen::gateways_for_seed(seed)) o << ' ' << g;
    return o.str();
}

int main() {
    // Expected lines come from scratch Java (Random-based port of SpikeCacheLoader / EnderDragonFight.init).
    const char* expected[] = {
        "0 spikes 42,0,4,94,0 33,24,3,91,0 12,39,2,79,1 -13,39,4,97,0 -34,24,3,85,0 -42,-1,5,103,0 -34,-25,2,76,0 "
        "-13,-40,2,82,1 12,-40,3,88,0 33,-25,4,100,0 gateways 6 1 15 17 4 18 12 11 14 7 19 3 16 9 8 10 2 13 5 0",
        "12345 spikes 42,0,4,97,0 33,24,5,103,0 12,39,3,91,0 -13,39,2,76,0 -34,24,4,94,0 -42,-1,4,100,0 -34,-25,2,79,1 "
        "-13,-40,3,88,0 12,-40,3,85,0 33,-25,2,82,1 gateways 12 5 19 16 14 8 2 3 6 7 10 18 17 9 4 13 0 15 1 11",
        "-4172144997902289642 spikes 42,0,4,94,0 33,24,3,88,0 12,39,2,82,1 -13,39,3,91,0 -34,24,2,79,1 -42,-1,5,103,0 "
        "-34,-25,2,76,0 -13,-40,4,100,0 12,-40,3,85,0 33,-25,4,97,0 gateways 17 5 8 1 4 0 6 7 14 19 10 16 2 12 15 9 11 3 18 13",
    };
    int64_t seeds[] = {0, 12345, -4172144997902289642LL};
    for (int i = 0; i < 3; i++) {
        std::string got = describe(seeds[i]);
        if (got != expected[i]) {
            fprintf(stderr, "spikes mismatch\n got %s\nwant %s\n", got.c_str(), expected[i]);
            return 1;
        }
    }

    endgen::States st{};
    st.air = 0, st.bedrock = 1, st.end_stone = 2, st.end_portal = 3, st.end_gateway = 4, st.obsidian = 5, st.fire = 6;
    st.wall_torch_n = st.wall_torch_e = st.wall_torch_s = st.wall_torch_w = 7;
    std::map<std::tuple<int, int, int>, int> w;
    auto get = [&](int x, int y, int z) { auto it = w.find({x, y, z}); return it == w.end() ? 0 : it->second; };
    auto set = [&](int x, int y, int z, int s) { w[{x, y, z}] = s; };
    endgen::place_podium(0, 60, 0, true, st, get, set, [](int, int, int) {});
    int portals = 0, rim = 0;
    for (auto& [k, v] : w) portals += v == 3, rim += v == 1 && std::get<1>(k) == 60;
    assert(portals == 20);  // 5x5 minus corners minus the pillar
    assert(rim == 1 + 16);  // pillar + ring at y=60
    assert(get(0, 63, 0) == 1 && get(0, 64, 0) == 0 && get(0, 62, -1) == 7);

    w.clear();
    endgen::place_gateway(10, 75, 10, st, set);
    assert(get(10, 75, 10) == 4 && get(10, 77, 10) == 1 && get(10, 73, 10) == 1 && get(11, 76, 10) == 1 && get(11, 76, 11) == 0);

    // Mth tables
    assert(emth::sin(0.0) == 0.0f && emth::cos(0.0) == 1.0f);
    assert(std::abs(emth::atan2(1.0, 1.0) - M_PI / 4) < 1e-6 && std::abs(emth::atan2(-1.0, -1.0) + 3 * M_PI / 4) < 1e-6);
    assert(emth::wrapDegrees(370.0f) == 10.0f && emth::wrapDegrees(-190.0f) == 170.0f);
    // Wither summoning pattern: skulls on a soul sand T, found from any skull, spawn cell (1, 2, 0) = T foot
    {
        enum { AIR, SAND, SKULL, STONE };
        std::map<std::tuple<int, int, int>, int> t;
        for (int x = -1; x <= 1; x++) t[{x, 62, 5}] = SKULL, t[{x, 61, 5}] = SAND;
        t[{0, 60, 5}] = SAND;
        auto get = [&](int x, int y, int z) { auto it = t.find({x, y, z}); return it == t.end() ? int(AIR) : it->second; };
        boss::BlockPattern wither{{{"^^^", "###", "~#~"}}, [](char c, int s) {
            return c == '^' ? s == SKULL : c == '#' ? s == SAND : c == '~' ? s == AIR : true;
        }};
        for (int sx = -1; sx <= 1; sx++) {
            auto m = wither.find(sx, 62, 5, get);
            assert(m && m->at(1, 2, 0) == (std::array<int, 3>{0, 60, 5}) && m->forwards_x() == false);
        }
        t[{1, 60, 5}] = STONE;  // '~' must be air
        assert(!wither.find(0, 62, 5, get));
    }
    printf("boss_test ok\n");
    return 0;
}
