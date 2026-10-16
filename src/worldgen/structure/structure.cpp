#include "worldgen/structure/structure.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#include "worldgen/json.hpp"

namespace structure {

// ------------------------------------------------------------------------------------------- directions
Dir opposite(Dir d) {
  static const Dir o[6] = {Dir::UP, Dir::DOWN, Dir::SOUTH, Dir::NORTH, Dir::EAST, Dir::WEST};
  return o[(int)d];
}
Dir clockWise(Dir d) {
  switch (d) {
    case Dir::NORTH: return Dir::EAST;
    case Dir::EAST: return Dir::SOUTH;
    case Dir::SOUTH: return Dir::WEST;
    case Dir::WEST: return Dir::NORTH;
    default: return d;
  }
}
Dir counterClockWise(Dir d) {
  switch (d) {
    case Dir::NORTH: return Dir::WEST;
    case Dir::WEST: return Dir::SOUTH;
    case Dir::SOUTH: return Dir::EAST;
    case Dir::EAST: return Dir::NORTH;
    default: return d;
  }
}
int data2d(Dir d) {
  switch (d) {
    case Dir::SOUTH: return 0;
    case Dir::WEST: return 1;
    case Dir::NORTH: return 2;
    case Dir::EAST: return 3;
    default: return -1;
  }
}
Dir from2d(int v) {
  static const Dir t[4] = {Dir::SOUTH, Dir::WEST, Dir::NORTH, Dir::EAST};
  return t[((v % 4) + 4) % 4];
}
static const char* DIR_NAMES[6] = {"down", "up", "north", "south", "west", "east"};
const char* dirName(Dir d) { return DIR_NAMES[(int)d]; }
Dir dirByName(std::string_view n) {
  for (int i = 0; i < 6; i++)
    if (n == DIR_NAMES[i]) return (Dir)i;
  return Dir::NORTH;
}
Dir rotate(Rotation r, Dir d) {
  if (d == Dir::UP || d == Dir::DOWN) return d;
  switch (r) {
    case Rotation::CLOCKWISE_90: return clockWise(d);
    case Rotation::CLOCKWISE_180: return opposite(d);
    case Rotation::COUNTERCLOCKWISE_90: return counterClockWise(d);
    default: return d;
  }
}
Dir mirror(Mirror m, Dir d) {
  if (m == Mirror::FRONT_BACK && isXAxis(d)) return opposite(d);
  if (m == Mirror::LEFT_RIGHT && isZAxis(d)) return opposite(d);
  return d;
}
Rotation rotationFor(Mirror m, Dir d) {
  return (m != Mirror::LEFT_RIGHT || !isZAxis(d)) && (m != Mirror::FRONT_BACK || !isXAxis(d)) ? Rotation::NONE
                                                                                               : Rotation::CLOCKWISE_180;
}
Rotation getRandomRotation(RandomSource& r) { return (Rotation)r.nextInt(4); }
std::array<Rotation, 4> shuffledRotations(RandomSource& r) {
  std::vector<Rotation> v = {Rotation::NONE, Rotation::CLOCKWISE_90, Rotation::CLOCKWISE_180, Rotation::COUNTERCLOCKWISE_90};
  shuffle(v, r);
  return {v[0], v[1], v[2], v[3]};
}
Rotation rotationAdd(Rotation a, Rotation b) { return (Rotation)(((int)a + (int)b) % 4); }
Dir randomHorizontal(RandomSource& r) { return HORIZONTAL[r.nextInt(4)]; }

Heightmap heightmapByName(std::string_view n) {
  if (n == "WORLD_SURFACE_WG") return Heightmap::WORLD_SURFACE_WG;
  if (n == "OCEAN_FLOOR_WG") return Heightmap::OCEAN_FLOOR_WG;
  if (n == "WORLD_SURFACE") return Heightmap::WORLD_SURFACE;
  if (n == "OCEAN_FLOOR") return Heightmap::OCEAN_FLOOR;
  if (n == "MOTION_BLOCKING") return Heightmap::MOTION_BLOCKING;
  return Heightmap::MOTION_BLOCKING_NO_LEAVES;
}

// ------------------------------------------------------------------------------------------- BoundingBox
BoundingBox::BoundingBox(int x0, int y0, int z0, int x1, int y1, int z1)
    : minX(std::min(x0, x1)), minY(std::min(y0, y1)), minZ(std::min(z0, z1)),
      maxX(std::max(x0, x1)), maxY(std::max(y0, y1)), maxZ(std::max(z0, z1)) {}

BoundingBox BoundingBox::fromCorners(BlockPos a, BlockPos b) { return {a.x, a.y, a.z, b.x, b.y, b.z}; }

BoundingBox BoundingBox::orientBox(int fx, int fy, int fz, int ox, int oy, int oz, int w, int h, int d, Dir dir) {
  switch (dir) {
    case Dir::NORTH: return {fx + ox, fy + oy, fz - d + 1 + oz, fx + w - 1 + ox, fy + h - 1 + oy, fz + oz};
    case Dir::WEST: return {fx - d + 1 + oz, fy + oy, fz + ox, fx + oz, fy + h - 1 + oy, fz + w - 1 + ox};
    case Dir::EAST: return {fx + oz, fy + oy, fz + ox, fx + d - 1 + oz, fy + h - 1 + oy, fz + w - 1 + ox};
    default: return {fx + ox, fy + oy, fz + oz, fx + w - 1 + ox, fy + h - 1 + oy, fz + d - 1 + oz};
  }
}

BoundingBox& BoundingBox::encapsulate(const BoundingBox& o) {
  minX = std::min(minX, o.minX), minY = std::min(minY, o.minY), minZ = std::min(minZ, o.minZ);
  maxX = std::max(maxX, o.maxX), maxY = std::max(maxY, o.maxY), maxZ = std::max(maxZ, o.maxZ);
  return *this;
}
BoundingBox& BoundingBox::encapsulate(BlockPos p) { return encapsulate(BoundingBox(p)); }

// ------------------------------------------------------------------------------------------- Piece
void Piece::setOrientation(std::optional<Dir> d) {
  orientation_ = d;
  mirror_ = Mirror::NONE, rotation_ = Rotation::NONE;
  if (!d) return;
  switch (*d) {
    case Dir::SOUTH: mirror_ = Mirror::LEFT_RIGHT; break;
    case Dir::WEST: mirror_ = Mirror::LEFT_RIGHT, rotation_ = Rotation::CLOCKWISE_90; break;
    case Dir::EAST: rotation_ = Rotation::CLOCKWISE_90; break;
    default: break;
  }
}

BoundingBox Piece::makeBoundingBox(int x, int y, int z, Dir d, int w, int h, int depth) {
  return isZAxis(d) ? BoundingBox(x, y, z, x + w - 1, y + h - 1, z + depth - 1)
                    : BoundingBox(x, y, z, x + depth - 1, y + h - 1, z + w - 1);
}

int Piece::worldX(int x, int z) const {
  if (!orientation_) return x;
  switch (*orientation_) {
    case Dir::NORTH: case Dir::SOUTH: return boundingBox.minX + x;
    case Dir::WEST: return boundingBox.maxX - z;
    case Dir::EAST: return boundingBox.minX + z;
    default: return x;
  }
}
int Piece::worldZ(int x, int z) const {
  if (!orientation_) return z;
  switch (*orientation_) {
    case Dir::NORTH: return boundingBox.maxZ - z;
    case Dir::SOUTH: return boundingBox.minZ + z;
    case Dir::WEST: case Dir::EAST: return boundingBox.minZ + x;
    default: return z;
  }
}

void Piece::placeBlock(Level& level, int state, int x, int y, int z, const BoundingBox& chunkBB) {
  BlockPos p = worldPos(x, y, z);
  if (!chunkBB.isInside(p) || !canBeReplaced(level, x, y, z, chunkBB)) return;
  const Blocks& b = Blocks::get();
  if (mirror_ != Mirror::NONE) state = b.mirror(state, mirror_);
  if (rotation_ != Rotation::NONE) state = b.rotate(state, rotation_);
  level.setBlock(p.x, p.y, p.z, state);
  // ponytail: fluid ticks and SHAPE_CHECK_BLOCKS post-processing marks are not tracked (no fluid ticking yet).
}

int Piece::getBlock(Level& level, int x, int y, int z, const BoundingBox& chunkBB) {
  BlockPos p = worldPos(x, y, z);
  return chunkBB.isInside(p) ? level.getBlock(p.x, p.y, p.z) : 0;
}

bool Piece::isInterior(Level& level, int x, int y, int z, const BoundingBox& chunkBB) {
  BlockPos p = worldPos(x, y + 1, z);
  return chunkBB.isInside(p) && p.y < level.getHeight(Heightmap::OCEAN_FLOOR_WG, p.x, p.z);
}

void Piece::generateAirBox(Level& level, const BoundingBox& bb, int x0, int y0, int z0, int x1, int y1, int z1) {
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      for (int z = z0; z <= z1; z++) placeBlock(level, 0, x, y, z, bb);
}

void Piece::generateBox(Level& level, const BoundingBox& bb, int x0, int y0, int z0, int x1, int y1, int z1, int edge, int fill,
                        bool skipAir) {
  const Blocks& b = Blocks::get();
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      for (int z = z0; z <= z1; z++) {
        if (skipAir && b.isAir(getBlock(level, x, y, z, bb))) continue;
        bool inner = y != y0 && y != y1 && x != x0 && x != x1 && z != z0 && z != z1;
        placeBlock(level, inner ? fill : edge, x, y, z, bb);
      }
}

void Piece::generateBox(Level& level, const BoundingBox& bb, int x0, int y0, int z0, int x1, int y1, int z1, bool skipAir,
                        RandomSource& random, BlockSelector& sel) {
  const Blocks& b = Blocks::get();
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      for (int z = z0; z <= z1; z++) {
        if (skipAir && b.isAir(getBlock(level, x, y, z, bb))) continue;
        sel.select(random, x, y, z, y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1);
        placeBlock(level, sel.next, x, y, z, bb);
      }
}

void Piece::generateMaybeBox(Level& level, const BoundingBox& bb, RandomSource& random, float p, int x0, int y0, int z0, int x1,
                             int y1, int z1, int edge, int fill, bool skipAir, bool hasToBeInside) {
  const Blocks& b = Blocks::get();
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      for (int z = z0; z <= z1; z++) {
        if (random.nextFloat() > p) continue;
        if (skipAir && b.isAir(getBlock(level, x, y, z, bb))) continue;
        if (hasToBeInside && !isInterior(level, x, y, z, bb)) continue;
        bool inner = y != y0 && y != y1 && x != x0 && x != x1 && z != z0 && z != z1;
        placeBlock(level, inner ? fill : edge, x, y, z, bb);
      }
}

void Piece::maybeGenerateBlock(Level& level, const BoundingBox& bb, RandomSource& random, float p, int x, int y, int z, int state) {
  if (random.nextFloat() < p) placeBlock(level, state, x, y, z, bb);
}

void Piece::generateUpperHalfSphere(Level& level, const BoundingBox& bb, int x0, int y0, int z0, int x1, int y1, int z1, int fill,
                                    bool skipAir) {
  const Blocks& b = Blocks::get();
  float diagX = (float)(x1 - x0 + 1), diagY = (float)(y1 - y0 + 1), diagZ = (float)(z1 - z0 + 1);
  float cx = (float)x0 + diagX / 2.0f, cz = (float)z0 + diagZ / 2.0f;
  for (int y = y0; y <= y1; y++) {
    float ny = (float)(y - y0) / diagY;
    for (int x = x0; x <= x1; x++) {
      float nx = ((float)x - cx) / (diagX * 0.5f);
      for (int z = z0; z <= z1; z++) {
        float nz = ((float)z - cz) / (diagZ * 0.5f);
        if (skipAir && b.isAir(getBlock(level, x, y, z, bb))) continue;
        float dist = nx * nx + ny * ny + nz * nz;
        if (dist <= 1.05f) placeBlock(level, fill, x, y, z, bb);
      }
    }
  }
}

bool Piece::isReplaceableByStructures(int s) {
  const Blocks& b = Blocks::get();
  return b.isAir(s) || (b.flags(s) & BS_LIQUID) || b.is(s, "glow_lichen") || b.is(s, "seagrass") || b.is(s, "tall_seagrass");
}

void Piece::fillColumnDown(Level& level, int state, int x, int startY, int z, const BoundingBox& bb) {
  BlockPos p = worldPos(x, startY, z);
  if (!bb.isInside(p)) return;
  while (isReplaceableByStructures(level.getBlock(p.x, p.y, p.z)) && p.y > MIN_Y + 1) {
    level.setBlock(p.x, p.y, p.z, state);
    p.y--;
  }
}

int Piece::reorient(Level& level, BlockPos pos, int state) {
  const Blocks& b = Blocks::get();
  std::optional<Dir> solid;
  for (Dir d : HORIZONTAL) {
    BlockPos r = pos.relative(d);
    int s = level.getBlock(r.x, r.y, r.z);
    if (b.is(s, "chest")) return state;
    if (b.flags(s) & BS_SOLID_RENDER) {
      if (solid) {
        solid.reset();
        break;
      }
      solid = d;
    }
  }
  if (solid) return b.with(state, "facing", dirName(opposite(*solid)));
  Dir lock = dirByName(b.prop(state, "facing"));
  auto solidAt = [&](Dir d) {
    BlockPos r = pos.relative(d);
    return (b.flags(level.getBlock(r.x, r.y, r.z)) & BS_SOLID_RENDER) != 0;
  };
  if (solidAt(lock)) lock = opposite(lock);
  if (solidAt(lock)) lock = clockWise(lock);
  if (solidAt(lock)) lock = opposite(lock);
  return b.with(state, "facing", dirName(lock));
}

bool Piece::createChest(Level& level, const BoundingBox& bb, RandomSource& random, int x, int y, int z) {
  return createChest(level, bb, random, worldPos(x, y, z), -1);
}

bool Piece::createChest(Level& level, const BoundingBox& bb, RandomSource& random, BlockPos pos, int state) {
  const Blocks& b = Blocks::get();
  if (!bb.isInside(pos) || b.is(level.getBlock(pos.x, pos.y, pos.z), "chest")) return false;
  if (state < 0) state = reorient(level, pos, b.defaultState("chest"));
  level.setBlock(pos.x, pos.y, pos.z, state);
  if (b.is(state, "chest") || b.is(state, "trapped_chest")) random.nextLong();  // setLootTable(table, random.nextLong())
  return true;
}

bool Piece::createDispenser(Level& level, const BoundingBox& bb, RandomSource& random, int x, int y, int z, Dir facing) {
  const Blocks& b = Blocks::get();
  BlockPos pos = worldPos(x, y, z);
  if (!bb.isInside(pos) || b.is(level.getBlock(pos.x, pos.y, pos.z), "dispenser")) return false;
  placeBlock(level, b.with(b.defaultState("dispenser"), "facing", dirName(facing)), x, y, z, bb);
  if (b.is(level.getBlock(pos.x, pos.y, pos.z), "dispenser")) random.nextLong();
  return true;
}

// ------------------------------------------------------------------------------------------- Builder
Piece* Builder::findCollisionPiece(const BoundingBox& box) const {
  for (auto& p : pieces)
    if (p->boundingBox.intersects(box)) return p.get();
  return nullptr;
}

BoundingBox Builder::boundingBox() const {
  BoundingBox b = pieces.at(0)->boundingBox;
  for (auto& p : pieces) b.encapsulate(p->boundingBox);
  return b;
}

int Builder::moveBelowSeaLevel(int seaLevel, int minY, RandomSource& random, int offset) {
  int maxY = seaLevel - offset;
  BoundingBox bb = boundingBox();
  int y1 = bb.ySpan() + minY + 1;
  if (y1 < maxY) y1 += random.nextInt(maxY - y1);
  int dy = y1 - bb.maxY;
  offsetPiecesVertically(dy);
  return dy;
}

void Builder::moveInsideHeights(RandomSource& random, int lowest, int highest) {
  BoundingBox bb = boundingBox();
  int span = highest - lowest + 1 - bb.ySpan();
  int y0 = span > 1 ? lowest + random.nextInt(span) : lowest;
  offsetPiecesVertically(y0 - bb.minY);
}

// ------------------------------------------------------------------------------------------- context
static WorldgenRandom makeRandom(int64_t seed, int cx, int cz) {
  WorldgenRandom r(std::make_unique<LegacyRandomSource>(0));
  r.setLargeFeatureSeed(seed, cx, cz);
  return r;
}

GenContext::GenContext(Generator& g, int64_t seed, int cx, int cz, const std::vector<uint8_t>& valid)
    : gen(g), seed(seed), cx(cx), cz(cz), random(makeRandom(seed, cx, cz)), validBiome(valid) {}

bool GenContext::couldStructureExistInColumn(int x, int z, int minY, int maxY) {
  for (int qy = minY >> 2; qy <= (maxY >> 2); qy++)
    if (validBiome[gen.noiseBiome(x >> 2, qy, z >> 2)]) return true;
  return false;
}

std::optional<Stub> GenContext::onTopOfChunkCenter(Heightmap h, std::function<void(Builder&)> g, bool biomeCheck) {
  if (biomeCheck && !couldValidBiomeExistOnTopOfChunkCenter()) return std::nullopt;
  int x = cx * 16 + 8, z = cz * 16 + 8;
  return Stub{{x, gen.firstOccupiedHeight(x, z, h), z}, std::move(g), nullptr};
}

int GenContext::lowestY(int minX, int minZ, int sizeX, int sizeZ) {
  int a = gen.firstOccupiedHeight(minX, minZ, Heightmap::WORLD_SURFACE_WG);
  int b = gen.firstOccupiedHeight(minX, minZ + sizeZ, Heightmap::WORLD_SURFACE_WG);
  int c = gen.firstOccupiedHeight(minX + sizeX, minZ, Heightmap::WORLD_SURFACE_WG);
  int d = gen.firstOccupiedHeight(minX + sizeX, minZ + sizeZ, Heightmap::WORLD_SURFACE_WG);
  return std::min(std::min(a, b), std::min(c, d));
}

int GenContext::meanFirstOccupiedHeight(int minX, int sizeX, int minZ, int sizeZ) {
  int a = gen.firstOccupiedHeight(minX, minZ, Heightmap::WORLD_SURFACE_WG);
  int b = gen.firstOccupiedHeight(minX, minZ + sizeZ, Heightmap::WORLD_SURFACE_WG);
  int c = gen.firstOccupiedHeight(minX + sizeX, minZ, Heightmap::WORLD_SURFACE_WG);
  int d = gen.firstOccupiedHeight(minX + sizeX, minZ + sizeZ, Heightmap::WORLD_SURFACE_WG);
  return (a + b + c + d) / 4;
}

std::optional<Stub> Structure::findValidGenerationPoint(GenContext& ctx) const {
  auto stub = findGenerationPoint(ctx);
  if (stub && !ctx.validAt(stub->position)) return std::nullopt;
  return stub;
}

// ------------------------------------------------------------------------------------------- StructureStart
BoundingBox StructureStart::boundingBox() const {
  BoundingBox b = pieces.at(0)->boundingBox;
  for (auto& p : pieces) b.encapsulate(p->boundingBox);
  return structure->adjustBoundingBox(b);
}

void StructureStart::placeInChunk(Level& level, RandomSource& random, const BoundingBox& chunkBB, int ccx, int ccz) {
  if (pieces.empty()) return;
  std::lock_guard lock(placeLock);
  const BoundingBox& centerBB = pieces[0]->boundingBox;
  BlockPos c = centerBB.center();
  BlockPos ref{c.x, centerBB.minY, c.z};
  for (auto& p : pieces)
    if (p->boundingBox.intersects(chunkBB)) p->postProcess(level, random, chunkBB, ccx, ccz, ref);
  structure->afterPlace(level, random, chunkBB, ccx, ccz, pieces);
  if (owner) {
    std::lock_guard l(owner->persistLock_);
    owner->dirty_.insert(this);  // starts stay cached (never freed), so the raw pointer is stable
  }
}

// ------------------------------------------------------------------------------------------- placement
static int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

std::pair<int, int> Placement::potentialStructureChunk(int64_t seed, int x, int z) const {
  int gx = floorDiv(x, spacing), gz = floorDiv(z, spacing);
  WorldgenRandom r(std::make_unique<LegacyRandomSource>(0));
  r.setLargeFeatureWithSalt(seed, gx, gz, salt);
  int limit = spacing - separation;
  auto eval = [&] { return triangular ? (r.nextInt(limit) + r.nextInt(limit)) / 2 : r.nextInt(limit); };
  int sx = eval();
  int sz = eval();
  return {gx * spacing + sx, gz * spacing + sz};
}

bool Placement::applyAdditionalChunkRestrictions(int x, int z, int64_t seed) const {
  if (!(frequency < 1.0f)) return true;
  WorldgenRandom r(std::make_unique<LegacyRandomSource>(0));
  switch (reduction) {
    case DEFAULT:
      r.setLargeFeatureWithSalt(seed, salt, x, z);
      return r.nextFloat() < frequency;
    case LEGACY_TYPE_1: {
      int cx = x >> 4, cz = z >> 4;
      r.setSeed((int64_t)(cx ^ (int32_t)((uint32_t)cz << 4)) ^ seed);
      r.nextInt();
      return r.nextInt((int)(1.0f / frequency)) == 0;
    }
    case LEGACY_TYPE_2:
      r.setLargeFeatureWithSalt(seed, x, z, 10387320);
      return r.nextFloat() < frequency;
    case LEGACY_TYPE_3:
      r.setLargeFeatureSeed(seed, x, z);
      return r.nextDouble() < (double)frequency;
  }
  return true;
}

// ------------------------------------------------------------------------------------------- manager
namespace {
std::vector<std::string> sortedJsonIds(const std::string& folder) {
  std::vector<std::string> ids;
  std::string dir = dataRoot() + "/minecraft/worldgen/" + folder;
  for (auto& e : std::filesystem::recursive_directory_iterator(dir))
    if (e.path().extension() == ".json")
      ids.push_back(std::filesystem::relative(e.path(), dir).replace_extension().generic_string());
  // Registry order: the resource TreeMap sorts full file identifiers ("<id>.json").
  std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) { return a + ".json" < b + ".json"; });
  return ids;
}
}  // namespace

