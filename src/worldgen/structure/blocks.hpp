// Block states for structure generation: BlockState.rotate/mirror and per-state flags extracted from the
// vanilla jar at build time (data/block_states.bin, tools/gen_block_dump.py) on top of bs::Registry, plus
// biome tags/ids read from vanilla/src/data/minecraft.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "worldgen/block_state.hpp"
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

// Names, properties and tags come from the shared bs::Registry (worldgen/block_state.hpp); this adds
// BlockState.rotate/mirror and the extra flags above.
class Blocks {
 public:
  static const Blocks& get();  // loaded once, thread-safe

  int blockOf(int state) const { return bs::R().block_of(state); }
  int blockIndex(std::string_view name) const { return bs::R().block_index(name); }  // -1 if unknown
  int defaultState(std::string_view name) const;                                     // -1 if unknown
  bool is(int state, std::string_view name) const { return blockOf(state) == blockIndex(name); }
  // Property access; "" / unchanged state when the block lacks the property or value.
  std::string prop(int state, std::string_view key) const { return std::string(bs::R().value(state, key)); }
  int with(int state, std::string_view key, std::string_view value) const;
  bool hasProp(int state, std::string_view key) const { return bs::R().has(state, key); }
  std::vector<std::string> propNames(int state) const;
  // BlockState.CODEC style {Name, Properties} with defaults for missing properties.
  int state(std::string_view name, const std::vector<std::pair<std::string, std::string>>& props) const;
  // BlockStateParser.parseForBlock: "minecraft:foo[a=b,c=d]" (NBT suffix ignored). -1 on error.
  int parse(std::string_view s) const;

  int rotate(int state, Rotation r) const;
  int mirror(int state, Mirror m) const;
  uint32_t flags(int state) const { return flags_[state]; }
  bool faceFull(int state, int dirOrdinal) const { return flags_[state] >> (16 + dirOrdinal) & 1; }
  bool isAir(int state) const { return flags_[state] & BS_AIR; }

  const bs::Tag& tag(std::string_view tag) const { return bs::R().tag(tag); }  // "minecraft:logs" / "#logs"
  bool inTag(int state, std::string_view t) const { return tag(t).has(blockOf(state)); }

 private:
  Blocks();
  std::vector<uint16_t> transforms_;  // 5 per state
  std::vector<uint32_t> flags_;
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
