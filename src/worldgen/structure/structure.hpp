// Port of vanilla 26.3 structure generation (net.minecraft.world.level.levelgen.structure): structure sets and
// placements (random_spread, concentric_rings), structure starts (ChunkGenerator.createStructures), references
// (createReferences), piece placement during the features stage (applyBiomeDecoration's structure part), the
// beardifier and /locate (ChunkGenerator.findNearestMapStructure).
//
// Starts are a pure function of (seed, chunk): they depend only on noise heights and biomes. The manager caches
// them and persists only (structure id, references) per chunk; pieces are regenerated on load, which is what
// vanilla already does for ocean monuments (OceanMonumentStructure.regeneratePiecesAfterLoad).
#pragma once
#include <array>
#include <climits>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "worldgen/random.hpp"
#include "worldgen/structure/blocks.hpp"

namespace df { struct Sampler; }

namespace structure {

// ------------------------------------------------------------------------------------------- geometry
enum class Dir : uint8_t { DOWN, UP, NORTH, SOUTH, WEST, EAST };
inline constexpr Dir HORIZONTAL[4] = {Dir::NORTH, Dir::EAST, Dir::SOUTH, Dir::WEST};  // Direction.Plane.HORIZONTAL
inline constexpr Dir ALL_DIRS[6] = {Dir::DOWN, Dir::UP, Dir::NORTH, Dir::SOUTH, Dir::WEST, Dir::EAST};  // Direction.values()
inline int stepX(Dir d) { return d == Dir::EAST ? 1 : d == Dir::WEST ? -1 : 0; }
inline int stepY(Dir d) { return d == Dir::UP ? 1 : d == Dir::DOWN ? -1 : 0; }
inline int stepZ(Dir d) { return d == Dir::SOUTH ? 1 : d == Dir::NORTH ? -1 : 0; }
Dir opposite(Dir d);
Dir clockWise(Dir d);         // around Y
Dir counterClockWise(Dir d);
inline bool isXAxis(Dir d) { return d == Dir::EAST || d == Dir::WEST; }
inline bool isZAxis(Dir d) { return d == Dir::NORTH || d == Dir::SOUTH; }
int data2d(Dir d);            // get2DDataValue
Dir from2d(int v);
Dir dirByName(std::string_view n);
const char* dirName(Dir d);
Dir rotate(Rotation r, Dir d);
Dir mirror(Mirror m, Dir d);
Rotation rotationFor(Mirror m, Dir d);  // Mirror.getRotation
Rotation getRandomRotation(RandomSource& r);
std::array<Rotation, 4> shuffledRotations(RandomSource& r);
Rotation rotationAdd(Rotation a, Rotation b);  // Rotation.getRotated
Dir randomHorizontal(RandomSource& r);

struct BlockPos {
  int x = 0, y = 0, z = 0;
  BlockPos offset(int dx, int dy, int dz) const { return {x + dx, y + dy, z + dz}; }
  BlockPos offset(const BlockPos& o) const { return {x + o.x, y + o.y, z + o.z}; }
  BlockPos subtract(const BlockPos& o) const { return {x - o.x, y - o.y, z - o.z}; }
  BlockPos relative(Dir d, int n = 1) const { return {x + stepX(d) * n, y + stepY(d) * n, z + stepZ(d) * n}; }
  BlockPos below(int n = 1) const { return {x, y - n, z}; }
  BlockPos above(int n = 1) const { return {x, y + n, z}; }
  bool operator==(const BlockPos& o) const { return x == o.x && y == o.y && z == o.z; }
  bool operator<(const BlockPos& o) const {  // Vec3i.compareTo: y, then z, then x
    return y != o.y ? y < o.y : z != o.z ? z < o.z : x < o.x;
  }
};

struct BoundingBox {
  int minX, minY, minZ, maxX, maxY, maxZ;
  BoundingBox() : minX(0), minY(0), minZ(0), maxX(0), maxY(0), maxZ(0) {}
  BoundingBox(int x0, int y0, int z0, int x1, int y1, int z1);
  explicit BoundingBox(BlockPos p) : BoundingBox(p.x, p.y, p.z, p.x, p.y, p.z) {}
  static BoundingBox fromCorners(BlockPos a, BlockPos b);
  static BoundingBox infinite() { return {INT_MIN, INT_MIN, INT_MIN, INT_MAX, INT_MAX, INT_MAX}; }
  static BoundingBox orientBox(int fx, int fy, int fz, int ox, int oy, int oz, int w, int h, int d, Dir dir);
  bool intersects(const BoundingBox& o) const {
    return maxX >= o.minX && minX <= o.maxX && maxZ >= o.minZ && minZ <= o.maxZ && maxY >= o.minY && minY <= o.maxY;
  }
  bool intersects(int x0, int z0, int x1, int z1) const { return maxX >= x0 && minX <= x1 && maxZ >= z0 && minZ <= z1; }
  bool isInside(int x, int y, int z) const {
    return x >= minX && x <= maxX && z >= minZ && z <= maxZ && y >= minY && y <= maxY;
  }
  bool isInside(BlockPos p) const { return isInside(p.x, p.y, p.z); }
  BoundingBox& encapsulate(const BoundingBox& o);
  BoundingBox& encapsulate(BlockPos p);
  BoundingBox& move(int dx, int dy, int dz) {
    minX += dx, minY += dy, minZ += dz, maxX += dx, maxY += dy, maxZ += dz;
    return *this;
  }
  BoundingBox moved(int dx, int dy, int dz) const { return BoundingBox(*this).move(dx, dy, dz); }
  BoundingBox inflatedBy(int a) const { return {minX - a, minY - a, minZ - a, maxX + a, maxY + a, maxZ + a}; }
  int xSpan() const { return maxX - minX + 1; }
  int ySpan() const { return maxY - minY + 1; }
  int zSpan() const { return maxZ - minZ + 1; }
  BlockPos center() const { return {minX + (maxX - minX + 1) / 2, minY + (maxY - minY + 1) / 2, minZ + (maxZ - minZ + 1) / 2}; }
  bool operator==(const BoundingBox& o) const {
    return minX == o.minX && minY == o.minY && minZ == o.minZ && maxX == o.maxX && maxY == o.maxY && maxZ == o.maxZ;
  }
};

enum class Heightmap : uint8_t { WORLD_SURFACE_WG, OCEAN_FLOOR_WG, WORLD_SURFACE, OCEAN_FLOOR, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES };
Heightmap heightmapByName(std::string_view n);
enum class TerrainAdjustment : uint8_t { NONE, BURY, BEARD_THIN, BEARD_BOX, ENCAPSULATE };
// GenerationStep.Decoration
enum Step : int { RAW_GENERATION, LAKES, LOCAL_MODIFICATIONS, UNDERGROUND_STRUCTURES, SURFACE_STRUCTURES, STRONGHOLDS,
                  UNDERGROUND_ORES, UNDERGROUND_DECORATION, FLUID_SPRINGS, VEGETAL_DECORATION, TOP_LAYER_MODIFICATION,
                  STEP_COUNT };

inline constexpr int MIN_Y = -64, GEN_HEIGHT = 384, MAX_Y = MIN_Y + GEN_HEIGHT - 1, SEA_LEVEL = 63;

// ------------------------------------------------------------------------------------------- world access
// ChunkGenerator view used while creating starts (noise-only terrain: no beardifier, like vanilla).
struct Generator {
  virtual ~Generator() = default;
  virtual int baseHeight(int x, int z, Heightmap type) = 0;  // getBaseHeight (= getFirstFreeHeight)
  int firstOccupiedHeight(int x, int z, Heightmap t) { return baseHeight(x, z, t) - 1; }
  virtual void baseColumn(int x, int z, std::vector<int>& out) = 0;  // getBaseColumn: states MIN_Y..MAX_Y
  virtual int noiseBiome(int qx, int qy, int qz) = 0;                // biome source at quart coords (point path)
  virtual float biomeTemperature(int biome, BlockPos p) = 0;         // Biome.getTemperature(pos, seaLevel)
  virtual int64_t seed() const = 0;
};

// WorldGenLevel (WorldGenRegion) view while placing pieces into a chunk.
struct Level {
  virtual ~Level() = default;
  virtual int getBlock(int x, int y, int z) = 0;
  virtual void setBlock(int x, int y, int z, int state) = 0;
  virtual int getHeight(Heightmap type, int x, int z) = 0;  // first free y (heightmap value)
  virtual int64_t seed() const = 0;
  virtual int biomeAt(int x, int y, int z) = 0;  // getBiome(pos) (noise biome of the quart containing pos)
  virtual RandomSource& random() = 0;              // WorldGenRegion.getRandom (worldgen_region_random at the chunk)
  // ChunkAccess.markPosForPostProcessing: the FULL step runs updateFromNeighbourShapes there.
  virtual void markPostProcessing(int x, int y, int z) { (void)x, (void)y, (void)z; }
  // StructureTemplate.placeInWorld without knownShape: updateShapeAtEdge over the placed positions (their box
  // starts at `min`), then updateFromNeighbourShapes for each in placement order.
  virtual void updatePlacedShapes(BlockPos min, BlockPos max, const std::vector<BlockPos>& placed) { (void)min, (void)max, (void)placed; }
  // FeaturePoolElement / feature-placing pieces: place a placed_feature by id. False when unavailable.
  virtual bool placeFeature(const std::string& placedFeature, RandomSource& random, BlockPos pos) {
    (void)placedFeature, (void)random, (void)pos;
    return false;
  }
  virtual bool placeConfiguredFeature(const std::string& feature, RandomSource& random, BlockPos pos) {
    (void)feature, (void)random, (void)pos;
    return false;
  }
  bool isEmpty(int x, int y, int z) { return Blocks::get().isAir(getBlock(x, y, z)); }
  int get(BlockPos p) { return getBlock(p.x, p.y, p.z); }
  void set(BlockPos p, int state) { setBlock(p.x, p.y, p.z, state); }
};

// ------------------------------------------------------------------------------------------- pieces
struct Builder;
struct Piece {
  struct BlockSelector {  // StructurePiece.BlockSelector
    int next = 0;  // air
    virtual ~BlockSelector() = default;
    virtual void select(RandomSource& random, int x, int y, int z, bool isEdge) = 0;
  };
  BoundingBox boundingBox;
  int genDepth = 0;
  virtual ~Piece() = default;
  Piece(int genDepth, const BoundingBox& box) : boundingBox(box), genDepth(genDepth) {}

