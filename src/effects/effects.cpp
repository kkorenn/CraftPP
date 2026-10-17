// Effects module TU: data (data/effects.json, tools/gen_effect_data.py), player effect ticking and sync, consume
// effects (food, milk, potions), the player API in mobs/api.hpp and /effect. Effect logic: effects.hpp.
#include "../mobs/mobs.hpp"

// ---------- potions (Potions.java; minecraft:potion registry order) ----------
struct Potion { std::string name; std::vector<EffectInstance> effects; };
static std::vector<Potion> potions;

// ---------- consume effects (net.minecraft.world.item.consume_effects) ----------
struct ConsumeEffect {
    enum T : uint8_t { APPLY, REMOVE, CLEAR_ALL, TELEPORT, SOUND } t = APPLY;
    float probability = 1.0f, diameter = 16.0f;
    std::vector<EffectInstance> effects;  // APPLY
    std::vector<int> remove;              // REMOVE
    std::string sound;
};
static std::unordered_map<int, std::vector<ConsumeEffect>> consume_effects;  // item id -> on_consume_effects
static std::unordered_map<std::string, std::array<int, 2>> block_xp;           // block -> UniformInt [min, max]; {-1,-1} spawner

static EffectInstance effect_from_json(const Json& j) {  // MobEffectInstance.CODEC
    EffectInstance x(effect_id(j["id"].asString()), j.getInt("duration", 0), j.getInt("amplifier", 0),
                     j.has("ambient") && j["ambient"].asBool(), !j.has("show_particles") || j["show_particles"].asBool());
    x.icon = j.has("show_icon") ? j["show_icon"].asBool() : x.visible;
    return x;
}
static bool load_effect_data() {
    Json j;
    try {
        j = Json::parseFile("data/effects.json");
    } catch (const std::exception& e) {
        fprintf(stderr, "data/effects.json: %s (run: python3 tools/gen_effect_data.py)\n", e.what());
        return false;
    }
    for (auto& p : j["potions"].arr) {
        Potion pot;
        pot.name = p[0].asString();
        for (auto& e : p[1].arr) pot.effects.emplace_back(effect_id(e[0].asString()), e[1].asInt(), e[2].asInt());
        potions.push_back(std::move(pot));
    }
    for (auto& [k, v] : j["block_xp"].obj)
        block_xp[k] = v.isString() ? std::array<int, 2>{-1, -1} : std::array<int, 2>{v[0].asInt(), v[1].asInt()};
    for (auto& [k, list] : j["consume"].obj) {
        auto& out = consume_effects[std::stoi(k)];
        for (auto& c : list.arr) {
            ConsumeEffect ce;
            std::string t = c["type"].asString();
            if (t == "minecraft:apply_effects") {
                ce.t = ConsumeEffect::APPLY;
                ce.probability = c.has("probability") ? c["probability"].asFloat() : 1.0f;
                for (auto& e : c["effects"].arr) ce.effects.push_back(effect_from_json(e));
            } else if (t == "minecraft:remove_effects") {
                ce.t = ConsumeEffect::REMOVE;
                auto& v = c["effects"];
                if (v.isString()) ce.remove.push_back(effect_id(v.asString()));
                else for (auto& e : v.arr) ce.remove.push_back(effect_id(e.asString()));
            } else if (t == "minecraft:clear_all_effects") {
                ce.t = ConsumeEffect::CLEAR_ALL;
            } else if (t == "minecraft:teleport_randomly") {
                ce.t = ConsumeEffect::TELEPORT;
                if (c.has("diameter")) ce.diameter = c["diameter"].asFloat();
            } else {
                ce.t = ConsumeEffect::SOUND;
                ce.sound = c["sound"].asString();
            }
            out.push_back(std::move(ce));
        }
    }
    return true;
}

