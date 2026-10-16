#include "worldgen/density.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace df {
namespace {

constexpr float INF = std::numeric_limits<float>::infinity();

// ---------------------------------------------------------------- Java float semantics
float jmin(float a, float b) {
  if (a != a) return a;
  if (a == 0.0f && b == 0.0f && std::signbit(b)) return b;
  return a <= b ? a : b;
}
float jmax(float a, float b) {
  if (a != a) return a;
  if (a == 0.0f && b == 0.0f && std::signbit(a)) return b;
  return a >= b ? a : b;
}
float clampf(float v, float mn, float mx) { return v < mn ? mn : jmin(v, mx); }  // Mth.clamp
float signum(float v) { return v != v || v == 0.0f ? v : (v > 0 ? 1.0f : -1.0f); }
int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
int floorMod(int a, int b) { return a - floorDiv(a, b) * b; }
uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
uint64_t bits(double f) { uint64_t u; std::memcpy(&u, &f, 8); return u; }

// ---------------------------------------------------------------- Interval
const Iv NaI{NAN, NAN};
Iv of(float a, float b) { return {a, b}; }
Iv exact(float v) { return {v, v}; }
bool contains(Iv i, float v) { return v >= i.min && v <= i.max; }
Iv encapsulating(const std::vector<Iv>& list) {
  float mn = INF, mx = -INF;
  for (auto& i : list)
    if (!i.isNaI()) mn = jmin(i.min, mn), mx = jmax(i.max, mx);
  return mx < mn ? NaI : of(mn, mx);
}
Iv encapsulating(float a, float b) {
  if (std::isnan(a) && std::isnan(b)) return NaI;
  if (std::isnan(a)) return exact(b);
  if (std::isnan(b)) return exact(a);
  return of(jmin(a, b), jmax(a, b));
}
Iv ivAdd(Iv l, Iv r) {
  float mn = l.min + r.min, mx = l.max + r.max;
  return std::isnan(mn) || std::isnan(mx) ? NaI : of(mn, mx);
}
Iv ivSub(Iv l, Iv r) {
  float mn = l.min - r.max, mx = l.max - r.min;
  return std::isnan(mn) || std::isnan(mx) ? NaI : of(mn, mx);
}
float mulBound(float l, float r) { return l != 0.0f && r != 0.0f ? l * r : 0.0f; }
Iv ivMul(Iv l, Iv r) {
  if (l.isNaI() || r.isNaI()) return NaI;
  float a = mulBound(l.min, r.min), b = mulBound(l.min, r.max), c = mulBound(l.max, r.min), d = mulBound(l.max, r.max);
  return of(jmin(jmin(a, b), jmin(c, d)), jmax(jmax(a, b), jmax(c, d)));
}
Iv ivReciprocal(Iv i) {
  if (i.isNaI() || (i.min == 0.0f && i.max == 0.0f)) return NaI;
  if (!contains(i, 0.0f)) return of(1.0f / i.max, 1.0f / i.min);
  if (i.max == 0.0f) return of(-INF, 1.0f / i.min);
  return i.min == 0.0f ? of(1.0f / i.max, INF) : of(-INF, INF);
}
Iv ivMin(Iv l, Iv r) { return l.isNaI() || r.isNaI() ? NaI : of(jmin(l.min, r.min), jmin(l.max, r.max)); }
Iv ivMax(Iv l, Iv r) { return l.isNaI() || r.isNaI() ? NaI : of(jmax(l.min, r.min), jmax(l.max, r.max)); }
Iv ivClamp(Iv i, float mn, float mx) {
  if (i.isNaI()) return NaI;
  if (i.min >= mx) return of(mx, mx);
  return i.max <= mn ? of(mn, mn) : of(jmax(i.min, mn), jmin(i.max, mx));
}
template <class F>
Iv mapMonotonic(Iv i, F fn) {
  if (i.isNaI()) return NaI;
  float a = fn(i.min), b = fn(i.max);
  return of(jmin(a, b), jmax(a, b));
}
Iv powExact(float base, Iv e) {  // Interval.pow(float, Interval), exact-exponent branch (only sqrt uses pow)
  if (std::isnan(base) || e.isNaI()) return NaI;
  if (e.min != e.max) throw std::runtime_error("df: interval pow with non-exact exponent not ported");
  float v = (float)std::pow((double)base, (double)e.min);
  return std::isnan(v) ? NaI : exact(v);
}
Iv ivPow(Iv b, Iv e) {
  if (b.isNaI() || e.isNaI()) return NaI;
  if (b.min == b.max) return powExact(b.min, e);
  Iv r = encapsulating({powExact(b.min, e), powExact(b.max, e)});
  if (contains(b, 0.0f)) {
    if (b.max > 0.0f) r = encapsulating({r, powExact(0.0f, e)});
    if (b.min < 0.0f) r = encapsulating({r, powExact(-0.0f, e)});
  }
  return r;
}
float lerpInfiniteBound(float a, float first, float second) {
  float p1 = mulBound(1.0f - a, first), p2 = mulBound(a, second);
  if (!std::isinf(p1) || !std::isinf(p2)) return p1 + p2;
  if (a <= 0.0f) return second > first ? -INF : INF;
  if (a >= 1.0f) return second > first ? INF : -INF;
  return NAN;
}
Iv ivLerp(Iv alpha, float first, float second) {
  if (alpha.isNaI() || std::isnan(first) || std::isnan(second)) return NaI;
  if (std::isfinite(first) && std::isfinite(second))
    return encapsulating(first + mulBound(alpha.min, second - first), first + mulBound(alpha.max, second - first));
  if (first == second) return exact(first);
  float mn = lerpInfiniteBound(alpha.min, first, second), mx = lerpInfiniteBound(alpha.max, first, second);
  return std::isnan(mn) || std::isnan(mx) ? NaI : encapsulating(mn, mx);
}
Iv ivLerp(Iv a, Iv f, Iv s) {
  if (a.isNaI() || f.isNaI() || s.isNaI()) return NaI;
  return encapsulating({ivLerp(a, f.min, s.min), ivLerp(a, f.max, s.min), ivLerp(a, f.min, s.max), ivLerp(a, f.max, s.max)});
}

float leakyRelu(float k, float v) { return v > 0.0f ? v : v * k; }
float squeeze(float v) {
  float c = clampf(v, -1.0f, 1.0f);
  return c / 2.0f - c * c * c / 24.0f;
}
inline float applyUnary(int op, float v) {
  switch (op) {
    case ABS: return std::fabs(v);
    case SQUARE: return v * v;
    case CUBE: return v * v * v;
    case SQRT: return (float)std::sqrt((double)v);
    case HALF_NEGATIVE: return leakyRelu(0.5f, v);
    case QUARTER_NEGATIVE: return leakyRelu(0.25f, v);
    case RECIPROCAL: return 1.0f / v;
    case NEGATE: return -v;
    case SQUEEZE: return squeeze(v);
    case LOG: return (float)std::log((double)v);
    default: return signum(v);
  }
}

// ---------------------------------------------------------------- DensityVolume helpers
bool operator==(const DensityVolume& a, const DensityVolume& b) {
  return a.sizeX == b.sizeX && a.sizeY == b.sizeY && a.sizeZ == b.sizeZ && a.minBlockX == b.minBlockX &&
         a.minBlockY == b.minBlockY && a.minBlockZ == b.minBlockZ && a.stepBlockX == b.stepBlockX &&
         a.stepBlockY == b.stepBlockY && a.stepBlockZ == b.stepBlockZ;
}
int idx(const DensityVolume& v, int x, int y, int z) { return y + (x + z * v.sizeX) * v.sizeY; }
int maxX(const DensityVolume& v) { return v.minBlockX + v.sizeX * v.stepBlockX - 1; }
int maxY(const DensityVolume& v) { return v.minBlockY + v.sizeY * v.stepBlockY - 1; }
int maxZ(const DensityVolume& v) { return v.minBlockZ + v.sizeZ * v.stepBlockZ - 1; }
int indexOfBlock(const DensityVolume& v, int bx, int by, int bz) {
  int rx = bx - v.minBlockX, ry = by - v.minBlockY, rz = bz - v.minBlockZ;
  if (v.stepBlockX == 1 && v.stepBlockY == 1 && v.stepBlockZ == 1) {
    if (rx >= 0 && ry >= 0 && rz >= 0 && rx < v.sizeX && ry < v.sizeY && rz < v.sizeZ) return idx(v, rx, ry, rz);
  } else if (rx >= 0 && ry >= 0 && rz >= 0 && rx < v.sizeX * v.stepBlockX && ry < v.sizeY * v.stepBlockY &&
             rz < v.sizeZ * v.stepBlockZ && floorMod(rx, v.stepBlockX) == 0 && floorMod(ry, v.stepBlockY) == 0 &&
             floorMod(rz, v.stepBlockZ) == 0) {
    return idx(v, floorDiv(rx, v.stepBlockX), floorDiv(ry, v.stepBlockY), floorDiv(rz, v.stepBlockZ));
  }
  return -1;
}
void setRange(DensityBuffer& b, int i, int n, float v) { std::fill(b.values.begin() + i, b.values.begin() + i + n, v); }
int64_t blockPosAsLong(int x, int y, int z) {
  return (int64_t)((((uint64_t)x & 0x3FFFFFFull) << 38) | ((uint64_t)y & 0xFFFull) | (((uint64_t)z & 0x3FFFFFFull) << 12));
}

// ---------------------------------------------------------------- samplers
// Per-thread scratch buffer pool (vanilla DensityBufferPool): bounded so idle threads hold at most ~2 MB.
constexpr size_t POOL_MAX_FLOATS = 512 * 1024;
struct ScratchPool {
  std::vector<std::vector<float>> free;
  size_t floats = 0;
};
thread_local ScratchPool scratch;
struct PBuf : DensityBuffer {
  explicit PBuf(int n) : DensityBuffer(0) {
    auto& f = scratch.free;
    for (size_t i = f.size(); i-- > 0;)
      if ((int)f[i].capacity() >= n) {
        values = std::move(f[i]);
        scratch.floats -= values.capacity();
        f.erase(f.begin() + i);
        break;
      }
    values.resize(n);
  }
  PBuf(PBuf&&) = default;
  ~PBuf() {
    size_t cap = values.capacity();
    if (cap == 0 || scratch.floats + cap > POOL_MAX_FLOATS) return;
    scratch.floats += cap;
    scratch.free.push_back(std::move(values));
  }
};

using S = SamplerP;
using Ctx = SamplerContext;
using Buf = DensityBuffer;
using Vol = DensityVolume;

struct ConstS : Sampler {
  float v;
  explicit ConstS(float v) : v(v) {}
  void sampleVolume(Ctx&, Buf& o, const Vol&) const override { o.fill(v); }
  float sampleValue(Ctx&, int, int, int) const override { return v; }
};

struct NoiseS : Sampler {  // NoiseFunction.Sampler
  std::shared_ptr<const Noise> noise;
  double xz, y;
  NoiseS(std::shared_ptr<const Noise> n, double xz, double y) : noise(std::move(n)), xz(xz), y(y) {}
  void sampleVolume(Ctx&, Buf& o, const Vol& v) const override {
    o.fill(0.0f);
    noise->addToVolume(o, v, xz, y, 1.0f);
  }
  float sampleValue(Ctx&, int bx, int by, int bz) const override { return noise->get(bx * xz, by * y, bz * xz); }
};

struct ShiftedS : Sampler {  // ShiftedXzSampler / ShiftedXyzSampler (sy null for Xz)
  S sx, sy, sz;
  std::shared_ptr<const Noise> noise;
  double xz, y;
  ShiftedS(S sx, S sy, S sz, std::shared_ptr<const Noise> n, double xz, double y)
      : sx(std::move(sx)), sy(std::move(sy)), sz(std::move(sz)), noise(std::move(n)), xz(xz), y(y) {}
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    sx->sampleVolume(c, o, v);
    PBuf by(sy ? v.size() : 0), bz(v.size());
    if (sy) sy->sampleVolume(c, by, v);
    sz->sampleVolume(c, bz, v);
    int i = 0;
    for (int z = 0; z < v.sizeZ; z++) {
      double baseZ = v.blockZ(z) * xz;
      for (int x = 0; x < v.sizeX; x++) {
        double baseX = v.blockX(x) * xz;
        for (int yy = 0; yy < v.sizeY; yy++, i++) {
          double nx = baseX + o.get(i);
          double ny = sy ? v.blockY(yy) * y + by.get(i) : v.blockY(yy) * y;
          double nz = baseZ + bz.get(i);
          o.set(i, noise->get(nx, ny, nz));
        }
      }
    }
  }
  float sampleValue(Ctx& c, int bx, int by, int bz) const override {
    double x = bx * xz + sx->sampleValue(c, bx, by, bz);
    double yy = sy ? by * y + sy->sampleValue(c, bx, by, bz) : by * y;
    double z = bz * xz + sz->sampleValue(c, bx, by, bz);
    return noise->get(x, yy, z);
  }
};

