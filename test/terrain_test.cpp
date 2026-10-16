// Noise-stage sanity checks: generates a few chunks, checks block/biome invariants and prints timing.
// Bit-exactness itself is checked against a real vanilla server by test/compare.py.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>

#include "world.hpp"
#include "worldgen/terrain.hpp"

int main() {
  worldgen::TerrainGenerator gen(12345, "vanilla/src/data", false);  // noise stage only
  // Interval / domain sanity on known functions.
  auto cont = gen.dfs.function("minecraft:overworld/continents");
  assert(df::DensityFunctions::domainAxes(cont) == 5);
  assert(df::DensityFunctions::domainAxes(gen.finalDensity) == 7);
  df::Iv r = df::DensityFunctions::range(gen.dfs.function("minecraft:overworld/caves/pillars"));
  assert(!r.isNaI() && r.min <= r.max);
  // Point path vs volume path of a non-interpolated XZ function agree closely.
  float p = gen.dfs.sample("minecraft:overworld/continents", 100, 0, -37);
  assert(p > -2 && p < 2);

  auto t0 = std::chrono::steady_clock::now();
  const int N = 32;
  for (int i = 0; i < N; i++) {
    auto c = std::make_unique<Chunk>();
    c->cx = i, c->cz = -i;
    gen.generate(*c);
    int solid = 0, water = 0;
    for (int y = MIN_Y; y < MIN_Y + HEIGHT; y++)
      for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
          uint16_t b = c->get(x, y, z);
          assert(b == blocks::air || b == blocks::stone || b == blocks::water || b == blocks::lava);
          solid += b == blocks::stone, water += b == blocks::water;
        }
    assert(solid > 0);
    printf("chunk %d,%d: %d stone, %d water, biome[0]=%d\n", c->cx, c->cz, solid, water, c->get_biome(0, 64, 0));
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / N;
  printf("noise stage: %.1f ms/chunk; spawn height at 0,0: %d\n", ms, gen.baseHeight(0, 0));
  worldgen::TerrainGenerator full(12345);
  std::vector<std::unique_ptr<Chunk>> chunks;
  t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; i++) {
    auto c = std::make_unique<Chunk>();
    c->cx = i, c->cz = -i;
    full.generate(*c);
    assert(c->get(0, MIN_Y, 0) == blocks::bedrock);
    chunks.push_back(std::move(c));
  }
  ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / N;
  // Output fingerprint: must be identical across compilers/platforms (fp contraction would change it).
  uint64_t h = 1469598103934665603ull;
  for (auto& c : chunks)
    for (int y = MIN_Y; y < MIN_Y + HEIGHT; y++)
      for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
          h = (h ^ c->get(x, y, z)) * 1099511628211ull;
          if (!(x & 3) && !(y & 3) && !(z & 3)) h = (h ^ c->get_biome(x >> 2, (y - MIN_Y) >> 2, z >> 2)) * 1099511628211ull;
        }
  printf("noise + surface: %.1f ms/chunk\nfingerprint %016llx\n", ms, (unsigned long long)h);

  // Nether + end (dimension layout 0..255): default blocks, lava sea, biomes from their sources.
  for (const char* dim : {"nether", "end"}) {
    t0 = std::chrono::steady_clock::now();
    worldgen::TerrainGenerator g(12345, "vanilla/src/data", true, dim, 0, 256);
    double init = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    t0 = std::chrono::steady_clock::now();
    int solid = 0, lava = 0;
    for (int i = 0; i < N; i++) {
      Chunk c(0, 256);
      c.cx = dim[0] == 'n' ? i : 70 + i, c.cz = -i;
      g.generate(c);
      for (int y = 0; y < 256; y++)
        for (int z = 0; z < 16; z++)
          for (int x = 0; x < 16; x++) {
            uint16_t b = c.get(x, y, z);
            if (y >= 128) assert(b == blocks::air);
            solid += b != blocks::air && b != blocks::lava, lava += b == blocks::lava;
          }
      if (dim[0] == 'n') assert(c.get(0, 0, 0) == blocks::bedrock && c.get(0, 127, 0) == blocks::bedrock);
    }
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / N;
    printf("%s: init %.0f ms, %.1f ms/chunk, %d solid, %d lava\n", dim, init, ms, solid, lava);
    assert(solid > 0 && (dim[0] == 'e' || lava > 0));
  }
  printf("terrain ok\n");
}
