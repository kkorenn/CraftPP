#pragma once
// Boss fights entry point (included from game/game.hpp; boss::init() runs after the game handlers so it can
// wrap them). The End fight lives on the level whose dimension is the End.
// ponytail: until the dimensions branch lands, `debug-end-fight=true` in server.properties runs the fight in
// the only level (the overworld), which is how the bot tests drive it.
#include "fight.hpp"

namespace boss {

inline std::set<std::string> seen_credits;  // player uuids (ServerPlayer.seenCredits)
inline std::set<int> won_game;               // player eids waiting for PERFORM_RESPAWN after the credits
inline std::map<int, int64_t> last_attack, gateway_cooldown;
inline std::string saved_state;

// ---------- persistence: level.dat keys ender-dragon-fight, seen-credits (entities: mob framework chunks) ----------
inline void save_state() {
    std::string f = fight ? fight->save() : "";
    std::string credits;
    for (auto& u : seen_credits) credits += u + ",";
    std::string all = f + "|" + credits;
    if (all == saved_state) return;
    saved_state = all;
    std::map<std::string, std::string> kv{{"seen-credits", credits}};
    if (fight) kv["ender-dragon-fight"] = f;
    write_level_data(kv);
}

// ---------- player actions ----------
inline float player_attack_damage(Player& p) {  // Player.attack: attribute damage x attack strength, crits
    const ItemStack& h = held(p);
    float base = 1.0f + (h.empty() ? 0.0f : item_info[size_t(h.item)].attack_damage);
    float speed = 4.0f + (h.empty() ? 0.0f : item_info[size_t(h.item)].attack_speed);
    float delay = 1.0f / speed * 20.0f;
    auto it = last_attack.find(p.eid);
    float ticker = it == last_attack.end() ? 1e9f : float(server_tick - it->second);
    float scale = std::clamp((ticker + 0.5f) / delay, 0.0f, 1.0f);
    float dmg = base * (0.2f + scale * scale * 0.8f);
    if (scale > 0.9f && p.g.fall_distance > 0 && !p.on_ground && !p.g.sprinting) dmg *= 1.5f;  // ponytail: no climbing/water/blindness checks
    last_attack[p.eid] = server_tick;
    return dmg;
}

// Level.getEntityOrPart for our entities: the dragon's parts are ids id+1..id+8.
inline BossEntity* find_target(int eid, int& part) {
    part = -1;
    if (auto* b = dynamic_cast<BossEntity*>(entity_by_id(eid)); b && !b->removed) return b;
    for (auto* d : all_of<EnderDragon>())
        if (eid > d->id && eid <= d->id + 8) return part = eid - d->id - 1, d;
    return nullptr;
}
inline void attack_entity(Player& p, BossEntity& e, int part) {
    if (p.gamemode == 3 || p.g.dead) return;
    float dmg = player_attack_damage(p);
    e.hurt_by(DamageSource::of("player_attack"), &p, dmg, part);
}

// EndCrystalItem.useOn: returns true when it handled the click.
inline bool use_end_crystal(Player& p, int hand, int x, int y, int z) {
    ItemStack& s = p.inv[hand == 1 ? 40u : size_t(p.selected)];
    if (s.empty() || s.item != item_by_name["minecraft:end_crystal"]) return false;
    int b = get_block(x, y, z);
    if (b != blocks::obsidian && b != blocks::bedrock) return true;  // InteractionResult.FAIL
    if (!is_air(get_block(x, y + 1, z))) return true;
    AABB box{double(x), double(y + 1), double(z), x + 1.0, y + 3.0, z + 1.0};
    bool blocked = false;
    for_each_entity_in(box, [&](::Entity&) { blocked = true; }, false);
    for (auto& q : players)
        if (q->gamemode != 3 && !q->g.dead && player_box(*q).intersects(box)) blocked = true;
    if (blocked) return true;
    auto c = std::make_unique<EndCrystal>();
    c->snap_to(x + 0.5, y + 1, z + 0.5, 0, 0);
    c->set_show_bottom(false);
    add_entity(std::move(c));
    if (fight) fight->try_respawn();
    if (p.gamemode != 1 && --s.count <= 0) s = {};
    send_slot(p, hand == 1 ? 40 : p.selected);  // client predicted the use; resync the stack
    return true;
}

// ServerPlayer.showEndCredits / EndPortalBlock (fromEnd) for players.
inline void respawn_after_win(Player& p) {  // PlayerList.respawn(player, keepAllPlayerData = true)
    place_new_player(p);  // ponytail: no beds yet, so always the world spawn
    p.send(Writer().varint(play_cb::respawn).varint(dimension_types::overworld).str("minecraft:overworld").i64(0)
               .varint(p.gamemode).varint(0).boolean(false).boolean(flat_world).boolean(false).varint(0).varint(63).u8(1));
    p.send(Writer().varint(play_cb::game_event).u8(13).f32(0));
    update_view(p, int(std::floor(p.x)) >> 4, int(std::floor(p.z)) >> 4, true);
    p.send(Writer().varint(play_cb::player_position).varint(0)
               .f64(p.x).f64(p.y).f64(p.z).f64(0).f64(0).f64(0).f32(p.yaw).f32(p.pitch).i32(0));
    p.send(Writer().varint(play_cb::set_held_slot).varint(p.selected));
    send_health(p);
    send_experience(p);
    entities_reset_view(p);
    broadcast(remove_entities_packet({p.eid}), &p);
    broadcast(spawn_entity(p), &p);
    for (auto& q : players)
        if (q.get() != &p) p.send(spawn_entity(*q));
}
inline void enter_end_portal(Player& p) {
    if (won_game.count(p.eid)) return;
    if (!seen_credits.count(player_uuid(p))) {
        won_game.insert(p.eid);
        seen_credits.insert(player_uuid(p));
        p.send(Writer().varint(play_cb::game_event).u8(4).f32(0.0f));  // WIN_GAME
        log_info("%s entered the exit portal: credits", p.name.c_str());
    } else {
        won_game.insert(p.eid);  // ponytail: portal travel back is the same respawn as after the credits
        respawn_after_win(p);
        won_game.erase(p.eid);
    }
}

inline void portal_checks(Player& p) {
    if (!fight || p.g.dead || p.gamemode == 3) return;
    AABB pb = player_box(p);
    int x0 = int(std::floor(pb.x0)), x1 = int(std::floor(pb.x1)), y0 = int(std::floor(pb.y0)), y1 = int(std::floor(pb.y1));
    int z0 = int(std::floor(pb.z0)), z1 = int(std::floor(pb.z1));
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++) {
                int s = get_block(x, y, z);
                if (s == blocks::end_portal && pb.intersects({double(x), y + 6 / 16.0, double(z), x + 1.0, y + 12 / 16.0, z + 1.0}))
                    return enter_end_portal(p);
                if (s != blocks::end_gateway) continue;
                if (server_tick - gateway_cooldown[p.eid] < 20) continue;  // ponytail: player portal cooldown, not per-gateway beam timer
                for (size_t i = 0; i < fight->placed_gateways.size(); i++) {
                    auto& g = fight->placed_gateways[i];
                    if (g.x != x || g.y != y || g.z != z) continue;
                    auto t = gateway_exit(i);
                    gateway_cooldown[p.eid] = server_tick;
                    p.x = t[0] + 0.5, p.y = t[1], p.z = t[2] + 0.5;
                    p.send(Writer().varint(play_cb::player_position).varint(0)
                               .f64(p.x).f64(p.y).f64(p.z).f64(0).f64(0).f64(0).f32(p.yaw).f32(p.pitch).i32(0x18));  // keep rotation
                    update_view(p, int(std::floor(p.x)) >> 4, int(std::floor(p.z)) >> 4, false);
                    return;
                }
            }
}

