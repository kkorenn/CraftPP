// Port of vanilla worldgen random sources (net.minecraft.util.RandomSource, levelgen/*Random*).
// All integer arithmetic wraps like Java (done in unsigned, then cast back).
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "worldgen/mth.hpp"

class PositionalRandomFactory;

class RandomSource {
 public:
  virtual ~RandomSource() = default;
  virtual std::unique_ptr<RandomSource> fork() = 0;
  virtual std::unique_ptr<PositionalRandomFactory> forkPositional() = 0;
  virtual void setSeed(int64_t seed) = 0;
  virtual int32_t nextInt() = 0;
  virtual int32_t nextInt(int32_t bound) = 0;
  virtual int64_t nextLong() = 0;
  virtual bool nextBoolean() = 0;
  virtual float nextFloat() = 0;
  virtual double nextDouble() = 0;
  virtual double nextGaussian() = 0;

  virtual void consumeCount(int32_t rounds) {
    for (int32_t i = 0; i < rounds; i++) nextInt();
  }
  int32_t nextIntBetweenInclusive(int32_t min, int32_t maxInclusive) {
    return (int32_t)((uint32_t)nextInt((int32_t)((uint32_t)maxInclusive - (uint32_t)min + 1u)) + (uint32_t)min);
  }
  int32_t nextInt(int32_t origin, int32_t bound) {
    if (origin >= bound) throw std::invalid_argument("bound - origin is non positive");
    return (int32_t)((uint32_t)origin + (uint32_t)nextInt((int32_t)((uint32_t)bound - (uint32_t)origin)));
  }
  double triangle(double mean, double spread) { return mean + spread * (nextDouble() - nextDouble()); }
  float triangle(float mean, float spread) { return mean + spread * (nextFloat() - nextFloat()); }
};

class PositionalRandomFactory {
 public:
  virtual ~PositionalRandomFactory() = default;
  virtual std::unique_ptr<RandomSource> at(int32_t x, int32_t y, int32_t z) const = 0;
  virtual std::unique_ptr<RandomSource> fromHashOf(std::string_view name) const = 0;
  virtual std::unique_ptr<RandomSource> fromSeed(int64_t seed) const = 0;
};

// ---------------------------------------------------------------------------------------------
namespace RandomSupport {
constexpr int64_t GOLDEN_RATIO_64 = -7046029254386353131LL;
constexpr int64_t SILVER_RATIO_64 = 7640891576956012809LL;

inline int64_t mixStafford13(int64_t z0) {
  uint64_t z = (uint64_t)z0;
  z = (z ^ z >> 30) * 0xBF58476D1CE4E5B9ull;  // -4658895280553007687L
  z = (z ^ z >> 27) * 0x94D049BB133111EBull;  // -7723592293110705685L
  return (int64_t)(z ^ z >> 31);
}

struct Seed128bit {
  int64_t seedLo, seedHi;
  Seed128bit xor_(int64_t lo, int64_t hi) const { return {seedLo ^ lo, seedHi ^ hi}; }
  Seed128bit xor_(const Seed128bit& o) const { return xor_(o.seedLo, o.seedHi); }
  Seed128bit mixed() const { return {mixStafford13(seedLo), mixStafford13(seedHi)}; }
};

inline Seed128bit upgradeSeedTo128bitUnmixed(int64_t legacySeed) {
  int64_t lo = legacySeed ^ SILVER_RATIO_64;
  int64_t hi = (int64_t)((uint64_t)lo + (uint64_t)GOLDEN_RATIO_64);
  return {lo, hi};
}
inline Seed128bit upgradeSeedTo128bit(int64_t legacySeed) { return upgradeSeedTo128bitUnmixed(legacySeed).mixed(); }

// RFC 1321 MD5.
inline std::array<uint8_t, 16> md5(std::string_view msg) {
  static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                            5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  std::string data(msg);
  uint64_t bitLen = (uint64_t)msg.size() * 8;
  data += (char)0x80;
  while (data.size() % 64 != 56) data += '\0';
  for (int i = 0; i < 8; i++) data += (char)(bitLen >> (8 * i));
  uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  for (size_t off = 0; off < data.size(); off += 64) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++) {
      const auto* p = (const uint8_t*)data.data() + off + i * 4;
      w[i] = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
      uint32_t f;
      int g;
      if (i < 16) f = (b & c) | (~b & d), g = i;
      else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
      else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
      else f = c ^ (b | ~d), g = (7 * i) % 16;
      uint32_t t = d;
      d = c;
      c = b;
      uint32_t x = a + f + K[i] + w[g];
      b = b + (x << R[i] | x >> (32 - R[i]));
      a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
  }
  std::array<uint8_t, 16> out;
  for (int i = 0; i < 16; i++) out[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
  return out;
}

inline Seed128bit seedFromHashOf(std::string_view input) {
  auto hash = md5(input);  // input is UTF-8 already
  uint64_t lo = 0, hi = 0;
  for (int i = 0; i < 8; i++) lo = lo << 8 | hash[i], hi = hi << 8 | hash[8 + i];
  return {(int64_t)lo, (int64_t)hi};
}
}  // namespace RandomSupport

