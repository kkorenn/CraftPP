#pragma once
// Mob effects (net.minecraft.world.effect: MobEffects, MobEffect + subclasses, MobEffectUtil; the effect parts of
// LivingEntity / ServerPlayer). Included at the end of mobs/living.hpp, so every mob TU has it. Tick thread only.
//
//   add_effect(e, inst, source) / remove_effect(e, id) / remove_all_effects(e)   LivingEntity.addEffect & co.
//   tick_effects(e)               LivingEntity.tickEffects (+ updateDirtyEffects: particles, invisible, glowing)
//   apply_instant(e, id, amp, scale, source, owner)   MobEffect.applyInstantaneousEffect (splash/lingering)
// Players keep their effects in PlayerGame::effects (PlayerEntity::effects()), so respawn clears them; the
// update/remove_mob_effect packets go to the player itself (ServerPlayer.onEffectAdded/Updated/Removed).

// ---------- registry ----------
namespace eff {
inline int speed = -1, slowness = -1, haste = -1, mining_fatigue = -1, strength = -1, instant_health = -1,
           instant_damage = -1, nausea = -1, regeneration = -1, resistance = -1, water_breathing = -1,
           invisibility = -1, blindness = -1, night_vision = -1, hunger = -1, weakness = -1, poison = -1, wither = -1,
           health_boost = -1, absorption = -1, saturation = -1, glowing = -1, luck = -1, unluck = -1, conduit_power = -1,
           bad_omen = -1, hero_of_the_village = -1, darkness = -1, trial_omen = -1, raid_omen = -1, wind_charged = -1,
           weaving = -1, oozing = -1, infested = -1, breath_of_the_nautilus = -1;
}
enum EffectCategory : uint8_t { EC_BENEFICIAL, EC_HARMFUL, EC_NEUTRAL };
struct EffectModifier { int attr; std::string id; double amount; uint8_t op; };  // AttributeTemplate
struct MobEffectDef {
    std::string name;  // without namespace
    uint8_t category = EC_BENEFICIAL;
    int color = 0;
    bool instant = false;
    int particle = -1;  // particle type id; -1 = entity_effect tinted with `color`
    std::string sound;  // soundOnAdded
    std::vector<EffectModifier> mods;
};
inline std::vector<MobEffectDef> mob_effects;  // by minecraft:mob_effect registry id

inline int effect_id(const std::string& name) {  // "speed" or "minecraft:speed"; -1 if unknown
    std::string n = name.rfind("minecraft:", 0) == 0 ? name.substr(10) : name;
    for (size_t i = 0; i < mob_effects.size(); i++) if (mob_effects[i].name == n) return int(i);
    return -1;
}

