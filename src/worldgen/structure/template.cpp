#include "worldgen/structure/template.hpp"

#include <algorithm>
#include <map>
#include <mutex>

#include "worldgen/structure/nbt_io.hpp"

namespace structure {

// ------------------------------------------------------------------------------------------- jigsaw blocks
static std::pair<Dir, Dir> orientation(int state) {
  std::string o = Blocks::get().prop(state, "orientation");  // FrontAndTop, e.g. "north_up"
  auto u = o.find('_');
  return {dirByName(o.substr(0, u)), dirByName(o.substr(u + 1))};
}
Dir jigsawFront(int state) { return orientation(state).first; }
Dir jigsawTop(int state) { return orientation(state).second; }

bool canAttach(const JigsawInfo& s, const JigsawInfo& t) {
  auto [sf, st] = orientation(s.state);
  auto [tf, tt] = orientation(t.state);
  return sf == opposite(tf) && (s.rollable || st == tt) && (t.name.empty() || s.target == t.name);
}

// ------------------------------------------------------------------------------------------- loading
static int intAt(const Nbt* l, size_t i) { return l && i < l->list.size() ? (int)l->list[i].i : 0; }
static std::string str(const Nbt& c, const char* k, const std::string& def = "") {
  const Nbt* v = c.get(k);
  return v && v->t == Nbt::String ? v->s : def;
}

static Template load(const std::string& id) {
  Template t;
  std::string f = fullId(id);
  auto c = f.find(':');
  std::string path = dataRoot() + "/" + f.substr(0, c) + "/structure/" + f.substr(c + 1) + ".nbt";
  Nbt root;
  try {
    root = readGzipNbtFile(path);
  } catch (const std::exception&) {
    return t;  // vanilla logs and returns an empty template
  }
  const Blocks& b = Blocks::get();
  const Nbt* size = root.get("size");
  t.size = {intAt(size, 0), intAt(size, 1), intAt(size, 2)};
  const Nbt* ents = root.get("entities");
  t.hasEntities = ents && !ents->list.empty();
  std::vector<const Nbt*> paletteLists;
  if (const Nbt* ps = root.get("palettes")) {
    for (auto& p : ps->list) paletteLists.push_back(&p);
  } else if (const Nbt* p = root.get("palette")) {
    paletteLists.push_back(p);
  }
  const Nbt* blocks = root.get("blocks");
  // Block entity NBT is shared by all palettes: parse once per block.
  std::vector<int> nbtIndex;
  std::vector<const Nbt*> rawNbt;
  if (blocks)
    for (auto& bt : blocks->list) {
      const Nbt* n = bt.get("nbt");
      rawNbt.push_back(n);
      if (!n) {
        nbtIndex.push_back(-1);
        continue;
      }
      TemplateNbt tn;
      tn.finalState = str(*n, "final_state", "minecraft:air");
      tn.metadata = str(*n, "metadata");
      tn.mode = str(*n, "mode");
      nbtIndex.push_back((int)t.nbts.size());
      t.nbts.push_back(std::move(tn));
    }
  for (const Nbt* pl : paletteLists) {
    std::vector<int> palette;
    for (auto& e : pl->list) {  // NbtUtils.readBlockState: {id, properties}
      int s = b.defaultState(str(e, "id"));
      if (s < 0) s = 0;
      if (const Nbt* props = e.get("properties"))
        for (auto& [k, v] : props->comp)
          if (v.t == Nbt::String) s = b.with(s, k, v.s);
      palette.push_back(s);
    }
    std::vector<BlockInfo> full, other, entities;
    std::vector<std::pair<BlockInfo, const Nbt*>> withNbt;
    if (blocks)
      for (size_t i = 0; i < blocks->list.size(); i++) {
        const Nbt& bt = blocks->list[i];
        const Nbt* pos = bt.get("pos");
        const Nbt* st = bt.get("state");
        int idx = st ? (int)st->i : 0;
        BlockInfo info{{intAt(pos, 0), intAt(pos, 1), intAt(pos, 2)}, idx >= 0 && idx < (int)palette.size() ? palette[idx] : 0,
                       nbtIndex[i]};
        if (info.nbt >= 0) {
          entities.push_back(info);
          withNbt.push_back({info, rawNbt[i]});
        } else if (b.flags(info.state) & BS_TEMPLATE_FULL) {
          full.push_back(info);
        } else {
          other.push_back(info);
        }
      }
    auto cmp = [](const BlockInfo& a, const BlockInfo& c) {
      return a.pos.y != c.pos.y ? a.pos.y < c.pos.y : a.pos.x != c.pos.x ? a.pos.x < c.pos.x : a.pos.z < c.pos.z;
    };
    std::stable_sort(full.begin(), full.end(), cmp);
    std::stable_sort(other.begin(), other.end(), cmp);
    std::stable_sort(entities.begin(), entities.end(), cmp);
    std::stable_sort(withNbt.begin(), withNbt.end(), [&](auto& a, auto& c) { return cmp(a.first, c.first); });
    std::vector<BlockInfo> all = std::move(full);
    all.insert(all.end(), other.begin(), other.end());
    all.insert(all.end(), entities.begin(), entities.end());
    std::vector<JigsawInfo> jig;
    int jigsawBlock = b.blockIndex("jigsaw");
    for (auto& [info, n] : withNbt) {
      if (b.blockOf(info.state) != jigsawBlock) continue;
      JigsawInfo j;
      j.pos = info.pos;
      j.state = info.state;
      std::string joint = str(*n, "joint");
      Dir front = jigsawFront(info.state);
      j.rollable = joint.empty() ? !(isXAxis(front) || isZAxis(front)) : joint == "rollable";
      j.name = fullId(str(*n, "name", "minecraft:empty"));
      j.pool = fullId(str(*n, "pool", "minecraft:empty"));
      j.target = fullId(str(*n, "target", "minecraft:empty"));
      const Nbt* pp = n->get("placement_priority");
      const Nbt* sp = n->get("selection_priority");
      j.placementPriority = pp ? (int)pp->i : 0;
      j.selectionPriority = sp ? (int)sp->i : 0;
      jig.push_back(std::move(j));
    }
    t.palettes.push_back(std::move(all));
    t.jigsaws.push_back(std::move(jig));
  }
  return t;
}

const Template& Template::get(const std::string& id) {
  static std::mutex m;
  static std::map<std::string, std::unique_ptr<Template>> cache;
  std::string key = fullId(id);
  {
    std::lock_guard lock(m);
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
  }
  auto t = std::make_unique<Template>(load(key));
  std::lock_guard lock(m);
  auto& slot = cache[key];
  if (!slot) slot = std::move(t);
  return *slot;
}

// ------------------------------------------------------------------------------------------- transforms
RandomSource& PlaceSettings::getRandom(BlockPos pos, std::unique_ptr<RandomSource>& holder) const {
  if (random) return *random;
  holder = std::make_unique<LegacyRandomSource>(Mth::getSeed(pos.x, pos.y, pos.z));
  return *holder;
}

const std::vector<BlockInfo>& PlaceSettings::randomPalette(const Template& t, BlockPos pos, int* index) const {
  std::unique_ptr<RandomSource> h;
  int i = getRandom(pos, h).nextInt((int)t.palettes.size());
  if (index) *index = i;
  return t.palettes[i];
}

BlockPos transform(BlockPos p, Mirror m, Rotation r, BlockPos pivot) {
  int x = p.x, y = p.y, z = p.z;
  if (m == Mirror::LEFT_RIGHT) z = -z;
  else if (m == Mirror::FRONT_BACK) x = -x;
  int px = pivot.x, pz = pivot.z;
  switch (r) {
    case Rotation::COUNTERCLOCKWISE_90: return {px - pz + z, y, px + pz - x};
    case Rotation::CLOCKWISE_90: return {px + pz - z, y, pz - px + x};
    case Rotation::CLOCKWISE_180: return {px + px - x, y, pz + pz - z};
    default: return {x, y, z};
  }
}

BoundingBox templateBoundingBox(const Template& t, const PlaceSettings& s, BlockPos pos) {
  BlockPos a = transform({0, 0, 0}, s.mirror, s.rotation, s.pivot);
  BlockPos b = transform({t.size.x - 1, t.size.y - 1, t.size.z - 1}, s.mirror, s.rotation, s.pivot);
  return BoundingBox::fromCorners(a, b).move(pos.x, pos.y, pos.z);
}

BlockPos zeroPositionWithTransform(BlockPos zero, Mirror m, Rotation r, int sizeX, int sizeZ) {
  sizeX--, sizeZ--;
  int mx = m == Mirror::FRONT_BACK ? sizeX : 0, mz = m == Mirror::LEFT_RIGHT ? sizeZ : 0;
  switch (r) {
    case Rotation::COUNTERCLOCKWISE_90: return zero.offset(mz, 0, sizeX - mx);
    case Rotation::CLOCKWISE_90: return zero.offset(sizeZ - mz, 0, mx);
    case Rotation::CLOCKWISE_180: return zero.offset(sizeX - mx, 0, sizeZ - mz);
    default: return zero.offset(mx, 0, mz);
  }
}

std::vector<JigsawInfo> templateJigsaws(const Template& t, BlockPos pos, Rotation r) {
  std::vector<JigsawInfo> out;
  if (t.palettes.empty()) return out;
  PlaceSettings s;
  s.rotation = r;
  int idx;
  s.randomPalette(t, pos, &idx);
  const Blocks& b = Blocks::get();
  for (auto& j : t.jigsaws[idx]) {
    JigsawInfo x = j;
    x.pos = relativePosition(s, j.pos).offset(pos);
    x.state = b.rotate(j.state, r);
    out.push_back(std::move(x));
  }
  return out;
}

std::vector<BlockInfo> filterBlocks(const Template& t, BlockPos pos, const PlaceSettings& s, std::string_view block, bool absolute) {
  std::vector<BlockInfo> out;
  if (t.palettes.empty()) return out;
  const Blocks& b = Blocks::get();
  int want = b.blockIndex(block);
  for (auto& info : s.randomPalette(t, pos)) {
    if (b.blockOf(info.state) != want) continue;
    BlockPos p = absolute ? relativePosition(s, info.pos).offset(pos) : info.pos;
    if (!s.boundingBox || s.boundingBox->isInside(p)) out.push_back({p, b.rotate(info.state, s.rotation), info.nbt});
  }
  return out;
}

std::vector<BlockInfo> processBlockInfos(Level& level, BlockPos pos, BlockPos ref, const PlaceSettings& s,
                                         const std::vector<BlockInfo>& infos, const Template& t) {
  s.tmpl = &t;
  std::vector<BlockInfo> original, processed;
  bool onlyInChunk = true;
  for (auto& p : s.processors)
    if (p->entirePiece()) onlyInChunk = false;
  for (auto& info : infos) {
    BlockPos p = relativePosition(s, info.pos).offset(pos);
    if (onlyInChunk && s.boundingBox && !s.boundingBox->isInside(p)) continue;
    BlockInfo cur{p, info.state, info.nbt};
    bool keep = true;
    for (auto& proc : s.processors)
      if (!(keep = proc->process(level, pos, ref, info.pos, cur, s))) break;
    if (keep) {
      processed.push_back(cur);
      original.push_back(info);
    }
  }
  for (auto& proc : s.processors) proc->finalize(level, pos, ref, original, processed, s);
  return processed;
}

bool isFluidSource(int state) { return Blocks::get().flags(state) & BS_FLUID_SOURCE; }
bool isWaterSource(int state) { return isFluidSource(state) && !Blocks::get().is(state, "lava"); }

bool placeInWorld(const Template& t, Level& level, BlockPos pos, BlockPos ref, const PlaceSettings& s, RandomSource& random) {
  if (t.palettes.empty()) return false;
  const auto& infos = s.randomPalette(t, pos);
  if ((infos.empty() && (s.ignoreEntities || !t.hasEntities)) || t.size.x < 1 || t.size.y < 1 || t.size.z < 1) return false;
  const Blocks& b = Blocks::get();
  std::vector<BlockPos> toFill, locked;
  auto processed = processBlockInfos(level, pos, ref, s, infos, t);
  for (auto& info : processed) {
    BlockPos p = info.pos;
    if (s.boundingBox && !s.boundingBox->isInside(p)) continue;
    int previous = s.applyWaterlogging ? level.getBlock(p.x, p.y, p.z) : -1;
    int state = b.rotate(b.mirror(info.state, s.mirror), s.rotation);
    level.setBlock(p.x, p.y, p.z, state);
    if (info.nbt != -1 && (b.flags(state) & BS_RANDOMIZABLE)) random.nextLong();  // LootTableSeed
    if (previous >= 0) {
      if (isFluidSource(state)) {
        locked.push_back(p);
      } else if (b.flags(state) & BS_LIQUID_CONTAINER) {
        // LiquidBlockContainer.placeLiquid: only SimpleWaterloggedBlock takes (source) water
        if ((b.flags(state) & BS_WATERLOGGABLE) && isWaterSource(previous) && b.prop(state, "waterlogged") == "false")
          level.setBlock(p.x, p.y, p.z, b.with(state, "waterlogged", "true"));
        if (!isFluidSource(previous)) toFill.push_back(p);
      }
    }
  }
  static const Dir DIRS[5] = {Dir::UP, Dir::NORTH, Dir::EAST, Dir::SOUTH, Dir::WEST};
  bool filled = true;
  while (filled && !toFill.empty()) {
    filled = false;
    for (auto it = toFill.begin(); it != toFill.end();) {
      BlockPos p = *it;
      int here = level.getBlock(p.x, p.y, p.z);
      bool source = isFluidSource(here);
      int toPlace = here;
      for (int i = 0; i < 5 && !source; i++) {
        BlockPos n = p.relative(DIRS[i]);
        int ns = level.getBlock(n.x, n.y, n.z);
        if (isFluidSource(ns) && std::find(locked.begin(), locked.end(), n) == locked.end()) toPlace = ns, source = true;
      }
      if (source) {
        int st = level.getBlock(p.x, p.y, p.z);
        if (b.flags(st) & BS_LIQUID_CONTAINER) {
          if ((b.flags(st) & BS_WATERLOGGABLE) && isWaterSource(toPlace) && b.prop(st, "waterlogged") == "false")
            level.setBlock(p.x, p.y, p.z, b.with(st, "waterlogged", "true"));
          filled = true;
          it = toFill.erase(it);
          continue;
        }
      }
      ++it;
    }
  }
  // ponytail: !knownShape neighbour shape updates (updateShapeAtEdge / updateFromNeighbourShapes) and template
  // entities are not applied; add with a block-update engine and entity support.
  return true;
}

// ------------------------------------------------------------------------------------------- processors
namespace {
int stateFromJson(const Json& j) {
  const Blocks& b = Blocks::get();
  if (j.isString()) return b.parse(j.asString());
  std::vector<std::pair<std::string, std::string>> props;  // {id, properties} (26.x) or {Name, Properties}
  const Json* p = j.find("properties") ? j.find("properties") : j.find("Properties");
  if (p)
    for (auto& [k, v] : p->obj) props.push_back({k, v.asString()});
  return b.state(j.find("id") ? j["id"].asString() : j["Name"].asString(), props);
}

struct RuleTest {
  enum Kind { ALWAYS, BLOCK, STATE, RANDOM_BLOCK, RANDOM_STATE, TAG } kind = ALWAYS;
  int block = -1, state = -1;
  float probability = 1;
  std::string tag;
  explicit RuleTest(const Json& j) {
    std::string t = j["predicate_type"].asString();
    const Blocks& b = Blocks::get();
    if (t == "minecraft:block_match") kind = BLOCK, block = b.blockIndex(j["block"].asString());
    else if (t == "minecraft:blockstate_match") kind = STATE, state = stateFromJson(j["block_state"]);
    else if (t == "minecraft:random_block_match")
      kind = RANDOM_BLOCK, block = b.blockIndex(j["block"].asString()), probability = j["probability"].asFloat();
    else if (t == "minecraft:random_blockstate_match")
      kind = RANDOM_STATE, state = stateFromJson(j["block_state"]), probability = j["probability"].asFloat();
    else if (t == "minecraft:tag_match") kind = TAG, tag = j["tag"].asString();
  }
  bool test(int s, RandomSource& r) const {
    const Blocks& b = Blocks::get();
    switch (kind) {
      case ALWAYS: return true;
      case BLOCK: return b.blockOf(s) == block;
      case STATE: return s == state;
      case RANDOM_BLOCK: return b.blockOf(s) == block && r.nextFloat() < probability;
      case RANDOM_STATE: return s == state && r.nextFloat() < probability;
      case TAG: return b.inTag(s, tag);
    }
    return false;
  }
};

struct PosTest {
  enum Kind { ALWAYS, LINEAR, AXIS } kind = ALWAYS;
  float minChance = 0, maxChance = 0;
  int minDist = 0, maxDist = 10;
  int axis = 1;
  explicit PosTest(const Json* j) {
    if (!j) return;
    std::string t = (*j)["predicate_type"].asString();
    if (t == "minecraft:always_true") return;
    kind = t == "minecraft:linear_pos" ? LINEAR : AXIS;
    if (auto* v = j->find("min_chance")) minChance = v->asFloat();
    if (auto* v = j->find("max_chance")) maxChance = v->asFloat();
    if (auto* v = j->find("min_dist")) minDist = v->asInt();
    if (auto* v = j->find("max_dist")) maxDist = v->asInt();
    if (auto* v = j->find("axis")) axis = v->asString() == "x" ? 0 : v->asString() == "y" ? 1 : 2;
  }
  bool test(BlockPos world, BlockPos refPos, RandomSource& r) const {
    if (kind == ALWAYS) return true;
    int dist;
    if (kind == LINEAR) {
      dist = std::abs(world.x - refPos.x) + std::abs(world.y - refPos.y) + std::abs(world.z - refPos.z);
    } else {
      float xd = (float)std::abs((world.x - refPos.x) * (axis == 0));
      float yd = (float)std::abs((world.y - refPos.y) * (axis == 1));
      float zd = (float)std::abs((world.z - refPos.z) * (axis == 2));
      dist = (int)(xd + yd + zd);
    }
    float rnd = r.nextFloat();
    float f = ((float)dist - (float)minDist) / ((float)maxDist - (float)minDist);  // Mth.inverseLerp
    float v = f < 0.0f ? minChance : f > 1.0f ? maxChance : minChance + f * (maxChance - minChance);  // clampedLerp
    return rnd <= v;
  }
};

struct Rule {
  RuleTest input, location;
  PosTest pos;
  int output;
  enum Modifier { PASSTHROUGH, CLEAR, APPEND } modifier = PASSTHROUGH;
  explicit Rule(const Json& j)
      : input(j["input_predicate"]), location(j["location_predicate"]), pos(j.find("position_predicate")),
        output(stateFromJson(j["output_state"])) {
    if (auto* m = j.find("block_entity_modifier")) {
      std::string t = (*m)["type"].asString();
      modifier = t == "minecraft:clear" ? CLEAR : t == "minecraft:passthrough" ? PASSTHROUGH : APPEND;
    }
  }
};

struct RuleProcessor : Processor {
  std::vector<Rule> rules;
  explicit RuleProcessor(const Json& j) {
    for (auto& r : j["rules"].arr) rules.emplace_back(r);
  }
  bool process(Level& level, BlockPos, BlockPos ref, BlockPos templatePos, BlockInfo& info, const PlaceSettings&) const override {
    (void)templatePos;
    LegacyRandomSource random(Mth::getSeed(info.pos.x, info.pos.y, info.pos.z));
    for (auto& r : rules) {
      if (!r.input.test(info.state, random)) continue;
      if (!r.location.test(level.getBlock(info.pos.x, info.pos.y, info.pos.z), random)) continue;
      if (!r.pos.test(info.pos, ref, random)) continue;
      info.state = r.output;
      if (r.modifier == Rule::CLEAR) info.nbt = -1;
      else if (r.modifier == Rule::APPEND) info.nbt = NBT_SYNTHETIC, random.nextLong();
      return true;
    }
    return true;
  }
};

struct BlockIgnore : Processor {
  std::vector<int> blocks;
  bool process(Level&, BlockPos, BlockPos, BlockPos, BlockInfo& info, const PlaceSettings&) const override {
    return std::find(blocks.begin(), blocks.end(), Blocks::get().blockOf(info.state)) == blocks.end();
  }
};

struct JigsawReplacement : Processor {
  bool process(Level&, BlockPos, BlockPos, BlockPos, BlockInfo& info, const PlaceSettings& s) const override {
    const Blocks& b = Blocks::get();
    if (!b.is(info.state, "jigsaw") || info.nbt < 0 || info.nbt == NBT_SYNTHETIC || !s.tmpl) return true;
    int st = b.parse(s.tmpl->nbts[info.nbt].finalState);
    if (st < 0 || b.is(st, "structure_void")) return false;
    info.state = st;
    info.nbt = -1;
    return true;
  }
};

struct Gravity : Processor {
  Heightmap h;
  int offset;
  Gravity(Heightmap h, int o) : h(h), offset(o) {}
  bool process(Level& level, BlockPos, BlockPos, BlockPos templatePos, BlockInfo& info, const PlaceSettings&) const override {
    info.pos.y = level.getHeight(h, info.pos.x, info.pos.z) + offset + templatePos.y;
    return true;
  }
};

struct BlockRot : Processor {
  float integrity;
  std::string tag;
  bool process(Level&, BlockPos, BlockPos, BlockPos, BlockInfo& info, const PlaceSettings& s) const override {
    std::unique_ptr<RandomSource> h;
    RandomSource& r = s.getRandom(info.pos, h);
    if (!tag.empty() && !Blocks::get().inTag(info.state, tag)) return true;
    return r.nextFloat() <= integrity;
  }
};

struct Protected : Processor {
  const bs::Tag* tag = nullptr;
  bool process(Level& level, BlockPos, BlockPos, BlockPos, BlockInfo& info, const PlaceSettings&) const override {
    return !tag->has(Blocks::get().blockOf(level.getBlock(info.pos.x, info.pos.y, info.pos.z)));
  }
};

struct Capped : Processor {
  ProcessorP delegate;
  int minLimit, maxLimit;
  bool entirePiece() const override { return true; }
  void finalize(Level& level, BlockPos pos, BlockPos ref, const std::vector<BlockInfo>& original, std::vector<BlockInfo>& processed,
                const PlaceSettings& s) const override {
    if (maxLimit == 0 || processed.empty()) return;
    if (original.size() != processed.size()) return;
    SingleThreadedRandomSource base(level.seed());
    auto random = base.forkPositional()->at(pos.x, pos.y, pos.z);
    int limit = minLimit == maxLimit ? minLimit : random->nextIntBetweenInclusive(minLimit, maxLimit);
    int maxToReplace = std::min(limit, (int)processed.size());
    if (maxToReplace < 1) return;
    std::vector<int> idx(processed.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = (int)i;
    for (int i = (int)idx.size(); i > 1; i--) {  // Util.toShuffledList(IntStream)
      int to = random->nextInt(i);
      std::swap(idx[i - 1], idx[to]);
    }
    int replaced = 0;
    for (size_t k = 0; k < idx.size() && replaced < maxToReplace; k++) {
      int i = idx[k];
      BlockInfo altered = processed[i];
      if (delegate->process(level, pos, ref, original[i].pos, altered, s) && !(altered == processed[i])) {
        replaced++;
        processed[i] = altered;
      }
    }
  }
};

struct BlockAge : Processor {
  float mossiness;
  bool process(Level&, BlockPos, BlockPos, BlockPos, BlockInfo& info, const PlaceSettings& s) const override {
    const Blocks& b = Blocks::get();
    std::unique_ptr<RandomSource> h;
    RandomSource& r = s.getRandom(info.pos, h);
    int st = info.state, ns = -1;
    auto pick = [&](std::vector<int> non, std::vector<int> mossy) {  // getRandomBlock
      if (r.nextFloat() < mossiness) return mossy[r.nextInt((int)mossy.size())];
      return non[r.nextInt((int)non.size())];
    };
    auto stairs = [&](const char* block) {
      int s0 = b.defaultState(block);
      s0 = b.with(s0, "facing", dirName(randomHorizontal(r)));
      return b.with(s0, "half", r.nextInt(2) == 0 ? "top" : "bottom");
    };
    auto withProps = [&](const char* block, int from) {  // withPropertiesOf
      int s0 = b.defaultState(block);
      for (auto& k : b.propNames(from))
        if (b.hasProp(s0, k)) s0 = b.with(s0, k, b.prop(from, k));
      return s0;
    };
    if (b.is(st, "stone_bricks") || b.is(st, "stone") || b.is(st, "chiseled_stone_bricks")) {
      if (r.nextFloat() < 0.5f) {
        int crack = b.defaultState("cracked_stone_bricks"), s1 = stairs("stone_brick_stairs");
        int moss = b.defaultState("mossy_stone_bricks"), s2 = stairs("mossy_stone_brick_stairs");
        ns = pick({crack, s1}, {moss, s2});
      }
    } else if (b.inTag(st, "stairs")) {
      if (r.nextFloat() < 0.5f)
        ns = pick({b.defaultState("stone_slab"), b.defaultState("stone_brick_slab")},
                  {withProps("mossy_stone_brick_stairs", st), b.defaultState("mossy_stone_brick_slab")});
    } else if (b.inTag(st, "slabs")) {
      if (r.nextFloat() < mossiness) ns = withProps("mossy_stone_brick_slab", st);
    } else if (b.inTag(st, "walls")) {
      if (r.nextFloat() < mossiness) ns = withProps("mossy_stone_brick_wall", st);
    } else if (b.is(st, "obsidian")) {
      if (r.nextFloat() < 0.15f) ns = b.defaultState("crying_obsidian");
    }
    if (ns >= 0) info.state = ns;
    return true;
  }
};

struct BlackstoneReplace : Processor {
  std::map<int, int> map;
  BlackstoneReplace() {
    const Blocks& b = Blocks::get();
    const char* pairs[][2] = {
        {"cobblestone", "blackstone"}, {"mossy_cobblestone", "blackstone"}, {"stone", "polished_blackstone"},
        {"stone_bricks", "polished_blackstone_bricks"}, {"mossy_stone_bricks", "polished_blackstone_bricks"},
        {"cobblestone_stairs", "blackstone_stairs"}, {"mossy_cobblestone_stairs", "blackstone_stairs"},
        {"stone_stairs", "polished_blackstone_stairs"}, {"stone_brick_stairs", "polished_blackstone_brick_stairs"},
        {"mossy_stone_brick_stairs", "polished_blackstone_brick_stairs"}, {"cobblestone_slab", "blackstone_slab"},
        {"mossy_cobblestone_slab", "blackstone_slab"}, {"smooth_stone_slab", "polished_blackstone_slab"},
        {"stone_slab", "polished_blackstone_slab"}, {"stone_brick_slab", "polished_blackstone_brick_slab"},
        {"mossy_stone_brick_slab", "polished_blackstone_brick_slab"}, {"stone_brick_wall", "polished_blackstone_brick_wall"},
        {"mossy_stone_brick_wall", "polished_blackstone_brick_wall"}, {"cobblestone_wall", "blackstone_wall"},
        {"mossy_cobblestone_wall", "blackstone_wall"}, {"chiseled_stone_bricks", "chiseled_polished_blackstone"},
        {"cracked_stone_bricks", "cracked_polished_blackstone_bricks"}, {"iron_bars", "iron_chain"}};
    for (auto& p : pairs) map[b.blockIndex(p[0])] = b.defaultState(p[1]);
  }
  bool process(Level&, BlockPos, BlockPos, BlockPos, BlockInfo& info, const PlaceSettings&) const override {
    const Blocks& b = Blocks::get();
    auto it = map.find(b.blockOf(info.state));
    if (it == map.end()) return true;
    int old = info.state, ns = it->second;
    for (const char* k : {"facing", "half", "type"})
      if (b.hasProp(old, k)) ns = b.with(ns, k, b.prop(old, k));
    info.state = ns;
    return true;
  }
};

struct LavaSubmerged : Processor {
  bool process(Level& level, BlockPos, BlockPos, BlockPos, BlockInfo& info, const PlaceSettings&) const override {
    const Blocks& b = Blocks::get();
    if (b.is(level.getBlock(info.pos.x, info.pos.y, info.pos.z), "lava") && !(b.flags(info.state) & BS_SHAPE_FULL))
      info.state = b.defaultState("lava");
    return true;
  }
};

ProcessorP parseProcessor(const Json& j) {
  std::string t = j["processor_type"].asString();
  if (t == "minecraft:rule") return std::make_shared<RuleProcessor>(j);
  if (t == "minecraft:block_rot") {
    auto p = std::make_shared<BlockRot>();
    p->integrity = j["integrity"].asFloat();
    if (auto* r = j.find("rottable_blocks")) p->tag = r->asString();
    return p;
  }
  if (t == "minecraft:protected_blocks") {
    auto p = std::make_shared<Protected>();
    p->tag = &Blocks::get().tag(j["value"].asString());
    return p;
  }
  if (t == "minecraft:capped") {
    auto p = std::make_shared<Capped>();
    p->delegate = parseProcessor(j["delegate"]);
    const Json& l = j["limit"];
    if (l.isNumber()) p->minLimit = p->maxLimit = l.asInt();
    else if (l["type"].asString() == "minecraft:constant") p->minLimit = p->maxLimit = l["value"].asInt();
    else p->minLimit = l["min_inclusive"].asInt(), p->maxLimit = l["max_inclusive"].asInt();
    return p;
  }
  if (t == "minecraft:block_age") return blockAge(j["mossiness"].asFloat());
  if (t == "minecraft:gravity") return gravity(heightmapByName(j["heightmap"].asString()), j.getInt("offset", 0));
  if (t == "minecraft:blackstone_replace") return blackstoneReplace();
  if (t == "minecraft:lava_submerged_block") return lavaSubmerged();
  if (t == "minecraft:block_ignore") {
    std::vector<std::string> names;
    for (auto& b : j["blocks"].arr) names.push_back(b.isString() ? b.asString() : b["Name"].asString());
    return blockIgnore(names);
  }
  if (t == "minecraft:jigsaw_replacement") return jigsawReplacement();
  return std::make_shared<Processor>();  // nop
}
}  // namespace

const ProcessorList& processorList(const Json& j) {
  static std::mutex m;
  static std::map<std::string, std::unique_ptr<ProcessorList>> byId;
  static std::vector<std::unique_ptr<ProcessorList>> inlineLists;
  std::lock_guard lock(m);
  auto build = [](const Json& l) {
    auto out = std::make_unique<ProcessorList>();
    const Json& arr = l.isArray() ? l : l["processors"];
    for (auto& p : arr.arr) out->push_back(parseProcessor(p));
    return out;
  };
  if (j.isString()) {
    std::string id = fullId(j.asString());
    auto& slot = byId[id];
    if (!slot) {
      auto c = id.find(':');
      slot = build(Json::parseFile(dataRoot() + "/" + id.substr(0, c) + "/worldgen/processor_list/" + id.substr(c + 1) + ".json"));
    }
    return *slot;
  }
  inlineLists.push_back(build(j));
  return *inlineLists.back();
}

ProcessorP blockIgnore(std::vector<std::string> blocks) {
  auto p = std::make_shared<BlockIgnore>();
  for (auto& n : blocks) p->blocks.push_back(Blocks::get().blockIndex(n));
  return p;
}
ProcessorP jigsawReplacement() {
  static auto p = std::make_shared<JigsawReplacement>();
  return p;
}
ProcessorP gravity(Heightmap h, int offset) { return std::make_shared<Gravity>(h, offset); }
ProcessorP blockRot(float integrity, std::string tag) {
  auto p = std::make_shared<BlockRot>();
  p->integrity = integrity;
  p->tag = std::move(tag);
  return p;
}
ProcessorP blockAge(float mossiness) {
  auto p = std::make_shared<BlockAge>();
  p->mossiness = mossiness;
  return p;
}
ProcessorP blackstoneReplace() {
  static auto p = std::make_shared<BlackstoneReplace>();
  return p;
}
ProcessorP lavaSubmerged() {
  static auto p = std::make_shared<LavaSubmerged>();
  return p;
}

}  // namespace structure
