// Jigsaw structures: template pools, pool elements, PoolElementStructurePiece, JigsawPlacement and
// JigsawStructure (villages, outposts, ancient cities, trail ruins, trial chambers, bastions, abandoned camps).
#include <algorithm>
#include <filesystem>
#include <climits>
#include <deque>
#include <map>
#include <mutex>

#include "worldgen/structure/jigsaw.hpp"

namespace structure {

// ------------------------------------------------------------------------------------------- anchors / heights
int resolveAnchor(const Json& j) {  // VerticalAnchor with WorldGenerationContext(minY -64, height 384)
  if (auto* v = j.find("absolute")) return v->asInt();
  if (auto* v = j.find("above_bottom")) return MIN_Y + v->asInt();
  if (auto* v = j.find("below_top")) return MIN_Y + GEN_HEIGHT - 1 - v->asInt();
  return 0;
}

int sampleHeight(const Json& j, RandomSource& r) {  // HeightProvider.sample
  if (!j.find("type")) return resolveAnchor(j);
  std::string t = j["type"].asString();
  if (t == "minecraft:constant") return resolveAnchor(j["value"]);
  if (t == "minecraft:uniform") {
    int mn = resolveAnchor(j["min_inclusive"]), mx = resolveAnchor(j["max_inclusive"]);
    return mn > mx ? mn : randomBetweenInclusive(r, mn, mx);
  }
  throw std::runtime_error("structure: unsupported height provider " + t);
}

// ------------------------------------------------------------------------------------------- pools
namespace {
std::mutex poolLock;
std::map<std::string, std::unique_ptr<Pool>> pools;
std::vector<std::unique_ptr<PoolElement>> elementStore;
const PoolElement* EMPTY_ELEMENT = nullptr;

const PoolElement* emptyElement() {
  if (!EMPTY_ELEMENT) {
    elementStore.push_back(std::make_unique<PoolElement>());
    EMPTY_ELEMENT = elementStore.back().get();
  }
  return EMPTY_ELEMENT;
}

PoolElement* parseElement(const Json& j, bool terrainMatching) {
  std::string t = j["element_type"].asString();
  if (t == "minecraft:empty_pool_element") return const_cast<PoolElement*>(emptyElement());
  auto e = std::make_unique<PoolElement>();
  if (auto* p = j.find("projection")) terrainMatching = p->asString() == "terrain_matching";
  e->terrainMatching = terrainMatching;
  if (t == "minecraft:single_pool_element" || t == "minecraft:legacy_single_pool_element") {
    e->kind = t == "minecraft:single_pool_element" ? PoolElement::SINGLE : PoolElement::LEGACY;
    e->location = fullId(j["location"].asString());
    if (auto* p = j.find("processors")) e->processors = &processorList(*p);
    if (auto* l = j.find("override_liquid_settings")) e->overrideLiquid = l->asString() == "apply_waterlogging" ? 1 : 0;
  } else if (t == "minecraft:feature_pool_element") {
    e->kind = PoolElement::FEATURE;
    e->location = fullId(j["feature"].asString());
  } else if (t == "minecraft:list_pool_element") {
    e->kind = PoolElement::LIST;
    for (auto& x : j["elements"].arr) {
      PoolElement* el = parseElement(x, terrainMatching);
      el->terrainMatching = terrainMatching;  // setProjectionOnEachElement
      e->list.push_back(el);
    }
  }
  elementStore.push_back(std::move(e));
  return elementStore.back().get();
}
}  // namespace

const Pool* Pool::get(const std::string& id) {
  std::string key = fullId(id);
  std::lock_guard lock(poolLock);
  auto it = pools.find(key);
  if (it != pools.end()) return it->second.get();
  auto c = key.find(':');
  std::string path = dataRoot() + "/" + key.substr(0, c) + "/worldgen/template_pool/" + key.substr(c + 1) + ".json";
  std::unique_ptr<Pool> p;
  try {
    Json j = Json::parseFile(path);
    p = std::make_unique<Pool>();
    p->id = key;
    p->fallback = fullId(j["fallback"].asString());
    for (auto& e : j["elements"].arr) {
      const PoolElement* el = parseElement(e["element"], false);
      for (int i = 0; i < e["weight"].asInt(); i++) p->templates.push_back(el);
    }
  } catch (const std::exception& e) {
    if (std::filesystem::exists(path)) fprintf(stderr, "structure: bad template pool %s: %s\n", key.c_str(), e.what());
    p = nullptr;  // missing pool
  }
  return (pools[key] = std::move(p)).get();
}

int Pool::maxSize() const {
  if (maxSize_ == INT_MIN) {
    int m = 0;
    for (auto* t : templates)
      if (!t->isEmpty()) m = std::max(m, t->boundingBox({0, 0, 0}, Rotation::NONE).ySpan());
    maxSize_ = m;
  }
  return maxSize_;
}

// ------------------------------------------------------------------------------------------- elements
const Template& PoolElement::tmpl() const { return Template::get(location); }

std::vector<JigsawInfo> PoolElement::shuffledJigsaws(BlockPos pos, Rotation r, RandomSource& random) const {
  switch (kind) {
    case SINGLE: case LEGACY: {
      auto j = templateJigsaws(tmpl(), pos, r);
      shuffle(j, random);
      std::stable_sort(j.begin(), j.end(), [](const JigsawInfo& a, const JigsawInfo& b) { return a.selectionPriority > b.selectionPriority; });
      return j;
    }
    case LIST: return list[0]->shuffledJigsaws(pos, r, random);
    case FEATURE: {
      JigsawInfo j;
      j.pos = pos;
      j.state = Blocks::get().with(Blocks::get().defaultState("jigsaw"), "orientation", "down_south");
      j.rollable = true;
      j.pool = "minecraft:empty";
      j.target = "minecraft:empty";
      return {j};
    }
    default: return {};
  }
}

BoundingBox PoolElement::boundingBox(BlockPos pos, Rotation r) const {
  switch (kind) {
    case SINGLE: case LEGACY: {
      PlaceSettings s;
      s.rotation = r;
      return templateBoundingBox(tmpl(), s, pos);
    }
    case LIST: {
      std::optional<BoundingBox> b;
      for (auto* e : list)
        if (!e->isEmpty()) {
          BoundingBox x = e->boundingBox(pos, r);
          b = b ? b->encapsulate(x) : x;
        }
      return *b;
    }
    case FEATURE: return {pos.x, pos.y, pos.z, pos.x, pos.y, pos.z};
    default: return {};
  }
}

bool PoolElement::place(Level& level, BlockPos pos, BlockPos ref, Rotation r, const BoundingBox& chunkBB, RandomSource& random,
                        bool waterlog) const {
  switch (kind) {
    case SINGLE: case LEGACY: {
      PlaceSettings s;
      s.boundingBox = chunkBB;
      s.rotation = r;
      s.knownShape = true;
      s.finalizeEntities = true;
      s.applyWaterlogging = overrideLiquid >= 0 ? overrideLiquid == 1 : waterlog;
      static const ProcessorP STRUCTURE_BLOCK = blockIgnore({"structure_block"});
      static const ProcessorP STRUCTURE_AND_AIR = blockIgnore({"air", "structure_block"});
      if (kind == SINGLE) s.processors.push_back(STRUCTURE_BLOCK);
      s.processors.push_back(jigsawReplacement());
      if (processors)
        for (auto& p : *processors) s.processors.push_back(p);
      if (terrainMatching) {
        static const ProcessorP GRAVITY = gravity(Heightmap::WORLD_SURFACE_WG, -1);
        s.processors.push_back(GRAVITY);
      }
      if (kind == LEGACY) s.processors.push_back(STRUCTURE_AND_AIR);
      return placeInWorld(tmpl(), level, pos, ref, s, random);
    }
    case LIST:
      for (auto* e : list)
        if (!e->place(level, pos, ref, r, chunkBB, random, waterlog)) return false;
      return true;
    case FEATURE: return level.placeFeature(location, random, pos);
    default: return true;
  }
}

// ------------------------------------------------------------------------------------------- piece
void PoolElementPiece::postProcess(Level& level, RandomSource& random, const BoundingBox& chunkBB, int, int, BlockPos ref) {
  element->place(level, position, ref, rot, chunkBB, random, waterlog);
}

// ------------------------------------------------------------------------------------------- placement
namespace {
// VoxelShape free space of JigsawPlacement as integer boxes: a container minus the boxes taken from it.
struct FreeSpace {
  BoundingBox container;  // block coordinates, inclusive
  std::vector<BoundingBox> taken;
  bool fits(const BoundingBox& b) const {
    if (b.minX < container.minX || b.minY < container.minY || b.minZ < container.minZ || b.maxX > container.maxX ||
        b.maxY > container.maxY || b.maxZ > container.maxZ)
      return false;
    for (auto& t : taken)
      if (t.intersects(b)) return false;
    return true;
  }
};
using FreeP = std::shared_ptr<FreeSpace>;

struct PieceState {
  PoolElementPiece* piece;
  std::shared_ptr<FreeP> free;  // MutableObject<VoxelShape>
  int depth;
};

struct Placer {
  int maxDepth;
  Generator& gen;
  std::vector<PoolElementPiece*>& pieces;
  std::vector<std::unique_ptr<PoolElementPiece>>& owned;
  RandomSource& random;
  const std::map<std::string, std::string>& aliases;
  bool waterlog;
  // SequencedPriorityIterator: highest priority first, FIFO within a priority
  std::map<int, std::deque<PieceState>> placing;

