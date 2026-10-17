// Value providers (net.minecraft.util.valueproviders), height providers + vertical anchors
// (levelgen/heightproviders, VerticalAnchor), parsed from worldgen JSON. Bit-exact sampling order.
#pragma once
#include <algorithm>
#include <climits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "worldgen/json.hpp"
#include "worldgen/random.hpp"

namespace worldgen {

// WorldGenerationContext of the dimension being generated (minGenY, genDepth) + its sea level. Pipeline::run_step
// sets it on the generating thread (thread_local: worker threads generate several dimensions); default overworld.
struct GenContext { int min_y = -64, depth = 384, sea_level = 63; bool skylight = true; };  // + DimensionType.hasSkyLight
inline thread_local GenContext gen_context;
#define GEN_MIN_Y (::worldgen::gen_context.min_y)
#define GEN_DEPTH (::worldgen::gen_context.depth)
#define GEN_SEA_LEVEL (::worldgen::gen_context.sea_level)

inline std::string strip_ns(const std::string& s) { return s.rfind("minecraft:", 0) == 0 ? s.substr(10) : s; }
// Biome JSON path: a datapack override (<pack>/data/minecraft/worldgen/biome/<name>.json) wins over vanilla.
std::string biome_file(const std::string& root, const std::string& pack, const std::string& name);

namespace mth {
inline int randomBetweenInclusive(RandomSource& r, int min, int max) { return r.nextInt(max - min + 1) + min; }
inline float randomBetween(RandomSource& r, float min, float max) { return r.nextFloat() * (max - min) + min; }
inline int nextInt(RandomSource& r, int min, int max) { return min >= max ? min : r.nextInt(max - min + 1) + min; }
inline float normal(RandomSource& r, float mean, float dev) { return mean + (float)r.nextGaussian() * dev; }
template <class T>  // Mth.clampedMap (float and double overloads)
T clampedMap(T v, T fromMin, T fromMax, T toMin, T toMax) {
  T f = (v - fromMin) / (fromMax - fromMin);
  return f < T(0) ? toMin : f > T(1) ? toMax : toMin + f * (toMax - toMin);
}
}  // namespace mth

struct IntProvider {
  enum T { CONSTANT, UNIFORM, BIASED_TO_BOTTOM, VERY_BIASED_TO_BOTTOM, CLAMPED, CLAMPED_NORMAL, TRAPEZOID, WEIGHTED_LIST } t = CONSTANT;
  int a = 0, b = 0, c = 0;  // value | min,max | min,max,plateau
  float mean = 0, dev = 0;
  std::vector<IntProvider> kids;  // clamped source / weighted entries
  std::vector<int> weights;
  int total = 0;

