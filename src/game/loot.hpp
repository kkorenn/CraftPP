#pragma once
// Loot tables (net.minecraft.world.level.storage.loot): compiled once from data/game.json into plain structs.
// Supported: pools/rolls/bonus_rolls, entries item/tag/alternatives/group/sequence/empty, conditions
// survives_explosion, match_tool, table_bonus, random_chance, match_block, location_check, inverted, any_of,
// all_of; functions set_count, apply_bonus, explosion_decay, limit_count (copy_components/copy_state: no-op,
// stacks carry no components). Enchantment levels are always 0 until enchantments exist.
#include "entity.hpp"

// LootContext.getRandom: a seeded RandomSource while one is set (container loot: RandomSource.create(seed)),
// else the game random. Tick thread only.
inline RandomSource* loot_rng = nullptr;
inline float loot_float() { return loot_rng ? loot_rng->nextFloat() : rand_float(); }
inline int loot_int(int bound) { return bound <= 0 ? 0 : loot_rng ? loot_rng->nextInt(bound) : rand_int(bound); }
struct LootRngScope {
    RandomSource* prev;
    explicit LootRngScope(RandomSource* r) : prev(loot_rng) { loot_rng = r; }
    ~LootRngScope() { loot_rng = prev; }
};

struct LootNumber {  // NumberProvider: constant / uniform / binomial
    enum T : uint8_t { CONST, UNIFORM, BINOMIAL } t = CONST;
    float a = 1, b = 0;
    float get_float() const {
        if (t == UNIFORM) return a >= b ? a : loot_float() * (b - a) + a;  // Mth.nextFloat
        return t == CONST ? a : float(get_int());
    }
    int get_int() const {
        if (t == CONST) return int(std::lround(a));
        if (t == UNIFORM) {  // Mth.nextInt
            int lo = int(std::lround(a)), hi = int(std::lround(b));
            return lo >= hi ? lo : lo + loot_int(hi - lo + 1);
        }
        int n = 0;
        for (int i = 0; i < int(a); i++) n += loot_float() < b;
        return n;
    }
};

struct LootContext {
    int state = 0, x = 0, y = 0, z = 0;
    ItemStack tool;
    float explosion_radius = 0;  // 0: not an explosion
    // entity tables (LootContextParamSets.ENTITY / SHEARING)
    Entity* this_entity = nullptr;
    Entity* attacker = nullptr;         // ATTACKING_ENTITY (damage source causing entity)
    Entity* direct = nullptr;           // DIRECT_ATTACKING_ENTITY
    Entity* last_player = nullptr;      // LAST_DAMAGE_PLAYER (only when killed by a player)
    const DamageSource* source = nullptr;
    bool origin = false;  // ORIGIN is (x, y, z) + 0.5 (container loot): location_check uses it
};
// entity_properties predicate check, pre-compiled by tools/gen_mob_data.py
struct EntityCheck {
    enum K : uint8_t { FLAG, TYPE, VEHICLE, PROP } k = FLAG;
    std::string key, value;
    bool want = true;
    std::vector<uint16_t> ids;
    std::vector<EntityCheck> sub;
};

struct StateMatch {  // match_block / location_check "state": property -> exact value or int range
    int prop = -1;  // index into the block's props, resolved per state at test time
    std::string name, value;
    bool range = false;
    int lo = INT32_MIN, hi = INT32_MAX;
};

struct LootCond {
    enum T : uint8_t { TRUE_, FALSE_, SURVIVES_EXPLOSION, MATCH_TOOL, TABLE_BONUS, RANDOM_CHANCE, MATCH_BLOCK,
                       LOCATION, INVERTED, ANY_OF, ALL_OF, KILLED_BY_PLAYER, ENTITY_PROPS, DAMAGE_SOURCE, BIOME } t = TRUE_;
    uint8_t which = 0;  // ENTITY_PROPS: 0 this, 1 attacker, 2 direct_attacker, 3 attacking_player
    std::vector<EntityCheck> checks, checks2;  // ENTITY_PROPS; DAMAGE_SOURCE: source_entity, direct_entity
    std::vector<std::pair<std::vector<uint16_t>, bool>> dtags;  // DAMAGE_SOURCE: damage type tags
    int is_direct = -1;
    std::vector<uint16_t> ids;  // items (match_tool) or blocks (match_block/location)
    bool any_id = true;
    std::vector<float> chances;
    LootNumber chance;
    std::vector<StateMatch> state;
    int off[3] = {0, 0, 0};
    std::vector<LootCond> kids;
};