  std::string alias(const std::string& k) const {
    auto it = aliases.find(k);
    return it == aliases.end() ? k : it->second;
  }

  void tryPlacingChildren(PoolElementPiece* source, std::shared_ptr<FreeP> contextFree, int depth, bool expansionHack) {
    const PoolElement* sourceElement = source->element;
    BlockPos sourceBoxPos = source->position;
    Rotation sourceRotation = source->rot;
    bool sourceRigid = !sourceElement->terrainMatching;
    auto sourceFree = std::make_shared<FreeP>();
    BoundingBox sourceBB = source->boundingBox;
    int sourceBoxY = sourceBB.minY;
    for (auto& sourceJigsaw : sourceElement->shuffledJigsaws(sourceBoxPos, sourceRotation, random)) {
      Dir sourceDir = jigsawFront(sourceJigsaw.state);
      BlockPos sourceJigsawPos = sourceJigsaw.pos;
      BlockPos targetJigsawPos = sourceJigsawPos.relative(sourceDir);
      int sourceJigsawLocalY = sourceJigsawPos.y - sourceBoxY;
      int sourceJigsawBaseHeight = INT_MIN;
      const Pool* targetPool = Pool::get(alias(sourceJigsaw.pool));
      if (!targetPool) continue;
      if (targetPool->templates.empty() && targetPool->id != "minecraft:empty") continue;
      const Pool* fallback = Pool::get(targetPool->fallback);
      if (!fallback || (fallback->templates.empty() && fallback->id != "minecraft:empty")) continue;
      bool attachInsideSource = sourceBB.isInside(targetJigsawPos);
      std::shared_ptr<FreeP> childrenFree;
      if (attachInsideSource) {
        childrenFree = sourceFree;
        if (!*sourceFree) *sourceFree = std::make_shared<FreeSpace>(FreeSpace{sourceBB, {}});
      } else {
        childrenFree = contextFree;
      }
      std::vector<const PoolElement*> targets;
      if (depth != maxDepth) {
        auto t = targetPool->templates;
        shuffle(t, random);
        targets.insert(targets.end(), t.begin(), t.end());
      }
      {
        auto t = fallback->templates;
        shuffle(t, random);
        targets.insert(targets.end(), t.begin(), t.end());
      }
      int placementPriority = sourceJigsaw.placementPriority;
      bool placed = false;
      for (const PoolElement* targetElement : targets) {
        if (targetElement->isEmpty()) break;
        for (Rotation targetRotation : shuffledRotations(random)) {
          auto targetJigsaws = targetElement->shuffledJigsaws({0, 0, 0}, targetRotation, random);
          BoundingBox hackBox = targetElement->boundingBox({0, 0, 0}, targetRotation);
          int expandTo = 0;
          if (expansionHack && hackBox.ySpan() <= 16) {
            for (auto& tj : targetJigsaws) {
              int v = 0;
              if (hackBox.isInside(tj.pos.relative(jigsawFront(tj.state)))) {
                const Pool* child = Pool::get(alias(tj.pool));
                const Pool* childFallback = child ? Pool::get(child->fallback) : nullptr;
                int a = child ? child->maxSize() : 0, b = childFallback ? childFallback->maxSize() : 0;
                v = std::max(a, b);
              }
              expandTo = std::max(expandTo, v);
            }
          }
          for (auto& targetJigsaw : targetJigsaws) {
            if (!canAttach(sourceJigsaw, targetJigsaw)) continue;
            BlockPos targetJigsawLocalPos = targetJigsaw.pos;
            BlockPos rawTargetBoxPos = targetJigsawPos.subtract(targetJigsawLocalPos);
            BoundingBox rawTargetBB = targetElement->boundingBox(rawTargetBoxPos, targetRotation);
            int rawTargetY = rawTargetBB.minY;
            bool targetRigid = !targetElement->terrainMatching;
            int targetJigsawLocalY = targetJigsawLocalPos.y;
            int deltaY = sourceJigsawLocalY - targetJigsawLocalY + stepY(jigsawFront(sourceJigsaw.state));
            int targetBoxY;
            if (sourceRigid && targetRigid) {
              targetBoxY = sourceBoxY + deltaY;
            } else {
              if (sourceJigsawBaseHeight == INT_MIN)
                sourceJigsawBaseHeight = gen.baseHeight(sourceJigsawPos.x, sourceJigsawPos.z, Heightmap::WORLD_SURFACE_WG);
              targetBoxY = sourceJigsawBaseHeight - targetJigsawLocalY;
            }
            int yOffset = targetBoxY - rawTargetY;
            BoundingBox targetBB = rawTargetBB.moved(0, yOffset, 0);
            BlockPos targetBoxPos = rawTargetBoxPos.offset(0, yOffset, 0);
            if (expandTo > 0) {
              int newSize = std::max(expandTo + 1, targetBB.maxY - targetBB.minY);
              targetBB.encapsulate(BlockPos{targetBB.minX, targetBB.minY + newSize, targetBB.minZ});
            }
            FreeSpace& freeShape = **childrenFree;
            if (!freeShape.fits(targetBB)) continue;
            freeShape.taken.push_back(targetBB);  // Shapes.joinUnoptimized(free, target, ONLY_FIRST)
            int sourceGroundLevelDelta = source->groundLevelDelta;
            int targetGroundLevelDelta = targetRigid ? sourceGroundLevelDelta - deltaY : targetElement->groundLevelDelta();
            auto targetPiece = std::make_unique<PoolElementPiece>(targetElement, targetBoxPos, targetGroundLevelDelta, targetRotation,
                                                                  targetBB, waterlog);
            int junctionY;
            if (sourceRigid) {
              junctionY = sourceBoxY + sourceJigsawLocalY;
            } else if (targetRigid) {
              junctionY = targetBoxY + targetJigsawLocalY;
            } else {
              if (sourceJigsawBaseHeight == INT_MIN)
                sourceJigsawBaseHeight = gen.baseHeight(sourceJigsawPos.x, sourceJigsawPos.z, Heightmap::WORLD_SURFACE_WG);
              junctionY = sourceJigsawBaseHeight + deltaY / 2;
            }
            source->junctions.push_back({targetJigsawPos.x, junctionY - sourceJigsawLocalY + sourceGroundLevelDelta,
                                         targetJigsawPos.z, deltaY, targetElement->terrainMatching});
            targetPiece->junctions.push_back({sourceJigsawPos.x, junctionY - targetJigsawLocalY + targetGroundLevelDelta,
                                              sourceJigsawPos.z, -deltaY, sourceElement->terrainMatching});
            PoolElementPiece* tp = targetPiece.get();
            owned.push_back(std::move(targetPiece));
            pieces.push_back(tp);
            if (depth + 1 <= maxDepth) placing[placementPriority].push_back({tp, childrenFree, depth + 1});
            placed = true;
            break;
          }
          if (placed) break;
        }
        if (placed) break;
      }
    }
  }

