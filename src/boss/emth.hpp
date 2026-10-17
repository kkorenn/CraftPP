#pragma once
// Entity-side Mth helpers (net.minecraft.util.Mth): lookup-table sin/cos, fast atan2, wrapDegrees.
// Entities use these, not std::sin, so motion matches vanilla bit for bit.
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace emth {

inline const std::array<float, 65536>& sin_table() {
    static const auto t = [] {
        std::array<float, 65536> s{};
        for (int i = 0; i < 65536; i++) s[size_t(i)] = float(std::sin(i / 10430.378350470453));
        return s;
    }();
    return t;
}
inline float sin(double i) { return sin_table()[size_t(int64_t(i * 10430.378350470453) & 65535)]; }
inline float cos(double i) { return sin_table()[size_t(int64_t(i * 10430.378350470453 + 16384.0) & 65535)]; }
inline float sqrt(float x) { return float(std::sqrt(double(x))); }

inline float wrapDegrees(float a) {
    float n = std::fmod(a, 360.0f);
    if (n >= 180.0f) n -= 360.0f;
    if (n < -180.0f) n += 360.0f;
    return n;
}
inline double wrapDegrees(double a) {
    double n = std::fmod(a, 360.0);
    if (n >= 180.0) n -= 360.0;
    if (n < -180.0) n += 360.0;
    return n;
}
template <class T> inline T clamp(T v, T mn, T mx) { return v < mn ? mn : (v > mx ? mx : v); }

inline double atan2(double y, double x) {  // Mth.atan2 (table + fastInvSqrt, not libm)
    static const auto tabs = [] {
        std::array<std::array<double, 257>, 2> t{};
        for (int i = 0; i < 257; i++) {
            double a = std::asin(i / 256.0);
            t[0][size_t(i)] = a, t[1][size_t(i)] = std::cos(a);
        }
        return t;
    }();
    const double FRAC_BIAS = std::bit_cast<double>(int64_t(4805340802404319232LL));
    double d2 = x * x + y * y;
    if (std::isnan(d2)) return NAN;
    bool negY = y < 0.0;
    if (negY) y = -y;
    bool negX = x < 0.0;
    if (negX) x = -x;
    bool steep = y > x;
    if (steep) std::swap(x, y);
    double xhalf = 0.5 * d2;
    double r = std::bit_cast<double>(int64_t(6910469410427058090LL) - (std::bit_cast<int64_t>(d2) >> 1));
    r = r * (1.5 - xhalf * r * r);
    x *= r, y *= r;
    double yp = FRAC_BIAS + y;
    int idx = int(std::bit_cast<int64_t>(yp));
    double phi = tabs[0][size_t(idx)], cphi = tabs[1][size_t(idx)];
    double sphi = yp - FRAC_BIAS;
    double sd = y * cphi - x * sphi;
    double theta = phi + (6.0 + sd * sd) * sd * 0.16666666666666666;
    if (steep) theta = M_PI / 2 - theta;
    if (negX) theta = M_PI - theta;
    if (negY) theta = -theta;
    return theta;
}

}  // namespace emth