inline void init_mob_effects() {  // MobEffects static init
    if (!mob_effects.empty()) return;
    const gen::Registry* r = registry("minecraft:mob_effect");
    mob_effects.resize(size_t(r->count));
    auto aid = [](const char* n) {
        for (size_t a = 0; a < attribute_info.size(); a++) if (attribute_info[a].id == n) return int(a);
        return -1;
    };
    struct Spec { const char* n; int* id; uint8_t cat; int color; bool instant; const char* particle; const char* sound;
                  const char* attr; double amount; uint8_t op; };
    // amounts typed as in MobEffects.java: float literals widen to double (0.2F != 0.2)
    const Spec specs[] = {
        {"speed", &eff::speed, EC_BENEFICIAL, 3402751, false, nullptr, nullptr, "movement_speed", double(0.2f), 2},
        {"slowness", &eff::slowness, EC_HARMFUL, 9154528, false, nullptr, nullptr, "movement_speed", double(-0.15f), 2},
        {"haste", &eff::haste, EC_BENEFICIAL, 14270531, false, nullptr, nullptr, "attack_speed", double(0.1f), 2},
        {"mining_fatigue", &eff::mining_fatigue, EC_HARMFUL, 4866583, false, nullptr, nullptr, "attack_speed", double(-0.1f), 2},
        {"strength", &eff::strength, EC_BENEFICIAL, 16762624, false, nullptr, nullptr, "attack_damage", 3.0, 0},
        {"instant_health", &eff::instant_health, EC_BENEFICIAL, 16262179, true, nullptr, nullptr, nullptr, 0, 0},
        {"instant_damage", &eff::instant_damage, EC_HARMFUL, 11101546, true, nullptr, nullptr, nullptr, 0, 0},
        {"jump_boost", &eff::jump_boost, EC_BENEFICIAL, 16646020, false, nullptr, nullptr, "safe_fall_distance", 1.0, 0},
        {"nausea", &eff::nausea, EC_HARMFUL, 5578058, false, nullptr, nullptr, nullptr, 0, 0},
        {"regeneration", &eff::regeneration, EC_BENEFICIAL, 13458603, false, nullptr, nullptr, nullptr, 0, 0},
        {"resistance", &eff::resistance, EC_BENEFICIAL, 9520880, false, nullptr, nullptr, nullptr, 0, 0},
        {"fire_resistance", &eff::fire_resistance, EC_BENEFICIAL, 16750848, false, nullptr, nullptr, nullptr, 0, 0},
        {"water_breathing", &eff::water_breathing, EC_BENEFICIAL, 10017472, false, nullptr, nullptr, nullptr, 0, 0},
        {"invisibility", &eff::invisibility, EC_BENEFICIAL, 16185078, false, nullptr, nullptr, "waypoint_transmit_range", -1.0, 2},
        {"blindness", &eff::blindness, EC_HARMFUL, 2039587, false, nullptr, nullptr, nullptr, 0, 0},
        {"night_vision", &eff::night_vision, EC_BENEFICIAL, 12779366, false, nullptr, nullptr, nullptr, 0, 0},
        {"hunger", &eff::hunger, EC_HARMFUL, 5797459, false, nullptr, nullptr, nullptr, 0, 0},
        {"weakness", &eff::weakness, EC_HARMFUL, 4738376, false, nullptr, nullptr, "attack_damage", -4.0, 0},
        {"poison", &eff::poison, EC_HARMFUL, 8889187, false, nullptr, nullptr, nullptr, 0, 0},
        {"wither", &eff::wither, EC_HARMFUL, 7561558, false, nullptr, nullptr, nullptr, 0, 0},
        {"health_boost", &eff::health_boost, EC_BENEFICIAL, 16284963, false, nullptr, nullptr, "max_health", 4.0, 0},
        {"absorption", &eff::absorption, EC_BENEFICIAL, 2445989, false, nullptr, nullptr, "max_absorption", 4.0, 0},
        {"saturation", &eff::saturation, EC_BENEFICIAL, 16262179, true, nullptr, nullptr, nullptr, 0, 0},
        {"glowing", &eff::glowing, EC_NEUTRAL, 9740385, false, nullptr, nullptr, nullptr, 0, 0},
        {"levitation", &eff::levitation, EC_HARMFUL, 13565951, false, nullptr, nullptr, nullptr, 0, 0},
        {"luck", &eff::luck, EC_BENEFICIAL, 5882118, false, nullptr, nullptr, "luck", 1.0, 0},
        {"unluck", &eff::unluck, EC_HARMFUL, 12624973, false, nullptr, nullptr, "luck", -1.0, 0},
        {"slow_falling", &eff::slow_falling, EC_BENEFICIAL, 15978425, false, nullptr, nullptr, nullptr, 0, 0},
        {"conduit_power", &eff::conduit_power, EC_BENEFICIAL, 1950417, false, nullptr, nullptr, nullptr, 0, 0},
        {"dolphins_grace", &eff::dolphins_grace, EC_BENEFICIAL, 8954814, false, nullptr, nullptr, nullptr, 0, 0},
        {"bad_omen", &eff::bad_omen, EC_NEUTRAL, 745784, false, nullptr, "minecraft:event.mob_effect.bad_omen", nullptr, 0, 0},
        {"hero_of_the_village", &eff::hero_of_the_village, EC_BENEFICIAL, 4521796, false, nullptr, nullptr, nullptr, 0, 0},
        {"darkness", &eff::darkness, EC_HARMFUL, 2696993, false, nullptr, nullptr, nullptr, 0, 0},
        {"trial_omen", &eff::trial_omen, EC_NEUTRAL, 1484454, false, "trial_omen", "minecraft:event.mob_effect.trial_omen", nullptr, 0, 0},
        {"raid_omen", &eff::raid_omen, EC_NEUTRAL, 14565464, false, "raid_omen", "minecraft:event.mob_effect.raid_omen", nullptr, 0, 0},
        {"wind_charged", &eff::wind_charged, EC_HARMFUL, 12438015, false, "small_gust", nullptr, nullptr, 0, 0},
        {"weaving", &eff::weaving, EC_HARMFUL, 7891290, false, "item_cobweb", nullptr, nullptr, 0, 0},
        {"oozing", &eff::oozing, EC_HARMFUL, 10092451, false, "item_slime", nullptr, nullptr, 0, 0},
        {"infested", &eff::infested, EC_HARMFUL, 9214860, false, "infested", nullptr, nullptr, 0, 0},
        {"breath_of_the_nautilus", &eff::breath_of_the_nautilus, EC_BENEFICIAL, 65518, false, nullptr, nullptr, nullptr, 0, 0},
    };
    for (auto& s : specs) {
        int id = registry_index(*r, std::string("minecraft:") + s.n);
        if (id < 0) continue;
        *s.id = id;
        MobEffectDef& d = mob_effects[size_t(id)];
        d.name = s.n, d.category = s.cat, d.color = s.color, d.instant = s.instant;
        if (s.particle) d.particle = reg_id("particle_type", std::string("minecraft:") + s.particle);
        if (s.sound) d.sound = s.sound;
        if (s.attr && aid(s.attr) >= 0)
            d.mods.push_back({aid(s.attr), std::string("minecraft:effect.") + (std::string(s.n) == "invisibility" ? "waypoint_transmit_range_hide" : s.n), s.amount, s.op});
    }
}

