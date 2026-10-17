// Mob module translation unit: registers the framework's entity types, hooks it into the tick, handles the
// combat/interaction packets and implements the api.hpp / command entry points. Overview: mobs.hpp.
#include "mobs.hpp"

#ifdef __APPLE__
#include <mach/mach.h>
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
static long heap_in_use_kib() {  // allocator bytes in use (exact, unlike RSS)
#ifdef __APPLE__
    malloc_statistics_t st{};
    malloc_zone_statistics(nullptr, &st);
    return long(st.size_in_use / 1024);
#elif defined(__GLIBC__)
    return long(mallinfo2().uordblks / 1024);
#else
    return 0;
#endif
}

static long current_rss_kib() {  // resident set now (rss_kib() in tick.hpp is the peak)
#ifdef __APPLE__
    mach_task_basic_info info{};
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return 0;
    return long(info.resident_size / 1024);
#else
    long pages = 0, res = 0;
    if (FILE* f = fopen("/proc/self/statm", "r")) { if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0; fclose(f); }
    return res * (sysconf(_SC_PAGESIZE) / 1024);
#endif
}

// ---------- registration ----------
static void register_core_types() {
    register_entity_factory(entity_types::item, make_entity<ItemEntity>);
    register_entity_factory(entity_types::experience_orb, make_entity<ExperienceOrb>);
    register_entity_factory(entity_types::arrow, make_entity<Arrow>);
    register_entity_factory(entity_types::cow, make_entity<Cow>);
    register_entity_factory(entity_types::pig, make_entity<Pig>);
    register_entity_factory(entity_types::sheep, make_entity<Sheep>);
    register_entity_factory(entity_types::chicken, make_entity<Chicken>);
    register_entity_factory(entity_types::zombie, make_entity<Zombie>);
    register_entity_factory(entity_types::skeleton, make_entity<Skeleton>);
    register_entity_factory(entity_types::spider, make_entity<Spider>);
    register_entity_factory(entity_types::creeper, make_entity<Creeper>);
}

// ---------- per-player combat state ----------
struct PlayerCombat {
    int attack_ticker = 0;
    int last_item = -2;
    Vec3 last_pos{0, 0, 0}, known_movement{0, 0, 0};
};
static std::unordered_map<const Player*, PlayerCombat> combat;

static void join_hook(Player& p) {
    auto e = std::make_unique<PlayerEntity>(&p);
    e->id = p.eid;
    e->uuid_hi = p.uuid_hi, e->uuid_lo = p.uuid_lo;
    e->set_size(0.6f, 1.8f, 1.62f);
    e->sync();
    add_player_entity(std::move(e));
    combat[&p] = {};
}
static void leave_hook(Player& p) {
    if (PlayerEntity* pe = player_entity(p)) {
        pe->removed = true;
        free_entity(*pe);  // now: the Player object may be gone by the end of the tick
    }
    combat.erase(&p);
    take_xp_delay.erase(&p);
}
static void pre_tick() {
    path_budget_left = path_budget_per_tick;
    for (Entity* e : player_entities()) {
        auto* pe = static_cast<PlayerEntity*>(e);
        pe->sync();
        auto& c = combat[pe->p];
        c.known_movement = pe->pos - c.last_pos;
        c.last_pos = pe->pos;
        c.attack_ticker++;
        int item = pe->main_hand().empty() ? -1 : pe->main_hand().item;
        if (item != c.last_item) c.attack_ticker = 0, c.last_item = item;  // resetAttackStrengthTicker on item change
        int& d = take_xp_delay[pe->p];
        if (d > 0) d--;
    }
}
static void post_tick() {
    if (cur_level != &worlds[DIM_OVERWORLD]) return;  // ponytail: nether/end mob spawning not wired yet
    tick_fresh_chunks();
    tick_natural_spawning();
    static bool dbg = getenv("CRAFTPP_MOBDEBUG"), dbg_verbose = dbg && std::string(getenv("CRAFTPP_MOBDEBUG")) == "2";
    if (dbg && server_tick % 40 == 0) {
        fprintf(stderr, "[mobs] tick %lld entities %zu active %zu mspt %.3f rss %ld KiB heap %ld KiB path nodes %lld searches %lld misses %lld\n",
                (long long)server_tick, entity_count(), ent_detail::to_tick.size(), mspt_avg, current_rss_kib(), heap_in_use_kib(), (long long)path_nodes_total,
                (long long)path_searches_total, (long long)path_budget_misses);
        if (dbg_verbose)
            for (Entity* e : player_entities())
                fprintf(stderr, "  player %d pos %.2f %.2f %.2f alive %d gm %d health %.1f\n", e->id, e->pos.x, e->pos.y, e->pos.z,
                        e->is_alive(), e->as_player()->gamemode, e->as_living()->health());
        if (dbg_verbose) for (Entity* e : ent_detail::to_tick)
            if (Mob* m = e->as_mob()) {
                std::string run;
                for (auto& w : m->goal_selector.goals) if (w.running) run += std::string(typeid(*w.goal).name()) + " ";
                fprintf(stderr, "  %s %d pos %.2f %.2f %.2f yaw %.1f head %.1f ground %d nav %d goals[%s] watchers %zu\n", e->info().id.c_str(), e->id,
                        e->pos.x, e->pos.y, e->pos.z, e->yaw, m->y_head_rot, e->on_ground, !m->navigation->is_done(), run.c_str(), e->trk.watchers.size());
            }
    }
}

