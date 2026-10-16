// BlockState.canSurvive and the placement helpers features call, per vanilla block class (blocks.bin keeps
// each block's Java class chain). Light-dependent rules use the values an unlit proto chunk gives during
// FEATURES: sky light 15, block light 0.
#include <unordered_map>

#include "worldgen/features.hpp"

namespace worldgen::feat {
namespace {
const bs::Registry& R() { return bs::R(); }
bool in(int s, const char* tag) { return R().in(s, R().tag(tag)); }
bool is(int s, int def) { return R().is(s, def); }
bool sturdy(Level& l, const P& p, bs::Dir d, bs::Support t = bs::FULL) { return R().sturdy(l.get(p), d, t); }

enum Kind {
  DEFAULT, VEGETATION, SUGAR_CANE, CACTUS, MANGROVE_PROPAGULE, AZALEA, DRY_VEGETATION, LILY_PAD, CARPET, MOSSY_CARPET,
  LEAF_LITTER, SEA_PICKLE, SEAGRASS, TALL_SEAGRASS, SMALL_DRIPLEAF, DOUBLE_PLANT, SPORE_BLOSSOM, BAMBOO, GROWING_UP,
  GROWING_DOWN, KELP, BIG_DRIPLEAF, BIG_DRIPLEAF_STEM, HANGING_ROOTS, SPELEOTHEM, MULTIFACE, VINE, COCOA, CACTUS_FLOWER,
  HANGING_MOSS, CORAL_PLANT, CORAL_WALL_FAN, AMETHYST_CLUSTER, SNOW_LAYER, MUSHROOM, CROP, STEM, NETHER_PLANT
};

Kind kind_of(int block) {
  static const std::vector<uint8_t> kinds = [] {
    std::vector<uint8_t> k(size_t(R().block_count()), DEFAULT);
    for (int b = 0; b < R().block_count(); b++) {
      const auto& B = R().block(b);
      auto has = [&](const char* c) { return B.is_a(c); };
      Kind v = DEFAULT;
      // Most specific overrides first (same order as the Java class hierarchy).
      if (has("SugarCaneBlock")) v = SUGAR_CANE;
      else if (has("CactusBlock")) v = CACTUS;
      else if (has("MangrovePropaguleBlock")) v = MANGROVE_PROPAGULE;
      else if (has("AzaleaBlock")) v = AZALEA;
      else if (has("DryVegetationBlock")) v = DRY_VEGETATION;
      else if (has("LilyPadBlock")) v = LILY_PAD;
      else if (has("MossyCarpetBlock")) v = MOSSY_CARPET;
      else if (has("CarpetBlock")) v = CARPET;
      else if (has("LeafLitterBlock")) v = LEAF_LITTER;
      else if (has("SeaPickleBlock")) v = SEA_PICKLE;
      else if (has("TallSeagrassBlock")) v = TALL_SEAGRASS;
      else if (has("SeagrassBlock")) v = SEAGRASS;
      else if (has("SmallDripleafBlock")) v = SMALL_DRIPLEAF;
      else if (has("DoublePlantBlock")) v = DOUBLE_PLANT;
      else if (has("SporeBlossomBlock")) v = SPORE_BLOSSOM;
      else if (has("BambooStalkBlock") || has("BambooSaplingBlock")) v = BAMBOO;
      else if (has("KelpBlock") || has("KelpPlantBlock")) v = KELP;
      else if (has("CaveVinesBlock") || has("CaveVinesPlantBlock") || has("WeepingVinesBlock") || has("WeepingVinesPlantBlock")) v = GROWING_DOWN;
      else if (has("TwistingVinesBlock") || has("TwistingVinesPlantBlock")) v = GROWING_UP;
      else if (has("BigDripleafStemBlock")) v = BIG_DRIPLEAF_STEM;
      else if (has("BigDripleafBlock")) v = BIG_DRIPLEAF;
      else if (has("HangingRootsBlock")) v = HANGING_ROOTS;
      else if (has("SpeleothemBlock")) v = SPELEOTHEM;
      else if (has("MultifaceBlock")) v = MULTIFACE;
      else if (has("VineBlock")) v = VINE;
      else if (has("CocoaBlock")) v = COCOA;
      else if (has("CactusFlowerBlock")) v = CACTUS_FLOWER;
      else if (has("HangingMossBlock")) v = HANGING_MOSS;
      else if (has("BaseCoralWallFanBlock")) v = CORAL_WALL_FAN;
      else if (has("BaseCoralPlantTypeBlock")) v = CORAL_PLANT;
      else if (has("AmethystClusterBlock")) v = AMETHYST_CLUSTER;
      else if (has("SnowLayerBlock")) v = SNOW_LAYER;
      else if (has("MushroomBlock")) v = MUSHROOM;
      else if (has("VegetationBlock")) v = VEGETATION;
      k[size_t(b)] = v;
    }
    return k;
  }();
  return Kind(kinds[size_t(block)]);
}

bool can_attach(Level& l, bs::Dir dir, const P& np) { return multiface_can_attach(l, dir, np); }

bool vegetation_base(Kind k, int below, Level& l, const P& belowPos) {  // mayPlaceOn of the vegetation subclasses
  switch (k) {
    case AZALEA: return in(below, "minecraft:supports_azalea");
    case DRY_VEGETATION: return in(below, "minecraft:supports_dry_vegetation");
    case MANGROVE_PROPAGULE: return in(below, "minecraft:supports_mangrove_propagule");
    case LILY_PAD: {
      static const uint8_t lily = R().fluids("#minecraft:supports_lily_pad");
      return (R().fluid_in(below, lily) || in(below, "minecraft:supports_lily_pad")) && !R().has_fluid(l.get(belowPos.above()));
    }
    case SEA_PICKLE: return R().collision_up_face(below) || R().sturdy(below, bs::UP);
    case SEAGRASS:
    case TALL_SEAGRASS: return R().sturdy(below, bs::UP) && !in(below, "minecraft:cannot_support_seagrass");
    case SMALL_DRIPLEAF: {
      int at = l.get(belowPos.above());
      return in(below, "minecraft:supports_small_dripleaf") ||
             (R().fluid_type(at) == bs::WATER && in(below, "minecraft:supports_vegetation"));
    }
    case CACTUS_FLOWER: return in(below, "minecraft:support_override_cactus_flower") || R().sturdy(below, bs::UP, bs::CENTER);
    default: return in(below, "minecraft:supports_vegetation");
  }
}
bool upper_half(int s) { return R().value(s, "half") == "upper"; }
}  // namespace

bool multiface_can_attach(Level& l, bs::Dir dir, const P& np) {
  int s = l.get(np);
  bs::Dir o = bs::opposite(dir);
  return R().sturdy(s, o, bs::FULL) || R().collision_face_full(s, o);
}

bool can_survive(Level& l, int state, const P& p) {
  int b = R().block_of(state);
  Kind k = kind_of(b);
  int below = l.get(p.below());
  switch (k) {
    case DEFAULT: return true;
    case VEGETATION:
    case AZALEA:
    case DRY_VEGETATION:
    case LILY_PAD:
    case SEA_PICKLE:
    case SEAGRASS:
    case CACTUS_FLOWER: return vegetation_base(k, below, l, p.below());
    case MANGROVE_PROPAGULE:
      if (R().bool_value(state, "hanging")) return in(l.get(p.above()), "minecraft:supports_hanging_mangrove_propagule");
      return vegetation_base(k, below, l, p.below());
    case SUGAR_CANE: {
      if (R().block_of(below) == b) return true;
      if (!in(below, "minecraft:supports_sugar_cane")) return false;
      static const uint8_t adj = R().fluids("#minecraft:supports_sugar_cane_adjacently");
      for (bs::Dir d : HORIZONTAL) {
        int s = l.get(p.below().rel(d));
        if (R().fluid_in(s, adj) || in(s, "minecraft:supports_sugar_cane_adjacently")) return true;
      }
      return false;
    }
    case CACTUS: {
      for (bs::Dir d : HORIZONTAL) {
        int s = l.get(p.rel(d));
        if (R().is_solid(s) || R().is_lava(s)) return false;
      }
      return (R().block_of(below) == b || in(below, "minecraft:supports_cactus")) && !R().liquid(l.get(p.above()));
    }
    case CARPET: return !R().is_air(below);
    case MOSSY_CARPET:
      return R().bool_value(state, "bottom") ? !R().is_air(below) : R().block_of(below) == b && R().bool_value(below, "bottom");
    case LEAF_LITTER: return R().sturdy(below, bs::UP);
    case TALL_SEAGRASS:
      if (upper_half(state)) return R().block_of(below) == b && !upper_half(below);
      return vegetation_base(k, below, l, p.below()) && R().is_water(l.get(p)) && R().fluid_amount(l.get(p)) == 8;
    case SMALL_DRIPLEAF:
      if (upper_half(state)) return R().block_of(below) == b && !upper_half(below);
      return vegetation_base(k, below, l, p.below());
    case DOUBLE_PLANT:
      if (upper_half(state)) return R().block_of(below) == b && !upper_half(below);
      return vegetation_base(k, below, l, p.below());
    case SPORE_BLOSSOM: return sturdy(l, p.above(), bs::DOWN, bs::CENTER) && !l.water_at(p);
    case BAMBOO: return in(below, "minecraft:supports_bamboo");
    case KELP:
    case GROWING_UP:
    case GROWING_DOWN: {
      bs::Dir grow = k == GROWING_DOWN ? bs::DOWN : bs::UP;
      P at = p.rel(bs::opposite(grow));
      int s = l.get(at);
      if (k == KELP && in(s, "minecraft:cannot_support_kelp")) return false;
      // head/body pair: kelp <-> kelp_plant, cave_vines <-> cave_vines_plant, ...
      std::string n = R().block(b).name, base = n.size() > 6 && n.substr(n.size() - 6) == "_plant" ? n.substr(0, n.size() - 6) : n;
      int head = R().block_index(base), body = R().block_index(base + "_plant");
      int sb = R().block_of(s);
      return sb == head || sb == body || R().sturdy(s, grow);
    }
    case BIG_DRIPLEAF: return R().block_of(below) == b || is(below, blocks::big_dripleaf_stem) || in(below, "minecraft:supports_big_dripleaf");
    case BIG_DRIPLEAF_STEM: {
      int above = l.get(p.above());
      return (R().block_of(below) == b || in(below, "minecraft:supports_big_dripleaf")) &&
             (R().block_of(above) == b || is(above, blocks::big_dripleaf));
    }
    case HANGING_ROOTS: return sturdy(l, p.above(), bs::DOWN);
    case SPELEOTHEM: {
      bs::Dir tip = dir_by_name(std::string(R().value(state, "vertical_direction")));
      P behind = p.rel(bs::opposite(tip));
      int s = l.get(behind);
      return R().sturdy(s, tip) || (R().block_of(s) == b && R().value(s, "vertical_direction") == dir_name(tip));
    }
    case MULTIFACE: {
      bool any = false;
      for (bs::Dir d : ALL_DIRS)
        if (R().bool_value(state, dir_name(d))) {
          if (!can_attach(l, d, p.rel(d))) return false;
          any = true;
        }
      return any;
    }
    case VINE: {  // hasFaces(getUpdatedState(state))
      bool up = R().bool_value(state, "up") && can_attach(l, bs::UP, p.above());
      if (up) return true;
      int above = -1;
      for (bs::Dir d : HORIZONTAL) {
        if (!R().bool_value(state, dir_name(d))) continue;
        if (can_attach(l, d, p.rel(d))) return true;  // canSupportAtFace
        if (above < 0) above = l.get(p.above());
        if (R().block_of(above) == b && R().bool_value(above, dir_name(d))) return true;
      }
      return false;
    }
    case COCOA: return in(l.get(p.rel(dir_by_name(std::string(R().value(state, "facing"))))), "minecraft:supports_cocoa");
    case HANGING_MOSS: return can_attach(l, bs::UP, p.above()) || R().block_of(l.get(p.above())) == b;
    case CORAL_PLANT: return R().sturdy(below, bs::UP);
    case CORAL_WALL_FAN:
    case AMETHYST_CLUSTER: {
      bs::Dir f = dir_by_name(std::string(R().value(state, "facing")));
      return R().sturdy(l.get(p.rel(bs::opposite(f))), f);
    }
    case SNOW_LAYER:
      if (in(below, "minecraft:cannot_support_snow_layer")) return false;
      if (in(below, "minecraft:support_override_snow_layer")) return true;
      return R().collision_face_full(below, bs::UP) || (is(below, blocks::snow) && R().int_value(below, "layers") == 8);
    case MUSHROOM:  // raw brightness is 15 in an unlit proto chunk: only the override tag allows it
      return in(below, "minecraft:overrides_mushroom_light_requirement");
    default: return true;
  }
}

// DoublePlantBlock.placeAt (copyWaterloggedFrom).
void place_double_plant(Level& l, int state, const P& p) {
  auto waterlogged = [&](const P& q, int s) {
    if (R().has(s, "waterlogged")) s = R().with_bool(s, "waterlogged", l.water_at(q));
    return s;
  };
  l.set(p, waterlogged(p, R().with(state, "half", "lower")));
  l.set(p.above(), waterlogged(p.above(), R().with(state, "half", "upper")));
}

}  // namespace worldgen::feat