// ---------- queries (MobEffectUtil) ----------
inline bool has_water_breathing(const LivingEntity& e) {
    return e.has_effect(eff::water_breathing) || e.has_effect(eff::conduit_power) || e.has_effect(eff::breath_of_the_nautilus);
}
inline bool effects_refill_air(const LivingEntity& e) {
    return !e.has_effect(eff::breath_of_the_nautilus) || e.has_effect(eff::water_breathing) || e.has_effect(eff::conduit_power);
}
inline bool has_dig_speed(const LivingEntity& e) { return e.has_effect(eff::haste) || e.has_effect(eff::conduit_power); }
inline int dig_speed_amp(const LivingEntity& e) {
    const EffectInstance* a = e.effect(eff::haste);
    const EffectInstance* b = e.effect(eff::conduit_power);
    return std::max(a ? a->amp : 0, b ? b->amp : 0);
}
inline double LivingEntity::effective_gravity() const {
    return vel.y <= 0.0 && has_effect(eff::slow_falling) ? std::min(gravity(), 0.01) : gravity();
}
inline float LivingEntity::jump_boost_power() const {
    const EffectInstance* j = effect(eff::jump_boost);
    return j ? 0.1f * (float(j->amp) + 1.0f) : 0.0f;
}

// ---------- packets ----------
inline Writer update_effect_packet(int eid, const EffectInstance& e, bool blend) {  // ClientboundUpdateMobEffectPacket
    uint8_t f = uint8_t((e.ambient ? 1 : 0) | (e.visible ? 2 : 0) | (e.icon ? 4 : 0) | (blend ? 8 : 0));
    return std::move(Writer().varint(play_cb::update_mob_effect).varint(eid).varint(e.id).varint(e.amp).varint(e.duration).u8(f));
}
inline Writer remove_effect_packet(int eid, int id) {
    return std::move(Writer().varint(play_cb::remove_mob_effect).varint(eid).varint(id));
}