// ---------- Player.attack ----------
static float attack_strength(const Player& p, PlayerEntity& pe, float a) {
    float delay = float(1.0 / pe.attr(attr::attack_speed) * 20.0);
    return std::clamp((float(combat[&p].attack_ticker) + a) / delay, 0.0f, 1.0f);
}
static void server_sound(Player& p, const char* s) { play_sound(s, SRC_PLAYERS, p.x, p.y, p.z, 1.0f, 1.0f); }
static void player_attack(Player& p, Entity& t) {
    PlayerEntity* pe = player_entity(p);
    if (!pe || !t.is_attackable()) return;
    float base = float(pe->attr(attr::attack_damage));
    float scale = attack_strength(p, *pe, 0.5f);
    base *= 0.2f + scale * scale * 0.8f;
    combat[&p].attack_ticker = 0;  // onAttack
    if (base <= 0) return;
    bool full = scale > 0.9f, knock = false;
    if (p.g.sprinting && full) { server_sound(p, "minecraft:entity.player.attack.knockback"); knock = true; }
    bool crit = full && p.g.fall_distance > 0.0 && !p.on_ground && !pe->on_climbable() && !pe->is_in_water() && t.as_living() && !p.g.sprinting;
    if (crit) base *= 1.5f;
    bool sweep = false;
    if (full && !crit && !knock && p.on_ground) {
        double sp = pe->attr(attr::movement_speed) * 2.5;
        if (combat[&p].known_movement.horizontal_sqr() < sp * sp) sweep = item_in_tag(pe->main_hand().item, "swords");
    }
    DamageSource ds = DamageSource::of("player_attack", pe);
    if (!t.hurt_server(ds, base)) { server_sound(p, "minecraft:entity.player.attack.nodamage"); return; }
    float kb = float(pe->attr(attr::attack_knockback)) / 2.0f + (knock ? 0.5f : 0.0f);
    if (kb > 0 && t.as_living()) {
        t.as_living()->knockback(kb, mth::sin(p.yaw * mth::DEG), -mth::cos(p.yaw * mth::DEG));
        if (knock) p.g.sprinting = false;
    }
    if (sweep) {  // doSweepAttack
        server_sound(p, "minecraft:entity.player.attack.sweep");
        float sd = 1.0f + float(pe->attr(attr::sweeping_damage_ratio)) * base;
        std::vector<LivingEntity*> near;
        for_each_entity_in(t.bb.inflate(1.0, 0.25, 1.0), [&](Entity& e) {
            if (&e == pe || &e == &t || !e.as_living() || pe->dist_sqr(e) >= 9.0) return;
            near.push_back(e.as_living());
        });
        for (LivingEntity* l : near)
            if (l->hurt_server(ds, sd * scale)) l->knockback(0.4f, mth::sin(p.yaw * mth::DEG), -mth::cos(p.yaw * mth::DEG));
        double dx = -mth::sin(p.yaw * mth::DEG), dz = mth::cos(p.yaw * mth::DEG);
        Writer w;
        w.varint(play_cb::level_particles).boolean(false).boolean(false).f64(p.x + dx).f64(pe->y_at(0.5)).f64(p.z + dz)
            .f32(float(dx)).f32(0).f32(float(dz)).f32(0).i32(0).varint(reg_id("particle_type", "sweep_attack"));
        broadcast_near(p.x, p.z, w);
    }
    if (crit) {
        server_sound(p, "minecraft:entity.player.attack.crit");
        Writer w = std::move(Writer().varint(play_cb::animate).varint(t.id).u8(4));
        broadcast_near(t.pos.x, t.pos.z, w);
    }
    if (!crit && !sweep) server_sound(p, full ? "minecraft:entity.player.attack.strong" : "minecraft:entity.player.attack.weak");
    pe->set_last_hurt_mob(t);
    if (survival_like(p)) {  // itemAttackInteraction: weapon wear (item_damage_per_attack)
        ItemStack& w = p.inv[size_t(p.selected)];
        if (!w.empty()) {
            auto c = item_combat.find(w.item);
            int maxd = item_info[size_t(w.item)].max_damage;
            if (c != item_combat.end() && c->second.weapon_damage > 0 && maxd > 0 && (w.damage += c->second.weapon_damage) >= maxd) {
                w = {};
                broadcast_near(p.x, p.z, entity_event_packet(p.eid, 47));
            }
        }
    }
    add_exhaustion(p, 0.1f);
}
static bool within_entity_range(Player& p, const Entity& t, double buffer) {  // isWithinEntityInteractionRange
    double r = 3.0 + buffer;
    return t.bb.distance_sqr({p.x, p.y + 1.62, p.z}) < r * r;
}
static void register_handlers() {
    on(play_sb::attack, [](Player& p, Reader& in, int) {
        int id = in.varint();
        if (!p.g.loaded || p.gamemode == 3 || p.g.dead) return;
        Entity* t = entity_by_id(id);
        if (getenv("CRAFTPP_MOBDEBUG")) fprintf(stderr, "[mobs] attack %s -> %d found %d range %d\n", p.name.c_str(), id, t != nullptr, t ? within_entity_range(p, *t, 3.0) : 0);
        if (!t || t->removed || t->as_player() == &p || !within_entity_range(p, *t, 3.0)) return;
        if (t->type == entity_types::item || t->type == entity_types::experience_orb || t->type == entity_types::arrow) return;
        player_attack(p, *t);
    });
    on(play_sb::interact, [](Player& p, Reader& in, int) {
        int id = in.varint();
        int hand = in.varint();
        read_lpvec3(in);
        p.g.sneaking = in.u8() != 0;
        Entity* t = entity_by_id(id);
        if (!t || t->removed || p.g.dead || p.gamemode == 3 || !within_entity_range(p, *t, 3.0)) return;
        if (t->interact(p, hand) == IR_SUCCESS)
            broadcast_near(p.x, p.z, std::move(Writer().varint(play_cb::animate).varint(p.eid).u8(uint8_t(hand ? 3 : 0))), &p);
    });
    on(play_sb::pick_item_from_entity, [](Player& p, Reader& in, int) {  // handlePickItemFromEntity
        int id = in.varint();
        in.u8();  // includeData: only for avatars (mannequins)
        Entity* t = entity_by_id(id);
        if (!t || t->removed || !within_entity_range(p, *t, 3.0)) return;
        int item = -1;
        if (t->as_mob()) item = item_id((t->info().id + "_spawn_egg").c_str());  // Mob.getPickResult: spawn egg
        else if (t->type == entity_types::item) item = static_cast<ItemEntity*>(t)->stack.item;
        else if (t->type == entity_types::arrow) item = item_id("arrow");
        if (item >= 0) pick_block(p, item);  // tryPickItem
    });
    on(play_sb::punch, [](Player& p, Reader&, int) {  // handlePunch: swing + resetAttackStrengthTicker
        combat[&p].attack_ticker = 0;
        broadcast_near(p.x, p.z, std::move(Writer().varint(play_cb::animate).varint(p.eid).u8(0)), &p);
    });
}

