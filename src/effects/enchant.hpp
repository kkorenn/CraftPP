#pragma once
// Enchantment effect components (net.minecraft.world.item.enchantment: Enchantment, EnchantmentHelper,
// LevelBasedValue, ConditionalEffect / TargetedConditionalEffect, effects/*) for the 26.x data-driven enchantments
// (data/vanilla/minecraft/enchantment, compiled by tools/gen_effect_data.py into data/effects.json). Included at the
// end of effects.hpp (mobs TU). Tick thread only.
//
// The JSON is compiled once at startup into small trees (LVal / ECond / VEff / EEff); the parse-time Json is freed.
// Entry points (EnchantmentHelper): enchant_damage_protection, enchant_damage_immune, enchant_modify_damage,
// enchant_modify_knockback, enchant_armor_effectiveness, enchant_durability_change, enchant_block_experience,
// enchant_mob_experience, enchant_repair_with_xp, enchant_post_attack, enchant_attribute_mods,
// enchant_location_changed, enchant_level, enchant_prevents_drop. An item's enchantments: item_enchants().

inline bool has_sorted(const std::vector<int>& v, int x) { return std::binary_search(v.begin(), v.end(), x); }

// ---------- LevelBasedValue ----------
struct LVal {
    enum T : uint8_t { CONST, LINEAR, SQUARED, CLAMPED, FRACTION, LOOKUP, EXPONENT } t = CONST;
    float a = 0, b = 0, c = 0;  // constant / linear base,per / squared added / clamped -,min,max
    std::vector<LVal> kids;     // clamped value; fraction num,den; lookup fallback; exponent base,power
    std::vector<float> values;  // lookup
    float calc(int level) const {
        switch (t) {
            case CONST: return a;
            case LINEAR: return a + b * float(level - 1);
            case SQUARED: return float(level * level) + a;
            case CLAMPED: return std::clamp(kids[0].calc(level), b, c);
            case FRACTION: { float d = kids[1].calc(level); return d == 0.0f ? 0.0f : kids[0].calc(level) / d; }
            case LOOKUP: return level >= 1 && size_t(level) <= values.size() ? values[size_t(level - 1)] : kids[0].calc(level);
            case EXPONENT: return float(std::pow(kids[0].calc(level), kids[1].calc(level)));
        }
        return 0;
    }
};
inline LVal lval(const Json& j) {
    LVal v;
    if (j.isNumber()) { v.a = j.asFloat(); return v; }
    std::string t = j["type"].asString();
    if (t == "minecraft:linear") v.t = LVal::LINEAR, v.a = j["base"].asFloat(), v.b = j["per_level_above_first"].asFloat();
    else if (t == "minecraft:levels_squared") v.t = LVal::SQUARED, v.a = j["added"].asFloat();
    else if (t == "minecraft:clamped") v.t = LVal::CLAMPED, v.kids = {lval(j["value"])}, v.b = j["min"].asFloat(), v.c = j["max"].asFloat();
    else if (t == "minecraft:fraction") v.t = LVal::FRACTION, v.kids = {lval(j["numerator"]), lval(j["denominator"])};
    else if (t == "minecraft:exponent") v.t = LVal::EXPONENT, v.kids = {lval(j["base"]), lval(j["power"])};
    else if (t == "minecraft:lookup") {
        v.t = LVal::LOOKUP, v.kids = {lval(j["fallback"])};
        for (auto& x : j["values"].arr) v.values.push_back(x.asFloat());
    } else if (t == "minecraft:constant") v.a = j["value"].asFloat();
    return v;
}

// ---------- context (LootContext of the enchanted_* param sets) ----------
struct ECtx {
    int level = 1;
    Entity* self = nullptr;      // THIS_ENTITY
    Entity* attacker = nullptr;  // ATTACKING_ENTITY (source.getEntity)
    Entity* direct = nullptr;    // DIRECT_ATTACKING_ENTITY
    const DamageSource* src = nullptr;
    const ItemStack* tool = nullptr;
    bool active = false;         // ENCHANTMENT_ACTIVE (location_changed)
};

