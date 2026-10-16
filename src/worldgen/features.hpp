// Biome decoration (ChunkGenerator.applyBiomeDecoration): placed features per GenerationStep.Decoration.
#pragma once
#include <string>

#include "worldgen/pipeline.hpp"

namespace worldgen {

struct Overworld;

class Features {
 public:
  Features(Overworld& ow, const std::string& dataRoot);
  ~Features();
  void decorate(Region& r);  // the FEATURES step

 private:
  Overworld& ow;
};

}  // namespace worldgen
