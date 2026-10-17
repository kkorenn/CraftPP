// Advancements: data (ServerAdvancementManager + AdvancementTree + TreeNodePosition), predicates, criteria
// triggers, PlayerAdvancements (progress, rewards, visibility, update_advancements), players/advancements/<uuid>.json
// and /advancement. Overview: progress.hpp.
#include "progress.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>

namespace progress {

// ---------- small helpers ----------
std::string uuid_str(const Player& p) {
    char b[40];
    uint64_t h = uint64_t(p.uuid_hi), l = uint64_t(p.uuid_lo);
    snprintf(b, sizeof b, "%08x-%04x-%04x-%04x-%012llx", unsigned(h >> 32), unsigned(h >> 16 & 0xFFFF), unsigned(h & 0xFFFF),
             unsigned(l >> 48), (unsigned long long)(l & 0xFFFFFFFFFFFFull));
    return b;
}
std::string json_quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (uint8_t(c) < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
// AdvancementProgress OBTAINED_TIME_FORMAT "yyyy-MM-dd HH:mm:ss Z" in the system zone.
static std::string fmt_time(int64_t ms) {
    time_t t = time_t(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    char b[64];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %z", &tm);
    return b;
}
static int64_t parse_time(const std::string& s) {
    struct tm tm{};
    char sign = '+';
    int off = 0;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d %c%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
               &tm.tm_sec, &sign, &off) < 6)
        return now_ms();
    tm.tm_year -= 1900, tm.tm_mon -= 1;
    int64_t secs = int64_t(timegm(&tm)) - (sign == '-' ? -1 : 1) * int64_t(off / 100 * 3600 + off % 100 * 60);
    return std::max<int64_t>(secs * 1000, 1);
}

// ---------- registries + tags ----------
static const gen::Registry* greg(const std::string& reg) {
    for (auto& r : gen::REGISTRIES) if ("minecraft:" + reg == r.name) return &r;
    return nullptr;
}
int reg_id(const std::string& reg, const std::string& raw) {
    std::string id = ns(raw);
    if (reg == "item") { auto it = item_by_name.find(id); return it == item_by_name.end() ? -1 : it->second; }
    if (reg == "block") { auto it = block_by_name.find(id); return it == block_by_name.end() ? -1 : it->second; }
    if (reg == "fluid") return id == "minecraft:water" || id == "minecraft:flowing_water" ? 0 : id == "minecraft:lava" || id == "minecraft:flowing_lava" ? 1 : -1;
    if (reg == "recipe") return find_recipe(id);
    if (const gen::Registry* r = greg(reg)) {
        static std::map<std::string, std::unordered_map<std::string, int>> idx;  // background loader + tick thread after wait_loaded
        auto& m = idx[reg];
        if (m.empty())
            for (int i = 0; i < r->count; i++) m["minecraft:" + std::string(r->entries[i])] = i;
        auto it = m.find(id);
        return it == m.end() ? -1 : it->second;
    }
    return -1;
}
// Tag file contents with nested tags expanded (TagLoader); entries are ids.
static const std::vector<std::string>& tag_names(const std::string& reg, const std::string& tag) {
    static std::map<std::string, std::vector<std::string>> cache;
    std::string key = reg + "|" + tag;
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    auto& out = cache[key];  // empty while resolving: breaks reference cycles
    std::string id = ns(tag);
    std::string path = "data/vanilla/" + id.substr(0, id.find(':')) + "/tags/" + reg + "/" + id.substr(id.find(':') + 1) + ".json";
    std::vector<std::string> v;
    try {
        Json j = Json::parseFile(path);
        for (auto& e : j["values"].arr) {
            std::string s = e.isString() ? e.asString() : e["id"].asString();
            if (s[0] == '#') for (auto& x : tag_names(reg, s.substr(1))) v.push_back(x);
            else v.push_back(ns(s));
        }
    } catch (const std::exception&) {
    }
    cache[key] = std::move(v);
    return cache[key];
}
std::vector<std::string> holder_names(const std::string& reg, const Json& j) {
    std::vector<std::string> v;
    if (j.isArray()) for (auto& e : j.arr) v.push_back(ns(e.asString()));
    else if (j.isString() && j.asString()[0] == '#') v = tag_names(reg, j.asString().substr(1));
    else if (j.isString()) v.push_back(ns(j.asString()));
    return v;
}
Ids holder_set(const std::string& reg, const Json& j, std::string* tag) {
    if (tag && j.isString() && j.asString()[0] == '#') *tag = ns(j.asString().substr(1));
    std::vector<uint16_t> ids;
    for (auto& n : holder_names(reg, j))
        if (int i = reg_id(reg, n); i >= 0) ids.push_back(uint16_t(i));
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return std::make_shared<const std::vector<uint16_t>>(std::move(ids));
}

// ---------- predicates ----------
static void warn_once(const std::string& what) {
    static std::set<std::string> seen;
    if (seen.insert(what).second) log_warn("advancements: unsupported %s (criteria using it never trigger)", what.c_str());
}
IRange irange(const Json* j) {
    IRange r;
    if (!j) return r;
    if (j->isNumber()) { r.lo = r.hi = int64_t(j->num); return r; }
    if (auto* a = j->find("min")) r.lo = int64_t(a->num);
    if (auto* b = j->find("max")) r.hi = int64_t(b->num);
    return r;
}
DRange drange(const Json* j) {
    DRange r;
    if (!j) return r;
    if (j->isNumber()) { r.lo = r.hi = j->num; return r; }
    if (auto* a = j->find("min")) r.lo = a->num;
    if (auto* b = j->find("max")) r.hi = b->num;
    return r;
}

ItemPred item_pred(const Json& j) {
    ItemPred p;
    if (auto* i = j.find("items")) p.items = holder_set("item", *i);
    p.count = irange(j.find("count"));
    for (auto& [k, v] : j.obj) {
        (void)v;
        if (k != "items" && k != "count") p.bad = true, warn_once("item predicate " + k);
    }
    return p;
}
bool ItemPred::test(const ItemStack& s) const {
    if (bad) return false;
    if (items && !has(items, s.item)) return false;
    if (!count.test(s.count)) return false;
    if (items == nullptr && s.empty()) return true;
    return true;
}

static BlockPred block_pred(const Json& j, const std::string& reg = "block") {
    BlockPred b;
    if (auto* x = j.find(reg == "block" ? "blocks" : "fluids")) b.blocks = holder_set(reg, *x);
    b.state = loot_detail::states(j.find("state"));
    if (j.find("nbt") || j.find("components") || j.find("predicates")) b.bad = true, warn_once("block predicate nbt/components");
    return b;
}
bool BlockPred::test(int s) const {
    if (bad) return false;
    if (blocks && !has(blocks, block_index(s))) return false;
    return state_matches(s, state);
}

static std::unique_ptr<LocPred> loc_pred(const Json& j) {
    auto l = std::make_unique<LocPred>();
    for (auto& [k, v] : j.obj) {
        if (k == "position") l->x = drange(v.find("x")), l->y = drange(v.find("y")), l->z = drange(v.find("z"));
        else if (k == "biomes") l->biomes = holder_set("worldgen/biome", v);
        else if (k == "structures") l->structures = holder_names("worldgen/structure", v);
        else if (k == "dimension") {
            std::string d = ns(v.asString());
            l->dim = d == "minecraft:overworld" ? DIM_OVERWORLD : d == "minecraft:the_nether" ? DIM_NETHER : d == "minecraft:the_end" ? DIM_END : 99;
        } else if (k == "block") l->block = block_pred(v);
        else if (k == "fluid") {
            BlockPred f;
            if (auto* x = v.find("fluids")) f.blocks = holder_set("fluid", *x);
            f.state = loot_detail::states(v.find("state"));
            l->fluid = f;
        } else l->bad = true, warn_once("location predicate " + k);
    }
    return l;
}
// StructureManager.getStructureWithPieceAt(pos, structures).isValid()
static bool in_structure(int x, int y, int z, const std::vector<std::string>& names) {
    auto sw = std::atomic_load(&structure_worlds[size_t(dim_of(cur_level))]);
    if (!sw) return false;
    auto& mgr = sw->manager();
    for (auto& [st, keys] : spawn_detail::structure_refs(mgr, x >> 4, z >> 4)) {
        if (std::find(names.begin(), names.end(), st->id) == names.end()) continue;
        for (int64_t k : keys)
            for (auto& start : mgr.starts(int32_t(uint32_t(k)), int32_t(uint32_t(uint64_t(k) >> 32))))
                if (start->structure == st && start->valid())
                    for (auto& piece : start->pieces)
                        if (piece->boundingBox.isInside(x, y, z)) return true;
    }
    return false;
}
bool LocPred::test(World* w, double px, double py, double pz) const {
    if (bad) return false;
    if (!x.test(px) || !y.test(py) || !z.test(pz)) return false;
    if (dim >= 0 && dim != dim_of(w)) return false;
    InLevel in_level(w);
    int bx = mth::floor(px), by = mth::floor(py), bz = mth::floor(pz);
    bool loaded = w->get_if_loaded(bx >> 4, bz >> 4) != nullptr;
    if (biomes && (!loaded || !has(biomes, lvl::biome_at({bx, by, bz})))) return false;
    if (!structures.empty() && (!loaded || !in_structure(bx, by, bz, structures))) return false;
    if (block && (!loaded || !block->test(w->get_block(bx, by, bz)))) return false;
    if (fluid) {
        if (!loaded) return false;
        int s = w->get_block(bx, by, bz);
        int b = block_index(s);
        int f = b == block_index(blocks::water) ? 0 : b == block_index(blocks::lava) ? 1 : -1;
        if (f < 0 && state_prop_str(s, "waterlogged") == "true") f = 0;
        if (f < 0 || (fluid->blocks && !has(fluid->blocks, f))) return false;
    }
    return true;
}

bool DistPred::test(double x0, double y0, double z0, double x1, double y1, double z1) const {
    float xd = float(x0 - x1), yd = float(y0 - y1), zd = float(z0 - z1);
    if (!x.test(std::abs(xd)) || !y.test(std::abs(yd)) || !z.test(std::abs(zd))) return false;
    return horizontal.test_sqr(double(xd * xd + zd * zd)) && absolute.test_sqr(double(xd * xd + yd * yd + zd * zd));
}
static std::unique_ptr<DistPred> dist_pred(const Json& j) {
    auto d = std::make_unique<DistPred>();
    d->x = drange(j.find("x")), d->y = drange(j.find("y")), d->z = drange(j.find("z"));
    d->horizontal = drange(j.find("horizontal")), d->absolute = drange(j.find("absolute"));
    return d;
}

static std::vector<EffectPred> effects_pred(const Json& j) {
    std::vector<EffectPred> v;
    for (auto& [k, e] : j.obj) {
        EffectPred p;
        p.effect = reg_id("mob_effect", k);
        p.amp = irange(e.find("amplifier")), p.dur = irange(e.find("duration"));
        if (auto* a = e.find("ambient")) p.ambient = a->b;
        if (auto* b = e.find("visible")) p.visible = b->b;
        v.push_back(p);
    }
    return v;
}
static bool effects_test(const std::vector<EffectPred>& v, Entity* e) {
    for (auto& p : v) {
        int amp = 0, dur = 0;
        bool ambient = false, visible = true;
        if (!player_effect || !player_effect(*e, p.effect, amp, dur, ambient, visible)) return false;
        if (!p.amp.test(amp) || !p.dur.test(dur)) return false;
        if (p.ambient >= 0 && bool(p.ambient) != ambient) return false;
        if (p.visible >= 0 && bool(p.visible) != visible) return false;
    }
    return true;
}

std::unique_ptr<EntPred> ent_pred(const Json& j) {
    auto e = std::make_unique<EntPred>();
    static const std::map<std::string, int> slots = {{"mainhand", ES_MAINHAND}, {"offhand", ES_OFFHAND}, {"head", ES_HEAD},
                                                     {"chest", ES_CHEST}, {"legs", ES_LEGS}, {"feet", ES_FEET}, {"body", ES_BODY}};
    for (auto& [rk, v] : j.obj) {
        std::string k = ns(rk);
        if (k == "minecraft:entity_type") e->types = holder_set("entity_type", v);
        else if (k == "minecraft:location") e->loc = loc_pred(v);
        else if (k == "minecraft:stepping_on") e->stepping = loc_pred(v);
        else if (k == "minecraft:distance") e->dist = dist_pred(v);
        else if (k == "minecraft:flags") for (auto& [f, b] : v.obj) e->flags.emplace_back(f, b.b);
        else if (k == "minecraft:equipment")
            for (auto& [s, ip] : v.obj) {
                auto it = slots.find(s);
                if (it == slots.end()) e->bad = true;
                else e->equipment.emplace_back(it->second, item_pred(ip));
            }
        else if (k == "minecraft:effects") e->effects = effects_pred(v);
        else e->bad = true, warn_once("entity predicate " + k);
    }
    return e;
}
static bool entity_flag(Entity* e, const std::string& f) {
    if (f == "is_on_fire") return e->is_on_fire();
    if (f == "is_baby") return e->loot_flag("is_baby");
    if (f == "is_on_ground") return e->on_ground;
    if (Player* p = e->as_player()) {
        if (f == "is_sneaking") return p->g.sneaking;
        if (f == "is_sprinting") return p->g.sprinting;
        if (f == "is_flying") return p->net.flying;
        if (f == "is_fall_flying") return p->net.fall_flying;
    }
    if (f == "is_sneaking") return e->shared_flag(1);
    if (f == "is_sprinting") return e->shared_flag(3);
    if (f == "is_swimming") return e->shared_flag(4);
    if (f == "is_fall_flying") return e->shared_flag(7);
    if (f == "is_in_water") return e->was_touching_water;
    return false;
}
bool EntPred::test(Entity* e, World* w, const Vec3& origin) const {
    if (!e || bad) return false;
    if (types && !has(types, e->type)) return false;
    World* lw = e->level() ? e->level() : w;
    if (loc && !loc->test(lw, e->pos.x, e->pos.y, e->pos.z)) return false;
    if (stepping && !stepping->test(lw, e->pos.x, std::floor(e->pos.y - 0.2), e->pos.z)) return false;  // getOnPos
    if (dist && !dist->test(origin.x, origin.y, origin.z, e->pos.x, e->pos.y, e->pos.z)) return false;
    for (auto& [f, want] : flags) if (entity_flag(e, f) != want) return false;
    if (!equipment.empty()) {
        LivingEntity* le = e->as_living();
        if (!le) return false;
        for (auto& [slot, ip] : equipment) if (!ip.test(le->item_in_slot(slot))) return false;
    }
    if (!effects.empty() && !effects_test(effects, e)) return false;
    return true;
}

static DamagePred damage_pred(const Json& j) {
    DamagePred d;
    if (auto* t = j.find("tags"))
        for (auto& x : t->arr) d.tags.emplace_back(tag_names("damage_type", ns(x["id"].asString())), x["expected"].b);
    if (auto* x = j.find("direct_entity")) d.direct = ent_pred(*x);
    if (auto* x = j.find("source_entity")) d.source = ent_pred(*x);
    if (auto* x = j.find("is_direct")) d.is_direct = x->b;
    return d;
}
bool DamagePred::test(const DamageSource& s, World* w, const Vec3& origin) const {
    std::string name = "minecraft:" + s.name();
    for (auto& [names, want] : tags)
        if ((std::find(names.begin(), names.end(), name) != names.end()) != want) return false;
    if (direct && !direct->test(s.direct, w, origin)) return false;
    if (source && !source->test(s.causing, w, origin)) return false;
    if (is_direct >= 0 && (s.causing == s.direct) != bool(is_direct)) return false;
    return true;
}

static LCond lcond(const Json& j) {
    LCond c;
    std::string t = ns(j.find("condition") ? j["condition"].asString() : j["type"].asString());
    if (t == "minecraft:entity_properties") {
        if (j.find("entity") && j["entity"].asString() != "this") { warn_once("entity_properties entity " + j["entity"].asString()); return c; }
        c.t = LCond::ENTITY;
        c.ent = j.find("predicate") ? std::shared_ptr<EntPred>(ent_pred(j["predicate"])) : std::make_shared<EntPred>();
    } else if (t == "minecraft:inverted") {
        c.t = LCond::INVERTED;
        c.kids.push_back(lcond(j["term"]));
    } else if (t == "minecraft:any_of" || t == "minecraft:all_of") {
        c.t = t == "minecraft:any_of" ? LCond::ANY_OF : LCond::ALL_OF;
        for (auto& k : j["terms"].arr) c.kids.push_back(lcond(k));
    } else if (t == "minecraft:location_check") {
        c.t = LCond::LOCATION;
        c.loc = j.find("predicate") ? std::shared_ptr<LocPred>(loc_pred(j["predicate"])) : std::make_shared<LocPred>();
        c.off[0] = j.getInt("offsetX", 0), c.off[1] = j.getInt("offsetY", 0), c.off[2] = j.getInt("offsetZ", 0);
    } else if (t == "minecraft:block_state_property") {
        c.t = LCond::BLOCK_STATE;
        c.block.blocks = holder_set("block", j["block"]);
        c.block.state = loot_detail::states(j.find("properties"));
    } else if (t == "minecraft:match_tool") {
        c.t = LCond::MATCH_TOOL;
        if (auto* p = j.find("predicate")) c.tool = item_pred(*p);
    } else if (t == "minecraft:random_chance") {
        c.t = LCond::RANDOM;
        c.chance = j["chance"].isNumber() ? j["chance"].asFloat() : 0;
    } else {
        warn_once("condition " + t);
    }
    return c;
}
Conds conds(const Json* j) {
    Conds v;
    if (!j) return v;
    if (j->isArray()) for (auto& x : j->arr) v.push_back(lcond(x));
    else v.push_back(lcond(*j));
    return v;
}
bool LCond::test(const LCtx& c) const {
    switch (t) {
        case FALSE_: return false;
        case ENTITY: return ent->test(c.self, c.level, c.origin);
        case INVERTED: return !kids[0].test(c);
        case ANY_OF: for (auto& k : kids) if (k.test(c)) return true; return false;
        case ALL_OF: for (auto& k : kids) if (!k.test(c)) return false; return true;
        case LOCATION: {
            int x = mth::floor(c.origin.x) + off[0], y = mth::floor(c.origin.y) + off[1], z = mth::floor(c.origin.z) + off[2];
            return loc->test(c.level, off[0] || off[1] || off[2] ? x : c.origin.x, off[0] || off[1] || off[2] ? y : c.origin.y,
                             off[0] || off[1] || off[2] ? z : c.origin.z);
        }
        case BLOCK_STATE: return c.state >= 0 && block.test(c.state);
        case MATCH_TOOL: return c.tool && tool.test(*c.tool);
        case RANDOM: return rand_float() < chance;
    }
    return false;
}
LCtx entity_ctx(Player& p, Entity* e) {
    LCtx c;
    c.self = e, c.level = p.level, c.origin = {p.x, p.y, p.z};
    return c;
}

// ---------- loading ----------
static const char* const TRIGGER_NAMES[T_COUNT] = {
    "impossible", "tick", "location", "slept_in_bed", "hero_of_the_village", "inventory_changed", "recipe_unlocked",
    "recipe_crafted", "player_killed_entity", "entity_killed_player", "changed_dimension", "enter_block", "placed_block",
    "item_used_on_block", "consume_item", "bred_animals", "tame_animal", "villager_trade", "brewed_potion",
    "enchanted_item", "effects_changed", "summoned_entity", "used_ender_eye", "construct_beacon", "levitation",
    "nether_travel", ""};

static Nbt json_to_nbt(const Json& j) {
    switch (j.type) {
        case Json::String: return Nbt::str(j.str);
        case Json::Bool: return Nbt::num(Nbt::Byte, j.b);
        case Json::Number: return j.num == std::floor(j.num) && std::abs(j.num) < 2e9 ? Nbt::num(Nbt::Int, int64_t(j.num)) : Nbt::real(Nbt::Double, j.num);
        case Json::Array: {
            Nbt l = Nbt::make_list();
            bool objects = false;
            for (auto& x : j.arr) objects |= x.isObject();
            for (auto& x : j.arr) {
                if (objects && x.isString()) { Nbt t = Nbt::compound(); t.set("text", Nbt::str(x.str)); l.list.push_back(t); }
                else l.list.push_back(json_to_nbt(x));
            }
            return l;
        }
        case Json::Object: {
            Nbt c = Nbt::compound();
            for (auto& [k, v] : j.obj) c.set(k, json_to_nbt(v));
            return c;
        }
        default: return Nbt::compound();
    }
}
static Nbt text_of(const Json& j) {
    if (j.isString()) { Nbt t = Nbt::compound(); t.set("text", Nbt::str(j.str)); return t; }
    return json_to_nbt(j);
}

static void compile_conditions(Criterion& c, const Json* cj) {
    if (!cj) return;
    for (auto& [k, v] : cj->obj) {
        if (k == "player") c.player = conds(&v);
        else if (k == "entity" || k == "villager" || k == "victims") c.entity = conds(&v);
        else if (k == "parent") c.parent = conds(&v);
        else if (k == "partner") c.partner = conds(&v);
        else if (k == "child") c.child = conds(&v);
        else if (k == "source") c.source = conds(&v);
        else if (k == "location") c.location = conds(&v);
        else if (k == "killing_blow") c.killing_blow = damage_pred(v);
        else if (k == "items") for (auto& x : v.arr) c.items.push_back(item_pred(x));
        else if (k == "item") c.item = item_pred(v);
        else if (k == "slots") c.occupied = irange(v.find("occupied")), c.full = irange(v.find("full")), c.empty = irange(v.find("empty"));
        else if (k == "levels") c.levels = irange(&v);
        else if (k == "level") c.level = irange(&v);
        else if (k == "duration") c.duration = irange(&v);
        else if (k == "distance") {
            if (c.trigger == T_USED_ENDER_EYE) c.range = drange(&v);
            else c.distance = *dist_pred(v);
        } else if (k == "start_position") c.start = *loc_pred(v);
        else if (k == "recipes" || k == "recipe_id")
            for (auto& n : holder_names("recipe", v)) c.recipes.push_back(find_recipe(n));
        else if (k == "ingredients") for (auto& x : v.arr) c.items.push_back(item_pred(x));
        else if (k == "from" || k == "to") {
            std::string d = ns(v.asString());
            (k == "from" ? c.from : c.to) = d == "minecraft:overworld" ? DIM_OVERWORLD : d == "minecraft:the_nether" ? DIM_NETHER : d == "minecraft:the_end" ? DIM_END : 99;
        } else if (k == "block" || k == "blocks") c.block = holder_set("block", v);
        else if (k == "state") c.state = loot_detail::states(&v);
        else if (k == "potion") c.potions = holder_set("potion", v);
        else if (k == "effects") c.effects = effects_pred(v);
        else c.bad = true, warn_once(std::string(TRIGGER_NAMES[c.trigger]) + " condition " + k);
    }
}

int Advancement::root() const {
    int a = int(this - advs.data());
    while (advs[size_t(a)].parent >= 0) a = advs[size_t(a)].parent;
    return a;
}
int find_adv(const std::string& id) {
    auto it = adv_by_id.find(ns(id));
    return it == adv_by_id.end() ? -1 : it->second;
}

// TreeNodePosition (Walker's tree layout) over the visible (display) nodes.
namespace layout {
struct Node {
    int adv;
    Node* parent;
    Node* prev;
    int child_index;
    std::vector<std::unique_ptr<Node>> kids;
    Node* ancestor = this;
    Node* thread = nullptr;
    int x;
    float y = -1, mod = 0, change = 0, shift = 0;
    Node(int a, Node* par, Node* pv, int ci, int depth) : adv(a), parent(par), prev(pv), child_index(ci), x(depth) {
        Node* previous = nullptr;
        for (int c : advs[size_t(a)].children) previous = add_child(c, previous);
    }
    Node* add_child(int a, Node* previous) {
        if (advs[size_t(a)].display) {
            kids.push_back(std::make_unique<Node>(a, this, previous, int(kids.size()) + 1, x + 1));
            previous = kids.back().get();
        } else {
            for (int g : advs[size_t(a)].children) previous = add_child(g, previous);
        }
        return previous;
    }
    void first_walk() {
        if (kids.empty()) {
            y = prev ? prev->y + 1.0f : 0.0f;
            return;
        }
        Node* def = nullptr;
        for (auto& c : kids) {
            c->first_walk();
            def = c->apportion(def ? def : c.get());
        }
        execute_shifts();
        float mid = (kids.front()->y + kids.back()->y) / 2.0f;
        if (prev) y = prev->y + 1.0f, mod = y - mid;
        else y = mid;
    }
    float second_walk(float mod_sum, int depth, float min) {
        y += mod_sum;
        x = depth;
        if (y < min) min = y;
        for (auto& c : kids) min = c->second_walk(mod_sum + mod, depth + 1, min);
        return min;
    }
    void third_walk(float off) {
        y += off;
        for (auto& c : kids) c->third_walk(off);
    }
    void execute_shifts() {
        float sh = 0, ch = 0;
        for (int i = int(kids.size()) - 1; i >= 0; i--) {
            Node* c = kids[size_t(i)].get();
            c->y += sh, c->mod += sh;
            ch += c->change;
            sh += c->shift + ch;
        }
    }
    Node* prev_or_thread() { return thread ? thread : !kids.empty() ? kids.front().get() : nullptr; }
    Node* next_or_thread() { return thread ? thread : !kids.empty() ? kids.back().get() : nullptr; }
    Node* apportion(Node* def) {
        if (!prev) return def;
        Node *vir = this, *vor = this, *vil = prev, *vol = parent->kids.front().get();
        float sir = mod, sor = mod, sil = vil->mod, sol = vol->mod;
        for (; vil->next_or_thread() && vir->prev_or_thread(); sor += vor->mod) {
            vil = vil->next_or_thread();
            vir = vir->prev_or_thread();
            vol = vol->prev_or_thread();
            vor = vor->next_or_thread();
            vor->ancestor = this;
            float s = vil->y + sil - (vir->y + sir) + 1.0f;
            if (s > 0.0f) {
                vil->get_ancestor(this, def)->move_subtree(this, s);
                sir += s, sor += s;
            }
            sil += vil->mod, sir += vir->mod, sol += vol->mod;
        }
        if (vil->next_or_thread() && !vor->next_or_thread()) {
            vor->thread = vil->next_or_thread();
            vor->mod += sil - sor;
        } else {
            if (vir->prev_or_thread() && !vol->prev_or_thread()) {
                vol->thread = vir->prev_or_thread();
                vol->mod += sir - sol;
            }
            def = this;
        }
        return def;
    }
    void move_subtree(Node* right, float s) {
        float subtrees = float(right->child_index - child_index);
        if (subtrees != 0.0f) right->change -= s / subtrees, change += s / subtrees;
        right->shift += s, right->y += s, right->mod += s;
    }
    Node* get_ancestor(Node* other, Node* def) {
        if (ancestor)
            for (auto& k : other->parent->kids) if (k.get() == ancestor) return ancestor;
        return def;
    }
    void finalize() {
        advs[size_t(adv)].x = float(x), advs[size_t(adv)].y = y;
        for (auto& c : kids) c->finalize();
    }
};
static void run(int root) {
    Node r(root, nullptr, nullptr, 1, 0);
    r.first_walk();
    float min = r.second_walk(0.0f, 0, r.y);
    if (min < 0.0f) r.third_walk(-min);
    r.finalize();
}
}  // namespace layout

static void load_advancements() {
    std::vector<std::string> ids;
    std::string base = VANILLA + "advancement";
    for (auto& e : std::filesystem::recursive_directory_iterator(base))
        if (e.path().extension() == ".json") {
            std::string rel = std::filesystem::relative(e.path(), base).generic_string();
            ids.push_back("minecraft:" + rel.substr(0, rel.size() - 5));
        }
    std::sort(ids.begin(), ids.end());
    std::vector<std::string> parents(ids.size());
    advs.resize(ids.size());
    for (size_t i = 0; i < ids.size(); i++) adv_by_id[ids[i]] = int(i);
    for (size_t i = 0; i < ids.size(); i++) {
        Advancement& a = advs[i];
        a.id = ids[i];
        Json j = Json::parseFile(base + "/" + ids[i].substr(10) + ".json");
        if (auto* p = j.find("parent")) parents[i] = ns(p->asString());
        if (auto* d = j.find("display")) {
            auto disp = std::make_unique<Display>();
            disp->title = text_of((*d)["title"]);
            disp->description = text_of((*d)["description"]);
            auto& icon = (*d)["icon"];
            disp->icon = reg_id("item", icon["id"].asString());
            disp->icon_count = icon.getInt("count", 1);
            if (auto* f = d->find("frame")) disp->frame = f->str == "challenge" ? 1 : f->str == "goal" ? 2 : 0;
            if (auto* x = d->find("show_toast")) disp->toast = x->b;
            if (auto* x = d->find("announce_to_chat")) disp->announce = x->b;
            if (auto* x = d->find("hidden")) disp->hidden = x->b;
            if (auto* x = d->find("background")) disp->background = ns(x->asString());
            a.display = std::move(disp);
        }
        if (auto* r = j.find("rewards")) {
            a.xp = r->getInt("experience", 0);
            if (auto* l = r->find("loot")) for (auto& n : holder_names("loot_table", *l)) a.loot.push_back(n);
            if (auto* rs = r->find("recipes"))
                for (auto& n : rs->arr) if (int k = find_recipe(n.asString()); k >= 0) a.reward_recipes.push_back(k);
            if (auto* f = r->find("function")) a.function = f->asString();
        }
        if (auto* t = j.find("sends_telemetry_event")) a.telemetry = t->b;
        a.first = int(criteria.size());
        std::map<std::string, int> local;
        for (auto& [name, cj] : j["criteria"].obj) {
            Criterion c;
            c.adv = uint16_t(i);
            c.name = name;
            std::string t = cj["trigger"].asString();
            t = t.rfind("minecraft:", 0) == 0 ? t.substr(10) : t;
            for (int k = 0; k < T_UNSUPPORTED; k++) if (t == TRIGGER_NAMES[k]) c.trigger = uint8_t(k);
            if (c.trigger == T_UNSUPPORTED) warn_once("trigger " + t);
            compile_conditions(c, cj.find("conditions"));
            local[name] = int(criteria.size());
            criteria.push_back(std::move(c));
        }
        a.count = int(criteria.size()) - a.first;
        if (auto* r = j.find("requirements")) {
            for (auto& set : r->arr) {
                std::vector<int> v;
                for (auto& n : set.arr) if (auto it = local.find(n.asString()); it != local.end()) v.push_back(it->second);
                a.req.push_back(std::move(v));
            }
        } else {
            for (int c = a.first; c < a.first + a.count; c++) a.req.push_back({c});  // AdvancementRequirements.allOf
        }
    }
    for (size_t i = 0; i < advs.size(); i++) {
        if (parents[i].empty()) continue;
        int p = find_adv(parents[i]);
        if (p < 0) { log_error("Couldn't load advancement %s: unknown parent %s", advs[i].id.c_str(), parents[i].c_str()); continue; }
        advs[i].parent = p;
        advs[size_t(p)].children.push_back(int(i));
    }
    for (size_t i = 0; i < criteria.size(); i++) by_trigger[criteria[i].trigger].push_back(int(i));
    for (size_t i = 0; i < advs.size(); i++)
        if (advs[i].parent < 0 && advs[i].display) layout::run(int(i));
}

Nbt adv_name(int a) {
    const Advancement& adv = advs[size_t(a)];
    if (!adv.display) return lit(adv.id);
    static const char* colors[] = {"green", "dark_purple", "green"};
    const char* color = colors[adv.display->frame];
    Nbt tooltip = adv.display->title;
    tooltip.set("color", Nbt::str(color));
    Nbt extra = Nbt::make_list();
    extra.list.push_back(lit("\n"));
    extra.list.push_back(adv.display->description);
    tooltip.set("extra", extra);
    Nbt title = adv.display->title;
    Nbt hover = Nbt::compound();
    hover.set("action", Nbt::str("show_text"));
    hover.set("value", tooltip);
    title.set("hover_event", hover);
    Nbt n = tr("chat.square_brackets", {title});
    n.set("color", Nbt::str(color));
    return n;
}

// ---------- background load ----------
static std::shared_future<void> loaded;
static std::atomic<bool> ready{false};
void wait_loaded() {
    if (ready.load(std::memory_order_acquire)) return;
    loaded.wait();
}

State& st(Player& p) {
    if (!p.prog) {
        wait_loaded();
        p.prog = std::make_shared<State>();
        State& s = *p.prog;
        s.obtained.assign(criteria.size(), 0);
        s.started.assign(advs.size(), 0);
        s.visible.assign(advs.size(), 0);
        s.known.assign(recipe_defs.size(), 0);
        s.highlight.assign(recipe_defs.size(), 0);
    }
    return *p.prog;
}

// ---------- PlayerAdvancements ----------
static bool crit_done(const State& s, int c) { return s.obtained[size_t(c)] != 0; }
bool is_done(const State& s, int a) {
    const Advancement& adv = advs[size_t(a)];
    if (adv.req.empty()) return false;
    for (auto& set : adv.req) {
        bool any = false;
        for (int c : set) any |= crit_done(s, c);
        if (!any) return false;
    }
    return true;
}
static bool has_progress(const State& s, int a) {
    for (int c = advs[size_t(a)].first; c < advs[size_t(a)].first + advs[size_t(a)].count; c++) if (crit_done(s, c)) return true;
    return false;
}

static void grant_rewards(Player& p, int a) {  // AdvancementRewards.grant
    const Advancement& adv = advs[size_t(a)];
    if (adv.xp) cont::give_experience(p, adv.xp);
    for (auto& name : adv.loot) {
        std::string n = name.rfind("minecraft:", 0) == 0 ? name.substr(10) : name;
        auto it = entity_loot_index.find(n);
        if (it == entity_loot_index.end()) { warn_once("reward loot table " + name); continue; }
        LootContext ctx;
        ctx.this_entity = player_entity(p);
        ctx.x = mth::floor(p.x), ctx.y = mth::floor(p.y), ctx.z = mth::floor(p.z);
        std::vector<ItemStack> out;
        roll_loot(loot_tables[size_t(it->second)], ctx, out);
        for (auto& s : out) {
            ItemStack left = s;
            inventory_add(p, left);
            if (!left.empty()) {
                ItemEntity& e = spawn_item(left, p.x, p.y + 1.62 - 0.3, p.z);
                e.pickup_delay = 0;
                e.owner_hi = p.uuid_hi, e.owner_lo = p.uuid_lo;
            }
        }
    }
    if (!adv.reward_recipes.empty()) add_recipes(p, adv.reward_recipes);
    if (!adv.function.empty()) warn_once("reward function " + adv.function);
}

static void mark_visibility(State& s, int a) { s.roots.insert(advs[size_t(a)].root()); }

bool award(Player& p, int a, int c) {
    State& s = st(p);
    bool was_done = is_done(s, a);
    s.started[size_t(a)] = 1;
    bool result = false;
    if (!crit_done(s, c)) {
        s.obtained[size_t(c)] = now_ms();
        s.changed.insert(a);
        result = true;
        if (!was_done && is_done(s, a)) {
            grant_rewards(p, a);
            const Advancement& adv = advs[size_t(a)];
            if (adv.display && adv.display->announce && rule("show_advancement_messages")) {
                static const char* frames[] = {"task", "challenge", "goal"};
                broadcast_text(tr(std::string("chat.type.advancement.") + frames[adv.display->frame], {player_name(p), adv_name(a)}));
            }
        }
    }
    if (!was_done && is_done(s, a)) mark_visibility(s, a);
    return result;
}
bool revoke(Player& p, int a, int c) {
    State& s = st(p);
    bool was_done = is_done(s, a);
    s.started[size_t(a)] = 1;
    bool result = false;
    if (crit_done(s, c)) {
        s.obtained[size_t(c)] = 0;
        s.changed.insert(a);
        result = true;
    }
    if (was_done && !is_done(s, a)) mark_visibility(s, a);
    return result;
}

// AdvancementVisibilityEvaluator: SHOW / HIDE / NO_CHANGE per node, two ancestors deep.
static bool eval_visibility(const State& s, int a, std::vector<int>& stack, std::vector<std::pair<int, bool>>& out) {
    const Advancement& adv = advs[size_t(a)];
    bool self_done = is_done(s, a);
    int rule = !adv.display ? 1 : self_done ? 0 : adv.display->hidden ? 1 : 2;  // 0 SHOW, 1 HIDE, 2 NO_CHANGE
    bool any_done = self_done;
    stack.push_back(rule);
    for (int c : adv.children) any_done |= eval_visibility(s, c, stack, out);
    bool vis = any_done;
    if (!vis)
        for (int i = 0; i <= 2; i++) {
            int r = stack[stack.size() - 1 - size_t(i)];
            if (r == 0) { vis = true; break; }
            if (r == 1) break;
        }
    stack.pop_back();
    out.emplace_back(a, vis);
    return any_done;
}

static void write_progress(Writer& w, const State& s, int a) {  // AdvancementProgress.STREAM_CODEC
    const Advancement& adv = advs[size_t(a)];
    std::vector<int> names;
    for (auto& set : adv.req) for (int c : set) if (std::find(names.begin(), names.end(), c) == names.end()) names.push_back(c);
    w.varint(int(names.size()));
    for (int c : names) {
        w.str(criteria[size_t(c)].name);
        int64_t t = s.obtained[size_t(c)];
        w.boolean(t != 0);
        if (t) w.i64(t);
    }
}
static void write_advancement(Writer& w, int a) {  // PositionedAdvancement: AdvancementHolder, x, y
    const Advancement& adv = advs[size_t(a)];
    w.str(adv.id);
    w.boolean(adv.parent >= 0);
    if (adv.parent >= 0) w.str(advs[size_t(adv.parent)].id);
    w.boolean(adv.display != nullptr);
    if (const Display* d = adv.display.get()) {
        write_nbt(w, d->title);
        write_nbt(w, d->description);
        w.varint(d->icon < 0 ? 0 : d->icon).varint(d->icon_count).varint(0).varint(0);  // ItemStackTemplate, no patch
        w.varint(d->frame);
        w.i32((d->background.empty() ? 0 : 1) | (d->toast ? 2 : 0) | (d->hidden ? 4 : 0));
        if (!d->background.empty()) w.str(d->background);
    }
    w.varint(int(adv.req.size()));
    for (auto& set : adv.req) {
        w.varint(int(set.size()));
        for (int c : set) w.str(criteria[size_t(c)].name);
    }
    w.boolean(adv.telemetry);
    w.f32(adv.x).f32(adv.y);
}

void flush(Player& p, bool show) {  // PlayerAdvancements.flushDirty
    State& s = st(p);
    if (!s.first_packet && s.roots.empty() && s.changed.empty()) return;
    std::vector<int> added, removed;
    std::vector<int> stack;
    std::vector<std::pair<int, bool>> out;
    for (int root : s.roots) {
        stack.assign(3, 2);
        out.clear();
        eval_visibility(s, root, stack, out);
        for (auto [a, vis] : out) {
            if (vis && !s.visible[size_t(a)]) {
                s.visible[size_t(a)] = 1;
                added.push_back(a);
                if (s.started[size_t(a)]) s.changed.insert(a);
            } else if (!vis && s.visible[size_t(a)]) {
                s.visible[size_t(a)] = 0;
                removed.push_back(a);
            }
        }
    }
    s.roots.clear();
    std::vector<int> progress;
    for (int a : s.changed) if (s.visible[size_t(a)]) progress.push_back(a);
    s.changed.clear();
    if (!progress.empty() || !added.empty() || !removed.empty()) {
        Writer w;
        w.varint(play_cb::update_advancements).boolean(s.first_packet);
        w.varint(int(added.size()));
        for (int a : added) write_advancement(w, a);
        w.varint(int(removed.size()));
        for (int a : removed) w.str(advs[size_t(a)].id);
        w.varint(int(progress.size()));
        for (int a : progress) {
            w.str(advs[size_t(a)].id);
            write_progress(w, s, a);
        }
        w.boolean(show);
        p.send(w);
    }
    s.first_packet = false;
}

// SimpleCriterionTrigger.trigger: listeners (criteria of unfinished advancements) whose player predicate and
// trigger-specific test pass, collected first and then awarded.
void trigger(Player& p, int t, const std::function<bool(const Criterion&)>& match) {
    State& s = st(p);
    if (!s.loaded) return;
    std::vector<int> hits;
    LCtx self;
    bool self_ready = false;
    for (int c : by_trigger[size_t(t)]) {
        const Criterion& cr = criteria[size_t(c)];
        if (cr.bad || crit_done(s, c) || is_done(s, cr.adv)) continue;
        if (!cr.player.empty()) {
            if (!self_ready) self = entity_ctx(p, player_entity(p)), self_ready = true;
            if (!test_all(cr.player, self)) continue;
        }
        if (match(cr)) hits.push_back(c);
    }
    for (int c : hits) award(p, criteria[size_t(c)].adv, c);
}

// ---------- persistence: players/advancements/<uuid>.json ----------
void load_advancements_file(Player& p) {
    State& s = st(p);
    std::string path = world_dir + "/players/advancements/" + uuid_str(p) + ".json";
    if (std::filesystem::is_regular_file(path)) {
        try {
            Json j = Json::parseFile(path);
            // PlayerAdvancements.Data.forEach: sorted by first progress date (we apply in file order; equivalent).
            for (auto& [id, prog] : j.obj) {
                if (id == "DataVersion") continue;
                int a = find_adv(id);
                if (a < 0) { log_warn("Ignored advancement '%s' in progress file %s - it doesn't exist anymore?", id.c_str(), path.c_str()); continue; }
                s.started[size_t(a)] = 1;
                if (auto* cr = prog.find("criteria"))
                    for (auto& [name, when] : cr->obj)
                        for (int c = advs[size_t(a)].first; c < advs[size_t(a)].first + advs[size_t(a)].count; c++)
                            if (criteria[size_t(c)].name == name) s.obtained[size_t(c)] = parse_time(when.asString());
                s.changed.insert(a);
                mark_visibility(s, a);
            }
        } catch (const std::exception& e) {
            log_error("Couldn't parse player advancements in %s: %s", path.c_str(), e.what());
        }
    }
    s.loaded = true;
    for (size_t a = 0; a < advs.size(); a++)  // checkForAutomaticTriggers: advancements without criteria
        if (advs[a].count == 0) { mark_visibility(s, int(a)); grant_rewards(p, int(a)); }
}
void save_advancements_file(const Player& p) {
    if (!p.prog || !p.prog->loaded) return;
    const State& s = *p.prog;
    std::string out = "{";
    bool first = true;
    for (size_t a = 0; a < advs.size(); a++) {
        if (!s.started[a] || !has_progress(s, int(a))) continue;
        out += first ? "\n  " : ",\n  ";
        first = false;
        out += json_quote(advs[a].id) + ": {\n    \"criteria\": {";
        bool fc = true;
        for (int c = advs[a].first; c < advs[a].first + advs[a].count; c++) {
            if (!crit_done(s, c)) continue;
            out += fc ? "\n      " : ",\n      ";
            fc = false;
            out += json_quote(criteria[size_t(c)].name) + ": " + json_quote(fmt_time(s.obtained[size_t(c)]));
        }
        out += "\n    },\n    \"done\": " + std::string(is_done(s, int(a)) ? "true" : "false") + "\n  }";
    }
    out += std::string(first ? "\n  " : ",\n  ") + "\"DataVersion\": " + std::to_string(DATA_VERSION) + "\n}";
    std::string dir = world_dir + "/players/advancements";
    std::filesystem::create_directories(dir);
    std::string path = dir + "/" + uuid_str(p) + ".json", tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << out;
        if (!f) { log_error("Couldn't save player advancements to %s", path.c_str()); return; }
    }
    std::filesystem::rename(tmp, path);
}

