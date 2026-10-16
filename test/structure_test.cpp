// Structure smoke test / locate CLI: structure_test [seed] [x z] [structure ids...]
// Prints /locate results (compare with vanilla via test/locate_compare.py) and checks basic invariants.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <typeinfo>

#include "world.hpp"
#include "worldgen/overworld.hpp"
#include "worldgen/structure/hooks.hpp"
#include "worldgen/structure/jigsaw.hpp"
#include "worldgen/structure/legacy.hpp"
#include "worldgen/structure/nbt_io.hpp"
#include "worldgen/structure/pieces.hpp"
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
// Re-places pieces chunk by chunk over the vanilla blocks they were generated into: every write that changes a
// block is a difference (fence/bars connections are compared by block only: vanilla post-processed them).
void checkAgainstVanilla(const char* file, const std::vector<structure::PieceP>& pieces) {
  FILE* f = fopen(file, "rb");
  assert(f);
  int32_t h[5];
  assert(fread(h, 4, 5, f) == 5);
  int cx0 = h[0], cz0 = h[1], n = h[2], minY = h[3], height = h[4];
  std::vector<uint16_t> blocks(size_t(n) * n * height * 256);
  assert(fread(blocks.data(), 2, blocks.size(), f) == blocks.size());
  fclose(f);
  struct VanillaLevel : FakeLevel {
    std::vector<uint16_t>& b;
    int cx0, cz0, n, minY, height;
    std::map<std::pair<std::string, std::string>, int> diffs;
    int examples = 0;
    VanillaLevel(std::vector<uint16_t>& b, int cx0, int cz0, int n, int minY, int height)
        : FakeLevel(0), b(b), cx0(cx0), cz0(cz0), n(n), minY(minY), height(height) {}
    uint16_t* at(int x, int y, int z) {
      int dx = (x >> 4) - cx0, dz = (z >> 4) - cz0;
      if (dx < 0 || dz < 0 || dx >= n || dz >= n || y < minY || y >= minY + height) return nullptr;
      return &b[((size_t(dz) * n + dx) * height + (y - minY)) * 256 + (z & 15) * 16 + (x & 15)];
    }
    int getBlock(int x, int y, int z) override { auto* p = at(x, y, z); return p ? *p : blocks::air; }
    std::vector<uint8_t> touched = std::vector<uint8_t>(b.size());
    void setBlock(int x, int y, int z, int state) override {
      if (auto* p = at(x, y, z)) *p = uint16_t(state), touched[size_t(p - b.data())] = 1;
    }
  };
  std::vector<uint16_t> placed = blocks;
  VanillaLevel level(placed, cx0, cz0, n, minY, height);
  LegacyRandomSource r(1);
  structure::BlockPos c = pieces[0]->boundingBox.center(), ref{c.x, pieces[0]->boundingBox.minY, c.z};  // StructureStart.placeInChunk
  for (int cz = cz0; cz < cz0 + n; cz++)
    for (int cx = cx0; cx < cx0 + n; cx++) {
      structure::BoundingBox chunkBB(cx * 16, minY, cz * 16, cx * 16 + 15, minY + height - 1, cz * 16 + 15);
      for (auto& p : pieces)
        if (p->boundingBox.intersects(chunkBB)) p->postProcess(level, r, chunkBB, cx, cz, ref);
    }
  // Vanilla's later feature step decorates over the fortress (nether vegetation, ...): those are not differences.
  const auto& R = bs::R();
  std::map<std::pair<std::string, std::string>, int> diffs;
  int total = 0, examples = 0;
  for (size_t i = 0; i < blocks.size(); i++) {
    if (!level.touched[i] || placed[i] == blocks[i]) continue;
    int v = blocks[i], o = placed[i];
    if (R.block(R.block_of(o)).is_a("CrossCollisionBlock") && R.block_of(o) == R.block_of(v)) continue;  // post-processed
    std::string vn = R.name(v);
    if (vn.find("wart_block") != std::string::npos || vn.find("_stem") != std::string::npos || vn.find("shroomlight") != std::string::npos ||
        vn.find("vines") != std::string::npos || vn.find("lava") != std::string::npos || vn.find("fungus") != std::string::npos ||
        vn.find("roots") != std::string::npos)
      continue;
    total++;
    diffs[{vn, R.name(o)}]++;
    if (examples++ < 10) {
      size_t col = i / (size_t(height) * 256), rem = i % (size_t(height) * 256);
      int x = (cx0 + int(col % size_t(n))) * 16 + int(rem % 16), z = (cz0 + int(col / size_t(n))) * 16 + int(rem / 16 % 16);
      printf("  (%d, %d, %d): vanilla %s ours %s\n", x, minY + int(rem / 256), z, vn.c_str(), R.name(o).c_str());
    }
  }
  printf("%d differing writes\n", total);
  for (auto& [k, v] : diffs) printf("  %s -> %s: %d\n", k.first.c_str(), k.second.c_str(), v);
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
  if (getenv("STUB")) {  // STUB=id,cx,cz: pieces of that structure's start in the chunk, biomes ignored, flat heights
    char id[128];
    int cx, cz;
    sscanf(getenv("STUB"), "%127[^,],%d,%d", id, &cx, &cz);
    struct FlatGen : structure::Generator {
      int baseHeight(int, int, structure::Heightmap) override { return 64; }
      void baseColumn(int, int, std::vector<int>& out) override { out.assign(structure::GEN_HEIGHT, blocks::air); }
      int noiseBiome(int, int, int) override { return 0; }
      float biomeTemperature(int, structure::BlockPos) override { return 0.5f; }
      int64_t seed() const override { return s; }
      int64_t s;
    } gen;
    gen.s = seed;
    const structure::Structure* st = m.structure(id);
    assert(st);
    std::vector<uint8_t> all(structure::biomeNames().size(), 1);
    structure::GenContext ctx(gen, seed, cx, cz, all);
    auto stub = st->findGenerationPoint(ctx);
    if (!stub) return printf("no start\n"), 0;
    structure::Builder b;
    if (stub->built) b.pieces = std::move(stub->built->pieces);
    else stub->generator(b);
    if (const char* f = getenv("CHECK")) {
      checkAgainstVanilla(f, b.pieces);
      return 0;
    }
    printf("pieces %zu\n", b.pieces.size());
    for (auto& p : b.pieces) {
      auto& bb = p->boundingBox;
      printf("%d %d %d %d %d %d\n", bb.minX, bb.minY, bb.minZ, bb.maxX, bb.maxY, bb.maxZ);
    }
    return 0;
  }
  if (getenv("ENDCITY")) {  // ENDCITY=cx,cz,y: end city pieces of a start in that chunk at height y (vanilla's start y)
    int cx, cz, y;
    sscanf(getenv("ENDCITY"), "%d,%d,%d", &cx, &cz, &y);
    WorldgenRandom r(std::make_unique<LegacyRandomSource>(0));
    r.setLargeFeatureSeed(seed, cx, cz);
    structure::Rotation rot = structure::getRandomRotation(r);
    structure::Builder b;
    structure::generateEndCity({cx * 16 + 7, y, cz * 16 + 7}, rot, b, r);
    if (const char* f = getenv("CHECK")) {
      checkAgainstVanilla(f, b.pieces);
      return 0;
    }
    printf("pieces %zu\n", b.pieces.size());
    for (auto& p : b.pieces) {
      auto& bb = p->boundingBox;
      auto* t = static_cast<structure::TemplatePiece*>(p.get());
      printf("%d %d %d %d %d %d %s %d %d %d %d\n", bb.minX, bb.minY, bb.minZ, bb.maxX, bb.maxY, bb.maxZ, t->templateName.c_str(),
             t->templatePosition.x, t->templatePosition.y, t->templatePosition.z, int(t->settings.rotation));
    }
    return 0;
  }
  if (getenv("FORTRESS")) {  // FORTRESS=cx,cz: fortress pieces of a start in that chunk (test/structure_pieces_vanilla.py)
    int cx, cz;
    sscanf(getenv("FORTRESS"), "%d,%d", &cx, &cz);
    WorldgenRandom r(std::make_unique<LegacyRandomSource>(0));
    r.setLargeFeatureSeed(seed, cx, cz);
    structure::Builder b;
    structure::generateFortress(b, r, cx * 16 + 2, cz * 16 + 2);
    b.moveInsideHeights(r, 48, 70);
    if (const char* f = getenv("CHECK")) {  // CHECK=blocks.bin (structure_pieces_vanilla.py --blocks-out): re-place on vanilla's blocks
      checkAgainstVanilla(f, b.pieces);
      return 0;
    }
    printf("pieces %zu\n", b.pieces.size());
    for (auto& p : b.pieces) {
      auto& bb = p->boundingBox;
      printf("%s %d %d %d %d %d %d %d\n", typeid(*p).name(), bb.minX, bb.minY, bb.minZ, bb.maxX, bb.maxY, bb.maxZ,
             p->orientation() ? structure::data2d(*p->orientation()) : -1);
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