// ---------- attributes ----------
inline void refresh_dirty_attributes(LivingEntity& e, uint64_t) {  // onAttributeUpdated (max health / absorption clamps)
    if (e.health() > e.max_health()) e.set_health(e.max_health());
    if (e.absorption > float(e.attr(attr::max_absorption)))
        e.absorption = float(e.attr(attr::max_absorption));
}
inline void remove_effect_modifiers(LivingEntity& e, int id) {
    for (auto& m : mob_effects[size_t(id)].mods)
        if (e.has_attr(m.attr)) e.remove_attr_modifier(m.attr, m.id);
}
inline void add_effect_modifiers(LivingEntity& e, int id, int amp) {
    for (auto& m : mob_effects[size_t(id)].mods)
        if (e.has_attr(m.attr)) e.add_attr_modifier(m.attr, m.id, m.amount * (amp + 1), m.op);
}

// ---------- callbacks (LivingEntity / ServerPlayer onEffect*) ----------
inline void on_effect_added(LivingEntity& e, const EffectInstance& in, Entity* source = nullptr) {
    e.effects_dirty = true;
    add_effect_modifiers(e, in.id, in.amp);
    if (Player* p = e.as_player()) {
        p->send(update_effect_packet(e.id, in, true));
        progress::effects_changed(*p, source);
    }
}
inline void on_effect_updated(LivingEntity& e, const EffectInstance& in, bool refresh_attrs, Entity* source = nullptr) {
    e.effects_dirty = true;
    if (refresh_attrs) {
        uint64_t before = e.attr_dirty;
        remove_effect_modifiers(e, in.id);
        add_effect_modifiers(e, in.id, in.amp);
        refresh_dirty_attributes(e, before);
    }
    if (Player* p = e.as_player()) {
        p->send(update_effect_packet(e.id, in, false));
        progress::effects_changed(*p, source);
    }
}
inline void on_effects_removed(LivingEntity& e, const std::vector<EffectInstance>& gone) {
    e.effects_dirty = true;
    uint64_t before = e.attr_dirty;
    for (auto& in : gone) {
        remove_effect_modifiers(e, in.id);
        if (Player* p = e.as_player()) p->send(remove_effect_packet(e.id, in.id));
    }
    refresh_dirty_attributes(e, before);
    if (Player* p = e.as_player()) progress::effects_changed(*p, nullptr);
}

// ---------- per-effect behaviour (MobEffect subclasses) ----------
inline void set_absorption(LivingEntity& e, float v) { e.absorption = std::clamp(v, 0.0f, float(e.attr(attr::max_absorption))); }
inline bool inverted_heal_and_harm(const LivingEntity& e) { return e.in_tag("inverted_healing_and_harm"); }