struct LootFunc {
    enum T : uint8_t { NOP, SET_COUNT, ORE_DROPS, UNIFORM_BONUS, BINOMIAL_BONUS, EXPLOSION_DECAY, LIMIT, SMELT, COPY_BE, EXT } t = NOP;
    int ext = -1;  // EXT: index in the extension module's table (loot_ext_compile / loot_ext_apply)
    std::vector<std::string> include;  // COPY_BE: component names
    LootNumber n;
    bool add = false;
    float p1 = 0, p2 = 0;
    int lo = INT32_MIN, hi = INT32_MAX;
    std::vector<LootCond> conds;
};

struct LootEntry {
    enum T : uint8_t { ITEM, TAG, ALTERNATIVES, GROUP, SEQUENCE, EMPTY, TABLE } t = EMPTY;
    int item = -1, weight = 1;
    std::string table;           // TABLE: referenced table name ("shearing/sheep/white"), resolved lazily
    std::vector<uint16_t> items;  // TAG: items of the tag (expand=false: one random... ponytail: first item)
    std::vector<LootCond> conds;
    std::vector<LootFunc> funcs;
    std::vector<LootEntry> kids;
};

struct LootPool {
    LootNumber rolls, bonus;
    std::vector<LootCond> conds;
    std::vector<LootFunc> funcs;
    std::vector<LootEntry> entries;
};
struct LootTable {
    std::vector<LootPool> pools;
    std::vector<LootFunc> funcs;
};
inline std::vector<LootTable> loot_tables;
// Extension functions (src/containers/chest_loot.hpp): compile returns true and sets f.t = EXT, f.ext.
inline bool (*loot_ext_compile)(const std::string& type, const Json& j, LootFunc& f) = nullptr;
inline void (*loot_ext_apply)(const LootFunc& f, ItemStack& s, const struct LootContext& ctx) = nullptr;
inline int (*loot_biome_at)(int x, int y, int z) = nullptr;
inline int (*loot_biome_id)(const std::string& name) = nullptr;