StructureManager::StructureManager(Generator& gen, std::vector<uint8_t> possibleBiomes)
    : gen_(gen), seed_(gen.seed()), possible_(std::move(possibleBiomes)) {
  std::map<std::string, const Structure*> byId;
  for (auto& id : sortedJsonIds("structure")) {
    Json j = Json::parseFile(dataRoot() + "/minecraft/worldgen/structure/" + id + ".json");
    std::string type = j["type"].asString();
    if (type.rfind("minecraft:", 0) == 0) type = type.substr(10);
    auto s = makeStructure(type, j);
    if (!s) throw std::runtime_error("structure: unsupported type " + type);
    s->id = "minecraft:" + id;
    s->type = type;
    s->biomes = biomeSet(j["biomes"]);
    static const char* STEPS[STEP_COUNT] = {"raw_generation", "lakes", "local_modifications", "underground_structures",
                                            "surface_structures", "strongholds", "underground_ores", "underground_decoration",
                                            "fluid_springs", "vegetal_decoration", "top_layer_modification"};
    for (int i = 0; i < STEP_COUNT; i++)
      if (j["step"].asString() == STEPS[i]) s->step = i;
    std::string ta = j.find("terrain_adaptation") ? j["terrain_adaptation"].asString() : "none";
    s->adaptation = ta == "bury" ? TerrainAdjustment::BURY : ta == "beard_thin" ? TerrainAdjustment::BEARD_THIN
                    : ta == "beard_box" ? TerrainAdjustment::BEARD_BOX : ta == "encapsulate" ? TerrainAdjustment::ENCAPSULATE
                    : TerrainAdjustment::NONE;
    s->registryIndex = (int)structures_.size();
    byId[s->id] = s.get();
    structures_.push_back(std::move(s));
  }
  std::map<std::string, StructureSet*> setById;
  std::vector<std::pair<StructureSet*, std::string>> exclusions;
  for (auto& id : sortedJsonIds("structure_set")) {
    Json j = Json::parseFile(dataRoot() + "/minecraft/worldgen/structure_set/" + id + ".json");
    auto set = std::make_unique<StructureSet>();
    set->id = "minecraft:" + id;
    for (auto& e : j["structures"].arr) set->structures.push_back({byId.at(fullId(e["structure"].asString())), e["weight"].asInt()});
    const Json& p = j["placement"];
    Placement& pl = set->placement;
    std::string t = p["type"].asString();
    pl.kind = t == "minecraft:concentric_rings" ? Placement::CONCENTRIC_RINGS : Placement::RANDOM_SPREAD;
    if (auto* o = p.find("locate_offset")) pl.locateOffset = {(*o)[0].asInt(), (*o)[1].asInt(), (*o)[2].asInt()};
    if (auto* f = p.find("frequency_reduction_method")) {
      std::string m = f->asString();
      pl.reduction = m == "legacy_type_1" ? Placement::LEGACY_TYPE_1 : m == "legacy_type_2" ? Placement::LEGACY_TYPE_2
                     : m == "legacy_type_3" ? Placement::LEGACY_TYPE_3 : Placement::DEFAULT;
    }
    if (auto* f = p.find("frequency")) pl.frequency = f->asFloat();
    pl.salt = p["salt"].asInt();
    if (auto* ex = p.find("exclusion_zone")) {
      pl.exclusionChunks = (*ex)["chunk_count"].asInt();
      exclusions.push_back({set.get(), fullId((*ex)["other_set"].asString())});
    }
    if (pl.kind == Placement::RANDOM_SPREAD) {
      pl.spacing = p["spacing"].asInt();
      pl.separation = p["separation"].asInt();
      pl.triangular = p.find("spread_type") && (*p.find("spread_type")).asString() == "triangular";
    } else {
      pl.distance = p["distance"].asInt(), pl.spread = p["spread"].asInt(), pl.count = p["count"].asInt();
      pl.preferredBiomes = biomeSet(p["preferred_biomes"]);
    }
    setById[set->id] = set.get();
    sets_.push_back(std::move(set));
  }
  for (auto& [s, other] : exclusions) s->placement.exclusionSet = setById.at(other);
  // ChunkGeneratorStructureState: sets with any structure whose biomes the biome source can produce.
  placementsFor_.resize(structures_.size());
  auto possible = [&](const Structure* s) {
    for (size_t i = 0; i < s->biomes.size() && i < possible_.size(); i++)
      if (s->biomes[i] && possible_[i]) return true;
    return false;
  };
  for (auto& set : sets_) {
    bool any = false;
    for (auto& e : set->structures)
      if (possible(e.structure)) {
        placementsFor_[e.structure->registryIndex].push_back(set.get());
        any = true;
      }
    if (any) possibleSets_.push_back(set.get());
  }
}

