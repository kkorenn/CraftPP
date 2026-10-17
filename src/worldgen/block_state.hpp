// Block states for worldgen features: layout (properties, values) and behaviour flags from data/blocks.bin
// (tools/gen_block_data.sh), block tags from data/vanilla/minecraft/tags/block. State ids are the global
// ids of gen/ids.hpp (blocks:: constants are default states). Read-only after load(): thread-safe.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bs {

// Per-state flags (BlockStateBase methods evaluated on an empty level).
enum Flag : uint32_t {
  AIR = 1,               // isAir()
  SOLID = 2,             // isSolid() (legacySolid)
  REPLACEABLE = 4,       // canBeReplaced()
  FULL_COLLISION = 8,    // isCollisionShapeFullBlock()
  NO_COLLISION = 16,     // getCollisionShape().isEmpty()
  SOLID_RENDER = 32,     // isSolidRender()
  BLOCK_ENTITY = 64,     // hasBlockEntity()
  IGNITED_BY_LAVA = 128,
};
// Direction order of Direction.values(): DOWN, UP, NORTH, SOUTH, WEST, EAST.
enum Dir { DOWN, UP, NORTH, SOUTH, WEST, EAST };
inline constexpr int DX[6] = {0, 0, 0, 0, -1, 1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};
inline constexpr Dir opposite(Dir d) { return Dir(d ^ 1); }
enum Support { FULL = 8, CENTER = 14, RIGID = 20 };  // bit offsets of isFaceSturdy(dir, type)
inline constexpr int FACE_OCCLUSION_FULL = 26;      // Block.isShapeFullBlock(getFaceOcclusionShape(dir))

// FluidState type (Fluids.*), packed with amount: type | amount << 3.
enum Fluid : uint8_t { NO_FLUID = 0, WATER = 1, FLOWING_WATER = 2, LAVA = 3, FLOWING_LAVA = 4 };

struct Prop {
  std::string name;
  std::vector<std::string> values;
  int stride;
};
struct Block {
  std::string name;  // without "minecraft:"
  int first, def, count;
  int block_entity;     // block_entity_types:: id created by this block, -1 = none
  std::string classes;  // vanilla Java class chain below Block, e.g. "TallSeagrassBlock,DoublePlantBlock,VegetationBlock"
  bool is_a(std::string_view cls) const {  // instanceof
    for (size_t i = 0; i <= classes.size();) {
      size_t j = classes.find(',', i);
      if (j == std::string::npos) j = classes.size();
      if (std::string_view(classes).substr(i, j - i) == cls) return true;
      i = j + 1;
    }
    return false;
  }
  std::vector<Prop> props;
};

// A set of blocks (block tag). Bit per block index.
struct Tag {
  std::vector<uint64_t> bits;
  bool has(int block) const { return block >= 0 && size_t(block >> 6) < bits.size() && (bits[size_t(block >> 6)] >> (block & 63)) & 1; }
  void add(int block) {
    if (size_t(block >> 6) >= bits.size()) bits.resize(size_t(block >> 6) + 1);
    bits[size_t(block >> 6)] |= 1ull << (block & 63);
  }
};

class Registry {
 public:
  static const Registry& get();  // loads once (data/blocks.bin + vanilla tags)

  int block_of(int state) const { return state_block[size_t(state)]; }
  const Block& block(int b) const { return blocks[size_t(b)]; }
  int block_index(std::string_view name) const;  // "minecraft:stone" or "stone", -1 if unknown
  int default_state(int b) const { return blocks[size_t(b)].def; }
  // "minecraft:oak_log[axis=y]" -> state id (unlisted properties keep the default); -1 if unknown.
  int parse(std::string_view spec) const;
  int with(int state, std::string_view prop, std::string_view value) const;  // -1 if no such property/value
  int with(int state, std::string_view prop, int valueIndex) const;
  int value_index(int state, std::string_view prop) const;  // -1 if the block has no such property
  std::string_view value(int state, std::string_view prop) const;  // "" if none
  bool has(int state, std::string_view prop) const { return value_index(state, prop) >= 0; }
  // Property by name with an int value (age=3, distance=7, ...): -1 if absent.
  int int_value(int state, std::string_view prop) const;
  int with_int(int state, std::string_view prop, int v) const { return with(state, prop, std::to_string(v)); }
  bool bool_value(int state, std::string_view prop) const { return value(state, prop) == "true"; }
  int with_bool(int state, std::string_view prop, bool v) const { return with(state, prop, v ? "true" : "false"); }
  std::string name(int state) const;  // "minecraft:x[a=b,...]"