// TeleportRandomlyConsumeEffect + LivingEntity.randomTeleport for players (chorus fruit).
static bool teleport_randomly(Player& p, PlayerEntity& pe, float diameter) {
    for (int attempt = 0; attempt < 16; attempt++) {
        double xx = p.x + (rand_double() - 0.5) * diameter;
        double yy = std::clamp(p.y + (rand_double() - 0.5) * diameter, double(MIN_Y), double(MIN_Y + HEIGHT - 1));
        double zz = p.z + (rand_double() - 0.5) * diameter;
        int bx = mth::floor(xx), by = mth::floor(yy), bz = mth::floor(zz);
        if (!loaded_chunk(bx >> 4, bz >> 4)) continue;
        double y = yy;
        // ponytail: BlockTags.ENTITIES_CAN_TELEPORT_TO approximated by "has collision"; the consumable's
        // avoidance tag (consumable_does_not_teleport_to) is not checked.
        while (by > MIN_Y) {
            by--;
            int s = block_at(bx, by, bz);
            if (has_collision(s)) {
                AABB box = AABB::at_feet(xx, y, zz, pe.width, pe.height);
                bool ok = true;
                for (int x = mth::floor(box.x0); x <= mth::floor(box.x1) && ok; x++)
                    for (int yb = mth::floor(box.y0); yb <= mth::floor(box.y1) && ok; yb++)
                        for (int z = mth::floor(box.z0); z <= mth::floor(box.z1) && ok; z++) {
                            int b = block_at(x, yb, z);
                            if (fluid_type(b)) ok = false;
                            for (const AABB* c = shape_begin(b); c != shape_end(b) && ok; c++)
                                if (c->move(x, yb, z).intersects(box)) ok = false;
                        }
                if (!ok) break;
                double ox = p.x, oy = p.y, oz = p.z;
                teleport(p, xx, y, zz);
                broadcast_near(p.x, p.z, entity_event_packet(p.eid, 46));
                play_sound("minecraft:item.chorus_fruit.teleport", SRC_PLAYERS, p.x, p.y, p.z, 1.0f, 1.0f);
                (void)ox, (void)oy, (void)oz;
                p.g.fall_distance = 0;
                return true;
            }
            y--;
        }
    }
    return false;
}

void player_consume_effects(Player& p, int item, const ItemStack& stack) {
    PlayerEntity* pe = player_entity(p);
    if (!pe) return;
    (void)stack;
    auto it = consume_effects.find(item);
    if (it == consume_effects.end()) return;
    for (auto& c : it->second) {
        switch (c.t) {
            case ConsumeEffect::APPLY:
                if (rand_float() < c.probability)
                    for (auto& e : c.effects) add_effect(*pe, e);
                break;
            case ConsumeEffect::REMOVE:
                for (int id : c.remove) remove_effect(*pe, id);
                break;
            case ConsumeEffect::CLEAR_ALL: remove_all_effects(*pe); break;
            case ConsumeEffect::TELEPORT: teleport_randomly(p, *pe, c.diameter); break;
            case ConsumeEffect::SOUND: play_sound(c.sound, SRC_PLAYERS, p.x, p.y, p.z, 1.0f, 1.0f); break;
        }
    }
}

// Block.spawnAfterBreak with dropExperience (ServerPlayerGameMode.destroyBlock -> playerDestroy -> dropResources).
void player_block_experience(Player& p, int state, int x, int y, int z) {
    auto it = block_xp.find(block_of(state).name);
    if (it == block_xp.end()) return;
    int n;
    if (it->second[0] < 0) n = 15 + rand_int(15) + rand_int(15);  // SpawnerBlock: no enchantment processing
    else n = enchant_block_experience(held(p), rand_between(it->second[0], it->second[1]));  // tryDropExperience
    if (n > 0 && rule("block_drops")) award_xp_orbs(x + 0.5, y + 0.5, z + 0.5, n);  // popExperience
}

void world_sound(const std::string& sound, int source, double x, double y, double z, float vol, float pitch) {
    play_sound(sound, source, x, y, z, vol, pitch);
}

