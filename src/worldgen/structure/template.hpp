// StructureTemplate (structure .nbt files), StructurePlaceSettings, structure processors and
// StructureTemplate.placeInWorld / processBlockInfos. Templates load lazily from data/<ns>/structure/<path>.nbt
// and stay cached (block lists as state ids; block entity NBT is reduced to what generation reads).
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "worldgen/structure/structure.hpp"

namespace structure {

struct BlockInfo {
  BlockPos pos;
  int state;
  int nbt = -1;  // -1: no block entity NBT; otherwise index into Template::nbts (or NBT_SYNTHETIC)
  bool operator==(const BlockInfo& o) const { return pos == o.pos && state == o.state && nbt == o.nbt; }
};
inline constexpr int NBT_SYNTHETIC = 1 << 30;  // processor-produced tag (append_loot / append_static)

struct JigsawInfo {  // StructureTemplate.JigsawBlockInfo
  BlockPos pos;
  int state;
  bool rollable;
  std::string name;  // empty: null name (feature pool element)
  std::string pool, target;
  int placementPriority = 0, selectionPriority = 0;
};
Dir jigsawFront(int state);
Dir jigsawTop(int state);
bool canAttach(const JigsawInfo& source, const JigsawInfo& target);

struct TemplateNbt {  // the parts of block entity NBT that generation reads
  std::string finalState = "minecraft:air";  // jigsaw final_state
  std::string metadata, mode;                // structure block data marker
  std::string lootTable;                     // RandomizableContainer LootTable
};

struct Template {
  BlockPos size;
  std::vector<std::vector<BlockInfo>> palettes;      // StructureTemplate.Palette.blocks (vanilla order)
  std::vector<std::vector<JigsawInfo>> jigsaws;      // per palette, Palette.jigsaws()
  std::vector<TemplateNbt> nbts;
  bool hasEntities = false;

  BlockPos sizeFor(Rotation r) const {
    return r == Rotation::CLOCKWISE_90 || r == Rotation::COUNTERCLOCKWISE_90 ? BlockPos{size.z, size.y, size.x} : size;
  }
  static const Template& get(const std::string& id);  // StructureTemplateManager.getOrCreate (empty when missing)
};

struct Processor;
struct PlaceSettings {
  Mirror mirror = Mirror::NONE;
  Rotation rotation = Rotation::NONE;
  BlockPos pivot;
  bool ignoreEntities = false;
  std::optional<BoundingBox> boundingBox;
  bool applyWaterlogging = true;
  RandomSource* random = nullptr;  // shared random when set (setRandom)
  bool knownShape = false;
  bool finalizeEntities = false;
  std::vector<std::shared_ptr<const Processor>> processors;
  mutable const struct Template* tmpl = nullptr;  // template being processed (jigsaw NBT lookups)

  // getRandom(pos): the shared random, or RandomSource.create(Mth.getSeed(pos)) owned by `holder`.
  RandomSource& getRandom(BlockPos pos, std::unique_ptr<RandomSource>& holder) const;
  const std::vector<BlockInfo>& randomPalette(const Template& t, BlockPos pos, int* index = nullptr) const;
};

BlockPos transform(BlockPos p, Mirror m, Rotation r, BlockPos pivot);
inline BlockPos relativePosition(const PlaceSettings& s, BlockPos p) { return transform(p, s.mirror, s.rotation, s.pivot); }
BoundingBox templateBoundingBox(const Template& t, const PlaceSettings& s, BlockPos pos);
BlockPos zeroPositionWithTransform(BlockPos zero, Mirror m, Rotation r, int sizeX, int sizeZ);
std::vector<JigsawInfo> templateJigsaws(const Template& t, BlockPos pos, Rotation r);  // getJigsaws
// filterBlocks(position, settings, block[, absolute])
std::vector<BlockInfo> filterBlocks(const Template& t, BlockPos pos, const PlaceSettings& s, std::string_view block, bool absolute = true);
std::vector<BlockInfo> processBlockInfos(Level& level, BlockPos pos, BlockPos ref, const PlaceSettings& s,
                                         const std::vector<BlockInfo>& infos, const Template& t);
bool placeInWorld(const Template& t, Level& level, BlockPos pos, BlockPos ref, const PlaceSettings& s, RandomSource& random);

// ------------------------------------------------------------------------------------------- processors
struct Processor {
  virtual ~Processor() = default;
  // false: block removed (vanilla null)
  virtual bool process(Level& level, BlockPos target, BlockPos ref, BlockPos templatePos, BlockInfo& info,
                       const PlaceSettings& s) const {
    (void)level, (void)target, (void)ref, (void)templatePos, (void)info, (void)s;
    return true;
  }
  virtual void finalize(Level& level, BlockPos pos, BlockPos ref, const std::vector<BlockInfo>& original,
                        std::vector<BlockInfo>& processed, const PlaceSettings& s) const {
    (void)level, (void)pos, (void)ref, (void)original, (void)processed, (void)s;
  }
  virtual bool entirePiece() const { return false; }
};
using ProcessorP = std::shared_ptr<const Processor>;
using ProcessorList = std::vector<ProcessorP>;

// processor_list holder: "id", {"processors": [...]} or [...].
const ProcessorList& processorList(const Json& j);
ProcessorP blockIgnore(std::vector<std::string> blocks);
ProcessorP jigsawReplacement();
ProcessorP gravity(Heightmap h, int offset);
ProcessorP blockRot(float integrity, std::string tag = "");
ProcessorP blockAge(float mossiness);
ProcessorP blackstoneReplace();
ProcessorP lavaSubmerged();

// Fluid state helpers (FluidState of a block state).
bool isWaterSource(int state);
bool isFluidSource(int state);

}  // namespace structure