  uint32_t flags(int state) const { return state_flags[size_t(state)]; }
  bool is_air(int s) const { return flags(s) & AIR; }
  bool is_solid(int s) const { return flags(s) & SOLID; }
  bool can_be_replaced(int s) const { return flags(s) & REPLACEABLE; }
  bool sturdy(int s, Dir d, Support t = FULL) const { return (flags(s) >> (int(t) + int(d))) & 1; }
  bool face_occlusion_full(int s, Dir d) const { return (flags(s) >> (FACE_OCCLUSION_FULL + d)) & 1; }
  uint8_t fluid_type(int s) const { return state_fluid[size_t(s)] & 7; }
  int fluid_amount(int s) const { return (state_fluid[size_t(s)] >> 3) & 15; }
  uint8_t fluid_packed(int s) const { return state_fluid[size_t(s)]; }  // FluidState identity
  bool fluid_falling(int s) const { return state_fluid[size_t(s)] & 0x80; }
  bool collision_up_face(int s) const { return state_flags2[size_t(s)] & 1; }  // !getCollisionShape().getFaceShape(UP).isEmpty()
  bool liquid(int s) const { return state_flags2[size_t(s)] & 2; }             // BlockState.liquid()
  bool can_occlude(int s) const { return state_flags2[size_t(s)] & 4; }
  bool redstone_conductor(int s) const { return state_flags2[size_t(s)] & 8; }
  bool collision_face_full(int s, Dir d) const { return (state_flags2[size_t(s)] >> (4 + int(d))) & 1; }  // Block.isFaceFull(getCollisionShape(), d)
  // WallBlock.isCovered(collision shape's DOWN face of s, test): test 0 = post, 1..4 = wall side N, S, W, E.
  bool wall_covered(int s, int test) const { return (state_flags2[size_t(s)] >> (10 + test)) & 1; }
  bool has_fluid(int s) const { return fluid_type(s) != NO_FLUID; }
  bool is_water(int s) const { return fluid_type(s) == WATER || fluid_type(s) == FLOWING_WATER; }  // FluidTags.WATER
  bool is_lava(int s) const { return fluid_type(s) == LAVA || fluid_type(s) == FLOWING_LAVA; }
  bool is_source(int s) const { return fluid_type(s) == WATER || fluid_type(s) == LAVA; }
  // FluidState.createLegacyBlock(): the block a fluid state stands for (water/lava with level), or air.
  int fluid_block(int s) const;

  bool is(int state, int defaultStateOfBlock) const { return block_of(state) == block_of(defaultStateOfBlock); }
  const Tag& tag(std::string_view name) const;  // "minecraft:logs" (or "#minecraft:logs"); empty tag if unknown
  // Fluid tag or fluid name ("minecraft:water", "#minecraft:lava") -> bit mask over Fluid types.
  uint8_t fluids(std::string_view nameOrTag) const;
  bool fluid_in(int state, uint8_t mask) const { return (mask >> fluid_type(state)) & 1; }
  bool in(int state, const Tag& t) const { return t.has(block_of(state)); }
  int state_count() const { return int(state_block.size()); }
  int block_count() const { return int(blocks.size()); }

 private:
  Registry();
  std::vector<Block> blocks;
  std::vector<uint16_t> state_block;
  std::vector<uint32_t> state_flags;
  std::vector<uint8_t> state_fluid;
  std::vector<uint16_t> state_flags2;
  std::unordered_map<std::string, int> by_name;
  std::unordered_map<std::string, Tag> tags;
  std::unordered_map<std::string, uint8_t> fluid_tags;
  Tag empty;
  const Prop* prop(int b, std::string_view name) const;
};

inline const Registry& R() { return Registry::get(); }

}  // namespace bs