// ---------- conditions (the LootItemCondition / EntityPredicate subset vanilla enchantments use) ----------
struct EntityPred {
    int type = -1;
    std::string type_tag;
    int8_t on_ground = -1, flying = -1, in_water = -1, fall_flying = -1;  // flags, -1 = any
    bool vehicle = false;          // "vehicle": {} -> must be riding (nothing rides here: never)
    int8_t sky = -1;               // location.can_see_sky
    std::vector<int> affected_by;  // movement_affected_by block indices
    float min_hspeed = -1, min_fall = -1;
    int periodic = 0;
    bool player = false;           // type_specific/player
    int gamemode = -1, min_food = -1;
    bool unsupported = false;
};
struct ECond {
    enum T : uint8_t { TRUE_, FALSE_, DAMAGE_SOURCE, ENTITY, MATCH_TOOL, INVERTED, ALL, ANY, CHANCE, ACTIVE, LOCATION, WEATHER } t = TRUE_;
    std::vector<ECond> kids;
    std::vector<std::pair<std::string, bool>> tags;  // damage type tags with the expected value
    int8_t is_direct = -1;
    uint8_t entity = 0;  // 0 this, 1 attacker, 2 direct_attacker
    EntityPred ep;
    std::vector<int> items;  // match_tool
    LVal chance;
    bool chance_by_level = false, flag = false;
    std::vector<int> blocks;  // location_check
};
inline std::map<std::string, std::vector<int>> ench_item_tags;   // data/effects.json item_tags
inline std::map<std::string, std::vector<int>> ench_block_tags;  // block indices
inline int block_named(const std::string& n) {
    auto it = block_by_name.find(n);
    return it == block_by_name.end() ? -1 : it->second;
}
inline std::vector<int> block_set(const Json& j) {
    std::vector<int> out;
    auto one = [&](const std::string& n) {
        if (n[0] == '#') { auto& v = ench_block_tags[n.substr(1)]; out.insert(out.end(), v.begin(), v.end()); }
        else if (int b = block_named(n); b >= 0) out.push_back(b);
    };
    if (j.isString()) one(j.asString());
    else for (auto& x : j.arr) one(x.asString());
    std::sort(out.begin(), out.end());
    return out;
}
inline EntityPred entity_pred(const Json& p) {
    EntityPred e;
    for (auto& [k, v] : p.obj) {
        if (k == "minecraft:entity_type") {
            std::string n = v.asString();
            if (n[0] == '#') e.type_tag = n.substr(n.find(':') + 1);
            else e.type = entity_type_id(n);
        } else if (k == "minecraft:flags") {
            for (auto& [f, b] : v.obj) {
                int8_t x = b.asBool() ? 1 : 0;
                if (f == "is_on_ground") e.on_ground = x;
                else if (f == "is_flying") e.flying = x;
                else if (f == "is_in_water") e.in_water = x;
                else if (f == "is_fall_flying") e.fall_flying = x;
                else e.unsupported = true;
            }
        } else if (k == "minecraft:vehicle") e.vehicle = true;
        else if (k == "minecraft:location") e.sky = v.has("can_see_sky") && v["can_see_sky"].asBool() ? 1 : 0;
        else if (k == "minecraft:movement_affected_by") e.affected_by = block_set(v["block"]["blocks"]);
        else if (k == "minecraft:movement") {
            if (auto* h = v.find("horizontal_speed")) e.min_hspeed = float((*h)["min"].asDouble());
            if (auto* f = v.find("fall_distance")) e.min_fall = float((*f)["min"].asDouble());
        } else if (k == "minecraft:periodic_tick") e.periodic = v.asInt();
        else if (k == "minecraft:type_specific/player") {
            e.player = true;
            if (auto* g = v.find("gamemode")) {
                static const char* gm[] = {"survival", "creative", "adventure", "spectator"};
                for (int i = 0; i < 4; i++) if ((*g)[0].asString() == gm[i]) e.gamemode = i;
            }
            if (auto* f = v.find("food")) e.min_food = (*f)["level"].getInt("min", -1);
        } else e.unsupported = true;
    }
    return e;
}
inline ECond econd(const Json& j) {
    ECond c;
    std::string t = j["type"].asString();
    if (t == "minecraft:damage_source_properties") {
        c.t = ECond::DAMAGE_SOURCE;
        auto& p = j["predicate"];
        if (auto* tags = p.find("tags"))
            for (auto& x : tags->arr) c.tags.push_back({x["id"].asString().substr(11), x["expected"].asBool()});  // "#minecraft:"
        if (auto* d = p.find("is_direct")) c.is_direct = d->asBool() ? 1 : 0;
    } else if (t == "minecraft:entity_properties") {
        c.t = ECond::ENTITY;
        std::string w = j["entity"].asString();
        c.entity = w == "attacker" ? 1 : w == "direct_attacker" ? 2 : 0;
        c.ep = entity_pred(j["predicate"]);
    } else if (t == "minecraft:match_tool") {
        c.t = ECond::MATCH_TOOL;
        std::string n = j["predicate"]["items"].asString();
        if (n[0] == '#') c.items = ench_item_tags[n.substr(1)];
        else if (auto it = item_by_name.find(n); it != item_by_name.end()) c.items = {it->second};
        std::sort(c.items.begin(), c.items.end());
    } else if (t == "minecraft:inverted") {
        c.t = ECond::INVERTED, c.kids = {econd(j["term"])};
    } else if (t == "minecraft:all_of" || t == "minecraft:any_of") {
        c.t = t == "minecraft:all_of" ? ECond::ALL : ECond::ANY;
        for (auto& x : j["terms"].arr) c.kids.push_back(econd(x));
    } else if (t == "minecraft:random_chance") {
        c.t = ECond::CHANCE;
        auto& ch = j["chance"];
        if (ch.isObject() && ch["type"].asString() == "minecraft:enchantment_level") c.chance = lval(ch["amount"]), c.chance_by_level = true;
        else c.chance = lval(ch);
    } else if (t == "minecraft:enchantment_active_check") {
        c.t = ECond::ACTIVE, c.flag = j["active"].asBool();
    } else if (t == "minecraft:location_check") {
        c.t = ECond::LOCATION;
        auto& p = j["predicate"];
        if (auto* b = p.find("block")) c.blocks = block_set((*b)["blocks"]);
        c.flag = p.has("can_see_sky") && p["can_see_sky"].asBool();
    } else if (t == "minecraft:weather_check") {
        c.t = ECond::WEATHER;
    } else {
        c.t = ECond::FALSE_;
    }
    return c;
}