  bool next(PieceState& out) {
    while (!placing.empty()) {
      auto it = std::prev(placing.end());
      if (it->second.empty()) {
        placing.erase(it);
        continue;
      }
      out = it->second.front();
      it->second.pop_front();
      return true;
    }
    return false;
  }
};

struct JigsawStructure : Structure {
  std::string startPool, startJigsawName;
  int maxDepth;
  Json startHeight;
  bool expansionHack;
  std::optional<Heightmap> projectStart;
  int maxHorizontal, maxVertical;
  Json aliases;
  int padBottom = 0, padTop = 0;
  bool waterlog = true;

  explicit JigsawStructure(const Json& j) {
    startPool = fullId(j["start_pool"].asString());
    if (auto* n = j.find("start_jigsaw_name")) startJigsawName = fullId(n->asString());
    maxDepth = j["size"].asInt();
    startHeight = j["start_height"];
    expansionHack = j.find("use_expansion_hack") && (*j.find("use_expansion_hack")).asBool();
    if (auto* p = j.find("project_start_to_heightmap")) projectStart = heightmapByName(p->asString());
    maxHorizontal = 80, maxVertical = GEN_HEIGHT;
    if (auto* m = j.find("max_distance_from_center")) {
      if (m->isNumber()) maxHorizontal = maxVertical = m->asInt();
      else maxHorizontal = (*m)["horizontal"].asInt(), maxVertical = m->getInt("vertical", GEN_HEIGHT);
    }
    if (auto* a = j.find("pool_aliases")) aliases = *a;
    if (auto* d = j.find("dimension_padding")) {
      if (d->isNumber()) padBottom = padTop = d->asInt();
      else padBottom = d->getInt("bottom", 0), padTop = d->getInt("top", 0);
    }
    if (auto* l = j.find("liquid_settings")) waterlog = l->asString() == "apply_waterlogging";
  }