StructureManager::~StructureManager() = default;

const Structure* StructureManager::structure(std::string_view id) const {
  std::string f = fullId(id);
  for (auto& s : structures_)
    if (s->id == f) return s.get();
  return nullptr;
}

const std::vector<std::pair<int, int>>& StructureManager::ringPositions(const StructureSet& set) {
  std::lock_guard lock(ringsLock_);
  auto it = rings_.find(&set);
  if (it != rings_.end()) return it->second;
  // ChunkGeneratorStructureState.generateRingPositions (findBiomeHorizontal per position, in order).
  const Placement& p = set.placement;
  std::vector<std::pair<int, int>> out;
  LegacyRandomSource random(0);
  random.setSeed(seed_);
  double angle = random.nextDouble() * M_PI * 2.0;
  int positionInCircle = 0, circle = 0, spread = p.spread;
  for (int i = 0; i < p.count; i++) {
    double dist = (double)(4 * p.distance + p.distance * circle * 6) + (random.nextDouble() - 0.5) * (double)(p.distance * 2.5);
    auto round = [](double v) {  // Math.round: nearest, ties up (exact for 0.49999999999999994)
      double f = std::floor(v);
      return (int)Mth::d2l(v - f >= 0.5 ? f + 1 : f);
    };
    int initialX = round(std::cos(angle) * dist);
    int initialZ = round(std::sin(angle) * dist);
    auto search = random.fork();
    // BiomeSource.findBiomeHorizontal(x*16+8, 0, z*16+8, 112, preferred, search) with skipSteps 1, not closest.
    int ncx = (initialX * 16 + 8) >> 2, ncz = (initialZ * 16 + 8) >> 2, radius = 112 >> 2;
    std::optional<std::pair<int, int>> result;
    int found = 0;
    for (int z = -radius; z <= radius; z++)
      for (int x = -radius; x <= radius; x++) {
        int b = gen_.noiseBiome(ncx + x, 0, ncz + z);
        if (!p.preferredBiomes[b]) continue;
        if (!result || search->nextInt(found + 1) == 0) result = {(ncx + x) << 2, (ncz + z) << 2};
        found++;
      }
    out.push_back(result ? std::pair{result->first >> 4, result->second >> 4} : std::pair{initialX, initialZ});
    angle += M_PI * 2 / spread;
    if (++positionInCircle == spread) {
      circle++;
      positionInCircle = 0;
      spread += 2 * spread / (circle + 1);
      spread = std::min(spread, p.count - i);
      angle += random.nextDouble() * M_PI * 2.0;
    }
  }
  return rings_.emplace(&set, std::move(out)).first->second;
}