  virtual void addChildren(Piece* start, Builder& builder, RandomSource& random) { (void)start, (void)builder, (void)random; }
  virtual void postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int cx, int cz, BlockPos ref) = 0;
  virtual void move(int dx, int dy, int dz) { boundingBox.move(dx, dy, dz); }
  virtual bool isPoolElement() const { return false; }
  virtual bool isTemplate() const { return false; }
  // Mutable state that placement changes (vanilla saves it in the start's NBT): bounding box by default.
  virtual void saveState(std::vector<int>& out) const {
    out.insert(out.end(), {boundingBox.minX, boundingBox.minY, boundingBox.minZ, boundingBox.maxX, boundingBox.maxY, boundingBox.maxZ});
  }
  virtual void loadState(const int*& p) {
    boundingBox = BoundingBox(p[0], p[1], p[2], p[3], p[4], p[5]);
    p += 6;
  }
  bool isCloseToChunk(int cx, int cz, int distance) const {
    int x = cx * 16, z = cz * 16;
    return boundingBox.intersects(x - distance, z - distance, x + 15 + distance, z + 15 + distance);
  }

  std::optional<Dir> orientation() const { return orientation_; }
  void setOrientation(std::optional<Dir> d);
  Mirror mirror() const { return mirror_; }
  Rotation rotation() const { return rotation_; }