struct ShiftBS : Sampler {
  std::shared_ptr<const Noise> noise;
  explicit ShiftBS(std::shared_ptr<const Noise> n) : noise(std::move(n)) {}
  void sampleVolume(Ctx&, Buf& o, const Vol& v) const override {
    Vol t{v.sizeZ, v.sizeX, 1, v.minBlockZ, v.minBlockX, 0, v.stepBlockZ, v.stepBlockX, 1};
    PBuf tb(t.size());
    tb.fill(0.0f);
    noise->addToVolume(tb, t, 0.25, 0.25, 4.0f);
    for (int z = 0; z < v.sizeZ; z++)
      for (int x = 0; x < v.sizeX; x++) setRange(o, idx(v, x, 0, z), v.sizeY, tb.get(idx(t, z, x, 0)));
  }
  float sampleValue(Ctx&, int bx, int, int bz) const override { return noise->get(bz * 0.25, bx * 0.25, 0.0) * 4.0f; }
};

struct GradientS : Sampler {
  int axis, tiling, from, minC, maxC, range;
  float fromValue, factor;
  float compute(int c) const {
    if (tiling == 0) return fromValue + (float)(std::clamp(c, minC, maxC) - from) * factor;
    int rel = c - from;
    if (tiling == 1) return fromValue + (float)floorMod(rel, range) * factor;
    int tile = floorDiv(rel, range), local = rel - tile * range;
    return (tile & 1) == 0 ? fromValue + (float)local * factor : fromValue + (float)(range - local) * factor;
  }
  void sampleVolume(Ctx&, Buf& o, const Vol& v) const override {
    if (axis == 0) {
      for (int x = 0; x < v.sizeX; x++) {
        float val = compute(v.blockX(x));
        for (int z = 0; z < v.sizeZ; z++) setRange(o, idx(v, x, 0, z), v.sizeY, val);
      }
    } else if (axis == 1) {
      for (int y = 0; y < v.sizeY; y++) {
        float val = compute(v.blockY(y));
        for (int z = 0; z < v.sizeZ; z++)
          for (int x = 0; x < v.sizeX; x++) o.set(idx(v, x, y, z), val);
      }
    } else {
      for (int z = 0; z < v.sizeZ; z++) setRange(o, idx(v, 0, 0, z), v.sizeX * v.sizeY, compute(v.blockZ(z)));
    }
  }
  float sampleValue(Ctx&, int x, int y, int z) const override { return compute(axis == 0 ? x : axis == 1 ? y : z); }
};

struct UnaryS : Sampler {
  int op;
  S in;
  UnaryS(int op, S in) : op(op), in(std::move(in)) {}
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    in->sampleVolume(c, o, v);
    float* a = o.values.data();
    const int n = o.size();
    auto loop = [&](auto opc) { for (int i = 0; i < n; i++) a[i] = applyUnary(decltype(opc)::value, a[i]); };
    switch (op) {  // op as a compile-time constant: the switch leaves the loop
      case ABS: loop(std::integral_constant<int, ABS>{}); break;
      case SQUARE: loop(std::integral_constant<int, SQUARE>{}); break;
      case CUBE: loop(std::integral_constant<int, CUBE>{}); break;
      case SQRT: loop(std::integral_constant<int, SQRT>{}); break;
      case HALF_NEGATIVE: loop(std::integral_constant<int, HALF_NEGATIVE>{}); break;
      case QUARTER_NEGATIVE: loop(std::integral_constant<int, QUARTER_NEGATIVE>{}); break;
      case RECIPROCAL: loop(std::integral_constant<int, RECIPROCAL>{}); break;
      case NEGATE: loop(std::integral_constant<int, NEGATE>{}); break;
      case SQUEEZE: loop(std::integral_constant<int, SQUEEZE>{}); break;
      default: for (int i = 0; i < n; i++) a[i] = applyUnary(op, a[i]); break;
    }
  }
  float sampleValue(Ctx& c, int x, int y, int z) const override { return applyUnary(op, in->sampleValue(c, x, y, z)); }
};