bool StructureManager::isStructureChunk(const StructureSet& set, int x, int z) {
  const Placement& p = set.placement;
  if (p.kind == Placement::RANDOM_SPREAD) {
    auto [px, pz] = p.potentialStructureChunk(seed_, x, z);
    if (px != x || pz != z) return false;
  } else {
    bool placeable = std::find(possibleSets_.begin(), possibleSets_.end(), &set) != possibleSets_.end();
    if (!placeable) return false;
    auto& r = ringPositions(set);
    if (std::find(r.begin(), r.end(), std::pair{x, z}) == r.end()) return false;
  }
  if (!p.applyAdditionalChunkRestrictions(x, z, seed_)) return false;
  return !p.exclusionSet || !hasStructureChunkInRange(*p.exclusionSet, x, z, p.exclusionChunks);
}

bool StructureManager::hasStructureChunkInRange(const StructureSet& set, int x, int z, int range) {
  for (int tx = x - range; tx <= x + range; tx++)
    for (int tz = z - range; tz <= z + range; tz++)
      if (isStructureChunk(set, tx, tz)) return true;
  return false;
}

StartP StructureManager::tryGenerate(const Structure& s, int cx, int cz) {
  GenContext ctx(gen_, seed_, cx, cz, s.biomes);
  auto stub = s.findValidGenerationPoint(ctx);
  if (!stub) return nullptr;
  auto start = std::make_shared<StructureStart>();
  start->structure = &s;
  start->owner = this;
  start->cx = cx, start->cz = cz;
  if (stub->built) {
    start->pieces = std::move(stub->built->pieces);
  } else {
    Builder b;
    stub->generator(b);
    start->pieces = std::move(b.pieces);
  }
  if (!start->valid()) return nullptr;
  restore(*start);
  return start;
}