 protected:
  static BoundingBox makeBoundingBox(int x, int y, int z, Dir d, int w, int h, int depth);
  int worldX(int x, int z) const;
  int worldY(int y) const { return orientation_ ? y + boundingBox.minY : y; }
  int worldZ(int x, int z) const;
  BlockPos worldPos(int x, int y, int z) const { return {worldX(x, z), worldY(y), worldZ(x, z)}; }
  void placeBlock(Level& level, int state, int x, int y, int z, const BoundingBox& chunkBB);
  virtual bool canBeReplaced(Level& level, int x, int y, int z, const BoundingBox& chunkBB) {
    (void)level, (void)x, (void)y, (void)z, (void)chunkBB;
    return true;
  }
  int getBlock(Level& level, int x, int y, int z, const BoundingBox& chunkBB);
  bool isInterior(Level& level, int x, int y, int z, const BoundingBox& chunkBB);
  void generateAirBox(Level& level, const BoundingBox& bb, int x0, int y0, int z0, int x1, int y1, int z1);
  void generateBox(Level& level, const BoundingBox& bb, int x0, int y0, int z0, int x1, int y1, int z1, int edge, int fill, bool skipAir);
  void generateBox(Level& level, const BoundingBox& bb, const BoundingBox& box, int edge, int fill, bool skipAir) {
    generateBox(level, bb, box.minX, box.minY, box.minZ, box.maxX, box.maxY, box.maxZ, edge, fill, skipAir);
  }
  void generateBox(Level& level, const BoundingBox& bb, int x0, int y0, int z0, int x1, int y1, int z1, bool skipAir,
                   RandomSource& random, BlockSelector& selector);
  void generateBox(Level& level, const BoundingBox& bb, const BoundingBox& box, bool skipAir, RandomSource& random, BlockSelector& sel) {
    generateBox(level, bb, box.minX, box.minY, box.minZ, box.maxX, box.maxY, box.maxZ, skipAir, random, sel);
  }
  void generateMaybeBox(Level& level, const BoundingBox& bb, RandomSource& random, float p, int x0, int y0, int z0, int x1,
                        int y1, int z1, int edge, int fill, bool skipAir, bool hasToBeInside);
  void maybeGenerateBlock(Level& level, const BoundingBox& bb, RandomSource& random, float p, int x, int y, int z, int state);
  void generateUpperHalfSphere(Level& level, const BoundingBox& bb, int x0, int y0, int z0, int x1, int y1, int z1, int fill, bool skipAir);
  void fillColumnDown(Level& level, int state, int x, int startY, int z, const BoundingBox& bb);
  static bool isReplaceableByStructures(int state);
  bool createChest(Level& level, const BoundingBox& bb, RandomSource& random, int x, int y, int z);
  bool createChest(Level& level, const BoundingBox& bb, RandomSource& random, BlockPos pos, int state);  // state -1: reorient
  bool createDispenser(Level& level, const BoundingBox& bb, RandomSource& random, int x, int y, int z, Dir facing);
  static int reorient(Level& level, BlockPos pos, int state);

