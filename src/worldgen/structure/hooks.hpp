// Wires structure generation into the worldgen pipeline (pipeline.hpp hooks):
//   STRUCTURE_STARTS      ChunkGenerator.createStructures -> ProtoChunk::structure_data
//   STRUCTURE_REFERENCES  createReferences
//   TERRAIN               Overworld::beardifier (Beardifier.forStructuresInChunk)
//   FEATURES              piece placement (applyBiomeDecoration's structure part), when `place`
#pragma once
#include <memory>

#include "worldgen/structure/structure.hpp"

namespace worldgen { struct Overworld; }

namespace structure {

class StructureWorld {
 public:
  StructureWorld(worldgen::Overworld& ow, bool place);
  ~StructureWorld();
  StructureManager& manager();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace structure