std::vector<StartP> StructureManager::compute(int cx, int cz) {
  std::vector<StartP> out;
  for (const StructureSet* set : possibleSets_) {
    if (!isStructureChunk(*set, cx, cz)) continue;
    if (set->structures.size() == 1) {
      if (auto s = tryGenerate(*set->structures[0].structure, cx, cz)) out.push_back(std::move(s));
      continue;
    }
    std::vector<StructureSet::Entry> options = set->structures;
    WorldgenRandom random(std::make_unique<LegacyRandomSource>(0));
    random.setLargeFeatureSeed(seed_, cx, cz);
    int total = 0;
    for (auto& o : options) total += o.weight;
    while (!options.empty()) {
      int choice = random.nextInt(total);
      size_t index = 0;
      for (auto& o : options) {
        choice -= o.weight;
        if (choice < 0) break;
        index++;
      }
      auto selected = options[index];
      if (auto s = tryGenerate(*selected.structure, cx, cz)) {
        out.push_back(std::move(s));
        break;
      }
      options.erase(options.begin() + (long)index);
      total -= selected.weight;
    }
  }
  return out;
}

std::vector<StartP> StructureManager::starts(int cx, int cz) {
  int64_t key = chunkKey(cx, cz);
  {
    std::lock_guard lock(m_);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
  }
  auto v = compute(cx, cz);  // outside the lock: generation can be slow (jigsaw)
  std::lock_guard lock(m_);
  // ponytail: unbounded start cache (empty vectors for most chunks); evict with chunk unloading when that lands.
  auto [it, inserted] = cache_.emplace(key, std::move(v));
  return it->second;
}