inline bool entity_test(const EntityPred& p, Entity* e) {
    if (!e || p.unsupported || p.vehicle) return false;
    if (p.type >= 0 && e->type != p.type) return false;
    if (!p.type_tag.empty() && !e->in_tag(p.type_tag)) return false;
    Player* pl = e->as_player();
    bool ground = pl ? pl->on_ground : e->on_ground;
    bool flying = pl && pl->net.flying, fall_flying = pl ? pl->net.fall_flying && !pl->on_ground : e->shared_flag(7);
    if (p.on_ground >= 0 && ground != bool(p.on_ground)) return false;
    if (p.flying >= 0 && flying != bool(p.flying)) return false;
    if (p.in_water >= 0 && e->is_in_water() != bool(p.in_water)) return false;
    if (p.fall_flying >= 0 && fall_flying != bool(p.fall_flying)) return false;
    if (p.sky >= 0 && can_see_sky(e->block_x(), e->block_y(), e->block_z()) != bool(p.sky)) return false;
    if (!p.affected_by.empty() &&  // getBlockPosBelowThatAffectsMyMovement
        !has_sorted(p.affected_by, block_index(block_at(e->block_x(), mth::floor(e->pos.y - 0.5000001), e->block_z()))))
        return false;
    if (p.min_hspeed >= 0) {
        double dx = e->pos.x - e->old.x, dz = e->pos.z - e->old.z;
        if (std::sqrt(dx * dx + dz * dz) < p.min_hspeed) return false;
    }
    if (p.min_fall >= 0 && (pl ? pl->g.fall_distance : e->fall_distance) < p.min_fall) return false;
    if (p.periodic > 0 && e->tick_count % p.periodic != 0) return false;
    if (p.player) {
        if (!pl) return false;
        if (p.gamemode >= 0 && pl->gamemode != p.gamemode) return false;
        if (p.min_food >= 0 && pl->g.food < p.min_food) return false;
    }
    return true;
}
inline bool cond_test(const ECond& c, const ECtx& x) {
    switch (c.t) {
        case ECond::TRUE_: return true;
        case ECond::FALSE_: return false;
        case ECond::DAMAGE_SOURCE: {
            if (!x.src) return false;
            for (auto& [tag, want] : c.tags) {
                auto it = damage_tags.find(tag);
                bool has = it != damage_tags.end() && in_sorted(it->second, x.src->type);
                if (has != want) return false;
            }
            if (c.is_direct >= 0 && (x.src->causing == x.src->direct) != bool(c.is_direct)) return false;
            return true;
        }
        case ECond::ENTITY: return entity_test(c.ep, c.entity == 0 ? x.self : c.entity == 1 ? x.attacker : x.direct);
        case ECond::MATCH_TOOL: return x.tool && !x.tool->empty() && has_sorted(c.items, x.tool->item);
        case ECond::INVERTED: return !cond_test(c.kids[0], x);
        case ECond::ALL: for (auto& k : c.kids) if (!cond_test(k, x)) return false; return true;
        case ECond::ANY: for (auto& k : c.kids) if (cond_test(k, x)) return true; return false;
        case ECond::CHANCE: return rand_float() < c.chance.calc(c.chance_by_level ? x.level : 1);
        case ECond::ACTIVE: return x.active == c.flag;
        case ECond::LOCATION: {
            if (!x.self) return false;
            int bx = x.self->block_x(), by = x.self->block_y(), bz = x.self->block_z();
            if (!c.blocks.empty() && !has_sorted(c.blocks, block_index(block_at(bx, by, bz)))) return false;
            return !c.flag || can_see_sky(bx, by, bz);
        }
        case ECond::WEATHER: return false;  // ponytail: no weather simulation yet; rain/thunder checks fail
    }
    return false;
}

