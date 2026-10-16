// Block state registry for structure generation: name/property <-> state id (from the committed
// java/src/main/resources/block-states-26.3.txt table), BlockState.rotate/mirror and per-state flags extracted
// from the vanilla jar at build time (data/block_states.bin, tools/gen_block_dump.py), and block/biome tags
// read from vanilla/src/data/minecraft/tags. No per-state heap data: properties are decoded arithmetically.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "worldgen/json.hpp"

namespace structure {

enum class Rotation : uint8_t { NONE, CLOCKWISE_90, CLOCKWISE_180, COUNTERCLOCKWISE_90 };
enum class Mirror : uint8_t { NONE, LEFT_RIGHT, FRONT_BACK };

// data/block_states.bin flag bits (BlockDump.java).
enum : uint32_t {
  BS_TEMPLATE_FULL = 1,       // !hasDynamicShape && isCollisionShapeFullBlock (StructureTemplate.addToLists)
  BS_WATERLOGGABLE = 4,       // SimpleWaterloggedBlock
  BS_LIQUID_CONTAINER = 8,    // LiquidBlockContainer
  BS_FLUID_SOURCE = 16,       // getFluidState().isSource()
  BS_HAS_FLUID = 32,          // !getFluidState().isEmpty()
  BS_AIR = 64,
  BS_REPLACEABLE = 128,       // canBeReplaced()
  BS_SOLID = 256,             // isSolid()
  BS_BLOCK_ENTITY = 512,
  BS_STURDY_UP = 1024,        // isFaceSturdy(UP)
  BS_SOLID_RENDER = 2048,     // isSolidRender()
  BS_LIQUID = 4096,           // liquid()
  BS_RANDOMIZABLE = 8192,     // block entity is a RandomizableContainer (LootTableSeed consumes random.nextLong)
  BS_FACE_FULL_DOWN = 1u << 16,  // Block.isFaceFull(collision shape, dir): bit 16 + Direction ordinal
  BS_SHAPE_FULL = 1u << 22,      // Block.isShapeFullBlock(getShape)
};

class Blocks {
 public:
  static const Blocks& get();  // loaded once, thread-safe

  struct Block {
    std::string name;  // "minecraft:oak_stairs"
    int first, def;
    std::vector<std::pair<std::string, std::vector<std::string>>> props;  // sorted by name; last varies fastest
  };
  int blockOf(int state) const;  // index into blocks()
  const Block& block(int state) const { return blocks_[blockOf(state)]; }
  int blockIndex(std::string_view name) const;  // -1 if unknown ("minecraft:" optional)
  int defaultState(std::string_view name) const;  // -1 if unknown
  bool is(int state, std::string_view name) const { return blockOf(state) == blockIndex(name); }
  // Property access; "" / unchanged state when the block lacks the property or value.
  std::string prop(int state, std::string_view key) const;
  int with(int state, std::string_view key, std::string_view value) const;
  bool hasProp(int state, std::string_view key) const;
  // BlockState.CODEC style {Name, Properties} with defaults for missing properties.
  int state(std::string_view name, const std::vector<std::pair<std::string, std::string>>& props) const;
  // BlockStateParser.parseForBlock: "minecraft:foo[a=b,c=d]" (NBT suffix ignored). -1 on error.
  int parse(std::string_view s) const;

  int rotate(int state, Rotation r) const;
  int mirror(int state, Mirror m) const;
  uint32_t flags(int state) const { return flags_[state]; }
  bool faceFull(int state, int dirOrdinal) const { return flags_[state] >> (16 + dirOrdinal) & 1; }
  bool isAir(int state) const { return flags_[state] & BS_AIR; }

  // Block tag membership ("minecraft:logs" or "#minecraft:logs"); unknown tag -> false.
  bool inTag(int state, std::string_view tag) const;
  const std::vector<uint8_t>& tagMask(std::string_view tag) const;  // per block index

 private:
  Blocks();
  std::vector<Block> blocks_;
  std::vector<int> firstStates_;  // sorted first state per block (blocks_ is sorted by first)
  std::unordered_map<std::string, int> byName_;
  std::vector<uint16_t> transforms_;  // 5 per state
  std::vector<uint32_t> flags_;
  mutable std::unordered_map<std::string, std::vector<uint8_t>> tags_;
  mutable std::mutex tagLock_;
};

// Generic tag resolver for data/minecraft/tags/<kind>/... ("worldgen/biome", "block"): names in the tag.
std::vector<std::string> resolveTag(const std::string& kind, std::string_view tag);
// Biome registry ids (= biomes:: ids, sorted file names of worldgen/biome).
int biomeId(std::string_view name);
const std::vector<std::string>& biomeNames();
// HolderSet<Biome> codec: "#tag", "minecraft:x" or a list -> per-biome mask.
std::vector<uint8_t> biomeSet(const Json& j);

std::string fullId(std::string_view id);  // adds "minecraft:" when no namespace
std::string dataRoot();                   // "vanilla/src/data"
void setDataRoot(const std::string& root);

}  // namespace structure