namespace worldgen::feat {
namespace {
// MossyCarpetBlock.getUpdatedState / createTopperWithSideChance.
int mossy_updated(Level& l, int state, const P& p, bool createSides) {
  int above = -1, below = -1;
  bool base = R().bool_value(state, "bottom");
  createSides |= base;
  for (bs::Dir d : HORIZONTAL) {
    const char* prop = dir_name(d);
    std::string side = can_attach(l, d, p.rel(d)) ? (createSides ? "low" : std::string(R().value(state, prop))) : "none";
    if (side == "low") {
      if (above < 0) above = l.get(p.above());
      if (is(above, blocks::pale_moss_carpet) && R().value(above, prop) != "none" && !R().bool_value(above, "bottom")) side = "tall";
      if (!base) {
        if (below < 0) below = l.get(p.below());
        if (is(below, blocks::pale_moss_carpet) && R().value(below, prop) == "none") side = "none";
      }
    }
    state = R().with(state, prop, side);
  }
  return state;
}
bool mossy_has_faces(int s) {
  if (R().bool_value(s, "bottom")) return true;
  for (bs::Dir d : HORIZONTAL)
    if (R().value(s, dir_name(d)) != "none") return true;
  return false;
}
int mossy_topper(Level& l, const P& p, RandomSource& r) {
  P above = p.above();
  int prev = l.get(above);
  bool carpetAbove = is(prev, blocks::pale_moss_carpet);
  if ((carpetAbove && R().bool_value(prev, "bottom")) || (!carpetAbove && !R().can_be_replaced(prev))) return blocks::air;
  int s = mossy_updated(l, R().with_bool(blocks::pale_moss_carpet, "bottom", false), above, true);
  for (bs::Dir d : HORIZONTAL)
    if (R().value(s, dir_name(d)) != "none" && !r.nextBoolean()) s = R().with(s, dir_name(d), "none");
  return mossy_has_faces(s) && s != prev ? s : int(blocks::air);
}
}  // namespace

void place_mossy_carpet(Level& l, const P& p, RandomSource& r) {
  int carpet = mossy_updated(l, blocks::pale_moss_carpet, p, true);
  l.set(p, carpet);
  int top = mossy_topper(l, p, r);
  if (!R().is_air(top)) {
    l.set(p.above(), top);
    l.set(p, mossy_updated(l, carpet, p, true));
  }
}

}  // namespace worldgen::feat