// ---------- per-tick (ServerPlayer.tick / doTick) ----------
void tick_player(Player& p) {
    State& s = st(p);
    if (!s.loaded) return;
    // InventoryMenu.broadcastChanges -> containerListener.slotChanged -> INVENTORY_CHANGED, per changed slot
    // (menu order: armor head..feet, main 9-35, hotbar, offhand).
    static const int order[] = {39, 38, 37, 36, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
                                28, 29, 30, 31, 32, 33, 34, 35, 0, 1, 2, 3, 4, 5, 6, 7, 8, 40};
    for (int i : order) {
        const ItemStack& cur = p.inv[size_t(i)];
        if (cur == s.last_inv[size_t(i)]) continue;
        s.last_inv[size_t(i)] = cur;
        int full = 0, empty = 0, occupied = 0;
        for (int k = 0; k < 41; k++) {
            const ItemStack& x = p.inv[size_t(k)];
            if (x.empty()) empty++;
            else occupied++, full += x.count >= max_stack(x);
        }
        trigger(p, T_INVENTORY_CHANGED, [&](const Criterion& c) {
            if (!c.full.test(full) || !c.empty.test(empty) || !c.occupied.test(occupied)) return false;
            if (c.items.empty()) return true;
            if (c.items.size() == 1) return !cur.empty() && c.items[0].test(cur);
            std::vector<const ItemPred*> left;
            for (auto& ip : c.items) left.push_back(&ip);
            for (int k = 0; k < 41 && !left.empty(); k++) {
                const ItemStack& x = p.inv[size_t(k)];
                if (!x.empty()) std::erase_if(left, [&](const ItemPred* ip) { return ip->test(x); });
            }
            return left.empty();
        });
    }
    if (p.g.dead) return;
    trigger(p, T_TICK, [](const Criterion&) { return true; });
    if (server_tick % 20 == 0) trigger(p, T_LOCATION, [](const Criterion&) { return true; });  // tickCount % 20
}

