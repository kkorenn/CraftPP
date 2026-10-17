#pragma once
// BlockPattern / BlockPatternBuilder: aisles of rows of chars, each char a block predicate, matched in every
// forwards/up orientation (wither summoning; reusable for golems). Pure: blocks come from a getter.
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace boss {

struct BlockPattern {
    std::vector<std::vector<std::string>> aisles;    // [depth][height] rows of width chars
    std::function<bool(char, int state)> test;        // ' ' etc. handled by the caller's predicate
    static constexpr int DIRS[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};  // Direction.values()

    struct Match {
        std::array<int, 3> origin;  // frontTopLeft
        int forwards, up;           // Direction ordinals (DOWN UP NORTH SOUTH WEST EAST)
        std::array<int, 3> at(int x, int y, int z) const { return translate(origin, forwards, up, x, y, z); }
        bool forwards_x() const { return forwards >= 4; }
    };
    int depth() const { return int(aisles.size()); }
    int height() const { return int(aisles[0].size()); }
    int width() const { return int(aisles[0][0].size()); }

    // BlockPattern.translateAndRotate: right = forwards x up
    static std::array<int, 3> translate(const std::array<int, 3>& o, int fw, int up, int right, int down, int fwd) {
        const int* f = DIRS[fw];
        const int* u = DIRS[up];
        int r[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
        return {o[0] + u[0] * -down + r[0] * right + f[0] * fwd, o[1] + u[1] * -down + r[1] * right + f[1] * fwd,
                o[2] + u[2] * -down + r[2] * right + f[2] * fwd};
    }
    template <class Get> bool matches(const std::array<int, 3>& o, int fw, int up, Get&& get) const {
        for (int x = 0; x < width(); x++)
            for (int y = 0; y < height(); y++)
                for (int z = 0; z < depth(); z++) {
                    auto p = translate(o, fw, up, x, y, z);
                    if (!test(aisles[size_t(z)][size_t(y)][size_t(x)], get(p[0], p[1], p[2]))) return false;
                }
        return true;
    }
    // BlockPattern.find: palm positions in the cube [pos, pos + size - 1] (x fastest), then every orientation.
    template <class Get> std::optional<Match> find(int px, int py, int pz, Get&& get) const {
        int d = std::max({width(), height(), depth()});
        for (int z = pz; z < pz + d; z++)
            for (int y = py; y < py + d; y++)
                for (int x = px; x < px + d; x++)
                    for (int fw = 0; fw < 6; fw++)
                        for (int up = 0; up < 6; up++) {
                            if (up == fw || (up ^ 1) == fw) continue;  // opposite directions differ in the low bit
                            if (matches({x, y, z}, fw, up, get)) return Match{{x, y, z}, fw, up};
                        }
        return std::nullopt;
    }
};

}  // namespace boss
