// Port of net.minecraft.world.level.levelgen.synth (26.3): float-valued noises, NoiseStack, NormalNoise,
// BlendedNoise, plus Noises.instantiate / the noise parts of RandomState.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "worldgen/json.hpp"
#include "worldgen/random.hpp"

// net.minecraft.util.Interval (only what noise needs; NaI = NaN bounds).
struct Interval {
  float min = 0, max = 0;
  static Interval ofSymmetric(float r) { return {-r, r}; }
  static Interval ofExact(float v) { return {v, v}; }
  static Interval add(Interval a, Interval b);
  static Interval mul(Interval a, Interval b);
};

// densityfunction.DensityVolume / DensityBuffer (index = y + (x + z * sizeX) * sizeY).
struct DensityVolume {
  int sizeX, sizeY, sizeZ, minBlockX, minBlockY, minBlockZ, stepBlockX = 1, stepBlockY = 1, stepBlockZ = 1;
  int blockX(int x) const { return minBlockX + x * stepBlockX; }
  int blockY(int y) const { return minBlockY + y * stepBlockY; }
  int blockZ(int z) const { return minBlockZ + z * stepBlockZ; }
  int size() const { return sizeX * sizeY * sizeZ; }
};
struct DensityBuffer {
  std::vector<float> values;
  explicit DensityBuffer(int size) : values(size) {}
  void addTo(int i, float v) { values[i] = values[i] + v; }
  void set(int i, float v) { values[i] = v; }
  float get(int i) const { return values[i]; }
  void fill(float v) { std::fill(values.begin(), values.end(), v); }
  int size() const { return (int)values.size(); }
};

class Noise {
 public:
  virtual ~Noise() = default;
  virtual Interval range() const = 0;
  virtual float get(double x, double y) const = 0;
  virtual float get(double x, double y, double z) const = 0;
  virtual void addToVolume(DensityBuffer& buffer, const DensityVolume& volume, double xzScale, double yScale,
                           float amplitude) const;
};

class GradientNoise : public Noise {
 public:
  struct Gradient {
    int x, y, z;
    double dot(double dx, double dy, double dz) const { return x * dx + y * dy + z * dz; }
    float dot(float dx, float dy, float dz) const { return (float)x * dx + (float)y * dy + (float)z * dz; }
    float dotXz(float dx, float dz) const { return (float)x * dx + (float)z * dz; }
  };
  static const Gradient GRADIENT[16];

 protected:
  explicit GradientNoise(RandomSource& random, double noiseOffsetScale = 256.0);
  int permute(int x) const { return perms[x & 0xFF]; }
  const Gradient& permuteToGrad(int x) const { return GRADIENT[permute(x) & 15]; }
  static float gradDot(int hash, float x, float y, float z) { return GRADIENT[hash & 15].dot(x, y, z); }
  static double wrap(double x);

  uint8_t perms[256];
  double offsetX, offsetY, offsetZ;
};

class PerlinNoise : public GradientNoise {
 public:
  static constexpr double STANDARD_DEVIATION = 0.2702247831245211;
  explicit PerlinNoise(RandomSource& random) : GradientNoise(random) {}
  Interval range() const override { return Interval::ofSymmetric(2.0f); }
  float get(double x, double y) const override { return get(wrap(x), 0.0, wrap(y)); }
  float get(double x, double y, double z) const override;
  void addToVolume(DensityBuffer& buffer, const DensityVolume& volume, double xzScale, double yScale,
                   float amplitude) const override;

 protected:
  float sampleAndLerp(int x, int y, int z, float relativeX, float relativeY, float relativeZ,
                      float originalRelativeY) const;
};

class SmearedPerlinNoise : public PerlinNoise {
 public:
  SmearedPerlinNoise(RandomSource& random, double fudgeYScale) : PerlinNoise(random), fudgeYScale(fudgeYScale) {}
  static Interval range(double fudgeYScale);
  Interval range() const override { return range(fudgeYScale); }
  using PerlinNoise::get;
  float get(double x, double y, double z) const override;
  void addToVolume(DensityBuffer& buffer, const DensityVolume& volume, double xzScale, double yScale,
                   float amplitude) const override;

 private:
  double computeFudgeY(double originalY, double relativeY) const;
  double fudgeYScale;
};

class SimplexNoise : public GradientNoise {
 public:
  explicit SimplexNoise(RandomSource& random, bool discardNoiseOffset = false)
      : GradientNoise(random, discardNoiseOffset ? 0.0 : 256.0) {}
  Interval range() const override { return Interval::ofSymmetric(2.0f); }
  float get(double x, double y) const override;
  float get(double x, double y, double z) const override;

 private:
  double getCornerNoise3D(int index, double x, double y, double z, double base) const;
};