// ---------- commands: /summon for boss entities, entity-aware /kill ----------
inline std::unique_ptr<::Entity> make_crystal() { return std::make_unique<EndCrystal>(); }
inline std::unique_ptr<::Entity> make_fireball() { return std::make_unique<DragonFireball>(); }
inline std::unique_ptr<::Entity> make_cloud() { return std::make_unique<AreaEffectCloud>(); }
inline std::unique_ptr<::Entity> make_orb() { return std::make_unique<ExperienceOrb>(); }

// ponytail: until mobs-core's summon_entity covers these types, /summon spawns them here.
inline ::Entity* summon(const std::string& id, double x, double y, double z) {
    int t = entity_type_id(id);
    if (t != entity_types::ender_dragon && t != entity_types::end_crystal && t != entity_types::dragon_fireball &&
        t != entity_types::area_effect_cloud && t != entity_types::experience_orb)
        return nullptr;
    auto e = create_entity(t);
    if (!e) return nullptr;
    if (auto* d = dynamic_cast<EnderDragon*>(e.get()); d && fight) std::copy(fight->origin, fight->origin + 3, d->fight_origin);
    e->snap_to(x, y, z, 0, 0);
    return add_entity(std::move(e));
}

inline std::vector<::Entity*> select_entities(const Selector& sel, const Source& src) {
    std::vector<::Entity*> out;
    if (!sel.includes_entities || sel.self || sel.by_name || sel.by_uuid) return out;
    Vec3 o{sel.x.value_or(src.x), sel.y.value_or(src.y), sel.z.value_or(src.z)};
    for_each_entity([&](::Entity& e) {
        if (e.as_player()) return;
        bool ok = true;
        for (auto& [t, neg] : sel.types)
            if ((entity_type_id(t) == e.type) == neg) ok = false;
        if (!sel.names.empty() || !sel.tags.empty() || !sel.teams.empty() || !sel.gamemodes.empty() || !sel.level.any()) ok = false;
        if (!sel.distance.any() && !sel.distance.matches(std::sqrt(e.pos.dist_sqr(o)))) ok = false;
        if (ok) out.push_back(&e);
    });
    if (sel.sort == Selector::NEAREST)
        std::sort(out.begin(), out.end(), [&](::Entity* a, ::Entity* b) { return a->pos.dist_sqr(o) < b->pos.dist_sqr(o); });
    if (int(out.size()) > sel.limit) out.resize(size_t(sel.limit));
    return out;
}

