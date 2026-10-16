// Structure smoke test / locate CLI: structure_test [seed] [x z] [structure ids...]
// Prints /locate results (compare with vanilla via test/locate_compare.py) and checks basic invariants.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>

#include "worldgen/overworld.hpp"
#include "worldgen/structure/hooks.hpp"
#include "worldgen/structure/jigsaw.hpp"
#include "worldgen/structure/nbt_io.hpp"
#include "worldgen/structure/template.hpp"

int main(int argc, char** argv) {
  int64_t seed = argc > 1 ? std::stoll(argv[1]) : 12345;
  int x = argc > 3 ? std::stoi(argv[2]) : 0, z = argc > 3 ? std::stoi(argv[3]) : 0;
  std::vector<std::string> ids;
  for (int i = 4; i < argc; i++) ids.push_back(argv[i]);
  if (ids.empty() && argc <= 2) {
    // Self-checks: template loading and a known start invariant.
    const auto& t = structure::Template::get("minecraft:igloo/top");
    assert(t.size.x == 7 && t.size.y == 5 && t.size.z == 8 && !t.palettes.empty());
    ids = {"minecraft:village_plains", "minecraft:stronghold", "minecraft:desert_pyramid", "minecraft:monument",
           "minecraft:mineshaft", "minecraft:ruined_portal", "minecraft:shipwreck", "minecraft:ancient_city"};
  }
  worldgen::Overworld ow(seed, "structures");
  structure::StructureWorld sw(ow, false);
  auto& m = sw.manager();
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