// ---------- compile ----------
namespace loot_detail {
inline void warn(const std::string& what) {
    static std::map<std::string, bool> seen;
    if (!seen[what]) { seen[what] = true; log_warn("loot: unsupported %s (ignored)", what.c_str()); }
}
inline std::string strip(const std::string& s) { return s.rfind("minecraft:", 0) == 0 ? s.substr(10) : s; }

inline LootNumber number(const Json& j) {
    LootNumber n;
    if (j.isNumber()) { n.a = j.asFloat(); return n; }
    std::string t = strip(j.find("type") ? j["type"].asString() : "uniform");
    if (t == "constant") n.a = j["value"].asFloat();
    else if (t == "uniform") n.t = LootNumber::UNIFORM, n.a = number(j["min"]).a, n.b = number(j["max"]).a;
    else if (t == "binomial") n.t = LootNumber::BINOMIAL, n.a = number(j["n"]).a, n.b = number(j["p"]).a;
    else warn("number provider " + t);
    return n;
}
inline std::vector<StateMatch> states(const Json* j) {
    std::vector<StateMatch> v;
    if (!j) return v;
    for (auto& [k, val] : j->obj) {
        StateMatch m;
        m.name = k;
        if (val.isString()) m.value = val.asString();
        else if (val.isNumber()) m.value = val.str;
        else {
            m.range = true;
            if (auto* lo = val.find("min")) m.lo = lo->isString() ? std::stoi(lo->asString()) : lo->asInt();
            if (auto* hi = val.find("max")) m.hi = hi->isString() ? std::stoi(hi->asString()) : hi->asInt();
        }
        v.push_back(std::move(m));
    }
    return v;
}
inline std::vector<EntityCheck> entity_checks(const Json& a) {
    std::vector<EntityCheck> v;
    for (auto& c : a.arr) {
        EntityCheck e;
        std::string k = c[0].asString();
        if (k == "flag") e.k = EntityCheck::FLAG, e.key = c[1].asString(), e.want = c[2].asBool();
        else if (k == "type") {
            e.k = EntityCheck::TYPE;
            for (auto& i : c[1].arr) e.ids.push_back(uint16_t(i.asInt()));
        } else if (k == "vehicle") e.k = EntityCheck::VEHICLE, e.sub = entity_checks(c[1]);
        else e.k = EntityCheck::PROP, e.key = c[1].asString(), e.value = c[2].asString();
        v.push_back(std::move(e));
    }
    return v;
}
inline LootCond cond(const Json& j) {
    LootCond c;
    std::string t = strip(j["type"].asString());
    auto ids = [&](const Json* a) {
        if (a && a->isArray()) {
            c.any_id = false;
            for (auto& x : a->arr) c.ids.push_back(uint16_t(x.asInt()));
        }
    };
    if (t == "survives_explosion") c.t = LootCond::SURVIVES_EXPLOSION;
    else if (t == "match_tool") {
        c.t = j["unsupported"].asBool() ? LootCond::FALSE_ : LootCond::MATCH_TOOL;  // enchantment predicates: no enchantments yet
        ids(j.find("items"));
    } else if (t == "table_bonus") {
        c.t = LootCond::TABLE_BONUS;
        for (auto& x : j["chances"].arr) c.chances.push_back(x.asFloat());
    } else if (t == "random_chance") {
        c.t = LootCond::RANDOM_CHANCE;
        c.chance = number(j["chance"]);
    } else if (t == "match_block" || t == "location_check") {
        c.t = t == "match_block" ? LootCond::MATCH_BLOCK : LootCond::LOCATION;
        if (t == "location_check" && j.find("predicate")) {  // raw datapack form: biomes only
            const Json& pr = j["predicate"];
            const Json* bi = pr.find("biomes");
            if (!bi || pr.obj.size() != 1 || !loot_biome_id) {
                warn("location_check predicate");
                c.t = LootCond::FALSE_;
                return c;
            }
            c.t = LootCond::BIOME;
            std::vector<std::string> names;
            if (bi->isArray()) for (auto& v : bi->arr) names.push_back(v.asString());
            else names.push_back(bi->asString());
            for (auto& n : names) {
                int id = loot_biome_id(n);
                if (id < 0) warn("biome " + n);  // tags are not resolved here
                else c.ids.push_back(uint16_t(id));
            }
            std::sort(c.ids.begin(), c.ids.end());
            return c;
        }
        if (t == "location_check") {
            if (j["unsupported"].asBool()) c.t = LootCond::FALSE_;
            for (int i = 0; i < 3; i++) c.off[i] = j["off"][size_t(i)].asInt();
        }
        ids(j.find("blocks"));
        c.state = states(j.find("state"));
    } else if (t == "inverted") {
        c.t = LootCond::INVERTED;
        c.kids.push_back(cond(j["term"]));
    } else if (t == "any_of" || t == "all_of") {
        c.t = t == "any_of" ? LootCond::ANY_OF : LootCond::ALL_OF;
        for (auto& k : j["terms"].arr) c.kids.push_back(cond(k));
    } else if (t == "killed_by_player") {
        c.t = LootCond::KILLED_BY_PLAYER;
    } else if (t == "random_chance_with_enchanted_bonus") {  // no enchantments yet: the unenchanted chance
        c.t = LootCond::RANDOM_CHANCE;
        c.chance.a = j["unenchanted_chance"].asFloat();
    } else if (t == "entity_properties") {
        c.t = !j.find("checks") || j["unsupported"].asBool() ? LootCond::FALSE_ : LootCond::ENTITY_PROPS;  // raw = block tables
        if (c.t == LootCond::FALSE_) return c;
        std::string w = j["entity"].asString();
        c.which = w == "attacker" ? 1 : w == "direct_attacker" ? 2 : w == "attacking_player" ? 3 : 0;
        c.checks = entity_checks(j["checks"]);
    } else if (t == "damage_source_properties") {
        c.t = !j.find("unsupported") || j["unsupported"].asBool() ? LootCond::FALSE_ : LootCond::DAMAGE_SOURCE;
        if (auto* se = j.find("source_entity")) c.checks = entity_checks(*se);
        if (auto* de = j.find("direct_entity")) c.checks2 = entity_checks(*de);
        if (auto* tg = j.find("tags"))
            for (auto& x : tg->arr) {
                std::vector<uint16_t> ids;
                for (auto& i : x[0].arr) ids.push_back(uint16_t(i.asInt()));
                c.dtags.emplace_back(std::move(ids), x[1].asBool());
            }
        if (auto* d = j.find("is_direct")) c.is_direct = d->asBool();
    } else {
        warn("condition " + t);
        c.t = LootCond::FALSE_;
    }
    return c;
}
inline std::vector<LootCond> conds(const Json& j) {
    std::vector<LootCond> v;
    if (auto* c = j.find("condition")) {
        if (c->isArray()) for (auto& x : c->arr) v.push_back(cond(x));
        else v.push_back(cond(*c));
    }
    return v;
}
inline LootFunc func(const Json& j) {
    LootFunc f;
    std::string t = strip(j["type"].asString());
    f.conds = conds(j);
    if (t == "set_count") {
        f.t = LootFunc::SET_COUNT;
        f.n = number(j["count"]);
        f.add = j.find("add") && j["add"].asBool();
    } else if (t == "apply_bonus") {
        std::string formula = strip(j["formula"].asString());
        const Json* p = j.find("parameters");
        if (formula == "ore_drops") f.t = LootFunc::ORE_DROPS;
        else if (formula == "uniform_bonus_count") f.t = LootFunc::UNIFORM_BONUS, f.p1 = float((*p)["bonusMultiplier"].asInt());
        else if (formula == "binomial_with_bonus_count")
            f.t = LootFunc::BINOMIAL_BONUS, f.p1 = float((*p)["extra"].asInt()), f.p2 = (*p)["probability"].asFloat();
        else warn("apply_bonus formula " + formula);
    } else if (t == "explosion_decay") {
        f.t = LootFunc::EXPLOSION_DECAY;
    } else if (t == "limit_count") {
        f.t = LootFunc::LIMIT;
        auto& l = j["limit"];
        if (auto* lo = l.find("min")) f.lo = int(number(*lo).a);
        if (auto* hi = l.find("max")) f.hi = int(number(*hi).a);
    } else if (t == "copy_components" && j["source"].asString() == "block_entity" && j.find("include")) {
        f.t = LootFunc::COPY_BE;
        for (auto& c : j["include"].arr) f.include.push_back(strip(c.asString()));
    } else if (t == "furnace_smelt") {
        f.t = LootFunc::SMELT;
    } else if (loot_ext_compile && loot_ext_compile(t, j, f)) {
    } else if (t != "copy_components" && t != "copy_state" && t != "enchanted_count_increase" && t != "set_potion" &&
               t != "set_damage" && t != "enchant_with_levels" && t != "enchant_randomly" && t != "set_ominous_bottle_amplifier") {
        warn("function " + t);  // the listed ones need enchantments / components: no-ops until those exist
    }
    return f;
}
inline std::vector<LootFunc> funcs(const Json& j) {
    std::vector<LootFunc> v;
    if (auto* m = j.find("modifier")) {
        if (m->isArray()) for (auto& x : m->arr) v.push_back(func(x));
        else v.push_back(func(*m));
    }
    return v;
}
inline LootEntry entry(const Json& j) {
    LootEntry e;
    std::string t = strip(j["type"].asString());
    e.conds = conds(j);
    e.funcs = funcs(j);
    e.weight = j.getInt("weight", 1);
    if (t == "item") {
        e.t = LootEntry::ITEM;
        auto it = item_by_name.find(j["name"].asString());
        if (it == item_by_name.end()) e.t = LootEntry::EMPTY;
        else e.item = it->second;
    } else if (t == "alternatives" || t == "group" || t == "sequence") {
        e.t = t == "alternatives" ? LootEntry::ALTERNATIVES : t == "group" ? LootEntry::GROUP : LootEntry::SEQUENCE;
        for (auto& k : j["children"].arr) e.kids.push_back(entry(k));
    } else if (t == "loot_table" && j["value"].isString()) {
        e.t = LootEntry::TABLE;
        e.table = strip(j["value"].asString());
    } else if (t == "tag" && j.find("items")) {
        e.t = LootEntry::TAG;
        for (auto& x : j["items"].arr) e.items.push_back(uint16_t(x.asInt()));
    } else if (t != "empty") {
        warn("entry " + t);  // dynamic (block entity contents), inline loot_table
    }
    return e;
}
}  // namespace loot_detail