// shouldApplyEffectTickThisTick
inline bool should_apply_tick(int id, int tick, int amp) {
    auto every = [&](int base) { int i = base >> amp; return i > 0 ? tick % i == 0 : true; };
    if (id == eff::regeneration) return every(50);
    if (id == eff::poison) return every(25);
    if (id == eff::wither) return every(40);
    if (id == eff::hunger || id == eff::absorption || id == eff::bad_omen) return true;
    if (id == eff::raid_omen) return tick == 1;
    if (mob_effects[size_t(id)].instant) return tick >= 1;
    return false;
}
inline void heal_living(LivingEntity& e, float a);  // below
// applyEffectTick: false removes the effect
inline bool apply_effect_tick(LivingEntity& e, int id, int amp) {
    if (id == eff::regeneration) {
        if (e.health() < e.max_health()) heal_living(e, 1.0f);
    } else if (id == eff::poison) {
        if (e.health() > 1.0f) e.hurt_server(DamageSource::of("magic"), 1.0f);
    } else if (id == eff::wither) {
        e.hurt_server(DamageSource::of("wither"), 1.0f);
    } else if (id == eff::hunger) {
        if (Player* p = e.as_player()) add_exhaustion(*p, 0.005f * float(amp + 1));
    } else if (id == eff::absorption) {
        return e.absorption > 0.0f;
    } else if (id == eff::saturation) {
        if (Player* p = e.as_player()) {  // FoodData.eat(nutrition, saturationModifier)
            p->g.food = std::clamp(p->g.food + amp + 1, 0, 20);
            p->g.saturation = std::clamp(p->g.saturation + float(amp + 1) * 1.0f * 2.0f, 0.0f, float(p->g.food));
        }
    } else if (id == eff::instant_health || id == eff::instant_damage) {
        if ((id == eff::instant_damage) == inverted_heal_and_harm(e)) heal_living(e, float(std::max(4 << amp, 0)));
        else e.hurt_server(DamageSource::of("magic"), float(6 << amp));
    }
    // ponytail: bad_omen -> raid_omen and raid_omen -> raid start need raids (not implemented); they just tick down.
    return true;
}
// MobEffect.applyInstantaneousEffect (HealOrHarm scales by distance; others = applyEffectTick)
inline void apply_instant(LivingEntity& e, int id, int amp, double scale, Entity* source, Entity* owner) {
    if (id == eff::instant_health || id == eff::instant_damage) {
        if ((id == eff::instant_damage) == inverted_heal_and_harm(e)) {
            heal_living(e, float(int(scale * double(4 << amp) + 0.5)));
        } else {
            int amount = int(scale * double(6 << amp) + 0.5);
            e.hurt_server(source ? DamageSource::of("indirect_magic", owner, source) : DamageSource::of("magic"), float(amount));
        }
        return;
    }
    apply_effect_tick(e, id, amp);
}
inline void on_effect_started(LivingEntity& e, int id, int amp) {
    if (id == eff::absorption) set_absorption(e, std::max(e.absorption, float(4 * (1 + amp))));
}
inline void on_mob_removed_killed(LivingEntity& e, int id) {  // onMobRemoved(KILLED)
    if (id == eff::weaving && (e.as_player() || rule("mob_griefing"))) {
        // WeavingMobEffect.spawnCobwebsRandomlyAround: up to 2-3 cobwebs in a random walk of 15 positions
        int want = rand_between(2, 3);
        std::vector<std::array<int, 3>> picked;
        int bx = e.block_x(), by = e.block_y(), bz = e.block_z();
        for (int i = 0; i < 15 && int(picked.size()) < want; i++) {  // BlockPos.randomInCube(random, 15, pos, 1)
            int x = bx + rand_between(-1, 1), y = by + rand_between(-1, 1), z = bz + rand_between(-1, 1);
            std::array<int, 3> p{x, y, z};
            if (std::find(picked.begin(), picked.end(), p) != picked.end()) continue;
            // ponytail: isFaceSturdy(UP) approximated by a full collision box below
            int below = block_at(x, y - 1, z);
            if (block_of(block_at(x, y, z)).replaceable && has_collision(below) && shape_end(below) - shape_begin(below) == 1 &&
                shape_begin(below)->y1 >= 1.0)
                picked.push_back(p);
        }
        for (auto& p : picked) {
            change_block(p[0], p[1], p[2], blocks::cobweb);
            level_event(3018, p[0], p[1], p[2], 0, nullptr);
        }
    } else if (id == eff::oozing) {
        int want = 2, cram = rule("max_entity_cramming");
        int nearby = 0;
        for_each_entity_in(e.bb.inflate(2.0), [&](Entity& s) { if (&s != &e && s.type == entity_types::slime) nearby++; }, false);
        int n = cram < 1 ? want : std::clamp(cram - std::min(nearby, cram), 0, want);
        for (int i = 0; i < n; i++) {
            auto s = create_entity(entity_types::slime);
            if (!s) break;  // slime not implemented on this branch
            s->snap_to(e.pos.x, e.pos.y + 0.5, e.pos.z, rand_float() * 360.0f, 0);
            Nbt tag = Nbt::compound();
            tag.set("Size", Nbt::num(Nbt::Int, 1));  // setSize(2): stored size - 1
            s->load_extra(tag);
            add_entity(std::move(s));
        }
    }
    // ponytail: wind_charged's burst (Level.explode with the wind charge calculator) is not ported.
}
inline void on_mob_hurt(LivingEntity& e, int id) {  // InfestedMobEffect.onMobHurt
    if (id != eff::infested || rand_float() > 0.1f) return;
    int n = rand_between(1, 2);
    for (int i = 0; i < n; i++) {
        auto s = create_entity(entity_types::silverfish);
        if (!s) return;
        float ra = -float(M_PI / 2) + rand_float() * float(M_PI);  // Mth.randomBetween(-pi/2, pi/2)
        double lx = -mth::sin(e.yaw * mth::DEG) * mth::cos(e.pitch * mth::DEG), ly = -mth::sin(e.pitch * mth::DEG),
               lz = mth::cos(e.yaw * mth::DEG) * mth::cos(e.pitch * mth::DEG);
        float vx = float(lx) * 0.3f, vy = float(ly) * 0.3f * 1.5f, vz = float(lz) * 0.3f;
        float c = std::cos(ra), sn = std::sin(ra);  // Vector3f.rotateY
        s->snap_to(e.pos.x, e.pos.y + e.height / 2.0, e.pos.z, rand_float() * 360.0f, 0);
        s->vel = {double(vx * c + vz * sn), double(vy), double(-vx * sn + vz * c)};
        Entity* added = add_entity(std::move(s));
        added->play_sound("minecraft:entity.silverfish.hurt");
    }
}