// BinaryFunction samplers. kind: ADD SUB MUL DIV MIN MAX with two samplers; constant forms below.
struct BinS : Sampler {
  int op;
  S l, r;
  float rightBound;  // MinSampler.rightMinValue / MaxSampler.rightMaxValue
  BinS(int op, S l, S r, float rb = 0) : op(op), l(std::move(l)), r(std::move(r)), rightBound(rb) {}
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    l->sampleVolume(c, o, v);
    PBuf rb(v.size());
    r->sampleVolume(c, rb, v);
    // One loop per op (vectorizable); element-wise IEEE ops, so the same bits as the per-element switch.
    float* a = o.values.data();
    const float* b = rb.values.data();
    const int n = o.size();
    switch (op) {
      case ADD: for (int i = 0; i < n; i++) a[i] = a[i] + b[i]; break;
      case SUB: for (int i = 0; i < n; i++) a[i] = a[i] + -b[i]; break;
      case MUL: for (int i = 0; i < n; i++) a[i] = a[i] * b[i]; break;
      case DIV: for (int i = 0; i < n; i++) a[i] = a[i] / b[i]; break;
      case MIN: for (int i = 0; i < n; i++) a[i] = b[i] < a[i] ? b[i] : a[i]; break;
      default: for (int i = 0; i < n; i++) a[i] = b[i] > a[i] ? b[i] : a[i]; break;
    }
  }
  float sampleValue(Ctx& c, int x, int y, int z) const override {
    float a = l->sampleValue(c, x, y, z);
    switch (op) {
      case ADD: return a + r->sampleValue(c, x, y, z);
      case SUB: return a - r->sampleValue(c, x, y, z);
      case MUL: return a == 0.0f ? 0.0f : a * r->sampleValue(c, x, y, z);
      case DIV: return a == 0.0f ? 0.0f : a / r->sampleValue(c, x, y, z);
      case MIN: return a <= rightBound ? a : jmin(a, r->sampleValue(c, x, y, z));
      default: return a >= rightBound ? a : jmax(a, r->sampleValue(c, x, y, z));
    }
  }
};

// ConstAdd, ConstMul, ConstMin, ConstMax (input op k), ConstSub (k - input), ConstDiv (k / input).
enum ConstOp { CADD, CMUL, CMIN, CMAX, CSUB, CDIV };
struct ConstBinS : Sampler {
  int op;
  S in;
  float k;
  ConstBinS(int op, S in, float k) : op(op), in(std::move(in)), k(k) {}
  float apply(float a) const {
    switch (op) {
      case CADD: return a + k;
      case CMUL: return a * k;
      case CSUB: return k - a;
      default: return k / a;
    }
  }
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    in->sampleVolume(c, o, v);
    float* a = o.values.data();
    const int n = o.size();
    switch (op) {  // one loop per op, same element-wise ops as apply()
      case CMIN: for (int i = 0; i < n; i++) a[i] = k < a[i] ? k : a[i]; break;
      case CMAX: for (int i = 0; i < n; i++) a[i] = k > a[i] ? k : a[i]; break;
      case CADD: for (int i = 0; i < n; i++) a[i] = a[i] + k; break;
      case CMUL: for (int i = 0; i < n; i++) a[i] = a[i] * k; break;
      case CSUB: for (int i = 0; i < n; i++) a[i] = k - a[i]; break;
      default: for (int i = 0; i < n; i++) a[i] = k / a[i]; break;
    }
  }
  float sampleValue(Ctx& c, int x, int y, int z) const override {
    float a = in->sampleValue(c, x, y, z);
    return op == CMIN ? jmin(a, k) : op == CMAX ? jmax(a, k) : apply(a);
  }
};

struct ClampS : Sampler {
  S in;
  float mn, mx;
  ClampS(S in, float mn, float mx) : in(std::move(in)), mn(mn), mx(mx) {}
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    in->sampleVolume(c, o, v);
    for (int i = 0; i < o.size(); i++) o.set(i, clampf(o.get(i), mn, mx));
  }
  float sampleValue(Ctx& c, int x, int y, int z) const override { return clampf(in->sampleValue(c, x, y, z), mn, mx); }
};

struct LerpS : Sampler {  // Sampler / ConstFirstSampler (first null) / ConstSecondSampler (second null)
  S alpha, first, second;
  float kFirst = 0, kSecond = 0;
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    alpha->sampleVolume(c, o, v);
    PBuf fb(first ? v.size() : 0), sb(second ? v.size() : 0);
    if (first) first->sampleVolume(c, fb, v);
    if (second) second->sampleVolume(c, sb, v);
    for (int i = 0; i < o.size(); i++) {
      float a = o.get(i), f = first ? fb.get(i) : kFirst, s = second ? sb.get(i) : kSecond;
      o.set(i, a == 0.0f ? f : a == 1.0f ? s : Mth::lerp(a, f, s));
    }
  }
  float sampleValue(Ctx& c, int x, int y, int z) const override {
    float a = alpha->sampleValue(c, x, y, z);
    auto f = [&] { return first ? first->sampleValue(c, x, y, z) : kFirst; };
    auto s = [&] { return second ? second->sampleValue(c, x, y, z) : kSecond; };
    if (a == 0.0f) return f();
    if (a == 1.0f) return s();
    float fv = f();
    return Mth::lerp(a, fv, s());
  }
};

struct RangeChoiceS : Sampler {  // Sampler / ConstSampler (inRange null)
  S input, inRange, outRange;
  float mn, mx, kIn = 0, kOut = 0;
  bool in(float v) const { return v >= mn && v < mx; }
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    if (!inRange) {
      input->sampleVolume(c, o, v);
      for (int i = 0; i < o.size(); i++) o.set(i, in(o.get(i)) ? kIn : kOut);
      return;
    }
    inRange->sampleVolume(c, o, v);
    PBuf ib(v.size()), ob(v.size());
    input->sampleVolume(c, ib, v);
    outRange->sampleVolume(c, ob, v);
    for (int i = 0; i < o.size(); i++)
      if (!in(ib.get(i))) o.set(i, ob.get(i));
  }
  float sampleValue(Ctx& c, int x, int y, int z) const override {
    float v = input->sampleValue(c, x, y, z);
    if (!inRange) return in(v) ? kIn : kOut;
    return in(v) ? inRange->sampleValue(c, x, y, z) : outRange->sampleValue(c, x, y, z);
  }
};

struct IntervalSelectS : Sampler {
  S input;
  std::vector<float> thresholds;
  std::vector<S> samplers;  // single threshold: {ifBelow, ifAbove}
  int select(float v) const {
    for (size_t i = 0; i < thresholds.size(); i++)
      if (v < thresholds[i]) return (int)i;
    return (int)samplers.size() - 1;
  }
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    input->sampleVolume(c, o, v);
    std::vector<PBuf> bufs;
    bufs.reserve(samplers.size());
    for (auto& s : samplers) {
      bufs.emplace_back(v.size());
      s->sampleVolume(c, bufs.back(), v);
    }
    for (int i = 0; i < o.size(); i++) o.set(i, bufs[select(o.get(i))].get(i));
  }
  float sampleValue(Ctx& c, int x, int y, int z) const override {
    return samplers[select(input->sampleValue(c, x, y, z))]->sampleValue(c, x, y, z);
  }
};

struct CachingS : Sampler {
  int id;
  S in;
  CachingS(int id, S in) : id(id), in(std::move(in)) {}
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override { c.sampleVolumeCached(id, *in, o, v); }
  float sampleValue(Ctx& c, int x, int y, int z) const override { return c.sampleValueCached(id, *in, x, y, z); }
};

