#include "worldgen/noise.hpp"

#include <cmath>
#include <stdexcept>

// ---------------------------------------------------------------- Interval
Interval Interval::add(Interval a, Interval b) {
  float mn = a.min + b.min, mx = a.max + b.max;
  if (std::isnan(mn) || std::isnan(mx)) return {NAN, NAN};
  return {mn, mx};
}

Interval Interval::mul(Interval a, Interval b) {
  if (std::isnan(a.min) || std::isnan(b.min)) return {NAN, NAN};
  auto mb = [](float l, float r) { return l != 0.0f && r != 0.0f ? l * r : 0.0f; };
  float a1 = mb(a.min, b.min), a2 = mb(a.min, b.max), a3 = mb(a.max, b.min), a4 = mb(a.max, b.max);
  return {std::min(std::min(a1, a2), std::min(a3, a4)), std::max(std::max(a1, a2), std::max(a3, a4))};
}

// ---------------------------------------------------------------- Noise (default addToVolume)
void Noise::addToVolume(DensityBuffer& buffer, const DensityVolume& volume, double xzScale, double yScale,
                        float amplitude) const {
  int index = 0;
  for (int iz = 0; iz < volume.sizeZ; iz++) {
    double z = volume.blockZ(iz) * xzScale;
    for (int ix = 0; ix < volume.sizeX; ix++) {
      double x = volume.blockX(ix) * xzScale;
      for (int iy = 0; iy < volume.sizeY; iy++) {
        double y = volume.blockY(iy) * yScale;
        buffer.addTo(index++, amplitude * get(x, y, z));
      }
    }
  }
}