inline LootTable compile_table(const Json& j) {
    using namespace loot_detail;
    LootTable t;
    t.funcs = funcs(j);
    if (auto* pools = j.find("pools"))
        for (auto& pj : pools->arr) {
            LootPool p;
            p.rolls = number(pj["rolls"]);
            p.bonus.a = 0;
            if (auto* b = pj.find("bonus_rolls")) p.bonus = number(*b);
            p.conds = conds(pj);
            p.funcs = funcs(pj);
            for (auto& e : pj["entries"].arr) p.entries.push_back(entry(e));
            t.pools.push_back(std::move(p));
        }
    return t;
}
// Entity / shearing / gameplay tables from data/mobs.json, by name ("entities/cow").
inline void load_entity_loot(const Json& loot) {
    for (auto& [name, j] : loot.obj) {
        entity_loot_index[name] = int(loot_tables.size());
        loot_tables.push_back(compile_table(j));
    }
}
inline void load_loot_tables(const Json& loot) {
    using namespace loot_detail;
    for (auto& [name, j] : loot.obj) {
        LootTable t;
        t.funcs = funcs(j);
        if (auto* pools = j.find("pools"))
            for (auto& pj : pools->arr) {
                LootPool p;
                p.rolls = number(pj["rolls"]);
                p.bonus.a = 0;
                if (auto* b = pj.find("bonus_rolls")) p.bonus = number(*b);
                p.conds = conds(pj);
                p.funcs = funcs(pj);
                for (auto& e : pj["entries"].arr) p.entries.push_back(entry(e));
                t.pools.push_back(std::move(p));
            }
        auto b = block_by_name.find("minecraft:" + name);
        if (b == block_by_name.end()) continue;
        block_info[size_t(b->second)].loot = int(loot_tables.size());
        loot_tables.push_back(std::move(t));
    }
}