// ---------- /advancement ----------
static void register_command() {
    reg("advancement", [](Cmd& c) {
        bool grant = c.is("grant");
        std::string key = grant ? "grant" : "revoke";
        auto targets = c.players("targets");
        std::vector<int> list;
        int target = -1;
        if (c.has("advancement")) {
            target = find_adv(c.str("advancement"));
            if (target < 0) throw err("advancement.advancementNotFound", {c.str("advancement")});
        }
        if (c.is("everything")) {
            for (size_t a = 0; a < advs.size(); a++) list.push_back(int(a));
        } else {
            bool parents = c.is("until") || c.is("through"), children = c.is("from") || c.is("through");
            if (parents) {
                std::vector<int> up;
                for (int a = advs[size_t(target)].parent; a >= 0; a = advs[size_t(a)].parent) up.push_back(a);
                list.insert(list.end(), up.begin(), up.end());
            }
            list.push_back(target);
            if (children) {
                auto add = [&](auto& self, int a) -> void {
                    for (int ch : advs[size_t(a)].children) list.push_back(ch), self(self, ch);
                };
                add(add, target);
            }
        }
        // CommandResponseTracker over players: totalValue, only (non-zero) element
        int total = 0, nonzero = 0;
        PlayerPtr only_nz;
        if (c.has("criterion")) {
            std::string crit = c.str("criterion");
            int ci = -1;
            for (int k = advs[size_t(target)].first; k < advs[size_t(target)].first + advs[size_t(target)].count; k++)
                if (criteria[size_t(k)].name == crit) ci = k;
            if (ci < 0) throw err("commands.advancement.criterionNotFound", {adv_name(target), crit});
            for (auto& t : targets) {
                int v = grant ? award(*t, target, ci) : revoke(*t, target, ci);
                total += v;
                if (v) nonzero++, only_nz = nonzero == 1 ? t : nullptr;
            }
            if (total == 0) {
                if (targets.size() == 1) throw err("commands.advancement." + key + ".criterion.to.one.failure", {crit, adv_name(target), player_name(*targets[0])});
                throw err("commands.advancement." + key + ".criterion.to.many.failure", {crit, adv_name(target), int(targets.size())});
            }
            c.success(only_nz ? tr("commands.advancement." + key + ".criterion.to.one.success", {crit, adv_name(target), player_name(*only_nz)})
                              : tr("commands.advancement." + key + ".criterion.to.many.success", {crit, adv_name(target), nonzero}),
                      true);
            return total;
        }
        bool show = !c.is("everything") || !grant;  // grant everything: flushDirty(false) around it
        for (auto& t : targets) {
            if (!show) flush(*t, true);
            int v = 0;
            for (int a : list) {
                State& s = st(*t);
                bool did = false;
                if (grant) {
                    if (!is_done(s, a)) {
                        for (int k = advs[size_t(a)].first; k < advs[size_t(a)].first + advs[size_t(a)].count; k++)
                            if (!crit_done(s, k)) award(*t, a, k);
                        did = true;
                    }
                } else if (has_progress(s, a)) {
                    for (int k = advs[size_t(a)].first; k < advs[size_t(a)].first + advs[size_t(a)].count; k++)
                        if (crit_done(s, k)) revoke(*t, a, k);
                    did = true;
                }
                v += did;
            }
            if (!show) flush(*t, false);
            total += v;
            if (v) nonzero++, only_nz = nonzero == 1 ? t : nullptr;
        }
        bool one = list.size() == 1;
        if (total == 0) {
            if (one) {
                if (targets.size() == 1) throw err("commands.advancement." + key + ".one.to.one.failure", {adv_name(list[0]), player_name(*targets[0])});
                throw err("commands.advancement." + key + ".one.to.many.failure", {adv_name(list[0]), int(targets.size())});
            }
            if (targets.size() == 1) throw err("commands.advancement." + key + ".many.to.one.failure", {int(list.size()), player_name(*targets[0])});
            throw err("commands.advancement." + key + ".many.to.many.failure", {int(list.size()), int(targets.size())});
        }
        if (one)
            c.success(only_nz ? tr("commands.advancement." + key + ".one.to.one.success", {adv_name(list[0]), player_name(*only_nz)})
                              : tr("commands.advancement." + key + ".one.to.many.success", {adv_name(list[0]), nonzero}),
                      true);
        else
            c.success(only_nz ? tr("commands.advancement." + key + ".many.to.one.success", {int(list.size()), player_name(*only_nz)})
                              : tr("commands.advancement." + key + ".many.to.many.success", {int(list.size()), nonzero}),
                      true);
        return total;
    });
}

