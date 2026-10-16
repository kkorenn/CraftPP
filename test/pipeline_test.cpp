// Pipeline smoke test + profile: generates an R x R square of FULL chunks through the overworld pipeline on one
// thread and prints ms/chunk per status and peak RSS. Usage: pipeline_test [seed] [radius] [stages] [cx cz]
#undef NDEBUG
#include <sys/resource.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>

#include "world.hpp"
#include "worldgen/overworld.hpp"

int main(int argc, char** argv) {
  int64_t seed = argc > 1 ? std::stoll(argv[1]) : 12345;
  int r = argc > 2 ? std::stoi(argv[2]) : 3;
  std::string stages = argc > 3 ? argv[3] : "features";
  int ox = argc > 5 ? std::stoi(argv[4]) : 0, oz = argc > 5 ? std::stoi(argv[5]) : 0;
  auto t0 = std::chrono::steady_clock::now();
  worldgen::Overworld ow(seed, stages);
  double init = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  t0 = std::chrono::steady_clock::now();
  int n = 0;
  for (int cx = ox - r; cx <= ox + r; cx++)
    for (int cz = oz - r; cz <= oz + r; cz++) {
      Chunk c;
      c.cx = cx, c.cz = cz;
      ow.pipeline.take(cx, cz, c);
      assert(c.get(0, MIN_Y, 0) == blocks::bedrock);
      n++;
    }
  double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  printf("init %.0f ms; %d full chunks in %.0f ms (%.1f ms/chunk incl. neighbours)\n", init, n, total, total / n);
  for (int s = 0; s < worldgen::STATUS_COUNT; s++) {
    int64_t c = ow.pipeline.step_count[size_t(s)];
    if (c) printf("  %-20s %6lld steps %8.2f ms/step\n", worldgen::status_name(worldgen::Status(s)), (long long)c,
                  ow.pipeline.step_ns[size_t(s)] / 1e6 / double(c));
  }
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  printf("peak RSS %.0f MB, protos held %zu\n", u.ru_maxrss / (1024.0 * 1024.0), ow.pipeline.loaded());
}