// ---------- player API ----------
static double attr_by_name(const LivingEntity& e, const char* name) {
    for (size_t a = 0; a < attribute_info.size(); a++)
        if (attribute_info[a].id == name) return e.attr(int(a));
    return 0;
}
float player_max_health(const Player& p) {
    PlayerEntity* pe = player_entity(p);
    return pe ? pe->max_health() : 20.0f;
}
bool player_has_effect(const Player& p, const char* effect) {
    int id = effect_id(effect);
    for (auto& e : p.g.effects) if (e.id == id) return true;
    return false;
}
double player_attribute(const Player& p, const char* attribute) {
    PlayerEntity* pe = player_entity(p);
    return pe ? attr_by_name(*pe, attribute) : 0;
}
bool player_effect_blocks_damage(Player& p, const DamageSource& s) { return s.is(DT_IS_FIRE) && player_has_effect(p, "fire_resistance"); }
float player_damage_after_effects(Player& p, const DamageSource& s, float dmg) {
    PlayerEntity* pe = player_entity(p);
    if (!pe) return dmg;
    dmg = damage_after_magic_absorb(*pe, s, dmg);
    float rest = std::max(dmg - pe->absorption, 0.0f);  // Player.actuallyHurt: absorption first
    set_absorption(*pe, pe->absorption - (dmg - rest));
    return rest;
}
void player_hurt_effects(Player& p, const DamageSource&, float) {
    if (PlayerEntity* pe = player_entity(p)) effects_on_hurt(*pe);
}
float player_destroy_speed(Player& p, float speed, bool eye_in_water) {  // Player.getDestroySpeed
    PlayerEntity* pe = player_entity(p);
    if (!pe) return speed;
    if (speed > 1.0f) speed += float(attr_by_name(*pe, "mining_efficiency"));
    if (has_dig_speed(*pe)) speed *= 1.0f + float(dig_speed_amp(*pe) + 1) * 0.2f;
    if (const EffectInstance* f = pe->effect(eff::mining_fatigue)) speed *= float(std::pow(0.3, f->amp + 1));
    speed *= float(attr_by_name(*pe, "block_break_speed"));
    if (eye_in_water) speed *= float(attr_by_name(*pe, "submerged_mining_speed"));
    if (!p.on_ground) speed /= 5.0f;
    return speed;
}

// ---------- per-player effect sync ----------
static std::unordered_map<const Player*, float> loaded_absorption;  // applied to the PlayerEntity on its first tick
static std::unordered_map<const Player*, float> left_absorption;    // the PlayerEntity is gone when a leaving player saves
struct PlayerEffectSync {
    bool joined = false;
    std::vector<uint8_t> particles{0};
    bool ambient = false;
    float absorption = 0;
};
static std::unordered_map<const Player*, PlayerEffectSync> sync_state;

static Writer player_effect_data(const Player& p, const PlayerEffectSync& s) {  // set_entity_data: slots 10, 11, 17
    Writer w;
    w.varint(play_cb::set_entity_data).varint(p.eid);
    w.u8(uint8_t(edata::LivingEntity::EFFECT_PARTICLES)).varint(edser::PARTICLES).bytes(s.particles);
    w.u8(uint8_t(edata::LivingEntity::EFFECT_AMBIENCE)).varint(edser::BOOLEAN).boolean(s.ambient);
    w.u8(uint8_t(edata::Player::PLAYER_ABSORPTION)).varint(edser::FLOAT).f32(s.absorption);
    w.u8(0xFF);
    return w;
}

static void player_effects_tick(Player& p) {
    PlayerEntity* pe = player_entity(p);
    if (!pe) return;
    auto& st = sync_state[&p];
    if (!st.joined) {  // PlayerList.sendActivePlayerEffects + attribute modifiers for the loaded effects
        st.joined = true;
        for (auto& e : p.g.effects) {
            add_effect_modifiers(*pe, e.id, e.amp);
            p.send(update_effect_packet(p.eid, e, false));
        }
        pe->effects_dirty = true;
        if (auto it = loaded_absorption.find(&p); it != loaded_absorption.end()) pe->absorption = it->second, loaded_absorption.erase(it);
    }
    if (!p.g.dead) tick_effects(*pe);
    else update_dirty_effects(*pe);
    const auto* d = pe->data.find(edata::LivingEntity::EFFECT_PARTICLES);
    std::vector<uint8_t> parts = d ? pe->data.objs[size_t(d->v)] : std::vector<uint8_t>{0};
    bool amb = pe->data.get(edata::LivingEntity::EFFECT_AMBIENCE, 0) != 0;
    if (parts != st.particles || amb != st.ambient || pe->absorption != st.absorption) {
        st.particles = std::move(parts), st.ambient = amb, st.absorption = pe->absorption;
        pe->data.set_float(edata::Player::PLAYER_ABSORPTION, pe->absorption);  // watchers: via the tracker
        p.send(player_effect_data(p, st));  // the player itself (the tracker only sends to watchers)
    }
}
static void player_effects_leave(Player& p) {  // runs before the mobs module drops the PlayerEntity
    sync_state.erase(&p);
    loaded_absorption.erase(&p);
    if (PlayerEntity* pe = player_entity(p)) left_absorption[&p] = pe->absorption;
}

