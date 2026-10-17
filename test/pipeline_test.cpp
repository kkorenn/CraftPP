// Pipeline smoke test + profile: generates an R x R square of FULL chunks through the overworld pipeline on one
// thread and prints ms/chunk per status and peak RSS. Usage: pipeline_test [seed] [radius] [stages] [cx cz] [datapack]
#undef NDEBUG
#include <sys/resource.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>
#include <future>
#include <stdexcept>
#include <cstring>

#include "world.hpp"
#include "worldgen/overworld.hpp"

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--failure-recovery") == 0) {
    worldgen::Pipeline pipeline;
    bool failed = false;
    pipeline.set_step(worldgen::Status::TERRAIN, [&](worldgen::Region&) {
      if (!failed) { failed = true; throw std::runtime_error("injected terrain failure"); }
    });
    try { pipeline.ensure(0, 0, worldgen::Status::FULL); assert(false); }
    catch (const std::runtime_error&) {}
    auto retry = std::async(std::launch::async, [&] { pipeline.ensure(0, 0, worldgen::Status::FULL); });
    // A failed prefetch must not leave a ticket that waits forever for its abandoned worker.
    assert(retry.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    retry.get();
    puts("pipeline failure recovery ok");
    return 0;
  }
  int64_t seed = argc > 1 ? std::stoll(argv[1]) : 12345;
  int r = argc > 2 ? std::stoi(argv[2]) : 3;
  std::string stages = argc > 3 ? argv[3] : "features";
  int ox = argc > 5 ? std::stoi(argv[4]) : 0, oz = argc > 5 ? std::stoi(argv[5]) : 0;
  std::string settings = argc > 7 ? argv[7] : "overworld";
  int minY = settings == "overworld" ? MIN_Y : 0, height = settings == "overworld" ? HEIGHT : 256;
  auto t0 = std::chrono::steady_clock::now();
  worldgen::Overworld ow(seed, stages, "vanilla/src/data", argc > 6 ? argv[6] : "", settings, minY, height);
  double init = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  t0 = std::chrono::steady_clock::now();
  int n = 0;
  bool reverse = getenv("PIPELINE_REVERSE"), hashes = getenv("PIPELINE_HASH");
  std::vector<std::pair<int, int>> order;
  for (int cx = ox - r; cx <= ox + r; cx++)
    for (int cz = oz - r; cz <= oz + r; cz++) order.push_back({cx, cz});
  if (reverse) std::reverse(order.begin(), order.end());
  std::vector<std::pair<std::pair<int, int>, uint64_t>> h;
  for (auto [cx, cz] : order) {
    Chunk c(minY, height);
    c.cx = cx, c.cz = cz;
    ow.pipeline.take(cx, cz, c);
    if (settings != "end") assert(c.get(0, minY, 0) == blocks::bedrock);
    uint64_t hv = 1469598103934665603ull;
    for (int y = minY; y < minY + height; y++)
      for (int i = 0; i < 256; i++) hv = (hv ^ c.get(i & 15, y, i >> 4)) * 1099511628211ull;
    h.push_back({{cx, cz}, hv});
    n++;
  }
  if (hashes) {
    std::sort(h.begin(), h.end());
    for (auto& [k, v] : h) printf("%d,%d %016llx\n", k.first, k.second, (unsigned long long)v);
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
#ifdef __APPLE__
  double peak_mib = u.ru_maxrss / (1024.0 * 1024.0);
#else
  double peak_mib = u.ru_maxrss / 1024.0;  // Linux reports KiB, macOS reports bytes.
#endif
  printf("peak RSS %.0f MiB, protos held %zu\n", peak_mib, ow.pipeline.loaded());
}