// ------------------------------------------------------------------------------------------- persistence
namespace {
void putInt(std::vector<uint8_t>& o, int32_t v) {
  for (int i = 0; i < 4; i++) o.push_back((uint8_t)((uint32_t)v >> (8 * i)));
}
struct Rd {
  const uint8_t* p;
  size_t n, i = 0;
  bool ok = true;
  int32_t get() {
    if (i + 4 > n) return ok = false, 0;
    uint32_t v = p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
    i += 4;
    return (int32_t)v;
  }
};
}  // namespace

void StructureManager::setPersistDir(const std::string& dir) { persistDir_ = dir; }

StructureManager::RegionMap& StructureManager::region(int rx, int rz) {
  auto [it, inserted] = regions_.try_emplace({rx, rz});
  if (!inserted || persistDir_.empty()) return it->second;
  std::ifstream f(persistDir_ + "/r." + std::to_string(rx) + "." + std::to_string(rz) + ".bin", std::ios::binary);
  std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Rd r{raw.data(), raw.size()};
  int count = r.get();
  for (int k = 0; k < count && r.ok; k++) {
    int cx = r.get(), cz = r.get(), idx = r.get(), refs = r.get(), len = r.get();
    if (!r.ok || len < 0 || (size_t)len * 4 > raw.size()) break;
    Saved sv{refs, std::vector<int>((size_t)len)};
    for (auto& v : sv.state) v = r.get();
    if (r.ok) it->second[{chunkKey(cx, cz), idx}] = std::move(sv);
  }
  return it->second;
}

