// Structure smoke test / locate CLI: structure_test [seed] [x z] [structure ids...]
// Prints /locate results (compare with vanilla via test/locate_compare.py) and checks basic invariants.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <typeinfo>

#include "world.hpp"
#include "worldgen/overworld.hpp"
#include "worldgen/structure/hooks.hpp"
#include "worldgen/structure/jigsaw.hpp"
#include "worldgen/structure/nbt_io.hpp"
#include "worldgen/structure/template.hpp"

namespace {
struct FakeLevel : structure::Level {  // flat world at y = height, everything else air
  int height;
  explicit FakeLevel(int h) : height(h) {}
  int getBlock(int, int y, int) override { return y < height ? blocks::stone : blocks::air; }
  void setBlock(int, int, int, int) override {}
  int getHeight(structure::Heightmap, int, int) override { return height; }
  int64_t seed() const override { return 12345; }
  int biomeAt(int, int, int) override { return 0; }
  RandomSource& random() override { return r; }
  LegacyRandomSource r{1};
};

// Piece state written by one manager is restored by a fresh one (restart), not recomputed.
void persistCheck(worldgen::Overworld& ow) {
  std::string dir = "build/structure_test_persist";
  std::filesystem::remove_all(dir);
  std::vector<int> before;
  structure::BlockPos at;
  {
    structure::StructureWorld sw(ow, false);
    auto& m = sw.manager();
    m.setPersistDir(dir);
    auto f = m.findNearest({m.structure("minecraft:desert_pyramid")}, {0, 64, 0}, 100);
    assert(f);
    at = f->pos;
    auto st = m.starts(at.x >> 4, at.z >> 4);
    assert(st.size() == 1);
    FakeLevel level(70);  // first placement fixes heightPosition from this level
    LegacyRandomSource r(5);
    structure::BoundingBox bb(at.x, -63, at.z, at.x + 15, 319, at.z + 15);
    st[0]->placeInChunk(level, r, bb, at.x >> 4, at.z >> 4);
    for (auto& p : st[0]->pieces) p->saveState(before);
    m.saveDirty();
  }
  structure::StructureWorld sw(ow, false);
  auto& m = sw.manager();
  m.setPersistDir(dir);
  std::vector<int> after;
  for (auto& p : m.starts(at.x >> 4, at.z >> 4)[0]->pieces) p->saveState(after);
  assert(before == after);
  std::filesystem::remove_all(dir);
  printf("persist ok (%zu ints)\n", before.size());
}
}  // namespace

int main(int argc, char** argv) {
  int64_t seed = argc > 1 ? std::stoll(argv[1]) : 12345;
  int x = argc > 3 ? std::stoi(argv[2]) : 0, z = argc > 3 ? std::stoi(argv[3]) : 0;
  std::vector<std::string> ids;
  for (int i = 4; i < argc; i++) ids.push_back(argv[i]);
  if (ids.empty() && argc <= 2) {
    // Self-checks: template loading and a known start invariant.
    const auto& t = structure::Template::get("minecraft:igloo/top");
    assert(t.size.x == 7 && t.size.y == 5 && t.size.z == 8 && !t.palettes.empty());
    persistCheck(*std::make_unique<worldgen::Overworld>(seed, "structures"));
    ids = {"minecraft:village_plains", "minecraft:stronghold", "minecraft:desert_pyramid", "minecraft:monument",
           "minecraft:mineshaft", "minecraft:ruined_portal", "minecraft:shipwreck", "minecraft:ancient_city"};
  }
  worldgen::Overworld ow(seed, "structures");
  structure::StructureWorld sw(ow, false);
  auto& m = sw.manager();
  if (getenv("BENCH")) {  // BENCH=n: ms/chunk for an n x n square through the pipeline, structures on vs off
    int n = atoi(getenv("BENCH"));
    for (int on = 0; on < 2; on++) {
      worldgen::Overworld o(seed, "structures");
      std::unique_ptr<structure::StructureWorld> w;
      if (on) w = std::make_unique<structure::StructureWorld>(o, true);
      auto t0 = std::chrono::steady_clock::now();
      for (int cx = 0; cx < n; cx++)
        for (int cz = 0; cz < n; cz++) {
          Chunk c;
          c.cx = x / 16 + cx, c.cz = z / 16 + cz;
          o.pipeline.take(c.cx, c.cz, c);
        }
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / (n * n);
      printf("structures %s: %.2f ms/chunk\n", on ? "on" : "off", ms);
    }
    return 0;
  }
  if (getenv("BOXQ")) {  // BOXQ=x0,y0,z0,x1,y1,z1: pieces of nearby starts intersecting the box
    int a[6];
    sscanf(getenv("BOXQ"), "%d,%d,%d,%d,%d,%d", a, a + 1, a + 2, a + 3, a + 4, a + 5);
    structure::BoundingBox q(a[0], a[1], a[2], a[3], a[4], a[5]);
    for (int cx = (a[0] >> 4) - 9; cx <= (a[3] >> 4) + 9; cx++)
      for (int cz = (a[2] >> 4) - 9; cz <= (a[5] >> 4) + 9; cz++)
        for (auto& st : m.starts(cx, cz))
          for (auto& p : st->pieces)
            if (p->boundingBox.intersects(q)) {
              auto& b = p->boundingBox;
              printf("%s@%d,%d %s %d %d %d .. %d %d %d\n", st->structure->id.c_str(), cx, cz, typeid(*p).name(), b.minX, b.minY, b.minZ,
                     b.maxX, b.maxY, b.maxZ);
            }
    return 0;
  }
  if (getenv("PIECES")) {  // PIECES=cx,cz: dump the starts of that chunk
    int cx, cz;
    sscanf(getenv("PIECES"), "%d,%d", &cx, &cz);
    for (auto& st : m.starts(cx, cz)) {
      printf("start %s\n", st->structure->id.c_str());
      for (auto& p : st->pieces) {
        auto& b = p->boundingBox;
        std::string what = p->isPoolElement() ? static_cast<structure::PoolElementPiece*>(p.get())->element->location : "";
        printf("  %d %d %d .. %d %d %d %s\n", b.minX, b.minY, b.minZ, b.maxX, b.maxY, b.maxZ, what.c_str());
      }
    }
    return 0;
  }
  for (auto& id : ids) {
    std::vector<const structure::Structure*> wanted;
    bool tag = id[0] == '#';
    if (tag) {
      for (auto& n : structure::resolveTag("worldgen/structure", id.substr(1))) wanted.push_back(m.structure(n));
    } else {
      wanted.push_back(m.structure(id));
    }
    assert(wanted[0]);
    auto t0 = std::chrono::steady_clock::now();
    auto f = m.findNearest(wanted, {x, 64, z}, 100);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (f) printf("%s %d %d %s %.0fms\n", id.c_str(), f->pos.x, f->pos.z, f->structure->id.c_str(), ms);
    else printf("%s none %.0fms\n", id.c_str(), ms);
  }
}