 private:
  std::optional<Dir> orientation_;
  Mirror mirror_ = Mirror::NONE;
  Rotation rotation_ = Rotation::NONE;
};
using PieceP = std::unique_ptr<Piece>;

struct Builder {  // StructurePiecesBuilder
  std::vector<PieceP> pieces;
  void add(PieceP p) { pieces.push_back(std::move(p)); }
  Piece* findCollisionPiece(const BoundingBox& box) const;
  void offsetPiecesVertically(int dy) {
    for (auto& p : pieces) p->move(0, dy, 0);
  }
  int moveBelowSeaLevel(int seaLevel, int minY, RandomSource& random, int offset);
  void moveInsideHeights(RandomSource& random, int lowest, int highest);
  BoundingBox boundingBox() const;
  bool empty() const { return pieces.empty(); }
  void clear() { pieces.clear(); }
};

// ------------------------------------------------------------------------------------------- structures
struct GenContext;
struct Stub {  // Structure.GenerationStub
  BlockPos position;
  std::function<void(Builder&)> generator;  // lazy (Either.left)
  std::unique_ptr<Builder> built;           // eager (Either.right)
};

struct Structure {
  std::string id;
  std::string type;              // structure_type id without namespace
  std::vector<uint8_t> biomes;   // HolderSet<Biome> mask
  int step = SURFACE_STRUCTURES;
  TerrainAdjustment adaptation = TerrainAdjustment::NONE;
  int registryIndex = 0;
  virtual ~Structure() = default;
  virtual std::optional<Stub> findGenerationPoint(GenContext& ctx) const = 0;
  virtual void afterPlace(Level& level, RandomSource& random, const BoundingBox& chunkBB, int cx, int cz,
                          const std::vector<PieceP>& pieces) const {
    (void)level, (void)random, (void)chunkBB, (void)cx, (void)cz, (void)pieces;
  }
  BoundingBox adjustBoundingBox(const BoundingBox& b) const {
    return adaptation != TerrainAdjustment::NONE ? b.inflatedBy(12) : b;
  }
  std::optional<Stub> findValidGenerationPoint(GenContext& ctx) const;
};