// ---------- /summon ----------
Entity* summon_entity(int type, double x, double y, double z, const Nbt* tag, std::string& err) {
    if (type < 0 || size_t(type) >= entity_type_info.size() || !entity_type_info[size_t(type)].summon) { err = "commands.summon.failed"; return nullptr; }
    if (std::abs(x) > 3.0E7 || std::abs(z) > 3.0E7 || y < MIN_Y - 2048 || y > MIN_Y + HEIGHT + 2048) { err = "commands.summon.invalidPosition"; return nullptr; }
    if (difficulty() == 0 && !entity_type_info[size_t(type)].peaceful) { err = "commands.summon.failed.peaceful"; return nullptr; }
    auto e = create_entity(type);
    if (!e) { err = "commands.summon.failed"; return nullptr; }
    e->snap_to(x, y, z, 0, 0);
    if (tag && !tag->comp.empty()) {
        e->load(*tag);
        e->snap_to(x, y, z, e->yaw, e->pitch);
        if (entity_by_uuid(e->uuid_hi, e->uuid_lo)) { err = "commands.summon.failed.uuid"; return nullptr; }
    } else if (Mob* m = e->as_mob()) {
        m->finalize_spawn(SpawnReason::COMMAND, nullptr);
    }
    Entity* r = add_entity(std::move(e));
    mark_entity_sync(*r);
    return r;
}