// Java String.hashCode() over the UTF-16 code units of a UTF-8 string.
inline int32_t javaStringHashCode(std::string_view s) {
  uint32_t h = 0;
  for (size_t i = 0; i < s.size();) {
    uint8_t c = (uint8_t)s[i];
    uint32_t cp;
    int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (int k = 1; k < n && i + k < s.size(); k++) cp = cp << 6 | ((uint8_t)s[i + k] & 0x3F);
    i += n;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      h = 31 * h + (0xD800 + (cp >> 10));
      h = 31 * h + (0xDC00 + (cp & 0x3FF));
    } else {
      h = 31 * h + cp;
    }
  }
  return (int32_t)h;
}

// ---------------------------------------------------------------------------------------------
class MarsagliaPolarGaussian {
 public:
  void reset() { haveNextNextGaussian = false; }
  double nextGaussian(RandomSource& randomSource) {
    if (haveNextNextGaussian) {
      haveNextNextGaussian = false;
      return nextNextGaussian;
    }
    double x, y, radiusSquared;
    do {
      x = 2.0 * randomSource.nextDouble() - 1.0;
      y = 2.0 * randomSource.nextDouble() - 1.0;
      radiusSquared = Mth::square(x) + Mth::square(y);
    } while (radiusSquared >= 1.0 || radiusSquared == 0.0);
    // ponytail: std::log may differ from Java Math.log by 1ulp in rare cases; gaussian is not used by noise.
    double multiplier = std::sqrt(-2.0 * std::log(radiusSquared) / radiusSquared);
    nextNextGaussian = y * multiplier;
    haveNextNextGaussian = true;
    return x * multiplier;
  }

 private:
  double nextNextGaussian = 0;
  bool haveNextNextGaussian = false;
};

class BitRandomSource : public RandomSource {
 public:
  using RandomSource::nextInt;
  virtual int32_t next(int bits) = 0;

  int32_t nextInt() override { return next(32); }
  int32_t nextInt(int32_t bound) override {
    if (bound <= 0) throw std::invalid_argument("Bound must be positive");
    if ((bound & (bound - 1)) == 0) return (int32_t)((int64_t)bound * next(31) >> 31);
    int32_t sample, modulo;
    do {
      sample = next(31);
      modulo = sample % bound;
    } while ((int32_t)((uint32_t)sample - (uint32_t)modulo + (uint32_t)(bound - 1)) < 0);
    return modulo;
  }
  int64_t nextLong() override {
    int32_t upper = next(32);
    int32_t lower = next(32);
    return (int64_t)(((uint64_t)(int64_t)upper << 32) + (uint64_t)(int64_t)lower);
  }
  bool nextBoolean() override { return next(1) != 0; }
  float nextFloat() override { return (float)next(24) * 5.9604645E-8f; }
  double nextDouble() override {
    int32_t upper = next(26);
    int32_t lower = next(27);
    int64_t combined = ((int64_t)upper << 27) + lower;
    return (double)combined * 1.1102230246251565E-16;
  }
};

class LegacyRandomSource : public BitRandomSource {
 public:
  explicit LegacyRandomSource(int64_t seed) { LegacyRandomSource::setSeed(seed); }

  std::unique_ptr<RandomSource> fork() override { return std::make_unique<LegacyRandomSource>(nextLong()); }
  std::unique_ptr<PositionalRandomFactory> forkPositional() override;
  void setSeed(int64_t seed) override {
    this->seed = (seed ^ 25214903917LL) & 281474976710655LL;
    gaussianSource.reset();
  }
  int32_t next(int bits) override {
    seed = (int64_t)(((uint64_t)seed * 25214903917ull + 11ull) & 281474976710655ull);
    return (int32_t)(seed >> (48 - bits));
  }
  double nextGaussian() override { return gaussianSource.nextGaussian(*this); }

 protected:
  int64_t seed = 0;
  MarsagliaPolarGaussian gaussianSource;
};

// Same LCG as LegacyRandomSource, but in Java it is NOT an instanceof LegacyRandomSource (matters in WorldgenRandom::next).
class SingleThreadedRandomSource : public LegacyRandomSource {
 public:
  using LegacyRandomSource::LegacyRandomSource;
  std::unique_ptr<RandomSource> fork() override { return std::make_unique<SingleThreadedRandomSource>(nextLong()); }
};

