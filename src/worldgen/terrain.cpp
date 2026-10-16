#include "worldgen/terrain.hpp"

#include <climits>
#include <cmath>

#include "world.hpp"
#include "worldgen/material.hpp"

namespace worldgen {
namespace {
constexpr int WAY_BELOW_MIN_Y = -2032 << 4;  // DimensionType.WAY_BELOW_MIN_Y
const int SURFACE_SAMPLING_OFFSETS_IN_CHUNKS[13][2] = {{0, 0},  {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-3, 0}, {-2, 0},
                                                       {-1, 0}, {1, 0},   {-2, 1},  {-1, 1}, {0, 1},  {1, 1}};
int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
int gridX(int b) { return b >> 4; }
int gridY(int b) { return floorDiv(b, 12); }
int fromGridX(int g, int o) { return (g << 4) + o; }
int fromGridY(int g, int o) { return g * 12 + o; }
double similarity(int d1, int d2) { return 1.0 - (double)(d2 - d1) / 25.0; }
const double FLOWING_UPDATE_SIMILARITY = similarity(100, 144);
int64_t packXZ(int x, int z) { return (int64_t)(((uint64_t)(uint32_t)z << 32) | (uint32_t)x); }
double lerp(double a, double p0, double p1) { return p0 + a * (p1 - p0); }
double clampedMap(double v, double fMin, double fMax, double tMin, double tMax) {
  double f = (v - fMin) / (fMax - fMin);
  return f < 0.0 ? tMin : f > 1.0 ? tMax : lerp(f, tMin, tMax);
}
double map(double v, double fMin, double fMax, double tMin, double tMax) { return lerp((v - fMin) / (fMax - fMin), tMin, tMax); }
double clampd(double v, double mn, double mx) { return v < mn ? mn : std::min(v, mx); }
}  // namespace

// ---------------------------------------------------------------- Aquifer
int Aquifer::FluidStatus::at(int y) const { return y < level ? type : blocks::air; }

// NoiseBasedChunkGenerator.createFluidPicker: lava below -54, else sea level water.
Aquifer::FluidStatus Aquifer::globalFluid(int y) {
  return y < std::min(-54, TerrainGenerator::SEA_LEVEL) ? FluidStatus{-54, blocks::lava}
                                                        : FluidStatus{TerrainGenerator::SEA_LEVEL, blocks::water};
}

Aquifer::Aquifer(TerrainGenerator& g, df::SamplerContext& ctx, const DensityVolume& v)
    : barrier(g.dfs.bind(g.aqBarrier, ctx)),
      floodedness(g.dfs.bind(g.aqFloodedness, ctx)),
      spread(g.dfs.bind(g.aqSpread, ctx)),
      lava(g.dfs.bind(g.aqLava, ctx)),
      exclusion(g.dfs.bind(g.aqExclusion, ctx)),
      surface(g.dfs.bind(g.aqSurfaceLevel, ctx)),
      random(*g.aquiferRandom) {
  int maxBX = v.minBlockX + v.sizeX - 1, maxBY = v.minBlockY + v.sizeY - 1, maxBZ = v.minBlockZ + v.sizeZ - 1;
  minGridX = gridX(v.minBlockX - 5);
  int maxGridX = gridX(maxBX - 5) + 1;
  gridSizeX = maxGridX - minGridX + 1;
  minGridY = gridY(v.minBlockY + 1) - 1;
  int maxGridY = gridY(maxBY + 1) + 1;
  int gridSizeY = maxGridY - minGridY + 1;
  minGridZ = gridX(v.minBlockZ - 5);
  int maxGridZ = gridX(maxBZ - 5) + 1;
  gridSizeZ = maxGridZ - minGridZ + 1;
  size_t total = (size_t)gridSizeX * gridSizeY * gridSizeZ;
  statusCache.resize(total);
  statusSet.assign(total, false);
  locationCache.resize(total);
  int maxAdjusted = maxSurfaceLevel(fromGridX(minGridX, 0), fromGridX(minGridZ, 0), fromGridX(maxGridX, 9),
                                    fromGridX(maxGridZ, 9)) + 8;
  skipSamplingAboveY = fromGridY(gridY(maxAdjusted + 12) + 1, 11) - 1;
}

int Aquifer::surfaceLevel(int x, int z) {
  int qx = (x >> 2) << 2, qz = (z >> 2) << 2;
  auto [it, inserted] = surfaceLevelCache.try_emplace(packXZ(qx, qz), 0);
  if (inserted) it->second = Mth::floor(surface.sampleValue(qx, 0, qz));
  return it->second;
}

int Aquifer::maxSurfaceLevel(int minX, int minZ, int maxX, int maxZ) {
  int minQX = minX >> 2, maxQX = maxX >> 2, minQZ = minZ >> 2, maxQZ = maxZ >> 2;
  DensityVolume v{maxQX - minQX + 1, 1, maxQZ - minQZ + 1, minQX << 2, 0, minQZ << 2, 4, 1, 4};
  DensityBuffer buf(v.size());
  surface.sampleVolume(buf, v);
  int maxY = INT_MIN;
  for (int z = 0; z < v.sizeZ; z++)
    for (int x = 0; x < v.sizeX; x++) {
      int s = Mth::floor(buf.get(x + z * v.sizeX));
      surfaceLevelCache[packXZ(v.blockX(x), v.blockZ(z))] = s;
      maxY = std::max(maxY, s);
    }
  return maxY;
}

int Aquifer::getIndex(int gx, int gy, int gz) const {
  return ((gy - minGridY) * gridSizeZ + (gz - minGridZ)) * gridSizeX + (gx - minGridX);
}

const Aquifer::FluidStatus& Aquifer::status(int i) {
  if (!statusSet[i]) {
    statusCache[i] = computeFluid(locationCache[i].x, locationCache[i].y, locationCache[i].z);
    statusSet[i] = true;
  }
  return statusCache[i];
}

int Aquifer::computeSubstance(int bx, int by, int bz, double density) {
  // ponytail: shouldScheduleFluidUpdate (fluid post-processing marks) not tracked; add with fluid ticking.
  if (density > 0.0) return -1;
  FluidStatus global = globalFluid(by);
  if (by > skipSamplingAboveY) return global.at(by);
  if (global.at(by) == blocks::lava) return blocks::lava;
  int xa = gridX(bx - 5), ya = gridY(by + 1), za = gridX(bz - 5);
  int d1 = INT_MAX, d2 = INT_MAX, d3 = INT_MAX, d4 = INT_MAX, c1 = 0, c2 = 0, c3 = 0, c4 = 0;
  for (int x1 = 0; x1 <= 1; x1++)
    for (int y1 = -1; y1 <= 1; y1++)
      for (int z1 = 0; z1 <= 1; z1++) {
        int gx = xa + x1, gy = ya + y1, gz = za + z1;
        int index = getIndex(gx, gy, gz);
        Location& loc = locationCache[index];
        if (!loc.set) {
          auto r = random.at(gx, gy, gz);
          int ox = r->nextInt(10), oy = r->nextInt(9), oz = r->nextInt(10);
          loc = {fromGridX(gx, ox), fromGridY(gy, oy), fromGridX(gz, oz), true};
        }
        int dx = loc.x - bx, dy = loc.y - by, dz = loc.z - bz;
        int d = dx * dx + dy * dy + dz * dz;
        if (d1 >= d) c4 = c3, c3 = c2, c2 = c1, c1 = index, d4 = d3, d3 = d2, d2 = d1, d1 = d;
        else if (d2 >= d) c4 = c3, c3 = c2, c2 = index, d4 = d3, d3 = d2, d2 = d;
        else if (d3 >= d) c4 = c3, c3 = index, d4 = d3, d3 = d;
        else if (d4 >= d) c4 = index, d4 = d;
      }
  (void)c4, (void)d4;
  FluidStatus s1 = status(c1);
  double sim12 = similarity(d1, d2);
  int fluid = s1.at(by);
  if (sim12 <= 0.0) return fluid;
  if (fluid == blocks::water && globalFluid(by - 1).at(by - 1) == blocks::lava) return fluid;
  double barrierValue = NAN;
  FluidStatus s2 = status(c2);
  if (density + sim12 * calculatePressure(bx, by, bz, barrierValue, s1, s2) > 0.0) return -1;
  FluidStatus s3 = status(c3);
  double sim13 = similarity(d1, d3);
  if (sim13 > 0.0 && density + sim12 * sim13 * calculatePressure(bx, by, bz, barrierValue, s1, s3) > 0.0) return -1;
  double sim23 = similarity(d2, d3);
  if (sim23 > 0.0 && density + sim12 * sim23 * calculatePressure(bx, by, bz, barrierValue, s2, s3) > 0.0) return -1;
  return fluid;
}

double Aquifer::calculatePressure(int bx, int by, int bz, double& barrierValue, const FluidStatus& a, const FluidStatus& b) {
  int t1 = a.at(by), t2 = b.at(by);
  if ((t1 == blocks::lava && t2 == blocks::water) || (t1 == blocks::water && t2 == blocks::lava)) return 2.0;
  int diff = std::abs(a.level - b.level);
  if (diff == 0) return 0.0;
  double avg = 0.5 * (a.level + b.level);
  double above = by + 0.5 - avg;
  double base = diff / 2.0;
  double toMiddle = base - std::abs(above);
  double gradient;
  if (above > 0.0) {
    double c = 0.0 + toMiddle;
    gradient = c > 0.0 ? c / 1.5 : c / 2.5;
  } else {
    double c = 3.0 + toMiddle;
    gradient = c > 0.0 ? c / 3.0 : c / 10.0;
  }
  double noise = 0.0;
  if (!(gradient < -2.0) && !(gradient > 2.0)) {
    if (std::isnan(barrierValue)) barrierValue = barrier.sampleValue(bx, by, bz);
    noise = barrierValue;
  }
  return 2.0 * (noise + gradient);
}

Aquifer::FluidStatus Aquifer::computeFluid(int x, int y, int z) {
  FluidStatus global = globalFluid(y);
  int lowest = INT_MAX, top = y + 12, bottom = y - 12;
  bool centerUnder = false;
  for (auto& o : SURFACE_SAMPLING_OFFSETS_IN_CHUNKS) {
    int sx = x + (o[0] << 4), sz = z + (o[1] << 4);
    int level = surfaceLevel(sx, sz), adjusted = level + 8;
    bool start = o[0] == 0 && o[1] == 0;
    if (start && bottom > adjusted) return global;
    bool pokes = top > adjusted;
    if (pokes || start) {
      FluidStatus atSurface = globalFluid(adjusted);
      if (atSurface.at(adjusted) != blocks::air) {
        if (start) centerUnder = true;
        if (pokes) return atSurface;
      }
    }
    lowest = std::min(lowest, level);
  }
  // computeSurfaceLevel
  double partially, fully;
  if ((double)exclusion.sampleValue(x, y, z) > 0.0) {
    partially = fully = -1.0;
  } else {
    int below = lowest + 8 - y;
    double factor = centerUnder ? clampedMap((double)below, 0.0, 64.0, 1.0, 0.0) : 0.0;
    double noise = clampd((double)floodedness.sampleValue(x, y, z), -1.0, 1.0);
    double fullyT = map(factor, 1.0, 0.0, -0.3, 0.8), partiallyT = map(factor, 1.0, 0.0, -0.8, 0.4);
    partially = noise - partiallyT;
    fully = noise - fullyT;
  }
  int level;
  if (fully > 0.0) {
    level = global.level;
  } else if (partially > 0.0) {
    int cx = floorDiv(x, 16), cy = floorDiv(y, 40), cz = floorDiv(z, 16);
    double s = (double)(spread.sampleValue(cx, cy, cz) * 10.0f);
    level = std::min(lowest, cy * 40 + 20 + Mth::floor(s / 3) * 3);
  } else {
    level = WAY_BELOW_MIN_Y;
  }
  // computeFluidType
  int type = global.type;
  if (level <= -10 && level != WAY_BELOW_MIN_Y && global.type != blocks::lava) {
    double v = lava.sampleValue(floorDiv(x, 64), floorDiv(y, 40), floorDiv(z, 64));
    if (std::abs(v) > 0.3) type = blocks::lava;
  }
  return {level, type};
}

// ---------------------------------------------------------------- TerrainGenerator
TerrainGenerator::~TerrainGenerator() = default;

TerrainGenerator::TerrainGenerator(int64_t seed, const std::string& dataRoot, bool surface) : dfs(seed, dataRoot) {
  Json s = Json::parseFile(dataRoot + "/minecraft/worldgen/noise_settings/overworld.json");
  const Json& r = s["noise_router"];
  const Json& a = s["aquifers"];
  temperature = dfs.parse(r["temperature"]), vegetation = dfs.parse(r["vegetation"]);
  continents = dfs.parse(r["continents"]), erosion = dfs.parse(r["erosion"]), depth = dfs.parse(r["depth"]);
  ridges = dfs.parse(r["ridges"]), chunkSurfaceLevel = dfs.parse(r["chunk_surface_level"]);
  finalDensity = dfs.parse(r["final_density"]);
  aqBarrier = dfs.parse(a["barrier"]), aqFloodedness = dfs.parse(a["fluid_level_floodedness"]);
  aqSpread = dfs.parse(a["fluid_level_spread"]), aqLava = dfs.parse(a["lava"]);
  aqExclusion = dfs.parse(a["exclusion"]), aqSurfaceLevel = dfs.parse(a["surface_level"]);
  for (auto& t : s["spawn_target"].arr) {
    spawnTarget.emplace_back();
    for (auto& [id, r] : t.obj)  // Climate.Parameter.span(min, max), quantized
      spawnTarget.back().push_back({id, {quantize_coord(r[0].asFloat()), quantize_coord(r[1].asFloat())}});
  }
  for (auto& f : {temperature, vegetation, continents, erosion, depth, ridges, chunkSurfaceLevel, finalDensity, aqBarrier,
                  aqFloodedness, aqSpread, aqLava, aqExclusion, aqSurfaceLevel})
    dfs.sampler(f);  // compile up front
  aquiferRandom = &dfs.randomState().getOrCreateRandomFactory("minecraft:aquifer");
  if (surface) materials = std::make_unique<material::MaterialSystem>(dfs.randomState(), "minecraft:overworld", dataRoot);
}

std::unique_ptr<TerrainGenerator::NoiseChunk> TerrainGenerator::createNoiseChunk(const DensityVolume& volume) {
  auto nc = std::make_unique<NoiseChunk>();
  nc->volume = volume;
  nc->aquifer = std::make_unique<Aquifer>(*this, nc->ctx, volume);
  return nc;
}

std::unique_ptr<TerrainGenerator::NoiseChunk> TerrainGenerator::fillFromNoise(Chunk& c) {
  auto nc = createNoiseChunk({16, HEIGHT, 16, c.cx * 16, MIN_Y, c.cz * 16});
  const DensityVolume& v = nc->volume;
  DensityBuffer density(v.size());
  dfs.bind(finalDensity, nc->ctx).sampleVolume(density, v);
  for (int z = 0; z < 16; z++)
    for (int x = 0; x < 16; x++)
      for (int y = HEIGHT - 1; y >= 0; y--) {
        int by = MIN_Y + y;
        int state = nc->aquifer->computeSubstance(v.minBlockX + x, by, v.minBlockZ + z, density.get(y + (x + z * 16) * HEIGHT));
        if (state < 0) state = blocks::stone;
        if (state != blocks::air) c.set(x, by, z, (uint16_t)state);
      }
  return nc;
}

// doCreateBiomes for one chunk into out[(qy * 4 + qz) * 4 + qx] (qy from MIN_Y), find() in fillBiomesFromNoise order.
void TerrainGenerator::chunkBiomes(int cx, int cz, const BiomeSource& source, std::vector<uint16_t>& out) {
  df::SamplerContext ctx(true);
  DensityVolume v{4, HEIGHT / 4, 4, cx * 16, MIN_Y, cz * 16, 4, 4, 4};
  std::vector<DensityBuffer> b(6, DensityBuffer(v.size()));
  const df::NodeP* fns[6] = {&temperature, &vegetation, &continents, &erosion, &depth, &ridges};
  for (int i = 0; i < 6; i++) dfs.bind(*fns[i], ctx).sampleVolume(b[i], v);
  out.resize(v.size());
  for (int qy = 0; qy < HEIGHT / 4; qy += 4)  // per section: x, y, z
    for (int x = 0; x < 4; x++)
      for (int y = qy; y < qy + 4; y++)
        for (int z = 0; z < 4; z++) {
          int i = y + (x + z * 4) * v.sizeY;
          out[(y * 4 + z) * 4 + x] =
              (uint16_t)source.find(target(b[0].get(i), b[1].get(i), b[2].get(i), b[3].get(i), b[4].get(i), b[5].get(i)));
        }
}

void TerrainGenerator::createBiomes(Chunk& c) {
  std::vector<uint16_t> q;
  chunkBiomes(c.cx, c.cz, biomeSource, q);
  for (int y = 0; y < HEIGHT / 4; y++)
    for (int z = 0; z < 4; z++)
      for (int x = 0; x < 4; x++) c.set_biome(x, y, z, q[(y * 4 + z) * 4 + x]);
}

namespace {
// material::MaterialContext over a NoiseChunk's caching context (= vanilla noiseChunk.cachingSamplers()).
// noiseBiome reads the region's stored biomes like WorldGenRegion: this chunk's, or a neighbour's computed
// on demand with a separate BiomeSource (own tie cache, so this chunk's find() sequence stays vanilla's).
struct SurfaceContext : material::MaterialContext {
  TerrainGenerator& g;
  TerrainGenerator::NoiseChunk& nc;
  Chunk& chunk;
  std::map<std::string, df::Bound> bound;
  std::map<std::pair<int, int>, std::vector<uint16_t>> neighbours;
  SurfaceContext(TerrainGenerator& g, TerrainGenerator::NoiseChunk& nc, Chunk& c) : g(g), nc(nc), chunk(c) {}
  const df::Bound& get(const std::string& id) {
    auto it = bound.find(id);
    if (it == bound.end()) it = bound.emplace(id, g.dfs.bind(g.dfs.function(id), nc.ctx)).first;
    return it->second;
  }
  float sampleDensity(const std::string& id, int x, int y, int z) override { return get(id).sampleValue(x, y, z); }
  void sampleDensityVolume(const std::string& id, DensityBuffer& out, const DensityVolume& v) override {
    get(id).sampleVolume(out, v);
  }
  int noiseBiome(int qx, int qy, int qz) override {
    int y = std::clamp(qy - (TerrainGenerator::MIN_Y >> 2), 0, TerrainGenerator::HEIGHT / 4 - 1);
    int cx = qx >> 2, cz = qz >> 2;
    if (cx == chunk.cx && cz == chunk.cz) return chunk.sections[y >> 2].biomes[((y & 3) * 4 + (qz & 3)) * 4 + (qx & 3)];
    auto& q = neighbours[{cx, cz}];
    if (q.empty()) g.chunkBiomes(cx, cz, g.neighbourBiomeSource, q);
    return q[(y * 4 + (qz & 3)) * 4 + (qx & 3)];
  }
};
}  // namespace

void TerrainGenerator::generate(Chunk& c) {
  createBiomes(c);
  auto nc = fillFromNoise(c);
  if (materials) {
    SurfaceContext ctx(*this, *nc, c);
    materials->buildSurface(c, ctx);
  }
}

// NoiseBasedChunkGenerator.getOrigin -> NoiseSpawnFinder.findSpawnPosition (block x, z; y is 0).
std::pair<int, int> TerrainGenerator::findSpawnPosition() {
  df::SamplerContext ctx(true);
  struct Param { df::Bound f; int64_t min, max; };
  std::vector<std::vector<Param>> targets;
  for (auto& t : spawnTarget) {
    targets.emplace_back();
    for (auto& [id, range] : t) targets.back().push_back({dfs.bind(dfs.function(id), ctx), range.first, range.second});
  }
  auto fitness = [&](int bx, int bz) {
    int qx = (bx >> 2) << 2, qz = (bz >> 2) << 2;
    int64_t best = INT64_MAX;
    for (auto& t : targets) {
      uint64_t f = 0;
      for (auto& p : t) {
        int64_t v = quantize_coord(p.f.sampleValue(qx, 0, qz));
        int64_t above = (int64_t)((uint64_t)v - (uint64_t)p.max), below = (int64_t)((uint64_t)p.min - (uint64_t)v);
        uint64_t d = above > 0 ? above : std::max<int64_t>(below, 0);
        f += d * d;
      }
      best = std::min(best, (int64_t)f);
    }
    uint64_t dist = (uint64_t)((int64_t)bx * bx) + (uint64_t)((int64_t)bz * bz);
    return (int64_t)((uint64_t)best * (2048ull * 2048ull) + dist);
  };
  int rx = 0, rz = 0;
  int64_t rf = fitness(0, 0);
  for (auto [maxRadius, inc] : {std::pair{2048.0f, 512.0f}, std::pair{512.0f, 32.0f}}) {
    float angle = 0.0f, radius = inc;
    int ox = rx, oz = rz;
    while (radius <= maxRadius) {
      int x = ox + Mth::d2i(std::sin((double)angle) * (double)radius);
      int z = oz + Mth::d2i(std::cos((double)angle) * (double)radius);
      int64_t f = fitness(x, z);
      if (f < rf) rx = x, rz = z, rf = f;
      angle += inc / radius;
      if ((double)angle > M_PI * 2) angle = 0.0f, radius += inc;
    }
  }
  return {rx, rz};
}

int TerrainGenerator::baseHeight(int x, int z, bool oceanFloor) {
  auto nc = createNoiseChunk({1, HEIGHT, 1, x, MIN_Y, z});
  DensityBuffer density(HEIGHT);
  dfs.bind(finalDensity, nc->ctx).sampleVolume(density, nc->volume);
  for (int y = HEIGHT - 1; y >= 0; y--) {
    int state = nc->aquifer->computeSubstance(x, MIN_Y + y, z, density.get(y));
    if (state < 0) state = blocks::stone;
    if (oceanFloor ? state == blocks::stone : state != blocks::air) return MIN_Y + y + 1;
  }
  return MIN_Y;
}

void TerrainGenerator::baseColumn(int x, int z, std::vector<int>& out) {
  auto nc = createNoiseChunk({1, HEIGHT, 1, x, MIN_Y, z});
  DensityBuffer density(HEIGHT);
  dfs.bind(finalDensity, nc->ctx).sampleVolume(density, nc->volume);
  out.assign(HEIGHT, blocks::air);
  for (int y = HEIGHT - 1; y >= 0; y--) {
    int state = nc->aquifer->computeSubstance(x, MIN_Y + y, z, density.get(y));
    out[y] = state < 0 ? blocks::stone : state;
  }
}

}  // namespace worldgen
