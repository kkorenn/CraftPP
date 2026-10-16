#include "worldgen/carver.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

namespace worldgen {
namespace {
constexpr float PI_F = (float)M_PI;
constexpr int RANGE = 4;  // WorldCarver.getRange

// WorldCarver.carveEllipsoid; skip(xd, yd, zd, worldY).
template <class Skip>
void carveEllipsoid(int cx, int cz, double x, double y, double z, double hr, double vr, CarvingMask& out, Skip skip) {
  double centerX = cx * 16 + 8, centerZ = cz * 16 + 8;
  double maxDelta = 16.0 + hr * 2.0;
  if (std::abs(x - centerX) > maxDelta || std::abs(z - centerZ) > maxDelta) return;
  int minBX = cx * 16, minBZ = cz * 16;
  int minXI = std::max(Mth::floor(x - hr) - minBX - 1, 0), maxXI = std::min(Mth::floor(x + hr) - minBX, 15);
  int minY = std::max(Mth::floor(y - vr) - 1, out.minY), maxY = std::min(Mth::floor(y + vr) + 1, out.maxY);
  int minZI = std::max(Mth::floor(z - hr) - minBZ - 1, 0), maxZI = std::min(Mth::floor(z + hr) - minBZ, 15);
  for (int xi = minXI; xi <= maxXI; xi++) {
    double xd = (minBX + xi + 0.5 - x) / hr;
    for (int zi = minZI; zi <= maxZI; zi++) {
      double zd = (minBZ + zi + 0.5 - z) / hr;
      if (xd * xd + zd * zd >= 1.0) continue;
      for (int wy = maxY; wy > minY; wy--) {
        double yd = (wy - 0.5 - y) / vr;
        if (!skip(xd, yd, zd, wy)) out.carve(xi, wy, zi);
      }
    }
  }
}

bool canReach(int cx, int cz, double x, double z, int step, int total, float thickness) {
  double xd = x - (cx * 16 + 8), zd = z - (cz * 16 + 8);
  double remaining = total - step;
  double rr = thickness + 2.0F + 16.0F;
  return xd * xd + zd * zd - remaining * remaining <= rr * rr;
}
}  // namespace

void CarvingMask::visit(const std::function<void(int, int, int, int)>& f) const {
  size_t n = size_t(256 * height);
  auto test = [&](size_t i) { return (bits[i >> 6] >> (i & 63)) & 1; };
  size_t i = 0;
  while (i < n) {
    if (!bits[i >> 6]) { i = (i | 63) + 1; continue; }
    if (!test(i)) { i++; continue; }
    size_t start = i;
    while (i < n && test(i)) i++;
    int end = int(i) - 1;
    for (int col = int(start) / height; col <= end / height; col++) {
      int base = col * height;
      f(col >> 4 & 15, col & 15, std::max(int(start) - base, 0) + minY, std::min(end - base, height - 1) + minY);
    }
  }
}

Carvers::Carvers(const std::string& root) {
  std::map<std::string, int> index;
  std::string dir = root + "/minecraft/worldgen/carver";
  std::vector<std::string> names;
  for (auto& e : std::filesystem::directory_iterator(dir))
    if (e.path().extension() == ".json") names.push_back(e.path().stem().string());
  std::sort(names.begin(), names.end());
  for (auto& n : names) {
    Json j = Json::parseFile(dir + "/" + n + ".json");
    Carver c;
    c.canyon = strip_ns(j["type"].asString()) == "canyon";
    c.probability = j["probability"].asFloat();
    c.y = HeightProvider(j["y"]);
    if (c.canyon) {
      const Json& s = j["shape"];
      c.verticalRotation = FloatProvider(j["vertical_rotation"]);
      c.distanceFactor = FloatProvider(s["distance_factor"]);
      c.thickness = FloatProvider(s["thickness"]);
      c.widthSmoothness = s["width_smoothness"].asInt();
      c.horizontalRadiusFactor = FloatProvider(s["horizontal_radius_factor"]);
      c.verticalRadiusDefaultFactor = s["vertical_radius_default_factor"].asFloat();
      c.verticalRadiusCenterFactor = s["vertical_radius_center_factor"].asFloat();
      c.yScale = FloatProvider(s["y_scale"]);
    } else {
      c.count = IntProvider(j["count"]);
      c.thickness = FloatProvider(j["thickness"]);
      c.weirdThicknessBias = j.has("weird_thickness_bias") && j["weird_thickness_bias"].asBool();
      c.roomVerticalRadius = FloatProvider(j["room_vertical_radius_multiplier"]);
      c.horizontalRadius = FloatProvider(j["horizontal_radius_multiplier"]);
      c.verticalRadius = FloatProvider(j["vertical_radius_multiplier"]);
      c.startVerticalRadius = j.has("start_vertical_radius_multiplier") ? FloatProvider(j["start_vertical_radius_multiplier"]) : FloatProvider(1.0F);
      c.floorLevel = FloatProvider(j["floor_level"]);
    }
    index["minecraft:" + n] = int(carvers.size());
    carvers.push_back(c);
  }
  std::vector<std::string> biomes;
  std::string bdir = root + "/minecraft/worldgen/biome";
  for (auto& e : std::filesystem::directory_iterator(bdir))
    if (e.path().extension() == ".json") biomes.push_back(e.path().stem().string());
  std::sort(biomes.begin(), biomes.end());  // registry order = gen/ids.hpp biomes::
  for (auto& b : biomes) {
    Json j = Json::parseFile(bdir + "/" + b + ".json");
    auto& list = biomeCarvers.emplace_back();
    const Json& c = j["carvers"];
    auto add = [&](const std::string& id) {
      auto it = index.find(id.find(':') == std::string::npos ? "minecraft:" + id : id);
      if (it == index.end()) throw std::runtime_error("carver " + id);
      list.push_back(it->second);
    };
    if (c.isString()) add(c.str);
    else for (auto& x : c.arr) add(x.str);
  }
}

void Carvers::carve(int64_t seed, int cx, int cz, const std::function<int(int, int)>& biomeAt, CarvingMask& mask) const {
  WorldgenRandom random(std::make_unique<LegacyRandomSource>(0));
  for (int dx = -8; dx <= 8; dx++)
    for (int dz = -8; dz <= 8; dz++) {
      int sx = cx + dx, sz = cz + dz;
      const auto& list = biomeCarvers[size_t(biomeAt(sx, sz))];
      for (size_t index = 0; index < list.size(); index++) {
        const Carver& c = carvers[size_t(list[index])];
        random.setLargeFeatureSeed((int64_t)((uint64_t)seed + index), sx, sz);
        if (!(random.nextFloat() <= c.probability)) continue;
        if (c.canyon) carveCanyon(c, random, cx, cz, sx, sz, mask);
        else carveCave(c, random, cx, cz, sx, sz, mask);
      }
    }
}

void Carvers::carveCave(const Carver& c, RandomSource& r, int cx, int cz, int sx, int sz, CarvingMask& m) const {
  int maxDistance = (RANGE * 2 - 1) * 16;
  int caveCount = c.count.sample(r);
  for (int cave = 0; cave < caveCount; cave++) {
    double x = sx * 16 + r.nextInt(16);
    double y = c.y.sample(r);
    double z = sz * 16 + r.nextInt(16);
    double hMul = c.horizontalRadius.sample(r);
    double vMul = c.verticalRadius.sample(r);
    double startV = c.startVerticalRadius.sample(r);
    double floor = c.floorLevel.sample(r);
    auto skip = [floor](double xd, double yd, double zd, int) { return yd <= floor ? true : xd * xd + yd * yd + zd * zd >= 1.0; };
    int tunnels = 1;
    if (r.nextInt(4) == 0) {
      double yScale = c.roomVerticalRadius.sample(r);
      float thickness = 1.0F + r.nextFloat() * 6.0F;
      double hr = 1.5 + Mth::sin(PI_F / 2) * thickness;  // createRoom
      carveEllipsoid(cx, cz, x + 1.0, y, z, hr, hr * yScale, m, skip);
      tunnels += r.nextInt(4);
    }
    for (int i = 0; i < tunnels; i++) {
      float hRot = r.nextFloat() * (PI_F * 2);
      float vRot = (r.nextFloat() - 0.5F) / 4.0F;
      float thickness = c.thickness.sample(r);  // getThickness
      if (c.weirdThicknessBias && r.nextInt(10) == 0) {
        float f1 = r.nextFloat();
        thickness *= f1 * r.nextFloat() * 3.0F + 1.0F;
      }
      int distance = maxDistance - r.nextInt(maxDistance / 4);
      caveTunnel(c, cx, cz, r.nextLong(), x, y, z, hMul, vMul, thickness, hRot, vRot, 0, distance, startV, floor, m);
    }
  }
}

void Carvers::caveTunnel(const Carver& c, int cx, int cz, int64_t seed, double x, double y, double z, double hMul,
                         double vMul, float thickness, float hRot, float vRot, int step, int dist, double yScale,
                         double floor, CarvingMask& m) const {
  SingleThreadedRandomSource r(seed);
  auto skip = [floor](double xd, double yd, double zd, int) { return yd <= floor ? true : xd * xd + yd * yd + zd * zd >= 1.0; };
  int split = r.nextInt(dist / 2) + dist / 4;
  bool steep = r.nextInt(6) == 0;
  float yRota = 0.0F, xRota = 0.0F;
  for (int cur = step; cur < dist; cur++) {
    double hr = 1.5 + Mth::sin(PI_F * (float)cur / (float)dist) * thickness;
    double vr = hr * yScale;
    float cosX = Mth::cos(vRot);
    x += Mth::cos(hRot) * cosX;
    y += Mth::sin(vRot);
    z += Mth::sin(hRot) * cosX;
    vRot *= steep ? 0.92F : 0.7F;
    vRot += xRota * 0.1F;
    hRot += yRota * 0.1F;
    xRota *= 0.9F;
    yRota *= 0.75F;
    {
      float a = r.nextFloat(), b = r.nextFloat(), d = r.nextFloat();
      xRota += (a - b) * d * 2.0F;
      a = r.nextFloat(), b = r.nextFloat(), d = r.nextFloat();
      yRota += (a - b) * d * 4.0F;
    }
    if (cur == split && thickness > 1.0F) {
      int64_t s1 = r.nextLong();
      float t1 = r.nextFloat() * 0.5F + 0.5F;
      caveTunnel(c, cx, cz, s1, x, y, z, hMul, vMul, t1, hRot - PI_F / 2, vRot / 3.0F, cur, dist, 1.0, floor, m);
      int64_t s2 = r.nextLong();
      float t2 = r.nextFloat() * 0.5F + 0.5F;
      caveTunnel(c, cx, cz, s2, x, y, z, hMul, vMul, t2, hRot + PI_F / 2, vRot / 3.0F, cur, dist, 1.0, floor, m);
      return;
    }
    if (r.nextInt(4) != 0) {
      if (!canReach(cx, cz, x, z, cur, dist, thickness)) return;
      carveEllipsoid(cx, cz, x, y, z, hr * hMul, vr * vMul, m, skip);
    }
  }
}

void Carvers::carveCanyon(const Carver& c, RandomSource& rr, int cx, int cz, int sx, int sz, CarvingMask& m) const {
  int maxDistance = (RANGE * 2 - 1) * 16;
  double x = sx * 16 + rr.nextInt(16);
  int yi = c.y.sample(rr);
  double z = sz * 16 + rr.nextInt(16);
  float hRot = rr.nextFloat() * (PI_F * 2);
  float vRot = c.verticalRotation.sample(rr);
  double yScale = c.yScale.sample(rr);
  float thickness = c.thickness.sample(rr);
  int distance = (int)((float)maxDistance * c.distanceFactor.sample(rr));
  double y = yi;
  SingleThreadedRandomSource r(rr.nextLong());
  // initWidthFactors
  std::vector<float> width(GEN_DEPTH);
  float wf = 1.0F;
  for (int yIndex = 0; yIndex < GEN_DEPTH; yIndex++) {
    if (yIndex == 0 || r.nextInt(c.widthSmoothness) == 0) {
      float a = r.nextFloat();
      wf = 1.0F + a * r.nextFloat();
    }
    width[size_t(yIndex)] = wf * wf;
  }
  auto skip = [&](double xd, double yd, double zd, int wy) {
    return (xd * xd + zd * zd) * width[size_t(wy - GEN_MIN_Y - 1)] + yd * yd / 6.0 >= 1.0;
  };
  float yRota = 0.0F, xRota = 0.0F;
  for (int cur = 0; cur < distance; cur++) {
    double hr = 1.5 + Mth::sin((float)cur * PI_F / (float)distance) * thickness;
    double vr = hr * yScale;
    hr *= c.horizontalRadiusFactor.sample(r);
    {  // updateVerticalRadius
      float vm = 1.0F - std::abs(0.5F - (float)cur / (float)distance) * 2.0F;
      float factor = c.verticalRadiusDefaultFactor + c.verticalRadiusCenterFactor * vm;
      vr = factor * vr * mth::randomBetween(r, 0.75F, 1.0F);
    }
    float xc = Mth::cos(vRot), xs = Mth::sin(vRot);
    x += Mth::cos(hRot) * xc;
    y += xs;
    z += Mth::sin(hRot) * xc;
    vRot *= 0.7F;
    vRot += xRota * 0.05F;
    hRot += yRota * 0.05F;
    xRota *= 0.8F;
    yRota *= 0.5F;
    {
      float a = r.nextFloat(), b = r.nextFloat(), d = r.nextFloat();
      xRota += (a - b) * d * 2.0F;
      a = r.nextFloat(), b = r.nextFloat(), d = r.nextFloat();
      yRota += (a - b) * d * 4.0F;
    }
    if (r.nextInt(4) != 0) {
      if (!canReach(cx, cz, x, z, cur, distance, thickness)) return;
      carveEllipsoid(cx, cz, x, y, z, hr, vr, m, skip);
    }
  }
}

}  // namespace worldgen
