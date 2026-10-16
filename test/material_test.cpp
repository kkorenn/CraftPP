// Self-checks for src/worldgen/material (no density engine: fake MaterialContext). Run from the repo root.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>

#include "world.hpp"
#include "worldgen/material.hpp"

using namespace material;

struct FakeContext : MaterialContext {
  int biome = biomes::plains;
  float surfaceLevel = 70, oreDensity = -1, gap = 1;
  float value(const std::string& id) {
    if (id == "minecraft:overworld/chunk_surface_level") return surfaceLevel;
    if (id == "minecraft:overworld/ore_vein/gap") return gap;
    return oreDensity;  // copper/iron density, richness
  }
  float sampleDensity(const std::string& id, int, int, int) override { return value(id); }
  void sampleDensityVolume(const std::string& id, DensityBuffer& out, const DensityVolume&) override { out.fill(value(id)); }
  int noiseBiome(int, int, int) override { return biome; }
};

static std::unique_ptr<Chunk> flatChunk(int cx, int cz, int stoneTop, int waterTop) {
  auto c = std::make_unique<Chunk>();
  c->cx = cx, c->cz = cz;
  for (int y = MIN_Y; y <= std::max(stoneTop, waterTop); y++)
    for (int z = 0; z < 16; z++)
      for (int x = 0; x < 16; x++) c->set(x, y, z, y <= stoneTop ? blocks::stone : blocks::water);
  return c;
}