namespace worldgen::feat {
namespace {
bool has_face(int s, bs::Dir d) { return R().bool_value(s, dir_name(d)); }
bool multiface_valid(Level& l, int block, int old, const P& p, bs::Dir d) {  // isValidStateForPlacement
  if (!R().has(R().default_state(block), dir_name(d))) return false;  // isFaceSupported
  if (R().block_of(old) == block && has_face(old, d)) return false;
  return multiface_can_attach(l, d, p.rel(d));
}
}  // namespace

int multiface_placement(Level& l, int block, int old, const P& p, bs::Dir d) {
  if (!multiface_valid(l, block, old, p, d)) return -1;
  int s;
  if (R().block_of(old) == block) s = old;
  else if (R().fluid_type(old) == bs::WATER) s = R().with_bool(R().default_state(block), "waterlogged", true);
  else s = R().default_state(block);
  return R().with_bool(s, dir_name(d), true);
}

// MultifaceSpreader (DefaultSpreaderConfig).spreadFromFaceTowardRandomDirection.
bool multiface_spread_from_face_random(Level& l, int state, const P& p, bs::Dir face, RandomSource& r, bool postProcess) {
  int block = R().block_of(state);
  for (bs::Dir spread : all_shuffled(r)) {
    if (spread / 2 == face / 2) continue;  // same axis
    if (!(has_face(state, face) && !has_face(state, spread))) continue;
    P sp[3] = {p, p.rel(spread), p.rel(spread).rel(face)};
    bs::Dir sf[3] = {spread, face, bs::opposite(spread)};
    for (int t = 0; t < 3; t++) {
      int existing = l.get(sp[t]);
      bool replaceable = R().is_air(existing) || R().block_of(existing) == block ||
                         (R().is(existing, blocks::water) && R().is_source(existing));
      if (!replaceable || !multiface_valid(l, block, existing, sp[t], sf[t])) continue;
      int ns = multiface_placement(l, block, existing, sp[t], sf[t]);  // placeBlock
      if (ns < 0) break;
      if (postProcess) l.mark_post_processing(sp[t]);
      if (l.set(sp[t], ns)) return true;
      break;
    }
  }
  return false;
}

int update_shape(Level& l, int state, const P& p, bs::Dir dir, const P& np, int ns) {
  int b = R().block_of(state);
  Kind k = kind_of(b);
  auto air_or_water = [&] { return R().has(state, "waterlogged") && R().bool_value(state, "waterlogged") ? int(blocks::water) : int(blocks::air); };
  if (k == MULTIFACE) {  // MultifaceBlock.updateShape
    bool any = false;
    for (bs::Dir d : ALL_DIRS) any = any || (R().has(state, dir_name(d)) && has_face(state, d));
    if (!any) return air_or_water();
    if (has_face(state, dir) && !multiface_can_attach(l, dir, np)) {
      state = R().with_bool(state, dir_name(dir), false);
      bool left = false;
      for (bs::Dir d : ALL_DIRS) left = left || (R().has(state, dir_name(d)) && has_face(state, d));
      if (!left) return air_or_water();
    }
    return state;
  }
  if (k == VINE) {
    if (dir == bs::DOWN) return state;
    return can_survive(l, state, p) ? state : int(blocks::air);  // ponytail: faces not pruned, only removal
  }
  if (k == DOUBLE_PLANT || k == TALL_SEAGRASS || k == SMALL_DRIPLEAF) {  // DoublePlantBlock.updateShape
    bool upper = upper_half(state);
    if ((dir == bs::UP || dir == bs::DOWN) && (!upper == (dir == bs::UP)) && (R().block_of(ns) != b || upper_half(ns) == upper))
      return air_or_water();
    if (!upper && dir == bs::DOWN && !can_survive(l, state, p)) return air_or_water();
    if (!can_survive(l, state, p)) return air_or_water();
    return state;
  }
  if (k != DEFAULT) return can_survive(l, state, p) ? state : air_or_water();
  if (dir == bs::UP && R().has(state, "snowy") &&
      (R().is(state, blocks::grass_block) || R().is(state, blocks::podzol) || R().is(state, blocks::mycelium)))
    return R().with_bool(state, "snowy", R().in(ns, R().tag("minecraft:snow")));  // SnowyBlock
  return state;
}

int update_from_neighbour_shapes(Level& l, int state, const P& p) {
  static const bs::Dir order[6] = {bs::WEST, bs::EAST, bs::NORTH, bs::SOUTH, bs::DOWN, bs::UP};  // UPDATE_SHAPE_ORDER
  for (bs::Dir d : order) state = update_shape(l, state, p, d, p.rel(d), l.get(p.rel(d)));
  return state;
}

}  // namespace worldgen::feat