inline void register_commands() {
    auto prev_summon = command_handlers()["summon"];
    reg("summon", [prev_summon](Cmd& c) {
        std::string id = c.str("entity");
        Vec3 at = c.has("pos") ? c.vec("pos") : Vec3{c.src.x, c.src.y, c.src.z};
        if (at.y < MIN_Y - 2048 || at.y >= MIN_Y + HEIGHT + 2048) throw err("commands.summon.invalidPosition");
        ::Entity* e = summon(id, at.x, at.y, at.z);
        if (!e) {
            if (prev_summon) return prev_summon(c);
            throw fail(not_supported("summon " + id));
        }
        c.success(tr("commands.summon.success", {tr("entity.minecraft." + short_id(id))}), true);
        return 1;
    });
    auto prev = command_handlers()["kill"];
    reg("kill", [prev](Cmd& c) {
        if (!c.has("targets")) return prev(c);
        auto& sel = c.get<Selector>("targets");
        auto ents = select_entities(sel, c.src);
        if (ents.empty()) return prev(c);
        auto ps = select(sel, c.src);
        int type0 = ents[0]->type;
        for (auto* e : ents) e->kill();
        for (auto& p : ps) kill(*p, "generic_kill");
        size_t n = ents.size() + ps.size();
        if (n == 1) c.success(tr("commands.kill.success.single", {tr("entity.minecraft." + entity_type_info[size_t(type0)].id)}), true);
        else c.success(tr("commands.kill.success.multiple", {int(n)}), true);
        return int(n);
    });
}