// ---------- value effects (EnchantmentValueEffect) ----------
struct VEff {
    enum T : uint8_t { ADD, MULTIPLY, SET, REMOVE_BINOMIAL, ALL_OF } t = ADD;
    LVal v;
    std::vector<VEff> kids;
    float process(int level, float in) const {
        switch (t) {
            case ADD: return in + v.calc(level);
            case MULTIPLY: return in * v.calc(level);
            case SET: return v.calc(level);
            case REMOVE_BINOMIAL: {
                float p = v.calc(level);
                int drop = 0;
                if (!(in <= 128.0f) && !(in * p < 20.0f) && !(in * (1.0f - p) < 20.0f)) {
                    double miu = std::floor(in * p), sigma = std::sqrt(in * p * (1.0f - p));
                    drop = std::clamp(int(std::llround(miu + rand_gaussian() * sigma)), 0, int(in));
                } else {
                    for (int y = 0; float(y) < in; y++) if (rand_float() < p) drop++;
                }
                return in - float(drop);
            }
            case ALL_OF: for (auto& k : kids) in = k.process(level, in); return in;
        }
        return in;
    }
};
inline VEff veff(const Json& j) {
    VEff e;
    std::string t = j["type"].asString();
    if (t == "minecraft:add") e.t = VEff::ADD, e.v = lval(j["value"]);
    else if (t == "minecraft:multiply") e.t = VEff::MULTIPLY, e.v = lval(j["factor"]);
    else if (t == "minecraft:set") e.t = VEff::SET, e.v = lval(j["value"]);
    else if (t == "minecraft:remove_binomial") e.t = VEff::REMOVE_BINOMIAL, e.v = lval(j["chance"]);
    else if (t == "minecraft:all_of") { e.t = VEff::ALL_OF; for (auto& x : j["effects"].arr) e.kids.push_back(veff(x)); }
    return e;
}

// ---------- entity effects (EnchantmentEntityEffect + the location-based attribute effect) ----------
struct EEff {
    enum T : uint8_t { NONE, IGNITE, DAMAGE, ITEM_DAMAGE, MOB_EFFECT, SOUND, ATTRIBUTE, REPLACE_DISK, EXHAUSTION, ALL_OF } t = NONE;
    LVal a, b, c, d;           // ignite duration | damage min,max | item damage | effect dur min,max amp min,max | ...
    std::vector<int> effects;  // apply_mob_effect candidates
    std::string str;           // damage type / sound / attribute modifier id
    int attr = -1;
    uint8_t op = 0;
    int state = -1;            // replace_disk block state
    int offset[3] = {0, 0, 0};
    std::vector<EEff> kids;
};
inline int attribute_id(const std::string& n) {
    std::string s = n.rfind("minecraft:", 0) == 0 ? n.substr(10) : n;
    for (size_t a = 0; a < attribute_info.size(); a++) if (attribute_info[a].id == s) return int(a);
    return -1;
}
inline uint8_t attr_op(const std::string& s) { return s == "add_multiplied_base" ? 1 : s == "add_multiplied_total" ? 2 : 0; }
inline EEff eeff(const Json& j) {
    EEff e;
    std::string t = j.has("type") ? j["type"].asString() : "";
    if (t == "minecraft:ignite") e.t = EEff::IGNITE, e.a = lval(j["duration"]);
    else if (t == "minecraft:damage_entity") {
        e.t = EEff::DAMAGE, e.a = lval(j["min_damage"]), e.b = lval(j["max_damage"]), e.str = j["damage_type"].asString().substr(10);
    } else if (t == "minecraft:change_item_damage") e.t = EEff::ITEM_DAMAGE, e.a = lval(j["amount"]);
    else if (t == "minecraft:apply_mob_effect") {
        e.t = EEff::MOB_EFFECT;
        auto& ta = j["to_apply"];
        if (ta.isString()) e.effects.push_back(effect_id(ta.asString()));
        else for (auto& x : ta.arr) e.effects.push_back(effect_id(x.asString()));
        e.a = lval(j["min_duration"]), e.b = lval(j["max_duration"]), e.c = lval(j["min_amplifier"]), e.d = lval(j["max_amplifier"]);
    } else if (t == "minecraft:play_sound") {
        e.t = EEff::SOUND;
        auto& s = j["sound"];
        e.str = s.isString() ? s.asString() : s.arr.empty() ? "" : s[0].asString();
    } else if (t == "minecraft:attribute") {
        e.t = EEff::ATTRIBUTE, e.attr = attribute_id(j["attribute"].asString()), e.a = lval(j["amount"]), e.str = j["id"].asString();
        e.op = attr_op(j["operation"].asString());
    } else if (t == "minecraft:replace_disk") {
        e.t = EEff::REPLACE_DISK, e.a = lval(j["radius"]), e.b = lval(j["height"]);
        for (size_t i = 0; i < 3; i++) e.offset[i] = j["offset"][i].asInt();
        if (int b = block_named(j["block_state"]["id"].asString()); b >= 0) e.state = block_info[size_t(b)].first;  // frosted_ice age=0
    } else if (t == "minecraft:apply_exhaustion") e.t = EEff::EXHAUSTION, e.a = lval(j["amount"]);
    else if (t == "minecraft:all_of") { e.t = EEff::ALL_OF; for (auto& x : j["effects"].arr) e.kids.push_back(eeff(x)); }
    // ponytail: summon_entity (channeling lightning), explode (wind burst), apply_impulse (lunge) and spawn_particles
    // are not ported: no lightning, wind charges, maces or spears for players here yet.
    return e;
}