void save_player_effects(const Player& p, Nbt& t) {
    if (PlayerEntity* pe = player_entity(p)) t.set("AbsorptionAmount", Nbt::real(Nbt::Float, pe->absorption));
    else if (auto it = left_absorption.find(&p); it != left_absorption.end())
        t.set("AbsorptionAmount", Nbt::real(Nbt::Float, it->second)), left_absorption.erase(it);
    if (p.g.effects.empty()) return;
    Nbt l = Nbt::make_list();
    for (auto& x : p.g.effects) l.list.push_back(effect_to_nbt(x));
    t.set("active_effects", l);
}
void load_player_effects(Player& p, const Nbt& t) {
    p.g.effects.clear();
    if (const Nbt* l = t.get("active_effects"); l && l->t == Nbt::List)
        for (auto& c : l->list) {
            EffectInstance x;
            if (effect_from_nbt(c, x)) p.g.effects.push_back(std::move(x));
        }
    loaded_absorption[&p] = float(nbt_num(t, "AbsorptionAmount", 0));
}

// Respawn (a new ServerPlayer): the old effects' attribute modifiers and absorption go away.
void effects_player_respawned(Player& p) {
    PlayerEntity* pe = player_entity(p);
    if (!pe) return;
    for (size_t id = 0; id < mob_effects.size(); id++) remove_effect_modifiers(*pe, int(id));
    pe->absorption = 0;
    pe->effects_dirty = true;
}

// ---------- /effect (EffectCommands) ----------
static LivingEntity* living_target(const Target& t) {
    if (t.p) return player_entity(*t.p);
    return t.e ? t.e->as_living() : nullptr;
}
template <class FS, class FM>
static int track_feedback(Cmd& c, const std::vector<Target>& targets, const std::vector<bool>& hit, const char* fail, FS single, FM multiple) {
    int n = 0;
    const Target* only = nullptr;
    for (size_t i = 0; i < targets.size(); i++)
        if (hit[i]) only = n++ == 0 ? &targets[i] : nullptr;
    if (n == 0) throw err(fail);
    c.success(only ? single(only->display()) : multiple(n), true);
    return n;
}
static void register_effect_command() {
    reg("effect", [](Cmd& c) {
        std::vector<Target> targets = c.is("clear") && !c.has("targets") ? c.targets_or_self_any("targets") : c.targets("targets");
        std::vector<bool> hit(targets.size());
        if (c.is("give")) {
            int id = effect_id(c.str("effect"));
            const MobEffectDef& d = mob_effects[size_t(id)];
            int duration;  // computeDurationInTicks
            if (c.is("infinite")) duration = -1;
            else if (c.has("seconds")) duration = d.instant ? int(c.num("seconds")) : int(c.num("seconds")) * 20;
            else duration = d.instant ? 1 : 600;
            int amp = int(c.num("amplifier", 0));
            bool particles = !c.flag("hideParticles");
            for (size_t i = 0; i < targets.size(); i++)
                if (LivingEntity* l = living_target(targets[i])) hit[i] = add_effect(*l, EffectInstance(id, duration, amp, false, particles));
            Text name = tr("effect.minecraft." + d.name);
            return track_feedback(c, targets, hit, "commands.effect.give.failed",
                                  [&](Text n) { return tr("commands.effect.give.success.single", {name, n, duration / 20}); },
                                  [&](int k) { return tr("commands.effect.give.success.multiple", {name, k, duration / 20}); });
        }
        if (c.has("effect")) {
            int id = effect_id(c.str("effect"));
            Text name = tr("effect.minecraft." + mob_effects[size_t(id)].name);
            for (size_t i = 0; i < targets.size(); i++)
                if (LivingEntity* l = living_target(targets[i])) hit[i] = remove_effect(*l, id);
            return track_feedback(c, targets, hit, "commands.effect.clear.specific.failed",
                                  [&](Text n) { return tr("commands.effect.clear.specific.success.single", {name, n}); },
                                  [&](int k) { return tr("commands.effect.clear.specific.success.multiple", {name, k}); });
        }
        for (size_t i = 0; i < targets.size(); i++)
            if (LivingEntity* l = living_target(targets[i])) hit[i] = remove_all_effects(*l);
        return track_feedback(c, targets, hit, "commands.effect.clear.everything.failed",
                              [&](Text n) { return tr("commands.effect.clear.everything.success.single", {n}); },
                              [&](int k) { return tr("commands.effect.clear.everything.success.multiple", {k}); });
    });
}

static MobRegistration effects_registration([] {
    init_mob_effects();
    if (!load_effect_data()) _exit(1);
    on_player_tick(player_effects_tick);
    on_player_leave(player_effects_leave);
    register_effect_command();
});