struct GenContext {  // Structure.GenerationContext
  Generator& gen;
  int64_t seed;
  int cx, cz;
  WorldgenRandom random;
  const std::vector<uint8_t>& validBiome;
  GenContext(Generator& g, int64_t seed, int cx, int cz, const std::vector<uint8_t>& valid);
  bool validAt(BlockPos p) { return validBiome[gen.noiseBiome(p.x >> 2, p.y >> 2, p.z >> 2)]; }
  bool couldStructureExistInColumn(int x, int z, int minY, int maxY);
  bool couldValidBiomeExistInTerrainColumn(int x, int z) { return couldStructureExistInColumn(x, z, MIN_Y - 1, MAX_Y); }
  bool couldValidBiomeExistOnTopOfChunkCenter() { return couldValidBiomeExistInTerrainColumn(cx * 16 + 8, cz * 16 + 8); }
  // Structure.onTopOfChunkCenter[WithoutBiomeCheck]
  std::optional<Stub> onTopOfChunkCenter(Heightmap h, std::function<void(Builder&)> g, bool biomeCheck = true);
  int lowestY(int minX, int minZ, int sizeX, int sizeZ);
  int meanFirstOccupiedHeight(int minX, int sizeX, int minZ, int sizeZ);
};

class StructureManager;
struct StructureStart {
  const Structure* structure;
  StructureManager* owner = nullptr;  // marks the start dirty after placement (persistence)
  int cx, cz;
  int references = 0;
  std::vector<PieceP> pieces;
  std::mutex placeLock;  // pieces keep small per-piece state (e.g. placed-chest flags) across chunks
  BoundingBox boundingBox() const;  // structure.adjustBoundingBox(encapsulating pieces)
  bool valid() const { return !pieces.empty(); }
  void placeInChunk(Level& level, RandomSource& random, const BoundingBox& chunkBB, int cx, int cz);
};
using StartP = std::shared_ptr<StructureStart>;

// ------------------------------------------------------------------------------------------- placement
struct StructureSet;
struct Placement {
  enum Kind { RANDOM_SPREAD, CONCENTRIC_RINGS } kind = RANDOM_SPREAD;
  BlockPos locateOffset;
  enum Reduction { DEFAULT, LEGACY_TYPE_1, LEGACY_TYPE_2, LEGACY_TYPE_3 } reduction = DEFAULT;
  float frequency = 1.0f;
  int salt = 0;
  const StructureSet* exclusionSet = nullptr;
  int exclusionChunks = 0;
  int spacing = 0, separation = 0;
  bool triangular = false;
  int distance = 0, spread = 0, count = 0;
  std::vector<uint8_t> preferredBiomes;

  std::pair<int, int> potentialStructureChunk(int64_t seed, int x, int z) const;  // random_spread
  bool applyAdditionalChunkRestrictions(int x, int z, int64_t seed) const;
  BlockPos locatePos(int cx, int cz) const { return {cx * 16 + locateOffset.x, locateOffset.y, cz * 16 + locateOffset.z}; }
};

struct StructureSet {
  std::string id;
  struct Entry { const Structure* structure; int weight; };
  std::vector<Entry> structures;
  Placement placement;
};

class StructureManager;

// Registry + per-world generation state (ChunkGeneratorStructureState + StructureManager + StructureCheck).
class StructureManager {
 public:
  // possibleBiomes: biome ids the dimension's biome source can produce.
  StructureManager(Generator& gen, std::vector<uint8_t> possibleBiomes);
  ~StructureManager();
  const std::vector<std::unique_ptr<Structure>>& structures() const { return structures_; }
  const Structure* structure(std::string_view id) const;
  const std::vector<std::unique_ptr<StructureSet>>& sets() const { return sets_; }