struct InterpolatedS : Sampler {
  S in;
  int cxz, cy;
  float invXz, invY;
  InterpolatedS(S in, int cxz, int cy) : in(std::move(in)), cxz(cxz), cy(cy), invXz(1.0f / cxz), invY(1.0f / cy) {}
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    if ((v.stepBlockX == cxz || v.sizeX == 1) && (v.stepBlockY == cy || v.sizeY == 1) &&
        (v.stepBlockZ == cxz || v.sizeZ == 1) && floorMod(v.minBlockX, cxz) == 0 && floorMod(v.minBlockY, cy) == 0 &&
        floorMod(v.minBlockZ, cxz) == 0) {
      in->sampleVolume(c, o, v);
    } else if (v.stepBlockX == 1 && v.stepBlockY == 1 && v.stepBlockZ == 1) {
      blockStep(c, o, v);
    } else {
      Vol bv{v.sizeX * v.stepBlockX, v.sizeY * v.stepBlockY, v.sizeZ * v.stepBlockZ, v.minBlockX, v.minBlockY, v.minBlockZ};
      PBuf bb(bv.size());
      blockStep(c, bb, bv);
      for (int z = 0; z < v.sizeZ; z++)
        for (int x = 0; x < v.sizeX; x++)
          for (int y = 0; y < v.sizeY; y++)
            o.set(idx(v, x, y, z), bb.get(idx(bv, x * v.stepBlockX, y * v.stepBlockY, z * v.stepBlockZ)));
    }
  }
  void blockStep(Ctx& c, Buf& o, const Vol& v) const {
    int minCX = floorDiv(v.minBlockX, cxz), minCY = floorDiv(v.minBlockY, cy), minCZ = floorDiv(v.minBlockZ, cxz);
    int maxCX = floorDiv(maxX(v), cxz), maxCY = floorDiv(maxY(v), cy), maxCZ = floorDiv(maxZ(v), cxz);
    int nx = maxCX - minCX + 1, ny = maxCY - minCY + 1, nz = maxCZ - minCZ + 1;
    Vol cv{floorMod(maxX(v), cxz) == 0 ? nx : nx + 1, floorMod(maxY(v), cy) == 0 ? ny : ny + 1,
           floorMod(maxZ(v), cxz) == 0 ? nz : nz + 1, minCX * cxz, minCY * cy, minCZ * cxz, cxz, cy, cxz};
    PBuf cb(cv.size());
    in->sampleVolume(c, cb, cv);
    for (int cz = 0; cz < nz; cz++) {
      int ncz = std::min(cz + 1, cv.sizeZ - 1);
      for (int cx = 0; cx < nx; cx++) {
        int ncx = std::min(cx + 1, cv.sizeX - 1);
        float v000 = cb.get(idx(cv, cx, 0, cz)), v100 = cb.get(idx(cv, ncx, 0, cz));
        float v001 = cb.get(idx(cv, cx, 0, ncz)), v101 = cb.get(idx(cv, ncx, 0, ncz));
        for (int cyi = 0; cyi < ny; cyi++) {
          int ncy = std::min(cyi + 1, cv.sizeY - 1);
          float v010 = cb.get(idx(cv, cx, ncy, cz)), v110 = cb.get(idx(cv, ncx, ncy, cz));
          float v011 = cb.get(idx(cv, cx, ncy, ncz)), v111 = cb.get(idx(cv, ncx, ncy, ncz));
          fillCell(o, v, cv, cx, cyi, cz, v000, v100, v010, v110, v001, v101, v011, v111);
          v000 = v010, v100 = v110, v001 = v011, v101 = v111;
        }
      }
    }
  }
  void fillCell(Buf& o, const Vol& ov, const Vol& cv, int cellX, int cellY, int cellZ, float v000, float v100,
                float v010, float v110, float v001, float v101, float v011, float v111) const {
    int ox = cv.blockX(cellX) - ov.minBlockX, oy = cv.blockY(cellY) - ov.minBlockY, oz = cv.blockZ(cellZ) - ov.minBlockZ;
    int x0 = std::max(0, -ox), y0 = std::max(0, -oy), z0 = std::max(0, -oz);
    int x1 = std::min(cxz, ov.sizeX - ox) - 1, y1 = std::min(cy, ov.sizeY - oy) - 1, z1 = std::min(cxz, ov.sizeZ - oz) - 1;
    for (int z = z0; z <= z1; z++) {
      float az = (float)z * invXz;
      float v00 = Mth::lerp(az, v000, v001), v01 = Mth::lerp(az, v010, v011);
      float v10 = Mth::lerp(az, v100, v101), v11 = Mth::lerp(az, v110, v111);
      for (int x = x0; x <= x1; x++) {
        float ax = (float)x * invXz;
        float a = Mth::lerp(ax, v00, v10), b = Mth::lerp(ax, v01, v11);
        float step = (b - a) * invY;
        float value = a + step * (float)y0;
        int oi = idx(ov, ox + x, oy + y0, oz + z);
        for (int y = y0; y <= y1; y++) {
          o.set(oi++, value);
          value += step;
        }
      }
    }
  }
  float sampleValue(Ctx& c, int bx, int by, int bz) const override {
    int xi = floorMod(bx, cxz), yi = floorMod(by, cy), zi = floorMod(bz, cxz);
    if (xi == 0 && yi == 0 && zi == 0) return in->sampleValue(c, bx, by, bz);
    Vol v{2, 2, 2, bx - xi, by - yi, bz - zi, cxz, cy, cxz};
    PBuf b(8);
    in->sampleVolume(c, b, v);
    return Mth::lerp3((float)xi / (float)cxz, (float)yi / (float)cy, (float)zi / (float)cxz, b.get(idx(v, 0, 0, 0)),
                      b.get(idx(v, 1, 0, 0)), b.get(idx(v, 0, 1, 0)), b.get(idx(v, 1, 1, 0)), b.get(idx(v, 0, 0, 1)),
                      b.get(idx(v, 1, 0, 1)), b.get(idx(v, 0, 1, 1)), b.get(idx(v, 1, 1, 1)));
  }
};

struct SliceS : Sampler {  // axis 0 X, 1 Y, 2 Z, 3 XZ
  S in;
  int axis, a, b;  // coordinate (XZ: a = x, b = z)
  SliceS(S in, int axis, int a, int b = 0) : in(std::move(in)), axis(axis), a(a), b(b) {}
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    if (axis == 0) {
      if (v.sizeX == 1 && v.minBlockX == a) return in->sampleVolume(c, o, v);
      Vol iv{1, v.sizeY, v.sizeZ, a, v.minBlockY, v.minBlockZ, v.stepBlockX, v.stepBlockY, v.stepBlockZ};
      PBuf ib(iv.size());
      in->sampleVolume(c, ib, iv);
      int i = 0;
      for (int z = 0; z < v.sizeZ; z++)
        for (int x = 0; x < v.sizeX; x++)
          for (int y = 0; y < v.sizeY; y++) o.set(i++, ib.get(idx(iv, 0, y, z)));
    } else if (axis == 1) {
      if (v.sizeY == 1 && v.minBlockY == a) return in->sampleVolume(c, o, v);
      Vol iv{v.sizeX, 1, v.sizeZ, v.minBlockX, a, v.minBlockZ, v.stepBlockX, v.stepBlockY, v.stepBlockZ};
      PBuf ib(iv.size());
      in->sampleVolume(c, ib, iv);
      for (int z = 0; z < v.sizeZ; z++)
        for (int x = 0; x < v.sizeX; x++) setRange(o, idx(v, x, 0, z), v.sizeY, ib.get(idx(iv, x, 0, z)));
    } else if (axis == 2) {
      if (v.sizeZ == 1 && v.minBlockZ == a) return in->sampleVolume(c, o, v);
      Vol iv{v.sizeX, v.sizeY, 1, v.minBlockX, v.minBlockY, a, v.stepBlockX, v.stepBlockY, v.stepBlockZ};
      PBuf ib(iv.size());
      in->sampleVolume(c, ib, iv);
      int i = 0;
      for (int z = 0; z < v.sizeZ; z++)
        for (int x = 0; x < v.sizeX; x++)
          for (int y = 0; y < v.sizeY; y++) o.set(i++, ib.get(idx(iv, x, y, 0)));
    } else {
      if (v.sizeX == 1 && v.sizeZ == 1 && v.minBlockX == a && v.minBlockZ == b) return in->sampleVolume(c, o, v);
      Vol iv{1, v.sizeY, 1, a, v.minBlockY, b, v.stepBlockX, v.stepBlockY, v.stepBlockZ};
      PBuf ib(iv.size());
      in->sampleVolume(c, ib, iv);
      for (int y = 0; y < v.sizeY; y++) {
        float val = ib.get(idx(iv, 0, y, 0));
        int i = idx(v, 0, y, 0);
        for (int n = 0; n < v.sizeX * v.sizeZ; n++, i += v.sizeY) o.set(i, val);
      }
    }
  }
  float sampleValue(Ctx& c, int x, int y, int z) const override {
    switch (axis) {
      case 0: return in->sampleValue(c, a, y, z);
      case 1: return in->sampleValue(c, x, a, z);
      case 2: return in->sampleValue(c, x, y, a);
      default: return in->sampleValue(c, a, y, b);
    }
  }
};