void StructureManager::restore(StructureStart& s) {
  std::lock_guard l(persistLock_);
  RegionMap& m = region(s.cx >> 5, s.cz >> 5);
  auto it = m.find({chunkKey(s.cx, s.cz), s.structure->registryIndex});
  if (it == m.end()) return;
  std::vector<int> fresh;
  for (auto& p : s.pieces) p->saveState(fresh);
  if (fresh.size() != it->second.state.size()) return;  // different generator version: keep the fresh pieces
  const int* p = it->second.state.data();
  for (auto& piece : s.pieces) piece->loadState(p);
  s.references = it->second.references;
}

void StructureManager::saveDirty() {
  std::set<StructureStart*> todo;
  {
    std::lock_guard l(persistLock_);
    if (persistDir_.empty()) return;
    todo.swap(dirty_);
  }
  if (todo.empty()) return;
  std::vector<std::pair<StructureStart*, Saved>> states;  // placeLock before persistLock (placeInChunk's order)
  for (StructureStart* s : todo) {
    std::lock_guard pl(s->placeLock);
    Saved sv{s->references, {}};
    for (auto& p : s->pieces) p->saveState(sv.state);
    states.emplace_back(s, std::move(sv));
  }
  std::lock_guard l(persistLock_);
  std::set<std::pair<int, int>> touched;
  for (auto& [s, sv] : states) {
    region(s->cx >> 5, s->cz >> 5)[{chunkKey(s->cx, s->cz), s->structure->registryIndex}] = std::move(sv);
    touched.insert({s->cx >> 5, s->cz >> 5});
  }
  std::filesystem::create_directories(persistDir_);
  for (auto [rx, rz] : touched) {
    std::vector<uint8_t> out;
    RegionMap& m = regions_[{rx, rz}];
    putInt(out, (int)m.size());
    for (auto& [k, sv] : m) {
      for (int v : {keyX(k.first), keyZ(k.first), k.second, sv.references, (int)sv.state.size()}) putInt(out, v);
      for (int v : sv.state) putInt(out, v);
    }
    std::string path = persistDir_ + "/r." + std::to_string(rx) + "." + std::to_string(rz) + ".bin";
    {
      std::ofstream f(path + ".tmp", std::ios::binary);
      f.write((const char*)out.data(), (std::streamsize)out.size());
    }
    std::filesystem::rename(path + ".tmp", path);
  }
}

// Chunk data (vanilla ChunkSerializer "structures"): starts {id, references} of this chunk + References.
void StructureManager::encodeChunk(int cx, int cz, std::vector<uint8_t>& out) {
  auto st = starts(cx, cz);
  putInt(out, (int)st.size());
  for (auto& s : st) putInt(out, s->structure->registryIndex), putInt(out, s->references);
  auto refs = references(cx, cz);
  putInt(out, (int)refs.size());
  for (auto& [s, keys] : refs) {
    putInt(out, s->registryIndex);
    putInt(out, (int)keys.size());
    for (int64_t k : keys) putInt(out, keyX(k)), putInt(out, keyZ(k));
  }
}

void StructureManager::decodeChunk(int cx, int cz, const uint8_t* p, size_t n) {
  // Starts and references are recomputed deterministically; only the reference counts carry state.
  Rd r{p, n};
  int count = r.get();
  for (int k = 0; k < count && r.ok; k++) {
    int idx = r.get(), refs = r.get();
    if (!r.ok) break;
    for (auto& s : starts(cx, cz))
      if (s->structure->registryIndex == idx) s->references = refs;
  }
}

namespace {
// fastutil LongOpenHashSet insertion + iteration order (HashCommon.mix, linear probing, iterate high -> low).
struct LongSetOrder {
  std::vector<int64_t> table = std::vector<int64_t>(16, 0);
  bool used0 = false, containsNull = false;
  int size = 0;
  static uint64_t mix(int64_t x) {
    uint64_t h = (uint64_t)x * 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
    return h ^ (h >> 16);
  }
  int maxFill() const { return (int)std::min<int64_t>((int64_t)std::ceil(table.size() * 0.75), (int64_t)table.size() - 1); }
  void insertRaw(std::vector<int64_t>& t, int64_t k) {
    size_t mask = t.size() - 1, pos = mix(k) & mask;
    while (t[pos] != 0) pos = (pos + 1) & mask;
    t[pos] = k;
  }
  void add(int64_t k) {
    if (k == 0) {
      if (!containsNull) containsNull = true, size++;
      return;
    }
    size_t mask = table.size() - 1, pos = mix(k) & mask;
    while (table[pos] != 0) {
      if (table[pos] == k) return;
      pos = (pos + 1) & mask;
    }
    table[pos] = k;
    if (size++ >= maxFill()) {
      // rehash(arraySize(size + 1, 0.75)): next power of two >= ceil((size+1) / 0.75)
      size_t want = (size_t)std::ceil((size + 1) / 0.75), n = 1;
      while (n < want) n <<= 1;
      // fastutil rehash iterates the old table from the end, re-inserting each key
      std::vector<int64_t> t(n, 0);
      for (size_t i = table.size(); i-- > 0;)
        if (table[i] != 0) insertRaw(t, table[i]);
      table.swap(t);
    }
  }
  std::vector<int64_t> order() const {
    std::vector<int64_t> out;
    if (containsNull) out.push_back(0);
    for (size_t i = table.size(); i-- > 0;)
      if (table[i] != 0) out.push_back(table[i]);
    return out;
  }
};
}  // namespace