// ---------- selector / command entity operations ----------
Text entity_display_name(const Entity* e) {
    if (e->has_custom_name()) return lit(e->custom_name);
    return tr("entity.minecraft." + e->info().id);
}
std::string entity_name_string(const Entity* e) {  // getScoreboardName for non-players: the UUID
    return e->has_custom_name() ? e->custom_name : uuid_dashed(e->uuid_hi, e->uuid_lo);
}
Vec3 entity_position(const Entity* e) { return e->pos; }
World* entity_level(const Entity* e) { return e->level(); }
void entity_rotation(const Entity* e, float& yaw, float& pitch) { yaw = e->yaw, pitch = e->pitch; }
double entity_eye_height(const Entity* e) { return e->eye_height; }
void entity_kill(Entity* e) { e->kill(); }
bool entity_hurt(Entity* e, float amount, const std::string& type) { return e->hurt_server(DamageSource::of(type), amount); }
void entity_teleport(Entity* e, double x, double y, double z, const float* yaw, const float* pitch) {
    if (yaw) e->yaw = *yaw;
    if (pitch) e->pitch = *pitch;
    if (LivingEntity* l = e->as_living(); l && yaw) l->y_head_rot = l->y_body_rot = *yaw;
    e->teleport_to(x, y, z);
    if (Mob* m = e->as_mob()) m->navigation->stop();
    mark_entity_sync(*e);
}
void select_entity_candidates(const Selector& sel, const Source& src, double ox, double oy, double oz, std::vector<Entity*>& out) {
    if (!sel.gamemodes.empty() || !sel.level.any() || !sel.predicates.empty() || !sel.score_ranges.empty()) return;  // players only
    for (auto& [t, neg] : sel.teams) if (!neg || t.empty()) return;  // mobs are on no team
    auto test = [&](Entity& e) {
        if (e.as_player() || e.removed) return false;
        for (auto& [v, neg] : sel.names) if ((e.custom_name == v) == neg) return false;
        for (auto& [t, neg] : sel.types) {
            bool m = t[0] == '#' ? e.in_tag(t.substr(t.find(':') + 1)) : t == "minecraft:" + e.info().id;
            if (m == neg) return false;
        }
        for (auto& [t, neg] : sel.tags) {
            bool m = t.empty() ? !e.tags.empty() : std::find(e.tags.begin(), e.tags.end(), t) != e.tags.end();
            if (m == neg) return false;
        }
        if (!sel.xrot.any() && !sel.xrot.matches(e.pitch)) return false;
        if (!sel.yrot.any() && !sel.yrot.matches(mth::wrap_degrees(double(e.yaw)))) return false;
        if (sel.dx || sel.dy || sel.dz) {
            double dx = sel.dx.value_or(0), dy = sel.dy.value_or(0), dz = sel.dz.value_or(0);
            AABB box{std::min(ox, ox + dx), std::min(oy, oy + dy), std::min(oz, oz + dz),
                     std::max(ox, ox + dx) + 1, std::max(oy, oy + dy) + 1, std::max(oz, oz + dz) + 1};
            if (!e.bb.intersects(box)) return false;
        }
        if (!sel.distance.any() && !sel.distance.matches(std::sqrt(e.dist_sqr(ox, oy, oz)))) return false;
        return true;
    };
    if (sel.self) {
        if (src.mob && test(*src.mob)) out.push_back(src.mob);
        return;
    }
    if (sel.by_uuid) {
        for_each_entity([&](Entity& e) {
            if (!e.as_player() && strcasecmp(uuid_dashed(e.uuid_hi, e.uuid_lo).c_str(), sel.name.c_str()) == 0) out.push_back(&e);
        });
        return;
    }
    for_each_entity([&](Entity& e) { if (test(e)) out.push_back(&e); });
}