// ---------- LivingEntity API ----------
inline void heal_living(LivingEntity& e, float a) {
    if (Player* p = e.as_player()) { heal(*p, a); return; }
    e.heal(a);
}
inline bool can_be_affected(const LivingEntity& e, int id) {  // canBeAffected
    if (e.in_tag("immune_to_infested")) return id != eff::infested;
    if (e.in_tag("immune_to_oozing")) return id != eff::oozing;
    if (!e.in_tag("ignores_poison_and_regen")) return true;
    return id != eff::regeneration && id != eff::poison;
}
inline EffectInstance* find_effect(LivingEntity& e, int id) {
    for (auto& x : e.effects()) if (x.id == id) return &x;
    return nullptr;
}
inline bool add_effect(LivingEntity& e, const EffectInstance& in, Entity* source = nullptr) {  // addEffect
    if (in.id < 0 || size_t(in.id) >= mob_effects.size() || !can_be_affected(e, in.id)) return false;
    bool changed = false;
    if (EffectInstance* cur = find_effect(e, in.id)) {
        if (cur->update(in)) on_effect_updated(e, *cur, true, source), changed = true;
    } else {
        e.effects().push_back(in);
        on_effect_added(e, in, source);
        changed = true;
        if (!mob_effects[size_t(in.id)].sound.empty())  // onEffectAdded: soundOnAdded
            play_sound(mob_effects[size_t(in.id)].sound, e.sound_source(), e.pos.x, e.pos.y, e.pos.z, 1.0f, 1.0f);
    }
    on_effect_started(e, in.id, in.amp);
    return changed;
}
inline void force_add_effect(LivingEntity& e, const EffectInstance& in) {  // forceAddEffect
    if (!can_be_affected(e, in.id)) return;
    if (EffectInstance* cur = find_effect(e, in.id)) { *cur = in; on_effect_updated(e, *cur, true); }
    else { e.effects().push_back(in); on_effect_added(e, in); }
}
inline bool remove_effect(LivingEntity& e, int id) {
    auto& v = e.effects();
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].id == id) {
            std::vector<EffectInstance> gone;
            gone.push_back(std::move(v[i]));
            v.erase(v.begin() + std::ptrdiff_t(i));
            on_effects_removed(e, gone);
            return true;
        }
    return false;
}
inline bool remove_all_effects(LivingEntity& e) {
    auto& v = e.effects();
    if (v.empty()) return false;
    std::vector<EffectInstance> gone = std::move(v);
    v.clear();
    on_effects_removed(e, gone);
    return true;
}
inline void trigger_on_death_effects(LivingEntity& e) {  // triggerOnDeathMobEffects (KILLED)
    std::vector<EffectInstance> v = std::move(e.effects());
    e.effects().clear();
    for (auto& x : v) on_mob_removed_killed(e, x.id);
}
inline void effects_on_hurt(LivingEntity& e) {
    for (size_t i = 0; i < e.effects().size(); i++) on_mob_hurt(e, e.effects()[i].id);
}

