// Self-checks for src/worldgen/{random,noise,json}. Run from the repo root (reads vanilla/src/data).
// Expected noise/xoroshiro values come from an independent Python re-derivation of the Java source
// (float32 via numpy); java.util.Random values are well-known constants.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "worldgen/json.hpp"
#include "worldgen/noise.hpp"
#include "worldgen/random.hpp"

static uint32_t bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

static std::string hex(const std::array<uint8_t, 16>& h) {
  std::string s;
  char b[3];
  for (auto c : h) std::snprintf(b, 3, "%02x", c), s += b;
  return s;
}

int main() {
  // --- java.util.Random LCG
  {
    LegacyRandomSource r(0);
    assert(r.nextInt() == -1155484576);
    assert(r.nextInt() == -723955400);
    assert(LegacyRandomSource(42).nextLong() == -5025562857975149833LL);
    assert(LegacyRandomSource(0).nextDouble() == 0.730967787376657);
    assert(LegacyRandomSource(0).nextFloat() == 0.7309677600860596f);
    LegacyRandomSource r2(123);
    assert(r2.nextInt(10) == 2 && r2.nextInt(16) == 3 && r2.nextInt(1000) == 176);
    LegacyRandomSource a(7), b(7);
    for (int i = 0; i < 100; i++) assert(a.nextGaussian() == b.nextGaussian());
  }
  // --- MD5 (RFC 1321 vectors) and seeds
  {
    assert(hex(RandomSupport::md5("abc")) == "900150983cd24fb0d6963f7d28e17f72");
    assert(hex(RandomSupport::md5("")) == "d41d8cd98f00b204e9800998ecf8427e");
    assert(hex(RandomSupport::md5("12345678901234567890123456789012345678901234567890123456789012345678901234567890")) ==
           "57edf4a22be3c955ac49da2e2107b67a");
    auto s = RandomSupport::seedFromHashOf("abc");
    assert((uint64_t)s.seedLo == 0x900150983cd24fb0ull && (uint64_t)s.seedHi == 0xd6963f7d28e17f72ull);
    assert(RandomSupport::mixStafford13(0) == 0);
    assert(RandomSupport::mixStafford13(1) == 6238072747940578789LL);
    assert(RandomSupport::mixStafford13(12345) == -906084347102765743LL);
    assert(javaStringHashCode("hello") == 99162322);
  }
  // --- Xoroshiro
  {
    XoroshiroRandomSource x(0);
    assert(x.nextLong() == 3038984756725240190LL);
    assert(x.nextLong() == -3694039286755638414LL);
    XoroshiroRandomSource y(12345);
    assert(y.nextInt(100) == 1);
    assert(y.nextDouble() == 0.44677574066879455);
    assert(y.nextFloat() == 0.22463339567184448f);
    XoroshiroRandomSource p(99), q(99);
    auto fp = p.forkPositional(), fq = q.forkPositional();
    assert(fp->at(1, 2, 3)->nextLong() == fq->at(1, 2, 3)->nextLong());
    assert(fp->fromHashOf("minecraft:x")->nextLong() == fq->fromHashOf("minecraft:x")->nextLong());
  }
  // --- WorldgenRandom seeding is deterministic and delegates to the wrapped source
  {
    WorldgenRandom w1(std::make_unique<LegacyRandomSource>(0)), w2(std::make_unique<LegacyRandomSource>(0));
    assert(w1.setDecorationSeed(42, 3, -7) == w2.setDecorationSeed(42, 3, -7));
    w1.setFeatureSeed(42, 1, 2), w2.setFeatureSeed(42, 1, 2);
    int before = w1.getCount();  // setDecorationSeed consumed 2 nextLong = 4 next() calls
    assert(before == 4 && w1.nextInt() == w2.nextInt() && w1.getCount() == 5);
    WorldgenRandom w3(std::make_unique<LegacyRandomSource>(0));
    w3.setSeed(0);
    assert(w3.nextInt() == -1155484576);
  }
  // --- JSON
  {
    Json j = Json::parse(R"({"a":[1,2.5e3,-0.1],"b":"xé\"","c":true,"d":null,"e":{}})");
    assert(j["a"][1].asDouble() == 2500.0 && j["a"][2].asDouble() == -0.1);
    assert(j["b"].asString() == "x\xc3\xa9\"" && j["c"].asBool() && j["d"].isNull() && j["e"].isObject());
  }
  // --- NormalNoise values against the Python re-derivation
  {
    struct Case { int64_t seed; const char* id; double x, y, z; uint32_t expect; };
    const Case cases[] = {
        {0, "minecraft:temperature", 0.0, 0.0, 0.0, 0x3dfa8994},
        {0, "minecraft:temperature", 100.25, 0.0, -2000.5, 0x3aec06d8},
        {0, "minecraft:temperature", 123456.0, 64.0, -98765.0, 0xbd536f48},
        {0, "minecraft:continentalness", 0.0, 0.0, 0.0, 0xbcddb989},
        {0, "minecraft:continentalness", 100.25, 0.0, -2000.5, 0xbee9c82f},
        {0, "minecraft:continentalness", 123456.0, 64.0, -98765.0, 0x3c9f433e},
        {0, "minecraft:offset", 0.0, 0.0, 0.0, 0xbf0b2876},
        {0, "minecraft:offset", 100.25, 0.0, -2000.5, 0xbe7529a9},
        {0, "minecraft:offset", 123456.0, 64.0, -98765.0, 0xbf002ffe},
        {12345, "minecraft:temperature", 0.0, 0.0, 0.0, 0xbcca9ff2},
        {12345, "minecraft:temperature", 100.25, 0.0, -2000.5, 0xbe211090},
        {12345, "minecraft:temperature", 123456.0, 64.0, -98765.0, 0x3de017a5},
        {12345, "minecraft:continentalness", 0.0, 0.0, 0.0, 0xbece23a6},
        {12345, "minecraft:continentalness", 100.25, 0.0, -2000.5, 0xbd25a02e},
        {12345, "minecraft:continentalness", 123456.0, 64.0, -98765.0, 0x3dcf6400},
        {12345, "minecraft:offset", 0.0, 0.0, 0.0, 0xbf0982fd},
        {12345, "minecraft:offset", 100.25, 0.0, -2000.5, 0xbf046528},
        {12345, "minecraft:offset", 123456.0, 64.0, -98765.0, 0xbe41cbb1},
    };
    for (auto& c : cases) {
      RandomState rs(c.seed, false);
      float v = rs.getOrCreateNoise(c.id)->get(c.x, c.y, c.z);
      if (bits(v) != c.expect) std::printf("MISMATCH %s seed=%lld: %08x != %08x\n", c.id, (long long)c.seed, bits(v), c.expect);
      assert(bits(v) == c.expect);
    }
  }
  // --- every noise JSON instantiates; values finite and inside range(); volume path ~= point path
  {
    RandomState rs(-4172144997902289642LL, false), rs2(-4172144997902289642LL, false), legacy(1, true);
    int count = 0;
    std::string root = "vanilla/src/data/minecraft/worldgen/noise/";
    for (auto& e : std::filesystem::recursive_directory_iterator(root)) {
      if (e.path().extension() != ".json") continue;
      std::string rel = e.path().string().substr(root.size());
      std::string id = "minecraft:" + rel.substr(0, rel.size() - 5);
      auto n = rs.createNoiseSampler(id);
      auto n2 = rs2.createNoiseSampler(id);
      auto nl = legacy.createNoiseSampler(id);
      Interval r = n->range();
      assert(std::isfinite(r.min) && std::isfinite(r.max) && r.min <= r.max);
      DensityVolume vol{4, 3, 2, -37, -64, 1000, 4, 8, 4};
      DensityBuffer buf(vol.size());
      n->addToVolume(buf, vol, 1.0, 1.0, 1.0f);
      for (int i = 0; i < 200; i++) {
        double x = i * 37.3 - 3000, y = i * 1.7 - 64, z = i * -91.1 + 17;
        float v = n->get(x, y, z), v2 = n->get(x, z);
        assert(std::isfinite(v) && v >= r.min && v <= r.max && std::isfinite(v2) && v2 >= r.min && v2 <= r.max);
        assert(bits(v) == bits(n2->get(x, y, z)));
        assert(std::isfinite(nl->get(x, y, z)));
      }
      int idx = 0;
      for (int iz = 0; iz < vol.sizeZ; iz++)
        for (int ix = 0; ix < vol.sizeX; ix++)
          for (int iy = 0; iy < vol.sizeY; iy++, idx++)
            assert(std::fabs(buf.get(idx) - n->get(vol.blockX(ix), vol.blockY(iy), vol.blockZ(iz))) < 1e-4f);
      count++;
    }
    assert(count >= 60);
    std::printf("%d noises ok\n", count);
  }
  // --- BlendedNoise (overworld base_3d_noise)
  {
    Json j = Json::parseFile("vanilla/src/data/minecraft/worldgen/density_function/overworld/base_3d_noise.json");
    assert(j["type"].asString() == "minecraft:old_blended_noise");
    BlendedNoise bn = BlendedNoise::fromJson(j);
    RandomState rs(0, false);
    auto s = bn.compileSampler(*rs.createRandom(BlendedNoise::NOISE_SEED));
    auto s2 = bn.compileSampler(*rs.createRandom(BlendedNoise::NOISE_SEED));
    Interval r = bn.range();
    DensityVolume vol{4, 8, 4, 0, -64, 0, 4, 8, 4};
    DensityBuffer buf(vol.size());
    s.sampleVolume(buf, vol);
    int idx = 0;
    for (int iz = 0; iz < vol.sizeZ; iz++)
      for (int ix = 0; ix < vol.sizeX; ix++)
        for (int iy = 0; iy < vol.sizeY; iy++, idx++) {
          float v = s.sampleValue(vol.blockX(ix), vol.blockY(iy), vol.blockZ(iz));
          assert(std::isfinite(v) && v >= r.min && v <= r.max);
          assert(bits(v) == bits(s2.sampleValue(vol.blockX(ix), vol.blockY(iy), vol.blockZ(iz))));
          assert(std::fabs(buf.get(idx) - v) < 1e-3f);
        }
    RandomState lrs(0, true);
    auto ls = bn.compileSampler(*lrs.createRandom(BlendedNoise::NOISE_SEED));
    assert(std::isfinite(ls.sampleValue(10, 20, 30)));
  }
  // --- SimplexNoise (as used by Biome temperature noise)
  {
    WorldgenRandom wr(std::make_unique<LegacyRandomSource>(1234));
    SimplexNoise sn(wr, true);
    for (int i = 0; i < 100; i++) {
      float a = sn.get(i * 0.37, i * -1.3), b = sn.get(i * 0.37, 5.0, i * -1.3);
      assert(std::isfinite(a) && std::fabs(a) <= 2.0f && std::isfinite(b) && std::fabs(b) <= 2.0f);
    }
  }
  std::printf("worldgen_rng_test: all checks passed\n");
}