// ---------- compiled enchantments ----------
struct CondValue { ECond cond; VEff eff; };
struct Targeted { uint8_t enchanted = 0, affected = 0; ECond cond; EEff eff; };  // 0 attacker, 1 damaging_entity, 2 victim
struct CondEntity { ECond cond; EEff eff; };
struct EnchantDef {
    std::string name;
    uint32_t slots = 0;  // bit per EquipmentSlot (ES_*)
    std::vector<CondValue> protection, damage, knockback, armor_eff, item_damage, block_xp, mob_xp, repair_xp, drops, immunity;
    std::vector<Targeted> post_attack;
    std::vector<CondEntity> location;
    std::vector<EEff> attributes;  // minecraft:attributes (while equipped)
    bool prevent_drop = false, prevent_armor_change = false;
    bool matching_slot(int slot) const { return slots >> slot & 1; }
};
inline std::vector<EnchantDef> enchant_defs;  // by minecraft:enchantment registry id (sorted names)
inline int enchant_id(const std::string& name) {
    std::string n = name.rfind("minecraft:", 0) == 0 ? name : "minecraft:" + name;
    for (size_t i = 0; i < enchant_defs.size(); i++) if (enchant_defs[i].name == n) return int(i);
    return -1;
}
inline uint32_t slot_group_bits(const std::string& g) {  // EquipmentSlotGroup
    if (g == "any") return 0xFF;
    if (g == "mainhand") return 1u << ES_MAINHAND;
    if (g == "offhand") return 1u << ES_OFFHAND;
    if (g == "hand") return 1u << ES_MAINHAND | 1u << ES_OFFHAND;
    if (g == "feet") return 1u << ES_FEET;
    if (g == "legs") return 1u << ES_LEGS;
    if (g == "chest") return 1u << ES_CHEST;
    if (g == "head") return 1u << ES_HEAD;
    if (g == "armor") return 1u << ES_FEET | 1u << ES_LEGS | 1u << ES_CHEST | 1u << ES_HEAD | 1u << ES_BODY;
    if (g == "body") return 1u << ES_BODY;
    if (g == "saddle") return 1u << ES_SADDLE;
    return 0;
}
inline void load_enchantment_data(const Json& root) {
    for (auto& [k, v] : root["item_tags"].obj)
        for (auto& x : v.arr) ench_item_tags[k].push_back(x.asInt());
    for (auto& [k, v] : root["block_tags"].obj) ench_block_tags[k] = block_set(v);
    for (auto& j : root["enchantments"].arr) {
        EnchantDef d;
        d.name = j["name"].asString();
        for (auto& s : j["slots"].arr) d.slots |= slot_group_bits(s.asString());
        auto cond_of = [](const Json& c) { return c.has("requirements") ? econd(c["requirements"]) : ECond{}; };
        auto values = [&](const Json& list, std::vector<CondValue>& out) {
            for (auto& c : list.arr) out.push_back({cond_of(c), c.has("effect") ? veff(c["effect"]) : VEff{}});
        };
        auto target = [](const std::string& s) -> uint8_t { return s == "attacker" ? 0 : s == "damaging_entity" ? 1 : 2; };
        for (auto& [comp, list] : j["effects"].obj) {
            if (comp == "minecraft:damage_protection") values(list, d.protection);
            else if (comp == "minecraft:damage") values(list, d.damage);
            else if (comp == "minecraft:knockback") values(list, d.knockback);
            else if (comp == "minecraft:armor_effectiveness") values(list, d.armor_eff);
            else if (comp == "minecraft:item_damage") values(list, d.item_damage);
            else if (comp == "minecraft:block_experience") values(list, d.block_xp);
            else if (comp == "minecraft:mob_experience") values(list, d.mob_xp);
            else if (comp == "minecraft:repair_with_xp") values(list, d.repair_xp);
            else if (comp == "minecraft:equipment_drops") values(list, d.drops);
            else if (comp == "minecraft:damage_immunity") for (auto& c : list.arr) d.immunity.push_back({cond_of(c), VEff{}});
            else if (comp == "minecraft:post_attack") {
                for (auto& c : list.arr)
                    d.post_attack.push_back({target(c["enchanted"].asString()), target(c["affected"].asString()), cond_of(c), eeff(c["effect"])});
            } else if (comp == "minecraft:location_changed") {
                for (auto& c : list.arr) d.location.push_back({cond_of(c), eeff(c["effect"])});
            } else if (comp == "minecraft:attributes") {
                for (auto& c : list.arr) {
                    EEff a;
                    a.t = EEff::ATTRIBUTE, a.attr = attribute_id(c["attribute"].asString()), a.a = lval(c["amount"]);
                    a.str = c["id"].asString(), a.op = attr_op(c["operation"].asString());
                    d.attributes.push_back(std::move(a));
                }
            } else if (comp == "minecraft:prevent_equipment_drop") d.prevent_drop = true;
            else if (comp == "minecraft:prevent_armor_change") d.prevent_armor_change = true;
            // ponytail: projectile_*, ammo_use, crossbow_*, trident_*, fishing_*, smash_damage_per_fallen_block,
            // hit_block, post_piercing_attack and tick (soul speed particles): no player bows, crossbows, tridents,
            // fishing rods, maces or spears yet.
        }
        enchant_defs.push_back(std::move(d));
    }
}