// ---------- evaluate ----------
inline bool state_matches(int state, const std::vector<StateMatch>& sm) {
    for (auto& m : sm) {
        std::string v = state_prop_str(state, m.name);
        if (v.empty()) return false;
        if (m.range) {
            int iv = std::atoi(v.c_str());
            if (iv < m.lo || iv > m.hi) return false;
        } else if (v != m.value) {
            return false;
        }
    }
    return true;
}

inline bool check_entity(const std::vector<EntityCheck>& cs, const Entity* e) {
    if (!e) return false;
    for (auto& c : cs) {
        switch (c.k) {
            case EntityCheck::FLAG: if (e->loot_flag(c.key) != c.want) return false; break;
            case EntityCheck::TYPE: if (!in_sorted(c.ids, e->type)) return false; break;
            case EntityCheck::VEHICLE: return false;  // ponytail: no riding yet, so no vehicle ever matches
            case EntityCheck::PROP: if (e->loot_prop(c.key) != c.value) return false; break;
        }
    }
    return true;
}
inline bool test(const LootCond& c, const LootContext& ctx) {
    switch (c.t) {
        case LootCond::KILLED_BY_PLAYER: return ctx.last_player != nullptr;
        case LootCond::ENTITY_PROPS: {
            Entity* e = c.which == 0 ? ctx.this_entity : c.which == 1 ? ctx.attacker : c.which == 2 ? ctx.direct : ctx.last_player;
            return check_entity(c.checks, e);
        }
        case LootCond::DAMAGE_SOURCE: {
            if (!ctx.source) return false;
            if (!c.checks.empty() && !check_entity(c.checks, ctx.source->causing)) return false;
            if (!c.checks2.empty() && !check_entity(c.checks2, ctx.source->direct)) return false;
            for (auto& [ids, want] : c.dtags) if (in_sorted(ids, ctx.source->type) != want) return false;
            if (c.is_direct >= 0 && (ctx.source->causing == ctx.source->direct) != bool(c.is_direct)) return false;
            return true;
        }
        case LootCond::TRUE_: return true;
        case LootCond::FALSE_: return false;
        case LootCond::SURVIVES_EXPLOSION:
            return ctx.explosion_radius <= 0 || loot_float() <= 1.0f / ctx.explosion_radius;
        case LootCond::BIOME: return loot_biome_at && in_sorted(c.ids, loot_biome_at(ctx.x, ctx.y, ctx.z));
        case LootCond::MATCH_TOOL:
            return !ctx.tool.empty() && (c.any_id || in_sorted(c.ids, ctx.tool.item));
        case LootCond::TABLE_BONUS: return !c.chances.empty() && loot_float() < c.chances[0];  // enchantment level 0
        case LootCond::RANDOM_CHANCE: {
            float p = c.chance.get_float();
            return loot_float() < p;
        }
        case LootCond::MATCH_BLOCK:
            return (c.any_id || in_sorted(c.ids, block_index(ctx.state))) && state_matches(ctx.state, c.state);
        case LootCond::LOCATION: {
            int s = world.get_block(ctx.x + c.off[0], ctx.y + c.off[1], ctx.z + c.off[2]);
            return (c.any_id || in_sorted(c.ids, block_index(s))) && state_matches(s, c.state);
        }
        case LootCond::INVERTED: return !test(c.kids[0], ctx);
        case LootCond::ANY_OF:
            for (auto& k : c.kids) if (test(k, ctx)) return true;
            return false;
        case LootCond::ALL_OF:
            for (auto& k : c.kids) if (!test(k, ctx)) return false;
            return true;
    }
    return false;
}
inline bool all(const std::vector<LootCond>& cs, const LootContext& ctx) {
    for (auto& c : cs) if (!test(c, ctx)) return false;
    return true;
}