struct FindTopSurfaceS : Sampler {
  S density, upper;
  int lower, cellHeight;
  float find(Ctx& c, int x, int z, float ub) const {
    int topY = Mth::floor(ub / (float)cellHeight) * cellHeight;
    if (topY <= lower) return (float)lower;
    for (int y = topY; y >= lower; y -= cellHeight)
      if (density->sampleValue(c, x, y, z) > 0.0f) return (float)y;
    return (float)lower;
  }
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    if (v.sizeY != 1) throw std::invalid_argument("find_top_surface: sizeY != 1");
    upper->sampleVolume(c, o, v);
    int i = 0;
    for (int z = 0; z < v.sizeZ; z++)
      for (int x = 0; x < v.sizeX; x++, i++) o.set(i, find(c, v.blockX(x), v.blockZ(z), o.get(i)));
  }
  float sampleValue(Ctx& c, int x, int y, int z) const override { return find(c, x, z, upper->sampleValue(c, x, y, z)); }
};

struct CSpline {
  float value = 0;
  int coord = -1;
  std::vector<float> loc, der;
  std::vector<CSpline> values;
};
float linearExtend(float input, const CSpline& s, float value, int i) {
  float d = s.der[i];
  return d == 0.0f ? value : value + d * (input - s.loc[i]);
}
template <class In>
float sampleSpline(const CSpline& s, In& in) {
  if (s.coord < 0) return s.value;
  float input = in(s.coord);
  int from = 0, len = (int)s.loc.size();  // Mth.binarySearch(0, n, i -> input < loc[i]) - 1
  while (len > 0) {
    int half = len / 2, mid = from + half;
    if (input < s.loc[mid]) len = half;
    else from = mid + 1, len -= half + 1;
  }
  int start = from - 1, last = (int)s.loc.size() - 1;
  if (start < 0) return linearExtend(input, s, sampleSpline(s.values[0], in), 0);
  if (start == last) return linearExtend(input, s, sampleSpline(s.values[last], in), last);
  float x1 = s.loc[start], x2 = s.loc[start + 1];
  float t = (input - x1) / (x2 - x1);
  float d1 = s.der[start], d2 = s.der[start + 1];
  float y1 = sampleSpline(s.values[start], in);
  float y2 = sampleSpline(s.values[start + 1], in);
  float a = d1 * (x2 - x1) - (y2 - y1);
  float b = -d2 * (x2 - x1) + (y2 - y1);
  return Mth::lerp(t, y1, y2) + t * (1.0f - t) * Mth::lerp(t, a, b);
}

struct SplineS : Sampler {
  CSpline spline;
  std::vector<S> coords;
  void sampleVolume(Ctx& c, Buf& o, const Vol& v) const override {
    std::vector<std::unique_ptr<PBuf>> bufs(coords.size());
    int index = 0;
    auto in = [&](int ci) {
      if (!bufs[ci]) {
        bufs[ci] = std::make_unique<PBuf>(v.size());
        coords[ci]->sampleVolume(c, *bufs[ci], v);
      }
      return bufs[ci]->get(index);
    };
    for (; index < o.size(); index++) o.set(index, sampleSpline(spline, in));
  }
  float sampleValue(Ctx& c, int x, int y, int z) const override {
    std::vector<float> cached(coords.size(), NAN);
    auto in = [&](int ci) {
      if (!std::isnan(cached[ci])) return cached[ci];
      return cached[ci] = coords[ci]->sampleValue(c, x, y, z);
    };
    return sampleSpline(spline, in);
  }
};

// ---------------------------------------------------------------- node helpers
std::string fullId(const std::string& id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; }

std::shared_ptr<Node> mk(K k) {
  auto n = std::make_shared<Node>();
  n->k = k;
  return n;
}
NodeP constant(float v) {
  auto n = mk(K::Constant);
  n->f = {v};
  return n;
}
bool isConst(const NodeP& n) { return n->k == K::Constant; }
NodeP slice(int axis, int c, NodeP in) {
  auto n = mk(K::Slice);
  n->op = axis;
  n->i = {c};
  n->kids = {std::move(in)};
  return n;
}

// Structural form for record equality (HolderHolder / noise holders compare by key).
void ser(const Spline& s, std::string& o);
void ser(const NodeP& n, std::string& o) {
  char tmp[32];
  o += '(';
  o += std::to_string((int)n->k) + ':' + std::to_string(n->op);
  if (n->k == K::Ref) { o += '@' + n->id + ')'; return; }
  if (n->k == K::Prepared) { o += '#' + std::to_string(n->cacheId) + ')'; return; }
  for (float f : n->f) snprintf(tmp, sizeof tmp, ",f%08x", bits(f)), o += tmp;
  for (int i : n->i) o += ",i" + std::to_string(i);
  for (double d : n->d) snprintf(tmp, sizeof tmp, ",d%016llx", (unsigned long long)bits(d)), o += tmp;
  if (!n->id.empty()) o += ",n" + n->id;
  for (auto& k : n->kids) ser(k, o);
  if (n->spline) ser(*n->spline, o);
  o += ')';
}
void ser(const Spline& s, std::string& o) {
  char tmp[32];
  if (!s.coordinate) { snprintf(tmp, sizeof tmp, "[k%08x]", bits(s.value)); o += tmp; return; }
  o += '[';
  ser(s.coordinate, o);
  for (size_t i = 0; i < s.locations.size(); i++) {
    snprintf(tmp, sizeof tmp, "%08x,%08x", bits(s.locations[i]), bits(s.derivatives[i]));
    o += tmp;
    ser(*s.values[i], o);
  }
  o += ']';
}
std::string key(const NodeP& n) {
  std::string s;
  ser(n, s);
  return s;
}

template <class Rule>
std::shared_ptr<const Spline> mapSpline(const std::shared_ptr<const Spline>& s, Rule& rule, bool& changed) {
  if (!s->coordinate) return s;
  auto ns = std::make_shared<Spline>(*s);
  NodeP c = rule(s->coordinate);
  if (c != s->coordinate) changed = true;
  ns->coordinate = c;
  for (auto& v : ns->values) v = mapSpline(v, rule, changed);
  return ns;
}

// DensityFunction.rewriteChildren
template <class Rule>
NodeP rewriteChildren(const NodeP& n, Rule rule) {
  switch (n->k) {
    case K::Ref: {
      NodeP nf = rule(n->target);
      return nf == n->target ? n : nf;
    }
    case K::Spline: {
      bool changed = false;
      auto s = mapSpline(n->spline, rule, changed);
      if (!changed) return n;
      auto c = std::make_shared<Node>(*n);
      c->spline = s;
      return c;
    }
    default: {
      if (n->kids.empty()) return n;
      std::vector<NodeP> kids;
      bool changed = false;
      for (auto& k : n->kids) {
        kids.push_back(rule(k));
        changed |= kids.back() != k;
      }
      if (!changed) return n;
      auto c = std::make_shared<Node>(*n);
      c->kids = std::move(kids);
      return c;
    }
  }
}

void forEachCoordinate(const Spline& s, const std::function<void(const NodeP&)>& fn) {
  if (!s.coordinate) return;
  fn(s.coordinate);
  for (auto& v : s.values) forEachCoordinate(*v, fn);
}

