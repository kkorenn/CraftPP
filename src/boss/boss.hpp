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

// ---------- persistence: level.dat keys ender-dragon-fight, boss-entities, seen-credits ----------
inline std::string entities_state() {
    std::ostringstream o;
    for (auto& e : entities) {
        if (e->removed) continue;
        if (auto* c = dynamic_cast<EndCrystal*>(e.get())) {
            o << "c " << c->x << ' ' << c->y << ' ' << c->z << ' ' << c->yaw << ' ' << c->invulnerable << ' ' << c->show_bottom << ' '
              << bool(c->beam);
            if (c->beam) o << ' ' << (*c->beam)[0] << ' ' << (*c->beam)[1] << ' ' << (*c->beam)[2];
            o << ';';
        } else if (auto* d = dynamic_cast<EnderDragon*>(e.get())) {
            o << "d " << d->x << ' ' << d->y << ' ' << d->z << ' ' << d->yaw << ' ' << d->health << ' ' << int(d->current->phase()) << ' '
              << d->death_time << ' ' << d->sitting_damage << ' ' << (fight && fight->dragon_eid == d->eid) << ';';
        }
    }
    return o.str();
}
inline void load_entities(const std::string& s) {
    std::istringstream all(s);
    for (std::string item; std::getline(all, item, ';');) {
        std::istringstream in(item);
        std::string k;
        double x, y, z;
        float yaw;
        in >> k >> x >> y >> z >> yaw;
        if (k == "c") {
            auto& c = add<EndCrystal>(x, y, z);
            int hb = 0;
            c.yaw = yaw;
            in >> c.invulnerable >> c.show_bottom >> hb;
            if (hb) {
                std::array<int, 3> b{};
                in >> b[0] >> b[1] >> b[2];
                c.beam = b;
            }
        } else if (k == "d") {
            auto& d = add<EnderDragon>(x, y, z);
            int ph = 0, owned = 0;
            d.yaw = yaw;
            in >> d.health >> ph >> d.death_time >> d.sitting_damage >> owned;
            d.set_phase(Phase(std::clamp(ph, 0, int(PHASE_COUNT) - 1)));
            if (fight && owned) fight->dragon_eid = d.eid, std::copy(fight->origin, fight->origin + 3, d.fight_origin);
        }
    }
}
inline void save_state() {
    std::string ents = entities_state(), f = fight ? fight->save() : "";
    std::string credits;
    for (auto& u : seen_credits) credits += u + ",";
    std::string all = ents + "|" + f + "|" + credits;
    if (all == saved_state) return;
    saved_state = all;
    std::map<std::string, std::string> kv{{"boss-entities", ents}, {"seen-credits", credits}};
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

inline void attack_entity(Player& p, Entity& e, int eid) {
    if (p.gamemode == 3 || p.g.dead) return;
    Dmg d;
    d.type = "player_attack", d.player = &p;
    float dmg = player_attack_damage(p);
    int part = -1;
    if (e.type == entity_types::ender_dragon && eid != e.eid) part = eid - e.eid - 1;
    e.hurt(d, dmg, part);
}

// EndCrystalItem.useOn: returns true when it handled the click.
inline bool use_end_crystal(Player& p, int hand, int x, int y, int z) {
    ItemStack& s = p.inv[hand == 1 ? 40u : size_t(p.selected)];
    if (s.empty() || s.item != item_by_name["minecraft:end_crystal"]) return false;
    int b = get_block(x, y, z);
    if (b != blocks::obsidian && b != blocks::bedrock) return true;  // InteractionResult.FAIL
    if (!is_air(get_block(x, y + 1, z))) return true;
    AABB box{double(x), double(y + 1), double(z), x + 1.0, y + 3.0, z + 1.0};
    for (auto& e : entities)
        if (!e->removed && e->box().intersects(box)) return true;
    for (auto& q : players)
        if (q->gamemode != 3 && !q->g.dead && player_box(*q).intersects(box)) return true;
    auto& c = add<EndCrystal>(x + 0.5, y + 1, z + 0.5);
    c.show_bottom = false;
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
    for (auto& e : item_entities) std::erase(e.watchers, p.eid);
    forget_player(p);
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
inline Entity* summon(const std::string& id, double x, double y, double z) {
    std::string t = short_id(id);
    if (t == "ender_dragon") {
        auto& d = add<EnderDragon>(x, y, z);
        if (fight) std::copy(fight->origin, fight->origin + 3, d.fight_origin);
        return &d;
    }
    if (t == "end_crystal") return &add<EndCrystal>(x, y, z);
    if (t == "dragon_fireball") return &add<DragonFireball>(x, y, z);
    if (t == "area_effect_cloud") return &add<AreaEffectCloud>(x, y, z);
    if (t == "experience_orb") return &add<ExperienceOrb>(x, y, z, 0);
    return nullptr;
}

inline std::vector<Entity*> select_entities(const Selector& sel, const Source& src) {
    std::vector<Entity*> out;
    if (!sel.includes_entities || sel.self || sel.by_name || sel.by_uuid) return out;
    double ox = sel.x.value_or(src.x), oy = sel.y.value_or(src.y), oz = sel.z.value_or(src.z);
    for (auto& e : entities) {
        if (e->removed) continue;
        bool ok = true;
        for (auto& [t, neg] : sel.types)
            if ((short_id(t) == e->type_name()) == neg) ok = false;
        if (!sel.names.empty() || !sel.tags.empty() || !sel.teams.empty() || !sel.gamemodes.empty() || !sel.level.any()) ok = false;
        if (!sel.distance.any() && !sel.distance.matches(std::sqrt(e->pos().dist_sqr(ox, oy, oz)))) ok = false;
        if (ok) out.push_back(e.get());
    }
    if (sel.sort == Selector::NEAREST)
        std::sort(out.begin(), out.end(), [&](Entity* a, Entity* b) { return a->pos().dist_sqr(ox, oy, oz) < b->pos().dist_sqr(ox, oy, oz); });
    if (int(out.size()) > sel.limit) out.resize(size_t(sel.limit));
    return out;
}

inline void register_commands() {
    reg("summon", [](Cmd& c) {
        std::string id = c.str("entity");
        Vec3 at = c.has("pos") ? Vec3{c.vec("pos").x, c.vec("pos").y, c.vec("pos").z} : Vec3{c.src.x, c.src.y, c.src.z};
        if (at.y < MIN_Y - 2048 || at.y >= MIN_Y + HEIGHT + 2048) throw err("commands.summon.invalidPosition");
        Entity* e = summon(id, at.x, at.y, at.z);
        if (!e) throw fail(not_supported("summon " + id));
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
        for (auto* e : ents) e->kill();
        for (auto& p : ps) kill(*p, "generic_kill");
        size_t n = ents.size() + ps.size();
        if (n == 1) c.success(tr("commands.kill.success.single", {tr("entity.minecraft." + ents[0]->type_name())}), true);
        else c.success(tr("commands.kill.success.multiple", {int(n)}), true);
        return int(n);
    });
}

// ---------- tick ----------
inline void tick() {
    if (fight) fight->tick();
    for (size_t i = 0; i < entities.size(); i++)
        if (!entities[i]->removed) entities[i]->tick();  // index: ticks may add entities
    for (auto& p : players) portal_checks(*p);
    track();
    if (server_tick % 100 == 0) save_state();
}

inline void on_leave(Player& p) {
    forget_player(p);
    if (fight) std::erase(fight->bar_players, p.eid);
    won_game.erase(p.eid);
    last_attack.erase(p.eid), gateway_cooldown.erase(p.eid), xp_taken.erase(p.eid);
}

inline void init() {
    explosion_entities = explosion_hits_entities;
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
    if (kv.count("boss-entities")) load_entities(kv["boss-entities"]);
    if (kv.count("seen-credits")) {
        std::istringstream in(kv["seen-credits"]);
        for (std::string u; std::getline(in, u, ',');)
            if (!u.empty()) seen_credits.insert(u);
    }
    saved_state = kv["boss-entities"] + "|" + kv["ender-dragon-fight"] + "|" + kv["seen-credits"];  // what is on disk
    on_tick(tick);
    on_player_leave(on_leave);
    register_commands();

    auto chain = [](int id) { auto it = handlers().find(id); return it == handlers().end() ? PacketHandler{} : it->second; };
    on(play_sb::attack, [prev = chain(play_sb::attack)](Player& p, Reader& in, int id) {
        Reader copy = in;
        int eid = in.varint();
        if (Entity* e = find(eid)) return attack_entity(p, *e, eid);
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
        bool was_dead = p.g.dead;
        if (prev) prev(p, copy, id);
        if (was_dead && !p.g.dead) forget_player(p);  // a death respawn makes the client drop its entities too
    });
}

}  // namespace boss