  static void resolveAliases(const Json& bindings, RandomSource& r, std::map<std::string, std::string>& out) {
    for (auto& b : bindings.arr) {
      std::string t = b["type"].asString();
      if (t == "minecraft:direct") {
        out.emplace(fullId(b["alias"].asString()), fullId(b["target"].asString()));
      } else if (t == "minecraft:random") {
        int total = 0;
        for (auto& e : b["targets"].arr) total += e["weight"].asInt();
        int sel = r.nextInt(total);
        for (auto& e : b["targets"].arr)
          if ((sel -= e["weight"].asInt()) < 0) {
            out.emplace(fullId(b["alias"].asString()), fullId(e["data"].asString()));
            break;
          }
      } else if (t == "minecraft:random_group") {
        int total = 0;
        for (auto& e : b["groups"].arr) total += e["weight"].asInt();
        int sel = r.nextInt(total);
        for (auto& e : b["groups"].arr)
          if ((sel -= e["weight"].asInt()) < 0) {
            resolveAliases(e["data"], r, out);
            break;
          }
      }
    }
  }

  std::optional<Stub> findGenerationPoint(GenContext& ctx) const override {
    int height = sampleHeight(startHeight, ctx.random);
    BlockPos position{ctx.cx * 16, height, ctx.cz * 16};
    auto aliasMap = std::make_shared<std::map<std::string, std::string>>();
    if (aliases.isArray() && !aliases.arr.empty()) {  // PoolAliasLookup.create
      LegacyRandomSource base(ctx.seed);
      auto r = base.forkPositional()->at(position.x, position.y, position.z);
      resolveAliases(aliases, *r, *aliasMap);
    }
    auto alias = [&](const std::string& k) {
      auto it = aliasMap->find(k);
      return it == aliasMap->end() ? k : it->second;
    };
    RandomSource& random = ctx.random;
    Rotation centerRotation = getRandomRotation(random);
    const Pool* centerPool = Pool::get(alias(startPool));
    if (getenv("JDBG")) fprintf(stderr, "jigsaw %s at %d %d pool=%p\n", id.c_str(), ctx.cx, ctx.cz, (void*)centerPool);
    if (!centerPool) return std::nullopt;
    const PoolElement* centerElement =
        centerPool->templates.empty() ? emptyElement() : centerPool->templates[random.nextInt((int)centerPool->templates.size())];
    if (centerElement->isEmpty()) return std::nullopt;
    BlockPos anchored = position;
    if (!startJigsawName.empty()) {
      bool found = false;
      for (auto& j : centerElement->shuffledJigsaws(position, centerRotation, random))
        if (j.name == startJigsawName) {
          anchored = j.pos;
          found = true;
          break;
        }
      if (!found) return std::nullopt;
    }
    BlockPos localAnchor = anchored.subtract(position);
    BlockPos adjusted = position.subtract(localAnchor);
    auto center = std::make_shared<PoolElementPiece>(centerElement, adjusted, centerElement->groundLevelDelta(), centerRotation,
                                                     centerElement->boundingBox(adjusted, centerRotation), waterlog);
    BoundingBox box = center->boundingBox;
    int centerX = (box.maxX + box.minX) / 2, centerZ = (box.maxZ + box.minZ) / 2;
    int bottomY;
    if (!projectStart) {
      bottomY = adjusted.y;
    } else {
      if (!ctx.couldStructureExistInColumn(centerX, centerZ, MIN_Y, MAX_Y)) {
        if (getenv("JDBG")) fprintf(stderr, "  no biome in column\n");
        return std::nullopt;
      }
      bottomY = position.y + ctx.gen.baseHeight(centerX, centerZ, *projectStart);
    }
    int oldGround = box.minY + center->groundLevelDelta;
    center->move(0, bottomY - oldGround, 0);
    if ((padBottom || padTop) && (center->boundingBox.minY < MIN_Y + padBottom || center->boundingBox.maxY > MAX_Y - padTop))
      return std::nullopt;
    int centerY = bottomY + localAnchor.y;
    int maxDepthV = maxDepth, h = maxHorizontal, v = maxVertical, pb = padBottom, pt = padTop;
    bool hack = expansionHack, wl = waterlog;
    Generator* gen = &ctx.gen;
    RandomSource* rnd = &ctx.random;
    Stub stub;
    stub.position = {centerX, centerY, centerZ};
    stub.generator = [=](Builder& builder) {
      std::vector<PoolElementPiece*> list{center.get()};
      std::vector<std::unique_ptr<PoolElementPiece>> owned;
      if (maxDepthV > 0) {
        // AABB(centerX - h, max(centerY - v, minY + pb), ...) minus the center box, as inclusive block bounds
        BoundingBox container(centerX - h, std::max(centerY - v, MIN_Y + pb), centerZ - h, centerX + h,
                              std::min(centerY + v + 1, MAX_Y + 1 - pt) - 1, centerZ + h);
        auto free = std::make_shared<FreeP>(std::make_shared<FreeSpace>(FreeSpace{container, {box}}));
        Placer placer{maxDepthV, *gen, list, owned, *rnd, *aliasMap, wl, {}};
        placer.tryPlacingChildren(center.get(), free, 0, hack);
        PieceState st;
        while (placer.next(st)) placer.tryPlacingChildren(st.piece, st.free, st.depth, hack);
      }
      // The center piece object is shared with the stub; copy it into the builder.
      builder.add(std::make_unique<PoolElementPiece>(*center));
      for (auto& o : owned) builder.add(std::move(o));
    };
    return stub;
  }
};
}  // namespace

std::unique_ptr<Structure> makeJigsawStructure(const Json& j) { return std::make_unique<JigsawStructure>(j); }

}  // namespace structure
