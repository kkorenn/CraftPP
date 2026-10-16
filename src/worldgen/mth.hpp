// Port of the net.minecraft.util.Mth helpers used by worldgen, with Java numeric semantics.
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace Mth {

// Java (int)/(long) casts of double: truncate toward zero, saturate, NaN -> 0.
inline int32_t d2i(double v) {
  if (v != v) return 0;
  if (v >= 2147483647.0) return std::numeric_limits<int32_t>::max();
  if (v <= -2147483648.0) return std::numeric_limits<int32_t>::min();
  return (int32_t)v;
}
inline int64_t d2l(double v) {
  if (v != v) return 0;
  if (v >= 9223372036854775808.0) return std::numeric_limits<int64_t>::max();
  if (v <= -9223372036854775808.0) return std::numeric_limits<int64_t>::min();
  return (int64_t)v;
}

inline int32_t floor(double v) { return d2i(std::floor(v)); }
inline int32_t floor(float v) { return d2i(std::floor((double)v)); }
inline int64_t lfloor(double v) { return d2l(std::floor(v)); }

inline float smoothstep(float x) { return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f); }
inline float smoothstepDerivative(float x) { return 30.0f * x * x * (x - 1.0f) * (x - 1.0f); }

inline float lerp(float a, float p0, float p1) { return p0 + a * (p1 - p0); }
inline double lerp(double a, double p0, double p1) { return p0 + a * (p1 - p0); }
inline float lerp2(float a1, float a2, float x00, float x10, float x01, float x11) {
  return lerp(a2, lerp(a1, x00, x10), lerp(a1, x01, x11));
}
inline double lerp2(double a1, double a2, double x00, double x10, double x01, double x11) {
  return lerp(a2, lerp(a1, x00, x10), lerp(a1, x01, x11));
}
inline float lerp3(float a1, float a2, float a3, float x000, float x100, float x010, float x110,
                   float x001, float x101, float x011, float x111) {
  return lerp(a3, lerp2(a1, a2, x000, x100, x010, x110), lerp2(a1, a2, x001, x101, x011, x111));
}
inline double lerp3(double a1, double a2, double a3, double x000, double x100, double x010, double x110,
                    double x001, double x101, double x011, double x111) {
  return lerp(a3, lerp2(a1, a2, x000, x100, x010, x110), lerp2(a1, a2, x001, x101, x011, x111));
}

inline double square(double x) { return x * x; }
inline float square(float x) { return x * x; }

// Java: value < min ? min : Math.min(value, max)  (NaN propagates like Math.min)
inline float clamp(float v, float mn, float mx) { return v < mn ? mn : (v > mx ? mx : v); }
inline double clamp(double v, double mn, double mx) { return v < mn ? mn : (v > mx ? mx : v); }

// long seed = x * 3129871 ^ z * 116129781L ^ y;  (x * 3129871 is an int multiply)
inline int64_t getSeed(int32_t x, int32_t y, int32_t z) {
  int64_t xs = (int32_t)((uint32_t)x * 3129871u);
  uint64_t seed = (uint64_t)(xs ^ (int64_t)((uint64_t)(int64_t)z * 116129781ull) ^ (int64_t)y);
  seed = seed * seed * 42317861ull + seed * 11ull;
  return (int64_t)seed >> 16;
}

}  // namespace Mth