// ---------- EnchantmentHelper ----------
// runIterationOnEquipment: every enchantment of every equipped item, in slots the enchantment applies to
template <class F> inline void each_equipped_enchant(LivingEntity& e, F f) {
    for (int slot = 0; slot < 8; slot++) {
        const ItemStack& it = e.item_in_slot(slot);
        if (it.empty() || !item_has_comp(it)) continue;
        for (auto& [id, lvl] : item_enchants(it))
            if (size_t(id) < enchant_defs.size() && enchant_defs[size_t(id)].matching_slot(slot)) f(enchant_defs[size_t(id)], lvl, it, slot);
    }
}
template <class F> inline void each_item_enchant(const ItemStack& it, F f) {  // runIterationOnItem
    if (it.empty() || !item_has_comp(it)) return;
    for (auto& [id, lvl] : item_enchants(it))
        if (size_t(id) < enchant_defs.size()) f(enchant_defs[size_t(id)], lvl);
}
inline float apply_values(const std::vector<CondValue>& v, const ECtx& x, float val) {
    for (auto& c : v) if (cond_test(c.cond, x)) val = c.eff.process(x.level, val);
    return val;
}
inline ECtx damage_ctx(int level, Entity* victim, const DamageSource& s) {  // Enchantment.damageContext
    ECtx x;
    x.level = level, x.self = victim, x.attacker = s.causing, x.direct = s.direct, x.src = &s;
    return x;
}

inline float enchant_damage_protection(LivingEntity& victim, const DamageSource& s) {  // getDamageProtection
    float r = 0.0f;
    each_equipped_enchant(victim, [&](const EnchantDef& d, int lvl, const ItemStack&, int) {
        if (!d.protection.empty()) r = apply_values(d.protection, damage_ctx(lvl, &victim, s), r);
    });
    return r;
}
inline bool enchant_damage_immune(LivingEntity& victim, const DamageSource& s) {  // isImmuneToDamage
    bool r = false;
    each_equipped_enchant(victim, [&](const EnchantDef& d, int lvl, const ItemStack&, int) {
        for (auto& c : d.immunity) if (cond_test(c.cond, damage_ctx(lvl, &victim, s))) r = true;
    });
    return r;
}
// modifyDamageFilteredValue over the weapon's enchantments
inline float enchant_weapon_value(const ItemStack& w, std::vector<CondValue> EnchantDef::*comp, Entity* victim, const DamageSource& s, float v) {
    each_item_enchant(w, [&](const EnchantDef& d, int lvl) {
        if (!(d.*comp).empty()) v = apply_values(d.*comp, damage_ctx(lvl, victim, s), v);
    });
    return v;
}
inline float enchant_modify_damage(const ItemStack& w, Entity* victim, const DamageSource& s, float v) { return enchant_weapon_value(w, &EnchantDef::damage, victim, s, v); }
inline float enchant_modify_knockback(const ItemStack& w, Entity* victim, const DamageSource& s, float v) { return enchant_weapon_value(w, &EnchantDef::knockback, victim, s, v); }
inline float enchant_armor_effectiveness(const ItemStack& w, Entity* victim, const DamageSource& s, float v) { return enchant_weapon_value(w, &EnchantDef::armor_eff, victim, s, v); }
inline float item_filtered(const ItemStack& it, std::vector<CondValue> EnchantDef::*comp, float v) {  // modifyItemFilteredCount
    each_item_enchant(it, [&](const EnchantDef& d, int lvl) {
        if ((d.*comp).empty()) return;
        ECtx x;
        x.level = lvl, x.tool = &it;
        v = apply_values(d.*comp, x, v);
    });
    return v;
}
inline int enchant_durability_change(const ItemStack& it, int amount) { return int(item_filtered(it, &EnchantDef::item_damage, float(amount))); }
inline int enchant_block_experience(const ItemStack& it, int amount) { return int(item_filtered(it, &EnchantDef::block_xp, float(amount))); }
inline int enchant_repair_with_xp(const ItemStack& it, int durability) { return std::max(0, int(item_filtered(it, &EnchantDef::repair_xp, float(durability)))); }
inline int enchant_mob_experience(Entity* killer, Entity& killed, int amount) {  // processMobExperience
    LivingEntity* l = killer ? killer->as_living() : nullptr;
    if (!l) return amount;
    float v = float(amount);
    each_equipped_enchant(*l, [&](const EnchantDef& d, int lvl, const ItemStack& it, int) {
        if (d.mob_xp.empty()) return;
        ECtx x;
        x.level = lvl, x.self = &killed, x.tool = &it;
        v = apply_values(d.mob_xp, x, v);
    });
    return int(v);
}
inline int item_enchant_level(const ItemStack& it, int ench) {  // getItemEnchantmentLevel
    if (it.empty() || !item_has_comp(it)) return 0;
    for (auto& [id, lvl] : item_enchants(it)) if (id == ench) return lvl;
    return 0;
}
inline int enchant_level(int ench, LivingEntity& e) {  // getEnchantmentLevel: best level in the enchantment's slots
    if (ench < 0 || size_t(ench) >= enchant_defs.size()) return 0;
    int best = 0;
    for (int slot = 0; slot < 8; slot++)
        if (enchant_defs[size_t(ench)].matching_slot(slot)) best = std::max(best, item_enchant_level(e.item_in_slot(slot), ench));
    return best;
}
inline bool enchant_prevents_drop(const ItemStack& it) {  // has(PREVENT_EQUIPMENT_DROP): curse of vanishing
    bool r = false;
    each_item_enchant(it, [&](const EnchantDef& d, int) { r = r || d.prevent_drop; });
    return r;
}
inline bool enchant_prevents_armor_change(const ItemStack& it) {  // curse of binding
    bool r = false;
    each_item_enchant(it, [&](const EnchantDef& d, int) { r = r || d.prevent_armor_change; });
    return r;
}
// forEachModifier(stack, slot): minecraft:attributes modifiers of an item worn in `slot`
template <class F> inline void enchant_attribute_mods(const ItemStack& it, int slot, F f) {
    each_item_enchant(it, [&](const EnchantDef& d, int lvl) {
        if (!d.matching_slot(slot)) return;
        for (auto& a : d.attributes) if (a.attr >= 0) f(a.attr, double(a.a.calc(lvl)), a.op);
    });
}
// equipment_drops (looting on mob equipment): processEquipmentDropChance, attacker's gear (enchanted = attacker)
inline float enchant_equipment_drop_chance(LivingEntity&, const DamageSource& s, float chance) {
    LivingEntity* att = s.causing ? s.causing->as_living() : nullptr;
    if (!att) return chance;
    each_equipped_enchant(*att, [&](const EnchantDef& d, int lvl, const ItemStack&, int) {
        if (!d.drops.empty()) chance = apply_values(d.drops, damage_ctx(lvl, att, s), chance);  // vanilla passes the attacker as THIS
    });
    return chance;
}