  // ChunkGenerator.createStructures for (cx, cz): the valid starts of that chunk (cached; deterministic).
  std::vector<StartP> starts(int cx, int cz);
  // createReferences: per structure, source chunk keys of starts whose box intersects (cx, cz), in vanilla
  // LongOpenHashSet iteration order.
  using References = std::vector<std::pair<const Structure*, std::vector<int64_t>>>;
  References references(int cx, int cz);
  // applyBiomeDecoration structure half for one step: places every referenced start's pieces into the chunk.
  // `random` is the decoration WorldgenRandom (setDecorationSeed done by the caller), refs = references(cx, cz).
  void placeStep(Level& level, int cx, int cz, int step, WorldgenRandom& random, int64_t decorationSeed, const References& refs);
  // Whole structure part of the features stage for a chunk, for callers without the feature interleaving.
  void placeAll(Level& level, int cx, int cz);
  // Beardifier.forStructuresInChunk -> density value source for that chunk (nullptr when empty).
  std::shared_ptr<const df::Sampler> beardifier(int cx, int cz);

  // /locate: findNearestMapStructure over the given structures (radius in placement cells, vanilla 100).
  struct Found { BlockPos pos; const Structure* structure; };
  std::optional<Found> findNearest(const std::vector<const Structure*>& wanted, BlockPos from, int maxRadius);
  const std::vector<std::pair<int, int>>& ringPositions(const StructureSet& set);

  // Persistence. Piece state that placement mutates (scattered-piece heights, placed-chest flags, ...) is saved
  // per start origin chunk in <dir>/r.<rx>.<rz>.bin (32x32 origin chunks per file) and re-applied when the start
  // is regenerated, so a structure spanning chunks finishes the same way after a restart. Pieces themselves are
  // regenerated (deterministic), like vanilla does for monuments.
  void setPersistDir(const std::string& dir);
  void saveDirty();  // writes the region files of starts placed since the last save
  // Chunk save data (vanilla "structures": starts + References): encoded / decoded by the chunk file hooks.
  void encodeChunk(int cx, int cz, std::vector<uint8_t>& out);
  void decodeChunk(int cx, int cz, const uint8_t* p, size_t n);
  Generator& generator() { return gen_; }

 private:
  bool isStructureChunk(const StructureSet& set, int x, int z);
  bool hasStructureChunkInRange(const StructureSet& set, int x, int z, int range);
  StartP tryGenerate(const Structure& s, int cx, int cz);
  std::vector<StartP> compute(int cx, int cz);

  Generator& gen_;
  int64_t seed_;
  std::vector<uint8_t> possible_;
  std::vector<std::unique_ptr<Structure>> structures_;
  std::vector<std::unique_ptr<StructureSet>> sets_;
  std::vector<const StructureSet*> possibleSets_;
  std::vector<std::vector<const StructureSet*>> placementsFor_;  // per structure registryIndex
  std::mutex m_;
  std::unordered_map<int64_t, std::vector<StartP>> cache_;
  std::map<const StructureSet*, std::vector<std::pair<int, int>>> rings_;
  std::mutex ringsLock_;
  // persistence
  struct Saved { int references; std::vector<int> state; };
  using RegionMap = std::map<std::pair<int64_t, int>, Saved>;  // (origin chunk key, structure registryIndex)
  std::string persistDir_;
  std::mutex persistLock_;
  std::map<std::pair<int, int>, RegionMap> regions_;
  std::set<StructureStart*> dirty_;
  RegionMap& region(int rx, int rz);  // loads lazily; persistLock_ held
  void restore(StructureStart& s);
  friend struct StructureStart;
};

inline int64_t chunkKey(int x, int z) { return (int64_t)(uint32_t)x | (int64_t)(uint32_t)z << 32; }  // ChunkPos.pack
inline int keyX(int64_t k) { return (int)(uint32_t)k; }
inline int keyZ(int64_t k) { return (int)(uint32_t)(k >> 32); }

// Structure factories (one per structure_type), registered in structure.cpp.
std::unique_ptr<Structure> makeStructure(const std::string& type, const Json& j);

// Java helpers.
template <class T>
void shuffle(std::vector<T>& v, RandomSource& r) {  // Util.shuffle
  for (int i = (int)v.size(); i > 1; i--) std::swap(v[i - 1], v[r.nextInt(i)]);
}
inline int randomBetweenInclusive(RandomSource& r, int min, int max) { return r.nextInt(max - min + 1) + min; }

}  // namespace structure