// updateDirtyEffects: DATA_EFFECT_PARTICLES / DATA_EFFECT_AMBIENCE_ID, invisible + glowing shared flags
inline std::vector<uint8_t> effect_particles_bytes(const std::vector<EffectInstance>& v) {
    static const int entity_effect = reg_id("particle_type", "minecraft:entity_effect");
    Writer w;
    int n = 0;
    for (auto& x : v) n += x.visible;
    w.varint(n);
    for (auto& x : v) {
        if (!x.visible) continue;
        const MobEffectDef& d = mob_effects[size_t(x.id)];
        if (d.particle >= 0) { w.varint(d.particle); continue; }
        int alpha = x.ambient ? 38 : 255;  // AMBIENT_ALPHA = floor(38.25F)
        w.varint(entity_effect).i32(int32_t(uint32_t(alpha) << 24 | (uint32_t(d.color) & 0xFFFFFF)));
    }
    return std::move(w.b);
}
inline bool all_effects_ambient(const std::vector<EffectInstance>& v) {
    for (auto& x : v) if (x.visible && !x.ambient) return false;
    return true;
}
inline void update_dirty_effects(LivingEntity& e) {
    if (!e.effects_dirty) return;
    e.effects_dirty = false;
    auto& v = e.effects();
    bool invisible = !v.empty() && e.has_effect(eff::invisibility);
    bool glowing = e.has_effect(eff::glowing) || e.glowing_tag;
    if (Player* p = e.as_player())  // players: netplay's shared-flags sync carries these too
        p->g.effect_flags = uint8_t((invisible ? 0x20 : 0) | (glowing ? 0x40 : 0));
    e.set_shared_flag(5, invisible);
    if (e.shared_flag(6) != glowing) e.set_shared_flag(6, glowing);
    e.data.set_raw(edata::LivingEntity::EFFECT_PARTICLES, edser::PARTICLES, v.empty() ? std::vector<uint8_t>{0} : effect_particles_bytes(v));
    e.data.set_bool(edata::LivingEntity::EFFECT_AMBIENCE, !v.empty() && all_effects_ambient(v));
}

inline void tick_effects(LivingEntity& e) {  // LivingEntity.tickEffects (server side)
    auto& v = e.effects();
    for (size_t i = 0; i < v.size();) {
        EffectInstance& x = v[i];
        bool keep;
        // MobEffectInstance.tickServer
        if (!(x.infinite() || x.duration > 0)) keep = false;
        else {
            int tc = x.infinite() ? e.tick_count : x.duration;
            if (should_apply_tick(x.id, tc, x.amp) && !apply_effect_tick(e, x.id, x.amp)) keep = false;
            else {
                // apply_effect_tick may add/remove effects (damage -> death); re-find by id
                EffectInstance* y = find_effect(e, x.id);
                if (!y) continue;
                y->tick_down();
                if (y->downgrade_to_hidden()) on_effect_updated(e, *y, true);
                keep = y->infinite() || y->duration > 0;
            }
        }
        auto& vv = e.effects();
        if (i >= vv.size()) break;
        if (!keep) {
            std::vector<EffectInstance> gone;
            gone.push_back(std::move(vv[i]));
            vv.erase(vv.begin() + std::ptrdiff_t(i));
            on_effects_removed(e, gone);
            continue;
        }
        if (vv[i].duration % 600 == 0) on_effect_updated(e, vv[i], false);
        i++;
    }
    update_dirty_effects(e);
}