void advancements_init() {
    register_command();
    // ServerboundSeenAdvancementsPacket: OPENED_TAB selects the tab (setSelectedTab), CLOSED_SCREEN clears it.
    on(play_sb::seen_advancements, [](Player& p, Reader& in, int) {
        int action = in.varint();
        State& s = st(p);
        int tab = -1;
        if (action == 0) {
            int a = find_adv(in.str(32767));
            if (a >= 0 && advs[size_t(a)].parent < 0 && advs[size_t(a)].display) tab = a;
        }
        if (action == 0 && tab != s.tab) {
            s.tab = tab;
            Writer w;
            w.varint(play_cb::select_advancements_tab).boolean(tab >= 0);
            if (tab >= 0) w.str(advs[size_t(tab)].id);
            p.send(w);
        }
    });
}

// ---------- init: background load, hooks ----------
void init() {
    loaded = std::async(std::launch::async, [] {
                 try {
                     auto t0 = std::chrono::steady_clock::now();
                     load_recipes();
                     load_advancements();
                     ready.store(true, std::memory_order_release);
                     log_info("Loaded %zu recipes, %zu advancements (%zu criteria) in %.0f ms", recipe_defs.size(), advs.size(),
                              criteria.size(),
                              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                 } catch (const std::exception& e) {
                     log_error("progression load failed: %s", e.what());
                 }
             }).share();
    advancements_init();
    recipe_book_init();
    stats_init();
    on_player_join([](Player& p) {
        State& s = st(p);
        if (!s.loaded) load_advancements_file(p);  // not loaded with the player data (new player)
        send_recipe_book(p);
    });
    on_player_tick([](Player& p) {
        tick_player(p);
        flush(p, true);
    });
}

void load_player(Player& p, const Nbt& t) {
    State& s = st(p);
    if (const Nbt* rb = t.get("recipeBook")) {
        static const char* keys[4][2] = {{"isGuiOpen", "isFilteringCraftable"}, {"isFurnaceGuiOpen", "isFurnaceFilteringCraftable"},
                                         {"isBlastingFurnaceGuiOpen", "isBlastingFurnaceFilteringCraftable"},
                                         {"isSmokerGuiOpen", "isSmokerFilteringCraftable"}};
        for (int b = 0; b < 4; b++)
            for (int k = 0; k < 2; k++)
                if (const Nbt* v = rb->get(keys[b][k])) s.book[b][k] = v->i != 0;
        auto ids = [&](const char* key, std::vector<uint8_t>& out) {
            if (const Nbt* l = rb->get(key))
                for (auto& e : l->list) {
                    int r = find_recipe(e.s);
                    if (r < 0) log_error("Tried to load unrecognized recipe: %s removed now.", e.s.c_str());
                    else out[size_t(r)] = 1;
                }
        };
        ids("recipes", s.known);
        ids("toBeDisplayed", s.highlight);
    }
    if (const Nbt* e = t.get("entered_nether_pos"); e && e->list.size() == 3)
        s.entered_nether = Vec3{e->list[0].as_double(), e->list[1].as_double(), e->list[2].as_double()};
    load_advancements_file(p);
    load_stats_file(p);
}
void save_player(const Player& p, Nbt& t) {
    if (!p.prog) return;
    const State& s = *p.prog;
    Nbt rb = Nbt::compound();
    static const char* keys[4][2] = {{"isGuiOpen", "isFilteringCraftable"}, {"isFurnaceGuiOpen", "isFurnaceFilteringCraftable"},
                                     {"isBlastingFurnaceGuiOpen", "isBlastingFurnaceFilteringCraftable"},
                                     {"isSmokerGuiOpen", "isSmokerFilteringCraftable"}};
    for (int b = 0; b < 4; b++)
        for (int k = 0; k < 2; k++) rb.set(keys[b][k], Nbt::num(Nbt::Byte, s.book[b][k]));
    Nbt known = Nbt::make_list(), hl = Nbt::make_list();
    for (size_t r = 0; r < recipe_defs.size(); r++) {
        if (s.known[r]) known.list.push_back(Nbt::str(recipe_defs[r].id));
        if (s.highlight[r]) hl.list.push_back(Nbt::str(recipe_defs[r].id));
    }
    rb.set("recipes", known);
    rb.set("toBeDisplayed", hl);
    t.set("recipeBook", rb);
    if (s.entered_nether) {
        Nbt v = Nbt::make_list();
        for (double d : {s.entered_nether->x, s.entered_nether->y, s.entered_nether->z}) v.list.push_back(Nbt::real(Nbt::Double, d));
        t.set("entered_nether_pos", v);
    }
    save_advancements_file(p);
    save_stats_file(p);
}

// ---------- trigger entry points ----------
void player_killed_entity(Player& p, Entity& victim, const DamageSource& s) {
    // ServerPlayer.awardKillScore
    award_stat(p, KILLED, victim.type);
    award_stat(p, CUSTOM, custom_stat(victim.as_player() ? "player_kills" : "mob_kills"));
    LCtx ctx = entity_ctx(p, &victim);
    trigger(p, T_PLAYER_KILLED_ENTITY, [&](const Criterion& c) {
        return test_all(c.entity, ctx) && (!c.killing_blow || c.killing_blow->test(s, p.level, ctx.origin));
    });
}
void entity_killed_player(Player& p, Entity* killer, const DamageSource& s) {
    award_stat(p, CUSTOM, custom_stat("deaths"));
    State& st_ = st(p);
    for (uint32_t k : {uint32_t(CUSTOM) << 24 | uint32_t(custom_stat("time_since_death")),
                       uint32_t(CUSTOM) << 24 | uint32_t(custom_stat("time_since_rest"))}) {
        st_.stats[k] = 0;  // resetStat
        st_.dirty.insert(k);
    }
    if (!killer) return;
    award_stat(p, KILLED_BY, killer->type);
    LCtx ctx = entity_ctx(p, killer);
    trigger(p, T_ENTITY_KILLED_PLAYER, [&](const Criterion& c) {
        return test_all(c.entity, ctx) && (!c.killing_blow || c.killing_blow->test(s, p.level, ctx.origin));
    });
}
void changed_dimension(Player& p, int from, int to) {  // ServerPlayer.triggerDimensionChangeTriggers
    State& s = st(p);
    if (from == DIM_OVERWORLD && to == DIM_NETHER) s.entered_nether = Vec3{p.x, p.y, p.z};  // approximately the departure point
    trigger(p, T_CHANGED_DIMENSION, [&](const Criterion& c) { return (c.from < 0 || c.from == from) && (c.to < 0 || c.to == to); });
    if (from == DIM_NETHER && to == DIM_OVERWORLD && s.entered_nether) {
        Vec3 e = *s.entered_nether;
        trigger(p, T_NETHER_TRAVEL, [&](const Criterion& c) {
            if (c.start && !c.start->test(&worlds[DIM_OVERWORLD], e.x, e.y, e.z)) return false;
            return !c.distance || c.distance->test(e.x, e.y, e.z, p.x, p.y, p.z);
        });
    }
    if (to != DIM_NETHER) s.entered_nether.reset();
}
static void location_trigger(Player& p, int t, int x, int y, int z, const ItemStack& used) {  // ItemUsedOnLocationTrigger
    LCtx ctx;
    ctx.self = player_entity(p), ctx.level = p.level, ctx.origin = {x + 0.5, y + 0.5, z + 0.5};
    {
        InLevel in_level(p.level);
        ctx.state = world.get_block(x, y, z);
    }
    ctx.tool = &used;
    trigger(p, t, [&](const Criterion& c) { return test_all(c.location, ctx); });
}
void placed_block(Player& p, int x, int y, int z, const ItemStack& used) {
    award_stat(p, USED, used.item);
    location_trigger(p, T_PLACED_BLOCK, x, y, z, used);
}
void item_used_on_block(Player& p, int x, int y, int z, const ItemStack& used) { location_trigger(p, T_ITEM_USED_ON_BLOCK, x, y, z, used); }
void consume_item(Player& p, const ItemStack& s) {
    award_stat(p, USED, s.item);
    trigger(p, T_CONSUME_ITEM, [&](const Criterion& c) { return !c.item || c.item->test(s); });
}
void bred_animals(Player& p, Entity& parent, Entity& partner, Entity* child) {
    award_stat(p, CUSTOM, custom_stat("animals_bred"));
    LCtx a = entity_ctx(p, &parent), b = entity_ctx(p, &partner), k = entity_ctx(p, child);
    trigger(p, T_BRED_ANIMALS, [&](const Criterion& c) {
        if (!c.child.empty() && (!child || !test_all(c.child, k))) return false;
        return (test_all(c.parent, a) && test_all(c.partner, b)) || (test_all(c.parent, b) && test_all(c.partner, a));
    });
}
void tame_animal(Player& p, Entity& animal) {
    LCtx ctx = entity_ctx(p, &animal);
    trigger(p, T_TAME_ANIMAL, [&](const Criterion& c) { return test_all(c.entity, ctx); });
}
void villager_trade(Player& p, Entity& villager, const ItemStack& result) {
    award_stat(p, CUSTOM, custom_stat("traded_with_villager"));
    LCtx ctx = entity_ctx(p, &villager);
    trigger(p, T_VILLAGER_TRADE, [&](const Criterion& c) { return test_all(c.entity, ctx) && (!c.item || c.item->test(result)); });
}
void brewed_potion(Player& p, int potion) {
    trigger(p, T_BREWED_POTION, [&](const Criterion& c) { return !c.potions || has(c.potions, potion); });
}
void enchanted_item(Player& p, const ItemStack& s, int levels) {
    award_stat(p, CUSTOM, custom_stat("enchant_item"));
    trigger(p, T_ENCHANTED_ITEM, [&](const Criterion& c) { return (!c.item || c.item->test(s)) && c.levels.test(levels); });
}
void effects_changed(Player& p, Entity* source) {
    LCtx ctx = entity_ctx(p, source);
    Entity* self = player_entity(p);
    trigger(p, T_EFFECTS_CHANGED, [&](const Criterion& c) {
        if (!c.effects.empty() && !effects_test(c.effects, self)) return false;
        return c.source.empty() || (source && test_all(c.source, ctx));
    });
}
void summoned_entity(Player& p, Entity& e) {
    LCtx ctx = entity_ctx(p, &e);
    trigger(p, T_SUMMONED_ENTITY, [&](const Criterion& c) { return test_all(c.entity, ctx); });
}
void slept_in_bed(Player& p) { trigger(p, T_SLEPT_IN_BED, [](const Criterion&) { return true; }); }
void hero_of_the_village(Player& p) { trigger(p, T_HERO_OF_THE_VILLAGE, [](const Criterion&) { return true; }); }
void used_ender_eye(Player& p, int x, int, int z) {
    double dx = p.x - x, dz = p.z - z;  // UsedEnderEyeTrigger: horizontal distance squared
    trigger(p, T_USED_ENDER_EYE, [&](const Criterion& c) { return !c.range || c.range->test_sqr(dx * dx + dz * dz); });
}
void construct_beacon(Player& p, int levels) {
    trigger(p, T_CONSTRUCT_BEACON, [&](const Criterion& c) { return c.level.test(levels); });
}
void levitation(Player& p, double sx, double sy, double sz, int ticks) {
    trigger(p, T_LEVITATION, [&](const Criterion& c) {
        return (!c.distance || c.distance->test(sx, sy, sz, p.x, p.y, p.z)) && c.duration.test(ticks);
    });
}
void enter_block(Player& p, int state) {
    trigger(p, T_ENTER_BLOCK, [&](const Criterion& c) {
        return (!c.block || has(c.block, block_index(state))) && state_matches(state, c.state);
    });
}
}  // namespace progress