class LegacyPositionalRandomFactory : public PositionalRandomFactory {
 public:
  explicit LegacyPositionalRandomFactory(int64_t seed) : seed(seed) {}
  std::unique_ptr<RandomSource> at(int32_t x, int32_t y, int32_t z) const override {
    return std::make_unique<LegacyRandomSource>(Mth::getSeed(x, y, z) ^ seed);
  }
  std::unique_ptr<RandomSource> fromHashOf(std::string_view name) const override {
    return std::make_unique<LegacyRandomSource>((int64_t)javaStringHashCode(name) ^ seed);
  }
  std::unique_ptr<RandomSource> fromSeed(int64_t s) const override { return std::make_unique<LegacyRandomSource>(s); }

 private:
  int64_t seed;
};

inline std::unique_ptr<PositionalRandomFactory> LegacyRandomSource::forkPositional() {
  return std::make_unique<LegacyPositionalRandomFactory>(nextLong());
}

// ---------------------------------------------------------------------------------------------
class Xoroshiro128PlusPlus {
 public:
  explicit Xoroshiro128PlusPlus(RandomSupport::Seed128bit s) : Xoroshiro128PlusPlus(s.seedLo, s.seedHi) {}
  Xoroshiro128PlusPlus(int64_t lo, int64_t hi) : seedLo((uint64_t)lo), seedHi((uint64_t)hi) {
    if ((seedLo | seedHi) == 0) {
      seedLo = (uint64_t)RandomSupport::GOLDEN_RATIO_64;
      seedHi = (uint64_t)RandomSupport::SILVER_RATIO_64;
    }
  }
  int64_t nextLong() {
    uint64_t s0 = seedLo, s1 = seedHi;
    uint64_t result = rotl(s0 + s1, 17) + s0;
    s1 ^= s0;
    seedLo = rotl(s0, 49) ^ s1 ^ s1 << 21;
    seedHi = rotl(s1, 28);
    return (int64_t)result;
  }

 private:
  static uint64_t rotl(uint64_t x, int k) { return x << k | x >> (64 - k); }
  uint64_t seedLo, seedHi;
};

class XoroshiroRandomSource : public RandomSource {
 public:
  using RandomSource::nextInt;
  explicit XoroshiroRandomSource(int64_t seed) : rng(RandomSupport::upgradeSeedTo128bit(seed)) {}
  explicit XoroshiroRandomSource(RandomSupport::Seed128bit seed) : rng(seed) {}
  XoroshiroRandomSource(int64_t seedLo, int64_t seedHi) : rng(seedLo, seedHi) {}

  std::unique_ptr<RandomSource> fork() override {
    int64_t lo = rng.nextLong();
    int64_t hi = rng.nextLong();
    return std::make_unique<XoroshiroRandomSource>(lo, hi);
  }
  std::unique_ptr<PositionalRandomFactory> forkPositional() override;
  void setSeed(int64_t seed) override {
    rng = Xoroshiro128PlusPlus(RandomSupport::upgradeSeedTo128bit(seed));
    gaussianSource.reset();
  }
  int32_t nextInt() override { return (int32_t)rng.nextLong(); }
  int32_t nextInt(int32_t bound) override {
    if (bound <= 0) throw std::invalid_argument("Bound must be positive");
    uint64_t randomBits = (uint32_t)nextInt();
    uint64_t multiplied = randomBits * (uint64_t)bound;
    uint64_t fractional = multiplied & 0xFFFFFFFFull;
    if (fractional < (uint64_t)bound) {
      uint32_t unbiasedStart = (0u - (uint32_t)bound) % (uint32_t)bound;  // Integer.remainderUnsigned(~bound + 1, bound)
      while (fractional < unbiasedStart) {
        randomBits = (uint32_t)nextInt();
        multiplied = randomBits * (uint64_t)bound;
        fractional = multiplied & 0xFFFFFFFFull;
      }
    }
    return (int32_t)(multiplied >> 32);
  }
  int64_t nextLong() override { return rng.nextLong(); }
  bool nextBoolean() override { return (rng.nextLong() & 1) != 0; }
  float nextFloat() override { return (float)nextBits(24) * 5.9604645E-8f; }
  double nextDouble() override { return (double)nextBits(53) * 1.1102230246251565E-16; }
  double nextGaussian() override { return gaussianSource.nextGaussian(*this); }
  void consumeCount(int32_t rounds) override {
    for (int32_t i = 0; i < rounds; i++) rng.nextLong();
  }

 private:
  uint64_t nextBits(int bits) { return (uint64_t)rng.nextLong() >> (64 - bits); }
  Xoroshiro128PlusPlus rng;
  MarsagliaPolarGaussian gaussianSource;
};