// ---------- tick ----------
inline void tick() {  // before the entity tick (on_tick order: game_init's hooks run first, then ours)
    if (fight) {
        fight->tick();
        // EnderDragonFight's DRAGON ticket (radius 9): keep the arena's entity chunks loaded while it has players
        if (!fight->bar_players.empty() && server_tick % 20 == 0)
            for (int cx = -8; cx <= 8; cx++)
                for (int cz = -8; cz <= 8; cz++) ensure_entity_chunk_loaded(cx, cz);
    }
    for (auto& p : players) portal_checks(*p);
    if (server_tick % 100 == 0) save_state();
}

inline void on_leave(Player& p) {
    if (fight) std::erase(fight->bar_players, p.eid);
    won_game.erase(p.eid);
    last_attack.erase(p.eid), gateway_cooldown.erase(p.eid), xp_taken.erase(p.eid);
}

inline void init() {
    explosion_entities = explosion_hits_entities;
    register_entity_factory(entity_types::ender_dragon, make_dragon);
    register_entity_factory(entity_types::end_crystal, make_crystal);
    register_entity_factory(entity_types::dragon_fireball, make_fireball);
    if (!create_entity(entity_types::area_effect_cloud)) register_entity_factory(entity_types::area_effect_cloud, make_cloud);
    if (!create_entity(entity_types::experience_orb)) register_entity_factory(entity_types::experience_orb, make_orb);
    auto kv = read_level_data();
    if (props.count("debug-end-fight") && props["debug-end-fight"] == "true") {
        fight = new DragonFight();
        if (kv.count("ender-dragon-fight")) fight->load(kv["ender-dragon-fight"]);
        else {  // no End generator yet: a stand-in main island (top y 60) and the pillars (EndSpikeFeature)
            for (int x = -48; x <= 48; x++)
                for (int z = -48; z <= 48; z++)
                    for (int y = 52; y <= 60 && x * x + z * z <= 48 * 48; y++) boss_level().set_block(x, y, z, blocks::end_stone);
            for (auto& s : endgen::spikes_for_seed(level_seed())) DragonFight::place_spike_with_crystal(s, false, std::nullopt);
        }
        fight->init(level_seed());
    }
    if (kv.count("seen-credits")) {
        std::istringstream in(kv["seen-credits"]);
        for (std::string u; std::getline(in, u, ',');)
            if (!u.empty()) seen_credits.insert(u);
    }
    saved_state = kv["ender-dragon-fight"] + "|" + kv["seen-credits"];  // what is on disk
    on_tick(tick);
    on_player_leave(on_leave);
    register_commands();

    auto chain = [](int id) { auto it = handlers().find(id); return it == handlers().end() ? PacketHandler{} : it->second; };
    on(play_sb::attack, [prev = chain(play_sb::attack)](Player& p, Reader& in, int id) {
        Reader copy = in;
        int part;
        if (BossEntity* e = find_target(in.varint(), part)) return attack_entity(p, *e, part);
        if (prev) prev(p, copy, id);
    });
    on(play_sb::use_item_on, [prev = chain(play_sb::use_item_on)](Player& p, Reader& in, int id) {
        Reader copy = in;
        int hand = in.varint();
        int x, y, z;
        read_block_pos(in, x, y, z);
        if (!p.g.dead && p.gamemode != 3 && use_end_crystal(p, hand, x, y, z)) {
            in.varint(); in.f32(); in.f32(); in.f32(); in.u8(); in.u8();
            p.send(Writer().varint(play_cb::block_changed_ack).varint(in.varint()));
            return;
        }
        if (prev) prev(p, copy, id);
    });
    on(play_sb::client_command, [prev = chain(play_sb::client_command)](Player& p, Reader& in, int id) {
        Reader copy = in;
        if (in.varint() == 0 && won_game.count(p.eid)) {  // PERFORM_RESPAWN after the credits
            won_game.erase(p.eid);
            return respawn_after_win(p);
        }
        if (prev) prev(p, copy, id);
    });
}

}  // namespace boss