class NoiseStack : public Noise {
 public:
  struct Layer {
    std::shared_ptr<const Noise> noise;
    double frequency;
    float amplitude;
  };
  struct Builder {
    std::vector<Layer> layers;
    Builder& add(std::shared_ptr<const Noise> noise, double frequency, float amplitude);
    Builder& addStack(const NoiseStack& stack, double frequency, float amplitude);
    std::shared_ptr<NoiseStack> build() { return std::make_shared<NoiseStack>(std::move(layers)); }
  };

  explicit NoiseStack(std::vector<Layer> layers);
  Interval range() const override { return range_; }
  float get(double x, double y, double z) const override;
  float get(double x, double y) const override;
  void addToVolume(DensityBuffer& buffer, const DensityVolume& volume, double xzScale, double yScale,
                   float amplitude) const override;
  const std::vector<Layer>& layers() const { return layers_; }

 private:
  std::vector<Layer> layers_;
  Interval range_;
};

namespace LegacyFbmInitializer {
std::shared_ptr<NoiseStack> createForLegacyNetherBiome(RandomSource& random, int firstOctave,
                                                       const std::vector<double>& amplitudes);
}

class NormalNoise {
 public:
  enum class Normalization { DISABLED, ENABLED, LEGACY };
  struct Parameters {
    double baseAmplitude = 1.0;
    int baseOctave = 0;
    int octaveCount = 1;
    Normalization normalize = Normalization::ENABLED;
    std::vector<double> amplitudeModifiers;  // empty = all 1.0
    static Parameters fromJson(const Json& j);
  };

  explicit NormalNoise(Parameters parameters);
  std::shared_ptr<NoiseStack> create(RandomSource& random) const;
  std::shared_ptr<NoiseStack> createForLegacyNetherBiome(RandomSource& random) const;  // deprecated in vanilla
  Interval range() const { return range_; }
  const Parameters& parameters() const { return parameters_; }

 private:
  struct OctaveInfo {
    int octaveIndex;
    double frequency, amplitude;
  };
  Parameters parameters_;
  std::vector<OctaveInfo> octaves;
  double normalizationFactor;
  Interval range_;
};

// "minecraft:old_blended_noise" density function.
struct BlendedNoise {
  double xzScale, yScale, xzFactor, yFactor, smearScaleMultiplier;
  static constexpr const char* NOISE_SEED = "minecraft:terrain";

  static BlendedNoise fromJson(const Json& j);
  static std::shared_ptr<NoiseStack> createFbm(RandomSource& random, int firstOctave, double smearScaleY,
                                               double valueFactor);
  Interval range() const;

  // Equivalent of compileSampler(random): lerp(clamp(main + 0.5, 0, 1), minLimit, maxLimit).
  struct Sampler {
    std::shared_ptr<NoiseStack> minLimitNoise, maxLimitNoise, mainNoise;
    double xzMultiplier, yMultiplier, mainXzMultiplier, mainYMultiplier;
    float sampleValue(int blockX, int blockY, int blockZ) const;
    void sampleVolume(DensityBuffer& out, const DensityVolume& volume) const;
  };
  Sampler compileSampler(RandomSource& random) const;
};

// Loads <dataRoot>/<namespace>/worldgen/noise/<path>.json, e.g. id "minecraft:temperature".
NormalNoise loadNormalNoise(std::string_view id, const std::string& dataRoot = "vanilla/src/data");

// Noises.instantiate: parameters.create(random.fromHashOf(id)).
std::shared_ptr<NoiseStack> instantiateNoise(const PositionalRandomFactory& random, std::string_view id,
                                             const std::string& dataRoot = "vanilla/src/data");

// The noise/random parts of levelgen.RandomState.
class RandomState {
 public:
  RandomState(int64_t seed, bool useLegacyRandom, std::string dataRoot = "vanilla/src/data");
  int64_t seed() const { return seed_; }
  const PositionalRandomFactory& random() const { return *random_; }
  // RandomState.getOrCreateNoise (cached).
  std::shared_ptr<const Noise> getOrCreateNoise(const std::string& id);
  // CompileContext.createNoiseSampler: nether temperature/vegetation use the legacy nether init.
  std::shared_ptr<const Noise> createNoiseSampler(const std::string& id);
  // CompileContext.createRandom.
  std::unique_ptr<RandomSource> createRandom(std::string_view id) const;
  // RandomState.getOrCreateRandomFactory (cached).
  const PositionalRandomFactory& getOrCreateRandomFactory(const std::string& id);

 private:
  int64_t seed_;
  bool useLegacyRandom;
  std::string dataRoot;
  std::unique_ptr<PositionalRandomFactory> random_;
  std::map<std::string, std::shared_ptr<const Noise>> noiseInstances;
  std::map<std::string, std::unique_ptr<PositionalRandomFactory>> positionalRandoms;
};