  IntProvider() = default;
  explicit IntProvider(int v) : a(v) {}
  explicit IntProvider(const Json& j) {
    if (j.isNumber()) { a = j.asInt(); return; }
    std::string ty = strip_ns(j["type"].asString());
    if (ty == "constant") a = j["value"].asInt();
    else if (ty == "uniform" || ty == "biased_to_bottom" || ty == "very_biased_to_bottom") {
      t = ty == "uniform" ? UNIFORM : ty == "biased_to_bottom" ? BIASED_TO_BOTTOM : VERY_BIASED_TO_BOTTOM;
      a = j["min_inclusive"].asInt(), b = j["max_inclusive"].asInt();
    } else if (ty == "clamped") {
      t = CLAMPED, a = j["min_inclusive"].asInt(), b = j["max_inclusive"].asInt();
      kids.emplace_back(j["source"]);
    } else if (ty == "clamped_normal") {
      t = CLAMPED_NORMAL, mean = j["mean"].asFloat(), dev = j["deviation"].asFloat();
      a = j["min_inclusive"].asInt(), b = j["max_inclusive"].asInt();
    } else if (ty == "trapezoid") {
      t = TRAPEZOID, a = j["min"].asInt(), b = j["max"].asInt(), c = j["plateau"].asInt();
    } else if (ty == "weighted_list") {
      t = WEIGHTED_LIST;
      for (auto& e : j["distribution"].arr) kids.emplace_back(e["data"]), weights.push_back(e["weight"].asInt()), total += weights.back();
    } else throw std::runtime_error("int provider " + ty);
  }
  int sample(RandomSource& r) const {
    switch (t) {
      case CONSTANT: return a;
      case UNIFORM: return mth::randomBetweenInclusive(r, a, b);
      case BIASED_TO_BOTTOM: return a + r.nextInt(r.nextInt(b - a + 1) + 1);
      case VERY_BIASED_TO_BOTTOM: return a + r.nextInt(r.nextInt(r.nextInt(b - a + 1) + 1) + 1);
      case CLAMPED: return std::clamp(kids[0].sample(r), a, b);
      case CLAMPED_NORMAL: return (int)Mth::clamp(mth::normal(r, mean, dev), (float)a, (float)b);
      case TRAPEZOID: {
        if (c == 0 && b == -a) { int v1 = r.nextInt(b + 1); return v1 - r.nextInt(b + 1); }
        int range = b - a;
        if (c == range) return mth::randomBetweenInclusive(r, a, b);
        int ps = (range - c) / 2, pe = range - ps;
        int v1 = mth::randomBetweenInclusive(r, 0, pe);
        return a + v1 + mth::randomBetweenInclusive(r, 0, ps);
      }
      case WEIGHTED_LIST: return pick(r).sample(r);
    }
    return 0;
  }
  const IntProvider& pick(RandomSource& r) const {
    int sel = r.nextInt(total);
    for (size_t i = 0; i < kids.size(); i++)
      if ((sel -= weights[i]) < 0) return kids[i];
    return kids.back();
  }
  int minValue() const {
    switch (t) {
      case CONSTANT: return a;
      case CLAMPED: return std::max(a, kids[0].minValue());
      case WEIGHTED_LIST: { int m = INT32_MAX; for (auto& k : kids) m = std::min(m, k.minValue()); return m; }
      default: return a;
    }
  }
  int maxValue() const {
    switch (t) {
      case CONSTANT: return a;
      case CLAMPED: return std::min(b, kids[0].maxValue());
      case WEIGHTED_LIST: { int m = INT32_MIN; for (auto& k : kids) m = std::max(m, k.maxValue()); return m; }
      default: return b;
    }
  }
};

struct FloatProvider {
  enum T { CONSTANT, UNIFORM, CLAMPED_NORMAL, TRAPEZOID } t = CONSTANT;
  float a = 0, b = 0, c = 0, d = 0;
  FloatProvider() = default;
  explicit FloatProvider(float v) : a(v) {}
  explicit FloatProvider(const Json& j) {
    if (j.isNumber()) { a = j.asFloat(); return; }
    std::string ty = strip_ns(j["type"].asString());
    if (ty == "constant") a = j["value"].asFloat();
    else if (ty == "uniform") t = UNIFORM, a = j["min_inclusive"].asFloat(), b = j["max_exclusive"].asFloat();
    else if (ty == "clamped_normal")
      t = CLAMPED_NORMAL, a = j["mean"].asFloat(), b = j["deviation"].asFloat(), c = j["min"].asFloat(), d = j["max"].asFloat();
    else if (ty == "trapezoid") t = TRAPEZOID, a = j["min"].asFloat(), b = j["max"].asFloat(), c = j["plateau"].asFloat();
    else throw std::runtime_error("float provider " + ty);
  }
  float sample(RandomSource& r) const {
    switch (t) {
      case CONSTANT: return a;
      case UNIFORM: return mth::randomBetween(r, a, b);
      case CLAMPED_NORMAL: return Mth::clamp(mth::normal(r, a, b), c, d);
      case TRAPEZOID: {
        float range = b - a, ps = (range - c) / 2.0F, pe = range - ps;
        float v1 = r.nextFloat() * pe;
        return a + v1 + r.nextFloat() * ps;
      }
    }
    return 0;
  }
};

struct VerticalAnchor {
  enum T { ABSOLUTE, ABOVE_BOTTOM, BELOW_TOP, SEA_LEVEL } t = ABSOLUTE;
  int v = 0;
  VerticalAnchor() = default;
  explicit VerticalAnchor(const Json& j) {
    if (auto* x = j.find("absolute")) t = ABSOLUTE, v = x->asInt();
    else if (auto* y = j.find("above_bottom")) t = ABOVE_BOTTOM, v = y->asInt();
    else if (auto* z = j.find("below_top")) t = BELOW_TOP, v = z->asInt();
    else if (auto* w = j.find("relative_to_sea_level")) t = SEA_LEVEL, v = w->asInt();
    else throw std::runtime_error("vertical anchor");
  }
  int resolve() const {
    switch (t) {
      case ABSOLUTE: return v;
      case ABOVE_BOTTOM: return GEN_MIN_Y + v;
      case BELOW_TOP: return GEN_DEPTH - 1 + GEN_MIN_Y - v;
      case SEA_LEVEL: return GEN_SEA_LEVEL + v;
    }
    return 0;
  }
};

struct HeightProvider {
  enum T { CONSTANT, UNIFORM, BIASED_TO_BOTTOM, VERY_BIASED_TO_BOTTOM, TRAPEZOID, WEIGHTED_LIST } t = CONSTANT;
  VerticalAnchor min, max;
  int inner = 1;  // biased inner / trapezoid plateau
  std::vector<HeightProvider> kids;
  std::vector<int> weights;
  int total = 0;
  HeightProvider() = default;
  explicit HeightProvider(const Json& j) {
    if (!j.has("type")) { min = VerticalAnchor(j); return; }  // bare anchor = constant
    std::string ty = strip_ns(j["type"].asString());
    if (ty == "constant") { min = VerticalAnchor(j["value"]); return; }
    if (ty == "weighted_list") {
      t = WEIGHTED_LIST;
      for (auto& e : j["distribution"].arr) kids.emplace_back(e["data"]), weights.push_back(e["weight"].asInt()), total += weights.back();
      return;
    }
    min = VerticalAnchor(j["min_inclusive"]), max = VerticalAnchor(j["max_inclusive"]);
    if (ty == "uniform") t = UNIFORM;
    else if (ty == "biased_to_bottom") t = BIASED_TO_BOTTOM, inner = j.getInt("inner", 1);
    else if (ty == "very_biased_to_bottom") t = VERY_BIASED_TO_BOTTOM, inner = j.getInt("inner", 1);
    else if (ty == "trapezoid") t = TRAPEZOID, inner = j.getInt("plateau", 0);
    else throw std::runtime_error("height provider " + ty);
  }
  int sample(RandomSource& r) const {
    int lo = min.resolve(), hi = max.resolve();
    switch (t) {
      case CONSTANT: return lo;
      case UNIFORM: return lo > hi ? lo : mth::randomBetweenInclusive(r, lo, hi);
      case BIASED_TO_BOTTOM: {
        if (hi - lo - inner + 1 <= 0) return lo;
        int limit = r.nextInt(hi - lo - inner + 1);
        return r.nextInt(limit + inner) + lo;
      }
      case VERY_BIASED_TO_BOTTOM: {
        if (hi - lo - inner + 1 <= 0) return lo;
        int up = mth::nextInt(r, lo + inner, hi);
        int biased = mth::nextInt(r, lo, up - 1);
        return mth::nextInt(r, lo, biased - 1 + inner);
      }
      case TRAPEZOID: {
        if (lo > hi) return lo;
        int range = hi - lo;
        if (inner >= range) return mth::randomBetweenInclusive(r, lo, hi);
        int ps = (range - inner) / 2, pe = range - ps;
        int v1 = mth::randomBetweenInclusive(r, 0, pe);
        return lo + v1 + mth::randomBetweenInclusive(r, 0, ps);
      }
      case WEIGHTED_LIST: {
        int sel = r.nextInt(total);
        for (size_t i = 0; i < kids.size(); i++)
          if ((sel -= weights[i]) < 0) return kids[i].sample(r);
        return kids.back().sample(r);
      }
    }
    return 0;
  }
};

}  // namespace worldgen