// getDamageAfterMagicAbsorb: resistance, then enchantment protection
inline float enchant_damage_protection(LivingEntity& e, const DamageSource& s);  // src/effects/enchant.hpp
inline float damage_after_magic_absorb(LivingEntity& e, const DamageSource& s, float dmg) {
    if (s.is(DT_BYPASSES_EFFECTS)) return dmg;
    if (const EffectInstance* r = e.effect(eff::resistance); r && !s.is(DT_BYPASSES_RESISTANCE)) {
        int absorb = 25 - (r->amp + 1) * 5;
        float v = dmg * float(absorb);
        dmg = std::max(v / 25.0f, 0.0f);
    }
    if (dmg <= 0.0f) return 0.0f;
    if (s.is(DT_BYPASSES_ENCHANTMENTS)) return dmg;
    float prot = enchant_damage_protection(e, s);
    if (prot > 0.0f) dmg = dmg * (1.0f - std::clamp(prot, 0.0f, 20.0f) / 25.0f);  // CombatRules.getDamageAfterMagicAbsorb
    return dmg;
}

// ---------- persistence: "active_effects" (MobEffectInstance.CODEC) ----------
inline Nbt effect_to_nbt(const EffectInstance& x, bool with_id = true) {
    Nbt c = Nbt::compound();
    if (with_id) c.set("id", Nbt::str("minecraft:" + mob_effects[size_t(x.id)].name));
    c.set("amplifier", Nbt::num(Nbt::Byte, x.amp));
    c.set("duration", Nbt::num(Nbt::Int, x.duration));
    c.set("ambient", Nbt::num(Nbt::Byte, x.ambient));
    c.set("show_particles", Nbt::num(Nbt::Byte, x.visible));
    c.set("show_icon", Nbt::num(Nbt::Byte, x.icon));
    if (x.hidden) c.set("hidden_effect", effect_to_nbt(*x.hidden, false));
    return c;
}
inline bool effect_from_nbt(const Nbt& c, EffectInstance& x, int id = -1) {
    if (id < 0) {
        const Nbt* n = c.get("id");
        if (!n || n->t != Nbt::String || (id = effect_id(n->s)) < 0) return false;
    }
    x = EffectInstance(id, int(nbt_num(c, "duration", 0)), int(nbt_num(c, "amplifier", 0)) & 255, nbt_num(c, "ambient", 0) != 0,
                       nbt_num(c, "show_particles", 1) != 0);
    x.icon = c.get("show_icon") ? nbt_num(c, "show_icon", 1) != 0 : x.visible;
    if (const Nbt* h = c.get("hidden_effect"); h && h->t == Nbt::Compound) {
        x.hidden = std::make_unique<EffectInstance>();
        effect_from_nbt(*h, *x.hidden, id);
    }
    return true;
}
inline void save_effects(const LivingEntity& e, Nbt& o) {
    if (e.effects().empty()) return;
    Nbt l = Nbt::make_list();
    for (auto& x : e.effects()) l.list.push_back(effect_to_nbt(x));
    o.set("active_effects", l);
}
inline void load_effects(LivingEntity& e, const Nbt& in) {
    e.effects().clear();
    if (const Nbt* l = in.get("active_effects"); l && l->t == Nbt::List)
        for (auto& c : l->list) {
            EffectInstance x;
            if (effect_from_nbt(c, x)) e.effects().push_back(std::move(x));
        }
    e.effects_dirty = true;
    // attribute modifiers are saved with the attributes (vanilla does the same), nothing to reapply
}
#include "components.hpp"
#include "enchant.hpp"