void hurt_item_in_slot(LivingEntity& owner, int slot, int amount);  // effects.cpp: ItemStack.hurtAndBreak

inline void apply_entity_effect(const EEff& e, int level, LivingEntity* owner, int slot, Entity& target, bool active = true) {
    switch (e.t) {
        case EEff::IGNITE: target.ignite_for_ticks(mth::floor(e.a.calc(level) * 20.0f)); break;  // igniteForSeconds
        case EEff::DAMAGE: {  // Mth.randomBetween(min, max); the item's owner causes it
            float lo = e.a.calc(level), hi = e.b.calc(level);
            target.hurt_server(DamageSource::of(e.str, owner, owner), lo + rand_float() * (hi - lo));
            break;
        }
        case EEff::ITEM_DAMAGE:
            if (owner && slot >= 0) hurt_item_in_slot(*owner, slot, int(e.a.calc(level)));
            break;
        case EEff::MOB_EFFECT: {
            LivingEntity* l = target.as_living();
            if (!l || e.effects.empty()) break;
            int id = e.effects[size_t(rand_int(int(e.effects.size())))];
            float d0 = e.a.calc(level), d1 = e.b.calc(level);
            int ticks = int(std::lround((d0 + rand_float() * (d1 - d0)) * 20.0f));
            float a0 = e.c.calc(level), a1 = e.d.calc(level);
            int amp = std::max(0, int(std::lround(a0 + rand_float() * (a1 - a0))));
            add_effect(*l, EffectInstance(id, ticks, amp));
            break;
        }
        case EEff::SOUND: play_sound(e.str, target.sound_source(), target.pos.x, target.pos.y, target.pos.z, 1.0f, 1.0f); break;
        case EEff::EXHAUSTION: if (Player* p = target.as_player()) add_exhaustion(*p, e.a.calc(level)); break;
        case EEff::ATTRIBUTE:  // location based: added on activation, removed on deactivation ("<id>/<slot>")
            if (LivingEntity* l = target.as_living(); l && e.attr >= 0) {
                std::string id = e.str + "/" + equip_slot_name(slot < 0 ? 0 : slot);
                if (active) l->add_attr_modifier(e.attr, id, double(e.a.calc(level)), e.op);
                else l->remove_attr_modifier(e.attr, id);
            }
            break;
        case EEff::REPLACE_DISK: {  // ReplaceDisk (frost walker)
            if (e.state < 0 || !active) break;
            int cx = mth::floor(target.pos.x) + e.offset[0], cy = mth::floor(target.pos.y) + e.offset[1], cz = mth::floor(target.pos.z) + e.offset[2];
            int dist = int(e.a.calc(level)), height = int(e.b.calc(level));
            for (int y = cy; y <= cy + std::min(height - 1, 0); y++)
                for (int x = cx - dist; x <= cx + dist; x++)
                    for (int z = cz - dist; z <= cz + dist; z++) {
                        double dx = x + 0.5 - target.pos.x, dz = z + 0.5 - target.pos.z;  // distToCenterSqr(x, y + 0.5, z)
                        if (dx * dx + dz * dz >= double(dist * dist)) continue;
                        // ponytail: frost walker's BlockPredicate (air above, water source, unobstructed) is
                        // hard-coded rather than interpreted from the JSON.
                        if (block_at(x, y + 1, z) != blocks::air || block_at(x, y, z) != blocks::water) continue;
                        bool blocked = false;
                        AABB cell{double(x), double(y), double(z), x + 1.0, y + 1.0, z + 1.0};
                        for_each_entity_in(cell, [&](Entity& o) { if (o.as_living() && o.is_alive()) blocked = true; });
                        if (!blocked) change_block(x, y, z, e.state);
                    }
            break;
        }
        case EEff::ALL_OF: for (auto& k : e.kids) apply_entity_effect(k, level, owner, slot, target, active); break;
        case EEff::NONE: break;
    }
}