// ---------------------------------------------------------------- GradientNoise
const GradientNoise::Gradient GradientNoise::GRADIENT[16] = {
    {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
    {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}, {1, 1, 0},  {0, -1, 1}, {-1, 1, 0}, {0, -1, -1}};

GradientNoise::GradientNoise(RandomSource& random, double noiseOffsetScale) {
  offsetX = random.nextDouble() * noiseOffsetScale;
  offsetY = random.nextDouble() * noiseOffsetScale;
  offsetZ = random.nextDouble() * noiseOffsetScale;
  for (int i = 0; i < 256; i++) perms[i] = (uint8_t)i;
  for (int i = 0; i < 256; i++) {
    int offset = random.nextInt(256 - i);
    std::swap(perms[i], perms[offset + i]);
  }
}

double GradientNoise::wrap(double x) {
  static const double HALF_ROUND_OFF = std::nextafter(1.6777216E7, 0.0);  // Math.nextDown
  return x >= -HALF_ROUND_OFF && x < HALF_ROUND_OFF ? x : x - std::floor(x / 3.3554432E7 + 0.5) * 3.3554432E7;
}

// ---------------------------------------------------------------- PerlinNoise
float PerlinNoise::get(double _x, double _y, double _z) const {
  double x = wrap(_x) + offsetX;
  double y = wrap(_y) + offsetY;
  double z = wrap(_z) + offsetZ;
  int floorX = Mth::floor(x), floorY = Mth::floor(y), floorZ = Mth::floor(z);
  float relativeX = (float)(x - floorX);
  float relativeY = (float)(y - floorY);
  float relativeZ = (float)(z - floorZ);
  return sampleAndLerp(floorX, floorY, floorZ, relativeX, relativeY, relativeZ, relativeY);
}

float PerlinNoise::sampleAndLerp(int x, int y, int z, float rx, float ry, float rz, float originalRelativeY) const {
  int x0 = permute(x), x1 = permute(x + 1);
  int xy00 = permute(x0 + y), xy01 = permute(x0 + y + 1);
  int xy10 = permute(x1 + y), xy11 = permute(x1 + y + 1);
  float d000 = gradDot(permute(xy00 + z), rx, ry, rz);
  float d100 = gradDot(permute(xy10 + z), rx - 1.0f, ry, rz);
  float d010 = gradDot(permute(xy01 + z), rx, ry - 1.0f, rz);
  float d110 = gradDot(permute(xy11 + z), rx - 1.0f, ry - 1.0f, rz);
  float d001 = gradDot(permute(xy00 + z + 1), rx, ry, rz - 1.0f);
  float d101 = gradDot(permute(xy10 + z + 1), rx - 1.0f, ry, rz - 1.0f);
  float d011 = gradDot(permute(xy01 + z + 1), rx, ry - 1.0f, rz - 1.0f);
  float d111 = gradDot(permute(xy11 + z + 1), rx - 1.0f, ry - 1.0f, rz - 1.0f);
  float xAlpha = Mth::smoothstep(rx);
  float yAlpha = Mth::smoothstep(originalRelativeY);
  float zAlpha = Mth::smoothstep(rz);
  return Mth::lerp3(xAlpha, yAlpha, zAlpha, d000, d100, d010, d110, d001, d101, d011, d111);
}

// Shared body of PerlinNoise/SmearedPerlinNoise.addToVolume. relY(originalY, relativeY) returns the float
// relative Y used in the y-gradient terms; alphaY is always smoothstep((float)relativeY).
template <class RelY>
static void perlinAddToVolume(const uint8_t* perms, const GradientNoise::Gradient* GRADIENT, double offX, double offY,
                              double offZ, double (*wrap)(double), DensityBuffer& buffer, const DensityVolume& volume,
                              double xzScale, double yScale, float amplitude, RelY relY) {
  auto permute = [&](int v) { return (int)perms[v & 0xFF]; };
  auto grad = [&](int v) -> const GradientNoise::Gradient& { return GRADIENT[permute(v) & 15]; };
  float d000xz = 0, d100xz = 0, d010xz = 0, d110xz = 0, d001xz = 0, d101xz = 0, d011xz = 0, d111xz = 0;
  float g000y = 0, g100y = 0, g010y = 0, g110y = 0, g001y = 0, g101y = 0, g011y = 0, g111y = 0;
  int index = 0;
  for (int iz = 0; iz < volume.sizeZ; iz++) {
    double z = wrap(volume.blockZ(iz) * xzScale) + offZ;
    int floorZ = Mth::floor(z);
    float relativeZ = (float)(z - floorZ);
    float alphaZ = Mth::smoothstep(relativeZ);
    for (int ix = 0; ix < volume.sizeX; ix++) {
      double x = wrap(volume.blockX(ix) * xzScale) + offX;
      int floorX = Mth::floor(x);
      float relativeX = (float)(x - floorX);
      int x0 = permute(floorX), x1 = permute(floorX + 1);
      float alphaX = Mth::smoothstep(relativeX);
      int lastFloorY = INT32_MIN;
      for (int iy = 0; iy < volume.sizeY; iy++) {
        double originalY = volume.blockY(iy) * yScale;
        double y = wrap(originalY) + offY;
        int floorY = Mth::floor(y);
        double relativeY = y - floorY;
        float alphaY = Mth::smoothstep((float)relativeY);
        if (lastFloorY != floorY) {
          int xy00 = permute(x0 + floorY), xy01 = permute(x0 + floorY + 1);
          int xy10 = permute(x1 + floorY), xy11 = permute(x1 + floorY + 1);
          auto& g000 = grad(xy00 + floorZ); d000xz = g000.dotXz(relativeX, relativeZ); g000y = (float)g000.y;
          auto& g100 = grad(xy10 + floorZ); d100xz = g100.dotXz(relativeX - 1.0f, relativeZ); g100y = (float)g100.y;
          auto& g010 = grad(xy01 + floorZ); d010xz = g010.dotXz(relativeX, relativeZ); g010y = (float)g010.y;
          auto& g110 = grad(xy11 + floorZ); d110xz = g110.dotXz(relativeX - 1.0f, relativeZ); g110y = (float)g110.y;
          auto& g001 = grad(xy00 + floorZ + 1); d001xz = g001.dotXz(relativeX, relativeZ - 1.0f); g001y = (float)g001.y;
          auto& g101 = grad(xy10 + floorZ + 1); d101xz = g101.dotXz(relativeX - 1.0f, relativeZ - 1.0f); g101y = (float)g101.y;
          auto& g011 = grad(xy01 + floorZ + 1); d011xz = g011.dotXz(relativeX, relativeZ - 1.0f); g011y = (float)g011.y;
          auto& g111 = grad(xy11 + floorZ + 1); d111xz = g111.dotXz(relativeX - 1.0f, relativeZ - 1.0f); g111y = (float)g111.y;
          lastFloorY = floorY;
        }
        float ry = relY(originalY, relativeY);
        buffer.addTo(index, amplitude * Mth::lerp3(alphaX, alphaY, alphaZ,
                                                   d000xz + g000y * ry, d100xz + g100y * ry,
                                                   d010xz + g010y * (ry - 1.0f), d110xz + g110y * (ry - 1.0f),
                                                   d001xz + g001y * ry, d101xz + g101y * ry,
                                                   d011xz + g011y * (ry - 1.0f), d111xz + g111y * (ry - 1.0f)));
        index++;
      }
    }
  }
}

void PerlinNoise::addToVolume(DensityBuffer& buffer, const DensityVolume& volume, double xzScale, double yScale,
                              float amplitude) const {
  // Java: float relativeY = (float)(y - floorY); alphaY = smoothstep(relativeY)  (same value as below)
  perlinAddToVolume(perms, GRADIENT, offsetX, offsetY, offsetZ, &GradientNoise::wrap, buffer, volume, xzScale,
                    yScale, amplitude, [](double, double relativeY) { return (float)relativeY; });
}

// ---------------------------------------------------------------- SmearedPerlinNoise
Interval SmearedPerlinNoise::range(double fudgeYScale) {
  return Interval::ofSymmetric((float)(std::fabs(fudgeYScale) + 2.0));
}

double SmearedPerlinNoise::computeFudgeY(double originalY, double relativeY) const {
  double fudgeLimit = originalY >= 0.0 && originalY < relativeY ? originalY : relativeY;
  return Mth::floor(fudgeLimit / fudgeYScale + (double)1.0E-7f) * fudgeYScale;
}

float SmearedPerlinNoise::get(double _x, double _y, double _z) const {
  double x = wrap(_x) + offsetX;
  double y = wrap(_y) + offsetY;
  double z = wrap(_z) + offsetZ;
  int floorX = Mth::floor(x), floorY = Mth::floor(y), floorZ = Mth::floor(z);
  float relativeX = (float)(x - floorX);
  double relativeY = y - floorY;
  float relativeZ = (float)(z - floorZ);
  float fudgedRelativeY = (float)(relativeY - computeFudgeY(_y, relativeY));
  return sampleAndLerp(floorX, floorY, floorZ, relativeX, fudgedRelativeY, relativeZ, (float)relativeY);
}

void SmearedPerlinNoise::addToVolume(DensityBuffer& buffer, const DensityVolume& volume, double xzScale,
                                     double yScale, float amplitude) const {
  perlinAddToVolume(perms, GRADIENT, offsetX, offsetY, offsetZ, &GradientNoise::wrap, buffer, volume, xzScale,
                    yScale, amplitude, [this](double originalY, double relativeY) {
                      return (float)(relativeY - computeFudgeY(originalY, relativeY));
                    });
}

// ---------------------------------------------------------------- SimplexNoise
static const double SQRT_3 = std::sqrt(3.0);
static const double F2 = 0.5 * (SQRT_3 - 1.0);
static const double G2 = (3.0 - SQRT_3) / 6.0;

double SimplexNoise::getCornerNoise3D(int index, double x, double y, double z, double base) const {
  double t0 = base - x * x - y * y - z * z;
  if (t0 < 0.0) return 0.0;
  t0 *= t0;
  return t0 * t0 * GRADIENT[index].dot(x, y, z);
}

float SimplexNoise::get(double _xin, double _yin) const {
  double xin = _xin + offsetX;
  double yin = _yin + offsetY;
  double s = (xin + yin) * F2;
  int i = Mth::floor(xin + s);
  int j = Mth::floor(yin + s);
  double t = (i + j) * G2;
  double X0 = i - t, Y0 = j - t;
  double x0 = xin - X0, y0 = yin - Y0;
  int i1, j1;
  if (x0 > y0) i1 = 1, j1 = 0;
  else i1 = 0, j1 = 1;
  double x1 = x0 - i1 + G2;
  double y1 = y0 - j1 + G2;
  double x2 = x0 - 1.0 + 2.0 * G2;
  double y2 = y0 - 1.0 + 2.0 * G2;
  int ii = i & 0xFF, jj = j & 0xFF;
  int gi0 = permute(ii + permute(jj)) % 12;
  int gi1 = permute(ii + i1 + permute(jj + j1)) % 12;
  int gi2 = permute(ii + 1 + permute(jj + 1)) % 12;
  double n0 = getCornerNoise3D(gi0, x0, y0, 0.0, 0.5);
  double n1 = getCornerNoise3D(gi1, x1, y1, 0.0, 0.5);
  double n2 = getCornerNoise3D(gi2, x2, y2, 0.0, 0.5);
  return (float)(70.0 * (n0 + n1 + n2));
}

float SimplexNoise::get(double _xin, double _yin, double _zin) const {
  double xin = _xin + offsetX, yin = _yin + offsetY, zin = _zin + offsetZ;
  double s = (xin + yin + zin) * 0.3333333333333333;
  int i = Mth::floor(xin + s), j = Mth::floor(yin + s), k = Mth::floor(zin + s);
  double t = (i + j + k) * 0.16666666666666666;
  double X0 = i - t, Y0 = j - t, Z0 = k - t;
  double x0 = xin - X0, y0 = yin - Y0, z0 = zin - Z0;
  int i1, j1, k1, i2, j2, k2;
  if (x0 >= y0) {
    if (y0 >= z0) i1 = 1, j1 = 0, k1 = 0, i2 = 1, j2 = 1, k2 = 0;
    else if (x0 >= z0) i1 = 1, j1 = 0, k1 = 0, i2 = 1, j2 = 0, k2 = 1;
    else i1 = 0, j1 = 0, k1 = 1, i2 = 1, j2 = 0, k2 = 1;
  } else if (y0 < z0) i1 = 0, j1 = 0, k1 = 1, i2 = 0, j2 = 1, k2 = 1;
  else if (x0 < z0) i1 = 0, j1 = 1, k1 = 0, i2 = 0, j2 = 1, k2 = 1;
  else i1 = 0, j1 = 1, k1 = 0, i2 = 1, j2 = 1, k2 = 0;
  double x1 = x0 - i1 + 0.16666666666666666, y1 = y0 - j1 + 0.16666666666666666, z1 = z0 - k1 + 0.16666666666666666;
  double x2 = x0 - i2 + 0.3333333333333333, y2 = y0 - j2 + 0.3333333333333333, z2 = z0 - k2 + 0.3333333333333333;
  double x3 = x0 - 1.0 + 0.5, y3 = y0 - 1.0 + 0.5, z3 = z0 - 1.0 + 0.5;
  int ii = i & 0xFF, jj = j & 0xFF, kk = k & 0xFF;
  int gi0 = permute(ii + permute(jj + permute(kk))) % 12;
  int gi1 = permute(ii + i1 + permute(jj + j1 + permute(kk + k1))) % 12;
  int gi2 = permute(ii + i2 + permute(jj + j2 + permute(kk + k2))) % 12;
  int gi3 = permute(ii + 1 + permute(jj + 1 + permute(kk + 1))) % 12;
  double n0 = getCornerNoise3D(gi0, x0, y0, z0, 0.6);
  double n1 = getCornerNoise3D(gi1, x1, y1, z1, 0.6);
  double n2 = getCornerNoise3D(gi2, x2, y2, z2, 0.6);
  double n3 = getCornerNoise3D(gi3, x3, y3, z3, 0.6);
  return (float)(32.0 * (n0 + n1 + n2 + n3));
}

// ---------------------------------------------------------------- NoiseStack
NoiseStack::Builder& NoiseStack::Builder::add(std::shared_ptr<const Noise> noise, double frequency, float amplitude) {
  layers.push_back({std::move(noise), frequency, amplitude});
  return *this;
}

NoiseStack::Builder& NoiseStack::Builder::addStack(const NoiseStack& stack, double frequency, float amplitude) {
  for (auto& l : stack.layers_) layers.push_back({l.noise, l.frequency * frequency, l.amplitude * amplitude});
  return *this;
}

NoiseStack::NoiseStack(std::vector<Layer> layers) : layers_(std::move(layers)), range_(Interval::ofExact(0.0f)) {
  for (auto& l : layers_) range_ = Interval::add(range_, Interval::mul(l.noise->range(), Interval::ofExact(l.amplitude)));
}

float NoiseStack::get(double x, double y, double z) const {
  float value = 0.0f;
  for (auto& l : layers_) value += l.amplitude * l.noise->get(x * l.frequency, y * l.frequency, z * l.frequency);
  return value;
}

float NoiseStack::get(double x, double y) const {
  float value = 0.0f;
  for (auto& l : layers_) value += l.amplitude * l.noise->get(x * l.frequency, y * l.frequency);
  return value;
}

void NoiseStack::addToVolume(DensityBuffer& buffer, const DensityVolume& volume, double xzScale, double yScale,
                             float amplitude) const {
  for (auto& l : layers_)
    l.noise->addToVolume(buffer, volume, xzScale * l.frequency, yScale * l.frequency, amplitude * l.amplitude);
}

// ---------------------------------------------------------------- LegacyFbmInitializer
std::shared_ptr<NoiseStack> LegacyFbmInitializer::createForLegacyNetherBiome(RandomSource& random, int firstOctave,
                                                                             const std::vector<double>& amplitudes) {
  int octaves = (int)amplitudes.size();
  int zeroOctaveIndex = -firstOctave;
  std::vector<std::shared_ptr<const Noise>> noiseLevels(octaves);
  auto zeroOctave = std::make_shared<PerlinNoise>(random);
  if (zeroOctaveIndex >= 0 && zeroOctaveIndex < octaves && amplitudes[zeroOctaveIndex] != 0.0)
    noiseLevels[zeroOctaveIndex] = zeroOctave;
  for (int i = zeroOctaveIndex - 1; i >= 0; i--) {
    if (i < octaves && amplitudes[i] != 0.0) noiseLevels[i] = std::make_shared<PerlinNoise>(random);
    else random.consumeCount(262);  // skipOctave
  }
  long created = std::count_if(noiseLevels.begin(), noiseLevels.end(), [](auto& n) { return n != nullptr; });
  long nonZero = std::count_if(amplitudes.begin(), amplitudes.end(), [](double a) { return a != 0.0; });
  if (created != nonZero)
    throw std::logic_error("Failed to create correct number of noise levels for given non-zero amplitudes");
  if (zeroOctaveIndex < octaves - 1) throw std::invalid_argument("Positive octaves are temporarily disabled");
  double factor = std::pow(2.0, -zeroOctaveIndex);
  double valueFactor = std::pow(2.0, octaves - 1) / (std::pow(2.0, octaves) - 1.0);
  NoiseStack::Builder stack;
  for (int i = 0; i < octaves; i++) {
    if (noiseLevels[i]) stack.add(noiseLevels[i], factor, (float)(valueFactor * amplitudes[i]));
    factor *= 2.0;
    valueFactor /= 2.0;
  }
  return stack.build();
}

// ---------------------------------------------------------------- NormalNoise
// Java DoubleStream.sum(): Kahan compensated summation (JDK 18+ final step).
static double javaDoubleSum(const std::vector<double>& v) {
  double s0 = 0, s1 = 0, simple = 0;
  for (double d : v) {
    double tmp = d - s1;
    double velvel = s0 + tmp;
    s1 = (velvel - s0) - tmp;
    s0 = velvel;
    simple += d;
  }
  double tmp = s0 - s1;
  return std::isnan(tmp) && std::isinf(simple) ? simple : tmp;
}

static double getAmplitudeModifier(const std::vector<double>& mods, int i) { return mods.empty() ? 1.0 : mods[i]; }

static double parityExpectedDeviation(int octaveSpan) { return 0.1 * (1.0 + 1.0 / (octaveSpan + 1)); }

static double computeParityNormalizationFactor(double baseAmplitude, int octaveCount, const std::vector<double>& mods) {
  int minOctave = INT32_MAX, maxOctave = INT32_MIN;
  for (int i = 0; i < octaveCount; i++) {
    if (getAmplitudeModifier(mods, i) != 0.0) {
      minOctave = std::min(minOctave, i);
      maxOctave = std::max(maxOctave, i);
    }
  }
  int span = (int32_t)((uint32_t)maxOctave - (uint32_t)minOctave);
  return baseAmplitude * 0.5 * 0.3333333333333333 / parityExpectedDeviation(span);
}

NormalNoise::Parameters NormalNoise::Parameters::fromJson(const Json& j) {
  Parameters p;
  p.baseAmplitude = j.getDouble("base_amplitude", 1.0);
  p.baseOctave = j["base_octave"].asInt();
  p.octaveCount = j.getInt("octave_count", 1);
  if (auto* n = j.find("normalize")) {
    if (n->isBool()) p.normalize = n->asBool() ? Normalization::ENABLED : Normalization::DISABLED;
    else if (n->asString() == "legacy") p.normalize = Normalization::LEGACY;
    else throw std::runtime_error("Invalid normalization type: " + n->asString());
  }
  if (auto* m = j.find("amplitude_modifiers"))
    for (auto& v : m->arr) p.amplitudeModifiers.push_back(v.asDouble());
  if (!p.amplitudeModifiers.empty() && (int)p.amplitudeModifiers.size() != p.octaveCount)
    throw std::runtime_error("amplitude_modifiers size does not match octave_count");
  return p;
}

NormalNoise::NormalNoise(Parameters parameters) : parameters_(std::move(parameters)) {
  const Parameters& p = parameters_;
  double frequency = std::pow(2.0, p.baseOctave);
  double amplitude = p.baseAmplitude;
  if (p.normalize != Normalization::DISABLED)
    amplitude = p.baseAmplitude * (std::pow(0.5, -(p.octaveCount - 1)) / (std::pow(0.5, -p.octaveCount) - 1.0));
  for (int i = 0; i < p.octaveCount; i++) {
    double mod = getAmplitudeModifier(p.amplitudeModifiers, i);
    if (mod != 0.0) octaves.push_back({p.baseOctave + i, frequency, amplitude * mod});
    frequency *= 2.0;
    amplitude *= 0.5;
  }

  std::vector<double> abs;
  for (auto& o : octaves) abs.push_back(std::fabs(o.amplitude));
  double targetAmplitude = javaDoubleSum(abs);

  // computeNormalizationFactor / estimateDeviation
  double variance = 0.0;
  for (auto& o : octaves) variance += Mth::square(PerlinNoise::STANDARD_DEVIATION * std::fabs(o.amplitude));
  double inputDeviation = std::sqrt(variance);
  double nf = 0.0;
  if (inputDeviation != 0.0) nf = targetAmplitude * 0.3333333333333333 / (inputDeviation * std::sqrt(2.0));

  if (p.normalize == Normalization::LEGACY && nf != 0.0) {
    double parity = computeParityNormalizationFactor(p.baseAmplitude, p.octaveCount, p.amplitudeModifiers);
    targetAmplitude *= parity / nf;
    nf = parity;
  }
  normalizationFactor = nf;
  range_ = Interval::ofSymmetric((float)(targetAmplitude * 0.3333333333333333 * 6.0));
}

std::shared_ptr<NoiseStack> NormalNoise::create(RandomSource& random) const {
  auto firstRandom = random.forkPositional();
  auto secondRandom = random.forkPositional();
  NoiseStack::Builder stack;
  for (auto& o : octaves) {
    std::string octaveSeed = "octave_" + std::to_string(o.octaveIndex);
    auto firstNoise = std::make_shared<PerlinNoise>(*firstRandom->fromHashOf(octaveSeed));
    auto secondNoise = std::make_shared<PerlinNoise>(*secondRandom->fromHashOf(octaveSeed));
    float valueFactor = (float)(normalizationFactor * o.amplitude);
    stack.add(firstNoise, o.frequency, valueFactor);
    stack.add(secondNoise, o.frequency * 1.0181268882175227, valueFactor);
  }
  return stack.build();
}

std::shared_ptr<NoiseStack> NormalNoise::createForLegacyNetherBiome(RandomSource& random) const {
  std::vector<double> amplitudes = parameters_.amplitudeModifiers;
  if (amplitudes.empty()) amplitudes.assign(parameters_.octaveCount, 1.0);
  auto first = LegacyFbmInitializer::createForLegacyNetherBiome(random, parameters_.baseOctave, amplitudes);
  auto second = LegacyFbmInitializer::createForLegacyNetherBiome(random, parameters_.baseOctave, amplitudes);
  float valueFactor = (float)(normalizationFactor * parameters_.baseAmplitude);
  return NoiseStack::Builder().addStack(*first, 1.0, valueFactor).addStack(*second, 1.0181268882175227, valueFactor).build();
}

// ---------------------------------------------------------------- BlendedNoise
static constexpr double BASE_SCALE = 684.412;
static constexpr double LIMIT_FACTOR = 0.9999847412109375;  // (double)0.99998474F
static constexpr double MAIN_FACTOR = 12.75;

BlendedNoise BlendedNoise::fromJson(const Json& j) {
  return {j["xz_scale"].asDouble(), j["y_scale"].asDouble(), j["xz_factor"].asDouble(), j["y_factor"].asDouble(),
          j["smear_scale_multiplier"].asDouble()};
}

std::shared_ptr<NoiseStack> BlendedNoise::createFbm(RandomSource& random, int firstOctave, double smearScaleY,
                                                    double valueFactor) {
  if (firstOctave > 0) throw std::invalid_argument("firstOctave>0");
  int octaves = -firstOctave + 1;
  double factor = 1.0;
  valueFactor /= std::pow(2.0, octaves) - 1.0;
  NoiseStack::Builder stack;
  for (int i = octaves - 1; i >= 0; i--) {
    stack.add(std::make_shared<SmearedPerlinNoise>(random, smearScaleY * factor), factor, (float)valueFactor);
    factor /= 2.0;
    valueFactor *= 2.0;
  }
  return stack.build();
}

Interval BlendedNoise::range() const {
  double smearScaleY = BASE_SCALE * yScale * smearScaleMultiplier;
  int octaves = 16;  // -LIMIT_FIRST_OCTAVE + 1
  double factor = 1.0;
  double valueFactor = LIMIT_FACTOR / (std::pow(2.0, octaves) - 1.0);
  Interval range = Interval::ofExact(0.0f);
  for (int i = octaves - 1; i >= 0; i--) {
    range = Interval::add(range, Interval::mul(SmearedPerlinNoise::range(smearScaleY * factor),
                                               Interval::ofExact((float)valueFactor)));
    factor /= 2.0;
    valueFactor *= 2.0;
  }
  return range;
}

BlendedNoise::Sampler BlendedNoise::compileSampler(RandomSource& random) const {
  double limitSmearScaleY = BASE_SCALE * yScale * smearScaleMultiplier;
  double mainSmearScaleY = limitSmearScaleY / yFactor;
  Sampler s;
  s.minLimitNoise = createFbm(random, -15, limitSmearScaleY, LIMIT_FACTOR);
  s.maxLimitNoise = createFbm(random, -15, limitSmearScaleY, LIMIT_FACTOR);
  s.mainNoise = createFbm(random, -7, mainSmearScaleY, MAIN_FACTOR);
  s.xzMultiplier = BASE_SCALE * xzScale;
  s.yMultiplier = BASE_SCALE * yScale;
  s.mainXzMultiplier = s.xzMultiplier / xzFactor;
  s.mainYMultiplier = s.yMultiplier / yFactor;
  return s;
}

float BlendedNoise::Sampler::sampleValue(int bx, int by, int bz) const {
  // NoiseFunction.Sampler.sampleValue: noise.get(blockX * xzScale, blockY * yScale, blockZ * xzScale)
  float alpha = Mth::clamp(mainNoise->get(bx * mainXzMultiplier, by * mainYMultiplier, bz * mainXzMultiplier) + 0.5f,
                           0.0f, 1.0f);
  auto first = [&] { return minLimitNoise->get(bx * xzMultiplier, by * yMultiplier, bz * xzMultiplier); };
  auto second = [&] { return maxLimitNoise->get(bx * xzMultiplier, by * yMultiplier, bz * xzMultiplier); };
  if (alpha == 0.0f) return first();
  if (alpha == 1.0f) return second();
  float f = first();
  return Mth::lerp(alpha, f, second());
}

void BlendedNoise::Sampler::sampleVolume(DensityBuffer& out, const DensityVolume& volume) const {
  out.fill(0.0f);
  mainNoise->addToVolume(out, volume, mainXzMultiplier, mainYMultiplier, 1.0f);
  DensityBuffer first(out.size()), second(out.size());
  minLimitNoise->addToVolume(first, volume, xzMultiplier, yMultiplier, 1.0f);
  maxLimitNoise->addToVolume(second, volume, xzMultiplier, yMultiplier, 1.0f);
  for (int i = 0; i < out.size(); i++) {
    float alpha = Mth::clamp(out.get(i) + 0.5f, 0.0f, 1.0f);
    out.set(i, alpha == 0.0f ? first.get(i) : alpha == 1.0f ? second.get(i) : Mth::lerp(alpha, first.get(i), second.get(i)));
  }
}

// ---------------------------------------------------------------- Noises / RandomState
static std::string fullId(std::string_view id) {
  return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
}

NormalNoise loadNormalNoise(std::string_view id, const std::string& dataRoot) {
  std::string full = fullId(id);
  size_t colon = full.find(':');
  std::string path = dataRoot + "/" + full.substr(0, colon) + "/worldgen/noise/" + full.substr(colon + 1) + ".json";
  return NormalNoise(NormalNoise::Parameters::fromJson(Json::parseFile(path)));
}

std::shared_ptr<NoiseStack> instantiateNoise(const PositionalRandomFactory& random, std::string_view id,
                                             const std::string& dataRoot) {
  std::string full = fullId(id);
  return loadNormalNoise(full, dataRoot).create(*random.fromHashOf(full));
}

RandomState::RandomState(int64_t seed, bool useLegacyRandom, std::string dataRoot)
    : seed_(seed), useLegacyRandom(useLegacyRandom), dataRoot(std::move(dataRoot)) {
  auto alg = useLegacyRandom ? WorldgenRandom::Algorithm::LEGACY : WorldgenRandom::Algorithm::XOROSHIRO;
  random_ = WorldgenRandom::newInstance(alg, seed)->forkPositional();
}

std::shared_ptr<const Noise> RandomState::getOrCreateNoise(const std::string& id) {
  std::string full = fullId(id);
  auto& slot = noiseInstances[full];
  if (!slot) slot = instantiateNoise(*random_, full, dataRoot);
  return slot;
}

std::shared_ptr<const Noise> RandomState::createNoiseSampler(const std::string& id) {
  std::string full = fullId(id);
  if (full == "minecraft:nether/temperature" || full == "minecraft:nether/vegetation") {
    LegacyRandomSource legacy((int64_t)((uint64_t)seed_ + (full == "minecraft:nether/temperature" ? 0u : 1u)));
    return loadNormalNoise(full, dataRoot).createForLegacyNetherBiome(legacy);
  }
  return getOrCreateNoise(full);
}

std::unique_ptr<RandomSource> RandomState::createRandom(std::string_view id) const {
  std::string full = fullId(id);
  if (useLegacyRandom && full == BlendedNoise::NOISE_SEED) return std::make_unique<LegacyRandomSource>(seed_);
  return random_->fromHashOf(full);
}

const PositionalRandomFactory& RandomState::getOrCreateRandomFactory(const std::string& id) {
  std::string full = fullId(id);
  auto& slot = positionalRandoms[full];
  if (!slot) slot = random_->fromHashOf(full)->forkPositional();
  return *slot;
}