// ---------- api.hpp ----------
void spawn_xp_orbs(double x, double y, double z, int amount) { award_xp_orbs(x, y, z, amount); }
float player_damage_after_armor(Player& p, const DamageSource& s, float amount) {
    PlayerEntity* pe = player_entity(p);
    if (!pe || s.is(DT_BYPASSES_ARMOR)) return amount;
    if (survival_like(p)) {  // Player.hurtArmor -> doHurtEquipment(max(1, damage / 4)) on armour pieces
        int d = std::max(1, int(amount / 4.0f));
        for (int i = 36; i <= 39; i++) {
            ItemStack& a = p.inv[size_t(i)];
            if (a.empty() || item_info[size_t(a.item)].max_damage <= 0) continue;
            if ((a.damage += d) >= item_info[size_t(a.item)].max_damage) {
                a = {};
                broadcast_near(p.x, p.z, entity_event_packet(p.eid, 52 - (i - 36)));  // armour break (feet 52 .. head 49)
            }
        }
    }
    return pe->damage_after_armor(s, amount);
}
void player_knockback_from(Player& p, const DamageSource& s) {
    if (PlayerEntity* pe = player_entity(p)) pe->deal_default_knockback(s);
}
void mobs_save_all() { save_all_entities(); }
void player_entity_hidden(Player& p) {
    PlayerEntity* pe = player_entity(p);
    if (!pe) return;
    std::vector<Player*> w = pe->trk.watchers;
    for (Player* q : w) track_remove(*pe, *q);
}
void player_entity_respawned(Player& p) {
    PlayerEntity* pe = player_entity(p);
    if (!pe) return;
    effects_player_respawned(p);
    std::vector<Player*> w = pe->trk.watchers;
    for (Player* q : w) track_remove(*pe, *q);
    pe->sync();
    pe->trk.base_x = enc4096(pe->pos.x), pe->trk.base_y = enc4096(pe->pos.y), pe->trk.base_z = enc4096(pe->pos.z);
    update_players(*pe);
}

void mobs_init() {
    level_switch_hooks.push_back(ent_detail::switch_level);
    if (!load_mob_data()) _exit(1);
    register_core_types();
    for (auto f : mob_registrations()) f();
    on_player_join(join_hook);
    on_player_leave(leave_hook);
    entity_pre_tick_hooks.push_back(pre_tick);
    entity_post_tick_hooks.push_back(post_tick);
    register_handlers();
    reg("summon", [](Cmd& c) {
        std::string id = c.str("entity");
        int type = entity_type_id(id);
        Vec3 pos = c.has("pos") ? c.vec("pos") : Vec3{c.src.x, c.src.y, c.src.z};
        const Nbt* tag = c.has("nbt") ? &c.get<Nbt>("nbt") : nullptr;
        std::string e_key;
        Entity* e = summon_entity(type, pos.x, pos.y, pos.z, tag, e_key);
        if (!e) throw err(e_key);
        c.success(tr("commands.summon.success", {entity_display_name(e)}), true);
        return 1;
    });
    // Freshly generated chunks get their chunk-generation creature spawns on the tick thread: the ones generated
    // so far (spawn area, never saved) and, through the wrapped generator, every later one. Runs before the accept
    // loop starts, so no worker is generating while the generator is replaced.
    {
        std::lock_guard lock(worlds[DIM_OVERWORLD].m);
        for (auto& [k, c] : worlds[DIM_OVERWORLD].chunks)
            if (!c->on_disk) fresh_chunks.emplace_back(k.first, k.second);
    }
    auto gen = worlds[DIM_OVERWORLD].generator;
    worlds[DIM_OVERWORLD].generator = [gen](Chunk& c) {
        gen(c);
        std::lock_guard lock(fresh_chunks_mtx);
        fresh_chunks.emplace_back(c.cx, c.cz);
    };
}