int main() {
  // BiomeManager.obfuscateSeed (Guava sha256().hashLong().asLong(), reference values from Python hashlib).
  assert(MaterialSystem::obfuscateSeed(0) == 8794265229978523055LL);
  assert(MaterialSystem::obfuscateSeed(1) == -6467378160175308932LL);
  assert(MaterialSystem::obfuscateSeed(123456789) == -5458710421630621949LL);

  RandomState rs(12345, false);
  MaterialSystem ms(rs, "minecraft:overworld", "vanilla/src/data", "vanilla/generated/reports/blocks.json", true);
  assert(ms.biomeId("plains") == biomes::plains && ms.biomeId("eroded_badlands") == biomes::eroded_badlands);
  assert(ms.stateId("grass_block") == blocks::grass_block && ms.stateId("nope") == -1);

  // Stack PosRandom == RandomState.getOrCreateRandomFactory(id).at(...)
  for (const char* id : {"minecraft:bedrock_floor", "minecraft:ore", "minecraft:deepslate"}) {
    PosRandom p = ms.randomFactory(id);
    for (int i = 0; i < 20; i++) {
      int x = i * 37 - 300, y = i * 11 - 64, z = 1000 - i * 53;
      assert(p.at(x, y, z).nextLong() == rs.getOrCreateRandomFactory(id).at(x, y, z)->nextLong());
    }
  }

  // Every rule / condition file parses (nether/end included).
  int files = 0;
  for (const char* folder : {"material_rule", "material_condition"}) {
    std::string base = std::string("vanilla/src/data/minecraft/worldgen/") + folder;
    for (auto& e : std::filesystem::recursive_directory_iterator(base)) {
      if (e.path().extension() != ".json") continue;
      std::string rel = std::filesystem::relative(e.path(), base).replace_extension().generic_string();
      if (folder[9] == 'r') ms.parseRuleFile("minecraft:" + rel);
      else ms.parseConditionFile("minecraft:" + rel);
      files++;
    }
  }
  assert(files == 50);

  // Badlands bands only produce terracotta variants.
  for (int y = -64; y < 320; y++) {
    int b = ms.getBand(100, y, -100);
    assert(b == blocks::terracotta || b == blocks::orange_terracotta || b == blocks::yellow_terracotta ||
           b == blocks::brown_terracotta || b == blocks::red_terracotta || b == blocks::white_terracotta ||
           b == blocks::light_gray_terracotta);
  }

  // Plains, flat stone to y=70, preliminary surface 70.
  FakeContext ctx;
  auto c = flatChunk(3, -2, 70, 0);
  ms.buildSurface(*c, ctx);
  int bedrockMid = 0, mixedDeep = 0;
  for (int z = 0; z < 16; z++)
    for (int x = 0; x < 16; x++) {
      int bx = 48 + x, bz = -32 + z, sd = ms.getSurfaceDepth(bx, bz);
      assert(c->get(x, -64, z) == blocks::bedrock);  // vertical_gradient true_at_and_below bottom
      bedrockMid += c->get(x, -62, z) == blocks::bedrock;
      for (int y = -59; y <= 70; y++) assert(c->get(x, y, z) != blocks::bedrock);  // false_at_and_above bottom+5
      for (int y = -58; y <= 0; y++) assert(c->get(x, y, z) == blocks::deepslate);
      for (int y = 1; y <= 7; y++) {
        int b = c->get(x, y, z);
        assert(b == blocks::deepslate || b == blocks::stone);
        mixedDeep += b == blocks::deepslate;
      }
      for (int y = 8; y < 60; y++) assert(c->get(x, y, z) == blocks::stone);
      assert(c->get(x, 70, z) == blocks::grass_block);
      for (int k = 1; k <= 10; k++) {
        bool dirt = k <= sd && 70 - k >= 62 + sd;  // under_floor && above_preliminary_surface
        assert(c->get(x, 70 - k, z) == (dirt ? blocks::dirt : blocks::stone));
      }
      assert(c->get(x, 71, z) == blocks::air);
    }
  assert(bedrockMid > 0 && bedrockMid < 256 && mixedDeep > 0 && mixedDeep < 7 * 256);

  // Under water: stone to 59, water 60..62 -> dirt top (under_floor) instead of grass.
  ctx.surfaceLevel = 63;
  c = flatChunk(0, 0, 59, 62);
  ms.buildSurface(*c, ctx);
  for (int z = 0; z < 16; z++)
    for (int x = 0; x < 16; x++) {
      int sd = ms.getSurfaceDepth(x, z);
      bool dirt = sd >= 0 && 59 >= 55 + sd;
      assert(c->get(x, 59, z) == (dirt ? blocks::dirt : blocks::stone));
      assert(c->get(x, 60, z) == blocks::water);
    }

  // Ore veins: density 1, richness 1, gap < 0 -> copper vein (first ore rule) everywhere below the surface rules.
  ctx.oreDensity = 1, ctx.gap = -1, ctx.surfaceLevel = 70;
  c = flatChunk(5, 5, 70, 0);
  ms.buildSurface(*c, ctx);
  for (int y = -50; y < 60; y++) {
    int b = c->get(7, y, 7);
    assert(b == blocks::copper_ore || b == blocks::raw_copper_block);
  }

  // Timing (fake densities: measures the material stage itself).
  ctx = FakeContext{};
  const int N = 64;
  std::vector<std::unique_ptr<Chunk>> cs;
  for (int i = 0; i < N; i++) cs.push_back(flatChunk(i, -i, 70, 0));
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N / 2; i++) ms.buildSurface(*cs[i], ctx);
  auto t1 = std::chrono::steady_clock::now();
  std::vector<uint8_t> possible(128, 0);
  possible[biomes::plains] = 1;
  for (int i = N / 2; i < N; i++) ms.buildSurface(*cs[i], ctx, &possible);
  auto t2 = std::chrono::steady_clock::now();
  double ms_lazy = std::chrono::duration<double, std::milli>(t1 - t0).count() / (N / 2);
  double ms_per = std::chrono::duration<double, std::milli>(t2 - t1).count() / (N / 2);
  {  // possibleBiomes must not change results
    auto a = flatChunk(9, 9, 70, 0), b = flatChunk(9, 9, 70, 0);
    ms.buildSurface(*a, ctx);
    ms.buildSurface(*b, ctx, &possible);
    for (int y = MIN_Y; y < MIN_Y + HEIGHT; y++)
      for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) assert(a->get(x, y, z) == b->get(x, y, z));
  }
  std::printf("material_test: without possibleBiomes %.3f ms/chunk\n", ms_lazy);
  std::printf("material_test: %d files parsed, %d nodes, %.3f ms/chunk (fake densities)\n", files, ms.ruleCount(), ms_per);
  std::printf("material_test: OK\n");
}