StructureManager::References StructureManager::references(int cx, int cz) {
  std::vector<LongSetOrder> sets(structures_.size());
  std::vector<uint8_t> has(structures_.size(), 0);
  int x0 = cx * 16, z0 = cz * 16;
  for (int sx = cx - 8; sx <= cx + 8; sx++)
    for (int sz = cz - 8; sz <= cz + 8; sz++)
      for (auto& start : starts(sx, sz))
        if (start->boundingBox().intersects(x0, z0, x0 + 15, z0 + 15)) {
          int i = start->structure->registryIndex;
          sets[i].add(chunkKey(sx, sz));
          has[i] = 1;
        }
  std::vector<std::pair<const Structure*, std::vector<int64_t>>> out;
  for (size_t i = 0; i < structures_.size(); i++)
    if (has[i]) out.push_back({structures_[i].get(), sets[i].order()});
  return out;
}

void StructureManager::placeStep(Level& level, int cx, int cz, int step, WorldgenRandom& random, int64_t decorationSeed,
                                 const References& refs) {
  int index = 0;
  for (auto& s : structures_) {
    if (s->step != step) continue;
    random.setFeatureSeed(decorationSeed, index, step);
    for (auto& [st, keys] : refs) {
      if (st != s.get()) continue;
      for (int64_t k : keys)  // StructureManager.fillStartsForStructure
        for (auto& start : starts(keyX(k), keyZ(k)))
          if (start->structure == st) {
            // getWritableArea: a fresh box per start (ruined portals / nether fossils enlarge it)
            BoundingBox chunkBB(cx * 16, MIN_Y + 1, cz * 16, cx * 16 + 15, MAX_Y, cz * 16 + 15);
            start->placeInChunk(level, random, chunkBB, cx, cz);
          }
    }
    index++;
  }
}

void StructureManager::placeAll(Level& level, int cx, int cz) {
  WorldgenRandom random(std::make_unique<XoroshiroRandomSource>(0));
  int64_t decorationSeed = random.setDecorationSeed(level.seed(), cx * 16, cz * 16);
  auto refs = references(cx, cz);
  for (int step = 0; step < STEP_COUNT; step++) placeStep(level, cx, cz, step, random, decorationSeed, refs);
}

// ------------------------------------------------------------------------------------------- locate
std::optional<StructureManager::Found> StructureManager::findNearest(const std::vector<const Structure*>& wanted, BlockPos pos,
                                                                     int maxRadius) {
  std::vector<std::pair<const StructureSet*, std::vector<const Structure*>>> scans;  // Object2ObjectArrayMap order
  for (auto* s : wanted)
    for (auto* set : placementsFor_[s->registryIndex]) {
      auto it = std::find_if(scans.begin(), scans.end(), [&](auto& e) { return e.first == set; });
      if (it == scans.end()) scans.push_back({set, {s}});
      else if (std::find(it->second.begin(), it->second.end(), s) == it->second.end()) it->second.push_back(s);
    }
  if (scans.empty()) return std::nullopt;
  auto distSqr = [](BlockPos a, BlockPos b) {
    double dx = (double)a.x - b.x, dy = (double)a.y - b.y, dz = (double)a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
  };
  auto generatingAt = [&](const std::vector<const Structure*>& ss, const StructureSet& set, int cx, int cz) -> std::optional<Found> {
    std::vector<StartP> st;
    bool loaded = false;
    for (auto* s : ss) {
      if (!set.placement.applyAdditionalChunkRestrictions(cx, cz, seed_)) continue;
      if (!loaded) st = starts(cx, cz), loaded = true;
      for (auto& start : st)
        if (start->structure == s && start->valid()) return Found{set.placement.locatePos(start->cx, start->cz), s};
    }
    return std::nullopt;
  };
  std::optional<Found> nearest;
  double best = 1.7976931348623157e308;
  std::vector<std::pair<const StructureSet*, std::vector<const Structure*>>> spread;
  for (auto& [set, ss] : scans) {
    if (set->placement.kind == Placement::CONCENTRIC_RINGS) {
      std::optional<Found> closest;
      double closestD = 1.7976931348623157e308;
      for (auto [rx, rz] : ringPositions(*set)) {
        double d = distSqr({rx * 16 + 8, 32, rz * 16 + 8}, pos);
        if (!closest || d < closestD)
          if (auto g = generatingAt(ss, *set, rx, rz)) closest = g, closestD = d;
      }
      if (closest) {
        double d = distSqr(pos, closest->pos);
        if (d < best) best = d, nearest = closest;
      }
    } else {
      spread.push_back({set, ss});
    }
  }
  if (!spread.empty()) {
    int ox = pos.x >> 4, oz = pos.z >> 4;
    for (int radius = 0; radius <= maxRadius; radius++) {
      bool foundSomething = false;
      for (auto& [set, ss] : spread) {
        int spacing = set->placement.spacing;
        std::optional<Found> g;
        for (int x = -radius; x <= radius && !g; x++) {
          bool xEdge = x == -radius || x == radius;
          for (int z = -radius; z <= radius && !g; z++) {
            if (!xEdge && z != -radius && z != radius) continue;
            auto [tx, tz] = set->placement.potentialStructureChunk(seed_, ox + spacing * x, oz + spacing * z);
            g = generatingAt(ss, *set, tx, tz);
          }
        }
        if (g) {
          foundSomething = true;
          double d = distSqr(pos, g->pos);
          if (d < best) best = d, nearest = g;
        }
      }
      if (foundSomething) return nearest;
    }
  }
  return nearest;
}

}  // namespace structure