// copy_components from the (just removed) block entity at a position (src/containers).
inline void (*loot_copy_block_entity)(int x, int y, int z, const std::vector<std::string>& include, ItemStack& s) = nullptr;
inline void apply(const std::vector<LootFunc>& fs, ItemStack& s, const LootContext& ctx) {
    const int level = 0;  // fortune: no enchantments yet
    for (auto& f : fs) {
        if (!all(f.conds, ctx)) continue;
        switch (f.t) {
            case LootFunc::NOP: break;
            case LootFunc::SET_COUNT: s.count = (f.add ? s.count : 0) + f.n.get_int(); break;
            case LootFunc::ORE_DROPS:
                if (level > 0) s.count *= std::max(loot_int(level + 2) - 1, 0) + 1;
                break;
            case LootFunc::UNIFORM_BONUS: s.count += loot_int(int(f.p1) * level + 1); break;
            case LootFunc::BINOMIAL_BONUS:
                for (int i = 0; i < level + int(f.p1); i++) s.count += loot_float() < f.p2;
                break;
            case LootFunc::EXPLOSION_DECAY:
                if (ctx.explosion_radius > 0) {
                    float p = 1.0f / ctx.explosion_radius;
                    int n = 0;
                    for (int i = 0; i < s.count; i++) n += loot_float() <= p;
                    s.count = n;
                }
                break;
            case LootFunc::LIMIT: s.count = std::clamp(s.count, f.lo, f.hi); break;
            case LootFunc::COPY_BE:
                if (loot_copy_block_entity) loot_copy_block_entity(ctx.x, ctx.y, ctx.z, f.include, s);
                break;
            case LootFunc::EXT:
                if (loot_ext_apply) loot_ext_apply(f, s, ctx);
                break;
            case LootFunc::SMELT:
                if (auto it = smelt_result.find(s.item); it != smelt_result.end()) s.item = it->second;
                break;
        }
    }
}