Iv splineRange(const Spline& s) {
  if (!s.coordinate) return exact(s.value);
  int last = (int)s.locations.size() - 1;
  float mn = INF, mx = -INF;
  Iv in = DensityFunctions::range(s.coordinate);
  if (in.isNaI()) return in;
  auto ext = [&](float input, float value, int i) {
    float d = s.derivatives[i];
    return d == 0.0f ? value : value + d * (input - s.locations[i]);
  };
  if (in.min < s.locations[0]) {
    Iv r = splineRange(*s.values[0]);
    float e1 = ext(in.min, r.min, 0), e2 = ext(in.min, r.max, 0);
    mn = jmin(mn, jmin(e1, e2)), mx = jmax(mx, jmax(e1, e2));
  }
  if (in.max > s.locations[last]) {
    Iv r = splineRange(*s.values[last]);
    float e1 = ext(in.max, r.min, last), e2 = ext(in.max, r.max, last);
    mn = jmin(mn, jmin(e1, e2)), mx = jmax(mx, jmax(e1, e2));
  }
  std::vector<Iv> vr;
  for (auto& v : s.values) vr.push_back(splineRange(*v));
  for (auto& r : vr) mn = jmin(mn, r.min), mx = jmax(mx, r.max);
  for (int i = 0; i < last; i++) {
    float x1 = s.locations[i], x2 = s.locations[i + 1], xd = x2 - x1;
    float min1 = vr[i].min, max1 = vr[i].max, min2 = vr[i + 1].min, max2 = vr[i + 1].max;
    float d1 = s.derivatives[i], d2 = s.derivatives[i + 1];
    if (d1 != 0.0f || d2 != 0.0f) {
      float p1 = d1 * xd, p2 = d2 * xd;
      float minLerp1 = jmin(min1, min2), maxLerp1 = jmax(max1, max2);
      float minA = p1 - max2 + min1, maxA = p1 - min2 + max1;
      float minB = -p2 + min2 - max1, maxB = -p2 + max2 - min1;
      float minLerp2 = jmin(minA, minB), maxLerp2 = jmax(maxA, maxB);
      mn = jmin(mn, minLerp1 + 0.25f * minLerp2);
      mx = jmax(mx, maxLerp1 + 0.25f * maxLerp2);
    }
  }
  return of(mn, mx);
}

std::mutex noiseRangeMutex;
std::map<std::string, Iv> noiseRanges;
std::string noiseDataRoot = "vanilla/src/data";
Iv noiseRange(const std::string& id) {
  std::lock_guard l(noiseRangeMutex);
  auto it = noiseRanges.find(id);
  if (it != noiseRanges.end()) return it->second;
  Interval r = loadNormalNoise(id, noiseDataRoot).range();
  return noiseRanges[id] = {r.min, r.max};
}

}  // namespace

// ---------------------------------------------------------------- SamplerContext
void SamplerContext::sampleVolumeCached(int id, const Sampler& input, DensityBuffer& out, const DensityVolume& v) {
  if (!caches) return input.sampleVolume(*this, out, v);
  if (id >= (int)cells.size()) cells.resize(id + 1);
  if (!cells[id].hasVolume || !(v == cells[id].volume)) {
    cells[id].hasVolume = true;
    cells[id].volume = v;
    DensityBuffer b(v.size());
    input.sampleVolume(*this, b, v);  // may grow `cells`: re-index afterwards
    cells[id].buffer = std::move(b.values);
  }
  out.values = cells[id].buffer;
}

float SamplerContext::sampleValueCached(int id, const Sampler& input, int x, int y, int z) {
  if (!caches) return input.sampleValue(*this, x, y, z);
  if (id >= (int)cells.size()) cells.resize(id + 1);
  int64_t k = blockPosAsLong(x, y, z);
  Cell& c = cells[id];
  if (c.valueKey == k && !std::isnan(c.value)) return c.value;
  if (c.hasVolume) {
    int i = indexOfBlock(c.volume, x, y, z);
    if (i != -1) return c.buffer[i];
  }
  float value = input.sampleValue(*this, x, y, z);
  cells[id].valueKey = k;
  cells[id].value = value;
  return value;
}

// ---------------------------------------------------------------- DensityFunctions
DensityFunctions::DensityFunctions(int64_t seed, std::string dataRoot) : rs(seed, false, dataRoot), root(std::move(dataRoot)) {
  std::lock_guard l(noiseRangeMutex);
  noiseDataRoot = root;
}

NodeP DensityFunctions::function(const std::string& id) {
  std::lock_guard l(m);
  return functionLocked(id);
}

NodeP DensityFunctions::functionLocked(const std::string& rawId) {
  std::string id = fullId(rawId);
  auto it = registry.find(id);
  if (it != registry.end()) return it->second;
  size_t colon = id.find(':');
  std::string path = root + "/" + id.substr(0, colon) + "/worldgen/density_function/" + id.substr(colon + 1) + ".json";
  auto ref = mk(K::Ref);
  ref->id = id;
  ref->target = parseLocked(Json::parseFile(path));
  registry[id] = ref;
  return ref;
}

NodeP DensityFunctions::parse(const Json& j) {
  std::lock_guard l(m);
  return parseLocked(j);
}

static int axisOf(const std::string& a) { return a == "x" ? 0 : a == "y" ? 1 : 2; }

static std::shared_ptr<const Spline> parseSpline(const Json& j, const std::function<NodeP(const Json&)>& parseDf) {
  auto s = std::make_shared<Spline>();
  if (j.isNumber()) {
    s->value = j.asFloat();
    return s;
  }
  s->coordinate = parseDf(j["coordinate"]);
  for (auto& p : j["points"].arr) {
    s->locations.push_back(p["location"].asFloat());
    s->values.push_back(parseSpline(p["value"], parseDf));
    s->derivatives.push_back(p["derivative"].asFloat());
  }
  if (s->locations.empty()) throw std::runtime_error("df: spline without points");
  return s;
}

NodeP DensityFunctions::parseLocked(const Json& j) {
  if (j.isNumber()) return constant(j.asFloat());
  if (j.isString()) return functionLocked(j.asString());
  std::string type = fullId(j["type"].asString()).substr(10);
  auto df = [&](const char* k) { return parseLocked(j[k]); };
  auto optDf = [&](const char* k) { return j.has(k) ? parseLocked(j[k]) : constant(0.0f); };
  static const char* unary[] = {"abs", "square", "cube", "sqrt", "half_negative", "quarter_negative",
                                "reciprocal", "negate", "squeeze", "log", "sign"};
  static const char* binary[] = {"add", "sub", "mul", "div", "min", "max"};
  for (int u = 0; u < 11; u++)
    if (type == unary[u]) {
      auto n = mk(K::Unary);
      n->op = u;
      n->kids = {df("input")};
      return n;
    }
  for (int b = 0; b < 6; b++)
    if (type == binary[b]) {
      auto n = mk(K::Binary);
      n->op = b;
      n->kids = {df("left"), df("right")};
      return n;
    }
  if (type == "constant") return constant(j["value"].asFloat());
  if (type == "noise") {  // id, d = {xz_scale, y_scale}, kids = {shift_x, shift_y, shift_z}
    auto n = mk(K::Noise);
    n->id = fullId(j["noise"].asString());
    n->d = {j["xz_scale"].asDouble(), j["y_scale"].asDouble()};
    n->kids = {optDf("shift_x"), optDf("shift_y"), optDf("shift_z")};
    return n;
  }
  if (type == "shift_a" || type == "shift_b" || type == "shift") {
    auto n = mk(type == "shift_a" ? K::ShiftA : type == "shift_b" ? K::ShiftB : K::Shift);
    n->id = fullId(j["noise"].asString());
    return n;
  }
  if (type == "gradient") {  // op axis, i = {tiling, from, to}, f = {from_value, to_value}
    auto n = mk(K::Gradient);
    n->op = axisOf(j["axis"].asString());
    std::string t = j.has("tiling") ? j["tiling"].asString() : "clamp_to_edge";
    n->i = {t == "clamp_to_edge" ? 0 : t == "repeat" ? 1 : 2, j["from_coordinate"].asInt(), j["to_coordinate"].asInt()};
    n->f = {j["from_value"].asFloat(), j["to_value"].asFloat()};
    return n;
  }
  if (type == "clamp") {
    auto n = mk(K::Clamp);
    n->kids = {df("input")};
    n->f = {j["min"].asFloat(), j["max"].asFloat()};
    return n;
  }
  if (type == "lerp") {
    auto n = mk(K::Lerp);
    n->kids = {df("alpha"), df("first"), df("second")};
    return n;
  }
  if (type == "range_choice") {  // kids = {input, in, out}, f = {min_inclusive, max_exclusive}
    auto n = mk(K::RangeChoice);
    n->kids = {df("input"), df("when_in_range"), df("when_out_of_range")};
    n->f = {j["min_inclusive"].asFloat(), j["max_exclusive"].asFloat()};
    return n;
  }
  if (type == "interval_select") {  // kids = {input, functions...}, f = thresholds
    auto n = mk(K::IntervalSelect);
    n->kids = {df("input")};
    for (auto& f : j["functions"].arr) n->kids.push_back(parseLocked(f));
    for (auto& t : j["thresholds"].arr) n->f.push_back(t.asFloat());
    return n;
  }
  if (type == "cache" || type == "blend_density") {
    auto n = mk(type == "cache" ? K::Cache : K::BlendDensity);
    n->kids = {df("input")};
    return n;
  }
  if (type == "interpolated") {  // i = {cell_size_xz, cell_size_y}
    auto n = mk(K::Interpolated);
    n->kids = {df("input")};
    n->i = {j["cell_size_xz"].asInt(), j["cell_size_y"].asInt()};
    return n;
  }
  if (type == "slice") return slice(axisOf(j["axis"].asString()), j["coordinate"].asInt(), df("input"));
  if (type == "find_top_surface") {  // kids = {density, upper_bound}, i = {lower_bound, cell_height}
    auto n = mk(K::FindTopSurface);
    n->kids = {df("density"), df("upper_bound")};
    n->i = {j["lower_bound"].asInt(), j["cell_height"].asInt()};
    return n;
  }
  if (type == "spline") {
    auto n = mk(K::Spline);
    n->spline = parseSpline(j["spline"], [&](const Json& c) { return parseLocked(c); });
    return n;
  }
  if (type == "blend_alpha") return mk(K::BlendAlpha);
  if (type == "blend_offset") return mk(K::BlendOffset);
  if (type == "beardifier") return mk(K::Beardifier);
  if (type == "old_blended_noise") {
    auto n = mk(K::OldBlendedNoise);
    n->d = {j["xz_scale"].asDouble(), j["y_scale"].asDouble(), j["xz_factor"].asDouble(), j["y_factor"].asDouble(),
            j["smear_scale_multiplier"].asDouble()};
    return n;
  }
  throw std::runtime_error("df: unsupported density function type " + type);
}