// doPostAttackEffectsWithItemSource: the victim's equipment (enchanted = victim), then the weapon (enchanted = attacker)
inline void enchant_post_attack(Entity& victim, const DamageSource& s, const ItemStack* weapon) {
    auto run = [&](const Targeted& t, int lvl, LivingEntity* owner, int slot) {
        if (!cond_test(t.cond, damage_ctx(lvl, &victim, s))) return;
        Entity* target = t.affected == 0 ? s.causing : t.affected == 1 ? s.direct : &victim;
        if (target) apply_entity_effect(t.eff, lvl, owner, slot, *target);
    };
    if (LivingEntity* lv = victim.as_living())
        each_equipped_enchant(*lv, [&](const EnchantDef& d, int lvl, const ItemStack&, int slot) {
            for (auto& t : d.post_attack) if (t.enchanted == 2) run(t, lvl, lv, slot);
        });
    LivingEntity* attacker = s.causing ? s.causing->as_living() : nullptr;
    if (weapon && attacker)
        each_item_enchant(*weapon, [&](const EnchantDef& d, int lvl) {
            if (!d.matching_slot(ES_MAINHAND)) return;
            for (auto& t : d.post_attack) if (t.enchanted == 0) run(t, lvl, attacker, ES_MAINHAND);
        });
}

// runLocationChangedEffects (on block change) + stopLocationBasedEffects (item removed): which location effects
// are active per entity, slot and enchantment.
struct ActiveLocation { int slot, ench, level; };
inline std::unordered_map<int, std::vector<ActiveLocation>> active_location;  // entity id -> active effects
inline void enchant_location_changed(LivingEntity& e) {
    auto it = active_location.find(e.id);
    std::vector<ActiveLocation> none;
    auto& act = it != active_location.end() ? it->second : none;
    std::vector<ActiveLocation> next;
    for (int slot = 0; slot < 8; slot++) {
        const ItemStack& item = e.item_in_slot(slot);
        if (item.empty() || !item_has_comp(item)) continue;
        for (auto& [id, lvl] : item_enchants(item)) {
            if (size_t(id) >= enchant_defs.size()) continue;
            const EnchantDef& d = enchant_defs[size_t(id)];
            if (d.location.empty() || !d.matching_slot(slot)) continue;
            int ench_id = id;
            bool was = std::any_of(act.begin(), act.end(), [&](auto& a) { return a.slot == slot && a.ench == ench_id; });
            ECtx x;
            x.level = lvl, x.self = &e, x.active = was;
            bool any = false;
            for (auto& c : d.location) {
                if (cond_test(c.cond, x)) any = true, apply_entity_effect(c.eff, lvl, &e, slot, e, true);
                else if (was) apply_entity_effect(c.eff, lvl, &e, slot, e, false);
            }
            if (any) next.push_back({slot, ench_id, lvl});
        }
    }
    for (auto& a : act)  // gone (item removed or condition no longer met): deactivate
        if (std::none_of(next.begin(), next.end(), [&](auto& n) { return n.slot == a.slot && n.ench == a.ench; }))
            for (auto& c : enchant_defs[size_t(a.ench)].location) apply_entity_effect(c.eff, a.level, &e, a.slot, e, false);
    if (next.empty()) active_location.erase(e.id);
    else active_location[e.id] = std::move(next);
}