// LootPoolEntryContainer.expand: adds runnable singleton entries; returns whether this entry "succeeded".
inline bool expand(const LootEntry& e, const LootContext& ctx, std::vector<const LootEntry*>& out) {
    if (!all(e.conds, ctx)) return false;
    switch (e.t) {
        case LootEntry::ITEM: case LootEntry::TAG: case LootEntry::EMPTY: case LootEntry::TABLE: out.push_back(&e); return true;
        case LootEntry::ALTERNATIVES:
            for (auto& k : e.kids) if (expand(k, ctx, out)) return true;
            return false;
        case LootEntry::SEQUENCE:
            for (auto& k : e.kids) if (!expand(k, ctx, out)) return false;
            return true;
        case LootEntry::GROUP:
            for (auto& k : e.kids) expand(k, ctx, out);
            return true;
    }
    return false;
}

// LootTable.getRandomItems; `out` receives stacks (may exceed max stack size, pop_resource splits).
inline void roll_loot(const LootTable& t, const LootContext& ctx, std::vector<ItemStack>& out);
inline void roll_loot(const LootTable& t, const LootContext& ctx, std::vector<ItemStack>& out) {
    std::vector<const LootEntry*> cand;
    for (auto& p : t.pools) {
        if (!all(p.conds, ctx)) continue;
        int rolls = p.rolls.get_int();
        rolls += int(std::floor(p.bonus.get_float() * 0));  // luck 0
        for (int r = 0; r < rolls; r++) {
            cand.clear();
            for (auto& e : p.entries) expand(e, ctx, cand);
            std::erase_if(cand, [](const LootEntry* c) { return c->weight <= 0; });  // getWeight(luck) > 0
            const LootEntry* pick = nullptr;
            if (cand.size() == 1) pick = cand[0];
            else if (!cand.empty()) {
                int total = 0;
                for (auto* c : cand) total += c->weight;
                int v = loot_int(total);
                for (auto* c : cand)
                    if ((v -= c->weight) < 0) { pick = c; break; }
            }
            if (pick && pick->t == LootEntry::TABLE) {  // LootTableReference: roll the referenced table
                auto it = entity_loot_index.find(pick->table);
                if (it != entity_loot_index.end()) roll_loot(loot_tables[size_t(it->second)], ctx, out);
                continue;
            }
            if (pick && pick->t == LootEntry::TAG && !pick->items.empty()) {  // expand=false: every tag item
                for (uint16_t it : pick->items) out.push_back({it, 1, 0});
                continue;
            }
            if (!pick || pick->t != LootEntry::ITEM) continue;
            ItemStack s{pick->item, 1, 0};
            apply(pick->funcs, s, ctx);
            apply(p.funcs, s, ctx);
            apply(t.funcs, s, ctx);
            if (!s.empty()) out.push_back(s);
        }
    }
}

// Block drops for a broken block state (Block.getDrops).
inline void block_drops(int state, int x, int y, int z, const ItemStack& tool, std::vector<ItemStack>& out) {
    const BlockInfo& b = block_of(state);
    if (b.loot < 0) return;
    LootContext ctx{state, x, y, z, tool, 0};
    roll_loot(loot_tables[size_t(b.loot)], ctx, out);
}