int DensityFunctions::domainAxes(const NodeP& n) {
  int a = 0;
  switch (n->k) {
    case K::Constant: return 0;
    case K::Ref: return domainAxes(n->target);
    case K::Noise:
      a = 7;
      if (n->d[1] == 0.0) a &= ~2;
      if (n->d[0] == 0.0) a &= ~5;
      break;
    case K::ShiftA: case K::ShiftB: case K::BlendAlpha: case K::BlendOffset: return 5;
    case K::Shift: case K::Beardifier: case K::OldBlendedNoise: return 7;
    case K::Gradient: return 1 << n->op;
    case K::Slice: return domainAxes(n->kids[0]) & ~(1 << n->op);
    case K::Prepared: return n->axes;
    case K::Spline:
      forEachCoordinate(*n->spline, [&](const NodeP& c) { a |= domainAxes(c); });
      return a;
    default: break;
  }
  for (auto& k : n->kids) a |= domainAxes(k);
  return n->k == K::FindTopSurface ? a & ~2 : a;
}

Iv DensityFunctions::range(const NodeP& n) {
  auto r = [&](int i) { return range(n->kids[i]); };
  switch (n->k) {
    case K::Constant: return exact(n->f[0]);
    case K::Ref: return range(n->target);
    case K::Noise: return noiseRange(n->id);
    case K::ShiftA: case K::ShiftB: case K::Shift: return ivMul(noiseRange(n->id), exact(4.0f));
    case K::Gradient: return encapsulating(n->f[0], n->f[1]);
    case K::Unary: {
      Iv in = r(0);
      switch (n->op) {
        case ABS: {
          if (in.isNaI()) return NaI;
          float a = std::fabs(in.min), b = std::fabs(in.max), mx = jmax(a, b);
          return contains(in, 0.0f) ? of(0.0f, mx) : of(jmin(a, b), mx);
        }
        case SQUARE: {
          if (in.isNaI()) return NaI;
          float a = in.min * in.min, b = in.max * in.max, mx = jmax(a, b);
          return contains(in, 0.0f) ? of(0.0f, mx) : of(jmin(a, b), mx);
        }
        case SQRT: return ivPow(in, exact(0.5f));
        case RECIPROCAL: return ivReciprocal(in);
        case NEGATE: return ivSub(exact(0.0f), in);
        case LOG:
          if (in.max < 0.0f) return NaI;
          return mapMonotonic(ivMax(in, exact(0.0f)), [](float x) { return (float)std::log((double)x); });
        case SIGN:
          if (in.isNaI()) return NaI;
          if (in.min == in.max) return exact(signum(in.min));
          if (contains(in, 0.0f)) return in.min == 0.0f ? of(0, 1) : in.max == 0.0f ? of(-1, 0) : of(-1, 1);
          return exact(in.min > 0.0f ? 1.0f : -1.0f);
        default: {
          int op = n->op;
          return mapMonotonic(in, [op](float x) { return applyUnary(op, x); });
        }
      }
    }
    case K::Binary:
      switch (n->op) {
        case ADD: return ivAdd(r(0), r(1));
        case SUB: return ivSub(r(0), r(1));
        case MUL: return ivMul(r(0), r(1));
        case DIV: return ivMul(r(0), ivReciprocal(r(1)));
        case MIN: return ivMin(r(0), r(1));
        default: return ivMax(r(0), r(1));
      }
    case K::Clamp: return ivClamp(r(0), n->f[0], n->f[1]);
    case K::Lerp: return ivLerp(r(0), r(1), r(2));
    case K::RangeChoice: return encapsulating({r(1), r(2)});
    case K::IntervalSelect: {
      std::vector<Iv> list;
      for (size_t i = 1; i < n->kids.size(); i++) list.push_back(r((int)i));
      return encapsulating(list);
    }
    case K::FindTopSurface: return of((float)n->i[0], jmax((float)n->i[0], r(1).max));
    case K::Spline: return splineRange(*n->spline);
    case K::BlendAlpha: return of(0.0f, 1.0f);
    case K::BlendOffset: case K::Beardifier: return of(-INF, INF);
    case K::OldBlendedNoise: {
      BlendedNoise bn{n->d[0], n->d[1], n->d[2], n->d[3], n->d[4]};
      Interval i = bn.range();
      return {i.min, i.max};
    }
    case K::Prepared: return n->range;
    default: return r(0);  // cache, interpolated, slice, blend_density
  }
}

NodeP DensityFunctions::optimize(const NodeP& f) { return sliceRule(7, cacheRule(f)); }

NodeP DensityFunctions::cacheRule(const NodeP& f0) {
  NodeP f = f0->k == K::Ref ? f0->target : f0;  // INLINE_REFERENCE
  if (f->k != K::Cache) return rewriteChildren(f, [this](const NodeP& c) { return cacheRule(c); });
  std::string k = key(f->kids[0]);
  auto it = preparedCaches.find(k);
  if (it != preparedCaches.end()) return it->second;
  int id = nextCacheId++;
  NodeP input = optimize(f->kids[0]);
  auto p = mk(K::Prepared);
  p->cacheId = id;
  p->range = range(input);
  p->axes = domainAxes(input);
  p->sampler = std::make_shared<CachingS>(id, compile(input));
  preparedCaches[k] = p;
  return p;
}

NodeP DensityFunctions::sliceRule(int parentAxes, const NodeP& f) {
  if (f->k == K::Constant || f->k == K::Gradient) return f;
  int axes = domainAxes(f);
  if (parentAxes == axes) return rewriteChildren(f, [this, parentAxes](const NodeP& c) { return sliceRule(parentAxes, c); });
  NodeP nf = rewriteChildren(f, [this, axes](const NodeP& c) { return sliceRule(axes, c); });
  int existing = 0;
  for (NodeP s = nf; s->k == K::Slice; s = s->kids[0]) existing |= 1 << s->op;
  int filtered = (parentAxes & ~axes) & ~existing;
  if (filtered & 1) nf = slice(0, 0, nf);
  if (filtered & 4) nf = slice(2, 0, nf);
  if (filtered & 2) nf = slice(1, 0, nf);
  return nf;
}