class XoroshiroPositionalRandomFactory : public PositionalRandomFactory {
 public:
  XoroshiroPositionalRandomFactory(int64_t seedLo, int64_t seedHi) : seedLo(seedLo), seedHi(seedHi) {}
  std::unique_ptr<RandomSource> at(int32_t x, int32_t y, int32_t z) const override {
    return std::make_unique<XoroshiroRandomSource>(Mth::getSeed(x, y, z) ^ seedLo, seedHi);
  }
  std::unique_ptr<RandomSource> fromHashOf(std::string_view name) const override {
    return std::make_unique<XoroshiroRandomSource>(RandomSupport::seedFromHashOf(name).xor_(seedLo, seedHi));
  }
  std::unique_ptr<RandomSource> fromSeed(int64_t seed) const override {
    return std::make_unique<XoroshiroRandomSource>(seed ^ seedLo, seed ^ seedHi);
  }

 private:
  int64_t seedLo, seedHi;
};

inline std::unique_ptr<PositionalRandomFactory> XoroshiroRandomSource::forkPositional() {
  int64_t lo = rng.nextLong();
  int64_t hi = rng.nextLong();
  return std::make_unique<XoroshiroPositionalRandomFactory>(lo, hi);
}

// ---------------------------------------------------------------------------------------------
class WorldgenRandom : public LegacyRandomSource {
 public:
  explicit WorldgenRandom(std::unique_ptr<RandomSource> randomSource)
      : LegacyRandomSource(0), randomSource(std::move(randomSource)) {}

  int32_t getCount() const { return count; }
  std::unique_ptr<RandomSource> fork() override { return randomSource->fork(); }
  std::unique_ptr<PositionalRandomFactory> forkPositional() override { return randomSource->forkPositional(); }
  int32_t next(int bits) override {
    count++;
    auto* legacy = dynamic_cast<LegacyRandomSource*>(randomSource.get());
    if (legacy && !dynamic_cast<SingleThreadedRandomSource*>(legacy)) return legacy->next(bits);
    return (int32_t)((uint64_t)randomSource->nextLong() >> (64 - bits));
  }
  // Note: like vanilla, this does not reset WorldgenRandom's own gaussian state.
  void setSeed(int64_t seed) override {
    if (randomSource) randomSource->setSeed(seed);
  }

  int64_t setDecorationSeed(int64_t seed, int32_t chunkX, int32_t chunkZ) {
    setSeed(seed);
    uint64_t xScale = (uint64_t)(nextLong() | 1);
    uint64_t zScale = (uint64_t)(nextLong() | 1);
    int64_t result = (int64_t)((uint64_t)(int64_t)chunkX * xScale + (uint64_t)(int64_t)chunkZ * zScale) ^ seed;
    setSeed(result);
    return result;
  }
  void setFeatureSeed(int64_t seed, int32_t index, int32_t step) {
    int32_t stepPart = (int32_t)(10000u * (uint32_t)step);  // int multiply in Java
    setSeed((int64_t)((uint64_t)seed + (uint64_t)(int64_t)index + (uint64_t)(int64_t)stepPart));
  }
  void setLargeFeatureSeed(int64_t seed, int32_t chunkX, int32_t chunkZ) {
    setSeed(seed);
    uint64_t xScale = (uint64_t)nextLong();
    uint64_t zScale = (uint64_t)nextLong();
    setSeed((int64_t)((uint64_t)(int64_t)chunkX * xScale ^ (uint64_t)(int64_t)chunkZ * zScale) ^ seed);
  }
  void setLargeFeatureWithSalt(int64_t seed, int32_t x, int32_t z, int32_t blend) {
    setSeed((int64_t)((uint64_t)(int64_t)x * 341873128712ull + (uint64_t)(int64_t)z * 132897987541ull + (uint64_t)seed +
                      (uint64_t)(int64_t)blend));
  }
  static std::unique_ptr<RandomSource> seedSlimeChunk(int32_t x, int32_t z, int64_t seed, int64_t salt) {
    uint32_t ux = (uint32_t)x, uz = (uint32_t)z;
    uint64_t s = (uint64_t)seed + (uint64_t)(int64_t)(int32_t)(ux * ux * 4987142u) +
                 (uint64_t)(int64_t)(int32_t)(ux * 5947611u) +
                 (uint64_t)(int64_t)(int32_t)(uz * uz) * 4392871ull + (uint64_t)(int64_t)(int32_t)(uz * 389711u);
    return std::make_unique<SingleThreadedRandomSource>((int64_t)s ^ salt);
  }

  enum class Algorithm { LEGACY, XOROSHIRO };
  static std::unique_ptr<RandomSource> newInstance(Algorithm a, int64_t seed) {
    if (a == Algorithm::LEGACY) return std::make_unique<LegacyRandomSource>(seed);
    return std::make_unique<XoroshiroRandomSource>(seed);
  }

 private:
  std::unique_ptr<RandomSource> randomSource;
  int32_t count = 0;
};