SamplerP DensityFunctions::compile(const NodeP& n) {
  auto c = [&](int i) { return compile(n->kids[i]); };
  switch (n->k) {
    case K::Constant: return std::make_shared<ConstS>(n->f[0]);
    case K::Ref: return compile(n->target);
    case K::Noise: {
      auto noise = rs.createNoiseSampler(n->id);
      auto zero = [&](int i) { return isConst(n->kids[i]) && bits(n->kids[i]->f[0]) == 0u; };
      if (zero(0) && zero(1) && zero(2)) return std::make_shared<NoiseS>(noise, n->d[0], n->d[1]);
      S sx = c(0), sz = c(2);
      return std::make_shared<ShiftedS>(sx, zero(1) ? nullptr : c(1), sz, noise, n->d[0], n->d[1]);
    }
    case K::ShiftA: case K::Shift:
      return std::make_shared<ConstBinS>(
          CMUL, std::make_shared<NoiseS>(rs.createNoiseSampler(n->id), 0.25, n->k == K::Shift ? 0.25 : 0.0), 4.0f);
    case K::ShiftB: return std::make_shared<ShiftBS>(rs.createNoiseSampler(n->id));
    case K::Gradient: {
      auto g = std::make_shared<GradientS>();
      int from = n->i[1], to = n->i[2];
      g->axis = n->op, g->tiling = n->i[0], g->from = from, g->range = to - from;
      g->minC = std::min(from, to), g->maxC = std::max(from, to);
      g->fromValue = n->f[0];
      g->factor = (n->f[1] - n->f[0]) / (float)(to - from);
      return g;
    }
    case K::Unary: return std::make_shared<UnaryS>(n->op, c(0));
    case K::Binary: {
      S l = c(0), r = c(1);
      const NodeP &L = n->kids[0], &R = n->kids[1];
      switch (n->op) {
        case ADD:
          if (isConst(L)) return std::make_shared<ConstBinS>(CADD, r, L->f[0]);
          if (isConst(R)) return std::make_shared<ConstBinS>(CADD, l, R->f[0]);
          return std::make_shared<BinS>(ADD, l, r);
        case SUB:
          if (isConst(L)) return std::make_shared<ConstBinS>(CSUB, r, L->f[0]);
          if (isConst(R)) return std::make_shared<ConstBinS>(CADD, l, -R->f[0]);
          return std::make_shared<BinS>(SUB, l, r);
        case MUL:
          if (isConst(L)) return std::make_shared<ConstBinS>(CMUL, r, L->f[0]);
          if (isConst(R)) return std::make_shared<ConstBinS>(CMUL, l, R->f[0]);
          return std::make_shared<BinS>(MUL, l, r);
        case DIV:
          if (isConst(L)) return std::make_shared<ConstBinS>(CDIV, r, L->f[0]);
          if (isConst(R)) return std::make_shared<ConstBinS>(CMUL, l, 1.0f / R->f[0]);
          return std::make_shared<BinS>(DIV, l, r);
        case MIN: {
          Iv lr = range(L), rr = range(R);
          if (lr.max < rr.min) return l;
          if (rr.max < lr.min) return r;
          if (isConst(L)) return std::make_shared<ConstBinS>(CMIN, r, L->f[0]);
          if (isConst(R)) return std::make_shared<ConstBinS>(CMIN, l, R->f[0]);
          return std::make_shared<BinS>(MIN, l, r, rr.min);
        }
        default: {
          Iv lr = range(L), rr = range(R);
          if (lr.min > rr.max) return l;
          if (rr.min > lr.max) return r;
          if (isConst(L)) return std::make_shared<ConstBinS>(CMAX, r, L->f[0]);
          if (isConst(R)) return std::make_shared<ConstBinS>(CMAX, l, R->f[0]);
          return std::make_shared<BinS>(MAX, l, r, rr.max);
        }
      }
    }
    case K::Clamp: return std::make_shared<ClampS>(c(0), n->f[0], n->f[1]);
    case K::Lerp: {
      auto s = std::make_shared<LerpS>();
      s->alpha = c(0);
      S first = c(1), second = c(2);
      if (isConst(n->kids[1])) s->kFirst = n->kids[1]->f[0], s->second = second;
      else if (isConst(n->kids[2])) s->first = first, s->kSecond = n->kids[2]->f[0];
      else s->first = first, s->second = second;
      return s;
    }
    case K::RangeChoice: {
      auto s = std::make_shared<RangeChoiceS>();
      s->input = c(0);
      s->mn = n->f[0], s->mx = n->f[1];
      if (isConst(n->kids[1]) && isConst(n->kids[2])) s->kIn = n->kids[1]->f[0], s->kOut = n->kids[2]->f[0];
      else s->inRange = c(1), s->outRange = c(2);
      return s;
    }
    case K::IntervalSelect: {
      auto s = std::make_shared<IntervalSelectS>();
      s->input = c(0);
      s->thresholds = n->f;
      if (n->f.size() == 1) {
        s->samplers.push_back(c(1));
        s->samplers.push_back(c((int)n->kids.size() - 1));
      } else {
        for (size_t i = 1; i < n->kids.size(); i++) s->samplers.push_back(c((int)i));
      }
      return s;
    }
    case K::Cache: throw std::logic_error("df: cannot compile cache before it has been deduplicated");
    case K::Interpolated: return std::make_shared<InterpolatedS>(c(0), n->i[0], n->i[1]);
    case K::Slice: {
      const NodeP& in = n->kids[0];
      if (in->k == K::Slice && ((n->op == 0 && in->op == 2) || (n->op == 2 && in->op == 0))) {
        int x = n->op == 0 ? n->i[0] : in->i[0], z = n->op == 0 ? in->i[0] : n->i[0];
        return std::make_shared<SliceS>(compile(in->kids[0]), 3, x, z);
      }
      return std::make_shared<SliceS>(c(0), n->op, n->i[0]);
    }
    case K::FindTopSurface: {
      auto s = std::make_shared<FindTopSurfaceS>();
      s->density = c(0), s->upper = c(1), s->lower = n->i[0], s->cellHeight = n->i[1];
      return std::make_shared<SliceS>(s, 1, 0);
    }
    case K::Spline: {
      auto s = std::make_shared<SplineS>();
      std::map<std::string, int> index;  // coordinates deduplicated by DensityFunction equality
      std::function<CSpline(const Spline&)> build = [&](const Spline& sp) {
        CSpline cs;
        cs.value = sp.value;
        if (!sp.coordinate) return cs;
        std::string k = key(sp.coordinate);
        auto it = index.find(k);
        if (it == index.end()) {
          it = index.emplace(k, (int)s->coords.size()).first;
          s->coords.push_back(compile(sp.coordinate));
        }
        cs.coord = it->second;
        cs.loc = sp.locations, cs.der = sp.derivatives;
        for (auto& v : sp.values) cs.values.push_back(build(*v));
        return cs;
      };
      s->spline = build(*n->spline);
      return s;
    }
    case K::BlendAlpha: return std::make_shared<ConstS>(1.0f);  // no blender in context: ContextBoundSampler fallback
    case K::BlendOffset: case K::Beardifier: return std::make_shared<ConstS>(0.0f);
    case K::BlendDensity: return c(0);  // blender empty: pass-through
    case K::OldBlendedNoise: {
      BlendedNoise bn{n->d[0], n->d[1], n->d[2], n->d[3], n->d[4]};
      auto fbm = bn.compileSampler(*rs.createRandom(BlendedNoise::NOISE_SEED));
      auto s = std::make_shared<LerpS>();
      s->alpha = std::make_shared<ClampS>(
          std::make_shared<ConstBinS>(CADD, std::make_shared<NoiseS>(fbm.mainNoise, fbm.mainXzMultiplier, fbm.mainYMultiplier), 0.5f),
          0.0f, 1.0f);
      s->first = std::make_shared<NoiseS>(fbm.minLimitNoise, fbm.xzMultiplier, fbm.yMultiplier);
      s->second = std::make_shared<NoiseS>(fbm.maxLimitNoise, fbm.xzMultiplier, fbm.yMultiplier);
      return s;
    }
    case K::Prepared: return n->sampler;
  }
  throw std::logic_error("df: unknown node");
}

SamplerP DensityFunctions::sampler(const NodeP& f) {
  std::lock_guard l(m);
  auto it = samplers.find(f.get());
  if (it != samplers.end()) return it->second;
  keepAlive.push_back(f);
  return samplers[f.get()] = compile(optimize(f));
}

float DensityFunctions::sample(const std::string& id, int x, int y, int z) {
  SamplerContext uncached(false);
  return sampler(function(id))->sampleValue(uncached, x, y, z);
}

}  // namespace df
