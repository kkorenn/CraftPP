#pragma once
// EnderDragonFight + DragonRespawnStage: boss bar, crystal count, dragon (re)creation, exit portal / podium,
// egg, gateways (EndGatewayFeature + TheEndGatewayBlockEntity exits) and the 4-crystal respawn sequence.
#include "dragon.hpp"

#include <sstream>

namespace boss {

enum class Respawn { NONE, START, PREPARING_TO_SUMMON_PILLARS, SUMMONING_PILLARS, SUMMONING_DRAGON, END };
inline const char* RESPAWN_NAMES[] = {"", "start", "preparing_to_summon_pillars", "summoning_pillars", "summoning_dragon", "end"};

struct Gateway {
    int x, y, z;
    bool has_exit = false, exact = false;
    int ex = 0, ey = 0, ez = 0;
    int cooldown = 0;
    int64_t age = 0;
};

struct DragonFight {
    int origin[3] = {0, 0, 0};
    bool needs_state_scanning = true, dragon_killed = false, previously_killed = false;
    Respawn stage = Respawn::NONE;
    int respawn_time = 0;
    std::optional<std::array<int, 3>> exit_portal;
    std::vector<int> gateways;  // remaining gateway slots (spawnNewGateway pops the back)
    std::vector<int> respawn_crystals;  // eids
    int dragon_eid = -1;  // stands in for dragon_uuid (entities live only in memory + boss save)
    int ticks_since_dragon_seen = 0, alive_crystals = 0, ticks_since_crystals_scanned = 0, ticks_since_player_scan = 21;
    int64_t bar_uh = int64_t(game_rng()), bar_ul = int64_t(game_rng());
    float bar_progress = 1.0f;
    bool bar_visible = true;
    Text bar_name = tr("entity.minecraft.ender_dragon");
    std::vector<int> bar_players;  // eids
    std::vector<Gateway> placed_gateways;
    bool dirty = true;

    void init(int64_t seed) {
        if (gateways.empty()) gateways = endgen::gateways_for_seed(seed), dirty = true;
    }

    // ---------- boss bar (ServerBossEvent: pink, progress, boss music + world fog) ----------
    Writer bar_packet(int op) const {
        Writer w;
        w.varint(play_cb::boss_event).i64(bar_uh).i64(bar_ul).varint(op);
        if (op == 0) {
            write_nbt(w, bar_name);
            w.f32(bar_progress).varint(0).varint(0).u8(0x02 | 0x04);
        } else if (op == 2) w.f32(bar_progress);
        return w;
    }
    void bar_send(int op) {
        if (!bar_visible) return;
        for (int e : bar_players)
            if (Player* p = player_by_eid(e)) p->send(bar_packet(op));
    }
    void set_visible(bool v) {
        if (v == bar_visible) return;
        if (!v) bar_send(1);
        bar_visible = v;
        if (v) bar_send(0);
    }
    void set_progress(float f) {
        if (f == bar_progress) return;
        bar_progress = f;
        bar_send(2);
    }
    void update_players() {
        std::vector<int> now;
        for (auto& pp : players) {
            Player& p = *pp;
            if (p.g.dead) continue;  // ENTITY_STILL_ALIVE
            if (Vec3{p.x, p.y, p.z}.dist_sqr(origin[0], 128 + origin[1], origin[2]) > 192.0 * 192.0) continue;
            now.push_back(p.eid);
            if (std::find(bar_players.begin(), bar_players.end(), p.eid) == bar_players.end()) {
                bar_players.push_back(p.eid);
                if (bar_visible) p.send(bar_packet(0));
            }
        }
        std::erase_if(bar_players, [&](int e) {
            if (std::find(now.begin(), now.end(), e) != now.end()) return false;
            if (Player* p = player_by_eid(e); p && bar_visible) p->send(bar_packet(1));
            return true;
        });
    }

    EnderDragon* dragon() const {
        Entity* e = dragon_eid >= 0 ? find(dragon_eid) : nullptr;
        return e && e->type == entity_types::ender_dragon ? static_cast<EnderDragon*>(e) : nullptr;
    }

    void tick() {
        set_visible(!dragon_killed);
        if (++ticks_since_player_scan >= 20) update_players(), ticks_since_player_scan = 0;
        if (bar_players.empty()) return;  // chunk ticket removed; ponytail: chunks never unload here anyway
        if (needs_state_scanning) scan_state(), needs_state_scanning = false, dirty = true;
        if (stage != Respawn::NONE) {
            std::vector<EndCrystal*> cs;
            for (int e : respawn_crystals)
                if (Entity* c = find(e); c && c->type == entity_types::end_crystal) cs.push_back(static_cast<EndCrystal*>(c));
            if (cs.empty()) return abort_respawn();
            tick_respawn(cs, respawn_time++);
            dirty = true;
        }
        if (!dragon_killed) {
            if (dragon_eid < 0 || !dragon() || ++ticks_since_dragon_seen >= 1200) {
                if (!dragon()) find_or_create_dragon();
                ticks_since_dragon_seen = 0;
            }
            if (++ticks_since_crystals_scanned >= 100) update_crystal_count(), ticks_since_crystals_scanned = 0;
        }
    }

    // ponytail: hasActiveExitPortal looks at the podium column, not every block entity in 17x17 chunks
    bool has_active_exit_portal() {
        for (int x = origin[0] - 3; x <= origin[0] + 3; x++)
            for (int z = origin[2] - 3; z <= origin[2] + 3; z++)
                for (int y = MIN_Y; y < MIN_Y + HEIGHT; y++)
                    if (get_block(x, y, z) == blocks::end_portal) return true;
        return false;
    }
    void scan_state() {
        log_info("Scanning for legacy world dragon fight...");
        bool active = has_active_exit_portal();
        if (active) previously_killed = true;
        else {
            previously_killed = false;
            if (!find_exit_portal()) spawn_exit_portal(false);
        }
        auto ds = all_of<EnderDragon>();
        if (ds.empty()) dragon_killed = true;
        else {
            dragon_eid = ds[0]->eid;
            dragon_killed = false;
            if (!active) ds[0]->discard(), dragon_eid = -1;
        }
        if (!previously_killed && dragon_killed) dragon_killed = false;
        dirty = true;
    }
    void find_or_create_dragon() {
        auto ds = all_of<EnderDragon>();
        if (ds.empty()) create_new_dragon();
        else dragon_eid = ds[0]->eid, dirty = true;
    }
    EnderDragon* create_new_dragon() {
        auto& d = add<EnderDragon>(origin[0], 128 + origin[1], origin[2]);
        std::copy(origin, origin + 3, d.fight_origin);
        d.set_phase(HOLDING_PATTERN);
        d.yaw = rand_float() * 360.0f;
        dragon_eid = d.eid;
        dirty = true;
        return &d;
    }

    // exitPortalPattern match with its (3,3,3) cell at p: 3-high pillar, rim ring, 5x5 rounded base (all bedrock).
    static bool portal_pattern_at(int px, int py, int pz) {
        static const char* aisles[5][7] = {
            {"       ", "       ", "       ", "   #   ", "       ", "       ", "       "},
            {"       ", "       ", "       ", "   #   ", "       ", "       ", "       "},
            {"       ", "       ", "       ", "   #   ", "       ", "       ", "       "},
            {"  ###  ", " #   # ", "#     #", "#  #  #", "#     #", " #   # ", "  ###  "},
            {"       ", "  ###  ", " ##### ", " ##### ", " ##### ", "  ###  ", "       "}};
        for (int a = 0; a < 5; a++)
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 7; c++)
                    if (aisles[a][r][c] == '#' && get_block(px + c - 3, py + 3 - a, pz + r - 3) != blocks::bedrock) return false;
        return true;
    }
    // ponytail: vanilla first tries BlockPattern.find around every end portal block entity in the arena;
    // a podium is only ever generated at the origin column, so the column scan finds the same match.
    std::optional<std::array<int, 3>> find_exit_portal() {
        if (exit_portal && portal_pattern_at((*exit_portal)[0], (*exit_portal)[1], (*exit_portal)[2])) return exit_portal;
        int ox = origin[0], oz = origin[2];
        for (int y = heightmap(ox, oz, false); y >= MIN_Y; y--)
            if (portal_pattern_at(ox, y, oz)) {
                if (!exit_portal) exit_portal = std::array<int, 3>{ox, y, oz}, dirty = true;
                return std::array<int, 3>{ox, y, oz};
            }
        return std::nullopt;
    }
    void spawn_exit_portal(bool active) {
        if (!exit_portal) {
            int ox = origin[0], oz = origin[2];
            int y = heightmap(ox, oz, true) - 1;
            while (get_block(ox, y, oz) == blocks::bedrock && y > 63) y--;
            exit_portal = std::array<int, 3>{ox, std::max(MIN_Y + 1, y), oz};
            dirty = true;
        }
        auto& e = *exit_portal;
        endgen::place_podium(e[0], e[1], e[2], active, end_states(), get_block, set_block, [](int x, int y, int z) {
            int s = get_block(x, y, z);  // Level.destroyBlock(pos, true): drops + break particles
            std::vector<ItemStack> drops;
            if (!is_air(s)) {
                level_event(2001, x, y, z, s);
                block_drops(s, x, y, z, {}, drops);
                for (auto& d : drops) pop_resource(x, y, z, d);
            }
        });
    }

    void update_crystal_count() {
        ticks_since_crystals_scanned = 0;
        alive_crystals = 0;
        for (auto& s : endgen::spikes_for_seed(level_seed()))
            alive_crystals += int(in_box<EndCrystal>({double(s.x - s.radius), -2048.0, double(s.z - s.radius),
                                                      double(s.x + s.radius), 2048.0, double(s.z + s.radius)}).size());
    }

    void set_dragon_killed(EnderDragon& d) {
        if (d.eid != dragon_eid) return;
        set_progress(0.0f);
        set_visible(false);
        spawn_exit_portal(true);
        spawn_new_gateway();
        if (!previously_killed) set_block(origin[0], heightmap(origin[0], origin[2], false), origin[2], blocks::dragon_egg);
        previously_killed = dragon_killed = true;
        dirty = true;
    }
    void update_dragon(EnderDragon& d) {
        if (d.eid != dragon_eid) return;
        set_progress(d.health / d.max_health);
        ticks_since_dragon_seen = 0;
    }

    void spawn_new_gateway() {
        if (gateways.empty()) return;
        int g = gateways.back();
        gateways.pop_back();
        int x, y, z;
        endgen::gateway_pos(g, x, y, z);
        spawn_gateway(x, y, z, std::nullopt, false);
        dirty = true;
    }
    void spawn_gateway(int x, int y, int z, std::optional<std::array<int, 3>> exit, bool exact, bool event = true) {
        if (event) level_event(3000, x, y, z, 0);
        endgen::place_gateway(x, y, z, end_states(), set_block);
        Gateway gw{x, y, z};
        if (exit) gw.has_exit = true, gw.exact = exact, gw.ex = (*exit)[0], gw.ey = (*exit)[1], gw.ez = (*exit)[2];
        std::erase_if(placed_gateways, [&](const Gateway& o) { return o.x == x && o.y == y && o.z == z; });
        placed_gateways.push_back(gw);
        dirty = true;
    }

    void on_crystal_destroyed(EndCrystal& c, const Dmg& d) {
        if (stage != Respawn::NONE && std::find(respawn_crystals.begin(), respawn_crystals.end(), c.eid) != respawn_crystals.end())
            return abort_respawn();
        update_crystal_count();
        if (EnderDragon* dr = dragon()) dr->on_crystal_destroyed(c, d.player, d);
    }
    void abort_respawn() {
        stage = Respawn::NONE;
        respawn_time = 0;
        reset_spike_crystals();
        spawn_exit_portal(true);
        dirty = true;
    }
    void reset_spike_crystals() {
        for (auto& s : endgen::spikes_for_seed(level_seed()))
            for (auto* c : in_box<EndCrystal>({double(s.x - s.radius), -2048.0, double(s.z - s.radius), double(s.x + s.radius),
                                               2048.0, double(s.z + s.radius)}))
                c->invulnerable = false, c->set_beam(std::nullopt);
    }

    void try_respawn() {
        if (!dragon_killed || stage != Respawn::NONE) return;
        if (!exit_portal && !find_exit_portal()) spawn_exit_portal(true);
        auto& l = *exit_portal;
        std::vector<int> found;
        static const int dirs[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};  // north east south west
        for (auto& dir : dirs) {
            int bx = l[0] + dir[0] * 3, by = l[1] + 1, bz = l[2] + dir[1] * 3;
            auto cs = in_box<EndCrystal>({double(bx), double(by), double(bz), bx + 1.0, by + 1.0, bz + 1.0});
            if (cs.empty()) return;
            for (auto* c : cs) found.push_back(c->eid);
        }
        respawn_dragon(found);
    }
    void respawn_dragon(const std::vector<int>& crystals) {
        if (!dragon_killed || stage != Respawn::NONE) return;
        for (auto p = find_exit_portal(); p; p = find_exit_portal()) {
            static const char* rows[5] = {"pillar", "pillar", "pillar", "ring", "base"};
            (void)rows;
            for (int a = 0; a < 5; a++)
                for (int r = 0; r < 7; r++)
                    for (int c = 0; c < 7; c++) {
                        int x = (*p)[0] + c - 3, y = (*p)[1] + 3 - a, z = (*p)[2] + r - 3;
                        int s = get_block(x, y, z);
                        if (s == blocks::bedrock || s == blocks::end_portal) set_block(x, y, z, blocks::end_stone);
                    }
        }
        stage = Respawn::START;
        respawn_time = 0;
        spawn_exit_portal(false);
        respawn_crystals = crystals;
        dirty = true;
    }
    void set_stage(Respawn s) {
        respawn_time = 0;
        if (s == Respawn::END) {
            stage = Respawn::NONE;
            dragon_killed = false;
            create_new_dragon();
        } else stage = s;
        dirty = true;
    }
    void tick_respawn(std::vector<EndCrystal*>& cs, int time) {  // DragonRespawnStage.tick
        const std::array<int, 3> top{0, 128, 0};
        switch (stage) {
            case Respawn::START:
                for (auto* c : cs) c->set_beam(top);
                set_stage(Respawn::PREPARING_TO_SUMMON_PILLARS);
                break;
            case Respawn::PREPARING_TO_SUMMON_PILLARS:
                if (time < 100) {
                    if (time == 0 || time == 50 || time == 51 || time == 52 || time >= 95) level_event(3001, 0, 128, 0, 0);
                } else set_stage(Respawn::SUMMONING_PILLARS);
                break;
            case Respawn::SUMMONING_PILLARS: {
                bool start = time % 40 == 0, end = time % 40 == 39;
                if (!start && !end) break;
                auto spikes = endgen::spikes_for_seed(level_seed());
                size_t i = size_t(time / 40);
                if (i < spikes.size()) {
                    auto& s = spikes[i];
                    if (start) {
                        for (auto* c : cs) c->set_beam(std::array<int, 3>{s.x, s.height + 1, s.z});
                    } else {
                        for (int z = s.z - 10; z <= s.z + 10; z++)
                            for (int y = s.height - 10; y <= s.height + 10; y++)
                                for (int x = s.x - 10; x <= s.x + 10; x++) set_block(x, y, z, blocks::air);
                        explode(s.x + 0.5F, s.height, s.z + 0.5F, 5.0F, 1);
                        place_spike_with_crystal(s, true, std::array<int, 3>{0, 128, 0});
                    }
                } else if (start) set_stage(Respawn::SUMMONING_DRAGON);
                break;
            }
            case Respawn::SUMMONING_DRAGON:
                if (time >= 100) {
                    set_stage(Respawn::END);
                    reset_spike_crystals();
                    for (auto* c : cs) {
                        c->set_beam(std::nullopt);
                        explode(c->x, c->y, c->z, 6.0F, 0, false, c->eid);
                        c->discard();
                    }
                } else if (time >= 80) level_event(3001, 0, 128, 0, 0);
                else if (time == 0) for (auto* c : cs) c->set_beam(top);
                else if (time < 5) level_event(3001, 0, 128, 0, 0);
                break;
            default: break;
        }
    }

    // EndSpikeFeature.placeSpike with its crystal (worldgen and the respawn sequence).
    static void place_spike_with_crystal(const endgen::EndSpike& s, bool invulnerable, std::optional<std::array<int, 3>> beam) {
        endgen::place_spike(s, end_states(), MIN_Y, set_block);
        auto& c = add<EndCrystal>(s.x + 0.5, s.height + 1, s.z + 0.5);
        c.yaw = rand_float() * 360.0f;
        c.invulnerable = invulnerable;
        c.beam = beam;
    }

    // ---------- persistence (level.dat "ender-dragon-fight") ----------
    std::string save() const {
        std::ostringstream o;
        o << needs_state_scanning << ' ' << dragon_killed << ' ' << previously_killed << ' ' << int(stage) << ' ' << respawn_time << ' '
          << (exit_portal ? 1 : 0);
        if (exit_portal) o << ' ' << (*exit_portal)[0] << ' ' << (*exit_portal)[1] << ' ' << (*exit_portal)[2];
        o << ' ' << gateways.size();
        for (int g : gateways) o << ' ' << g;
        o << ' ' << placed_gateways.size();
        for (auto& g : placed_gateways) o << ' ' << g.x << ' ' << g.y << ' ' << g.z << ' ' << g.has_exit << ' ' << g.exact << ' ' << g.ex << ' ' << g.ey << ' ' << g.ez;
        return o.str();
    }
    void load(const std::string& s) {
        std::istringstream in(s);
        int st = 0, has = 0;
        size_t n = 0;
        in >> needs_state_scanning >> dragon_killed >> previously_killed >> st >> respawn_time >> has;
        stage = Respawn(st);
        if (has) {
            std::array<int, 3> e{};
            in >> e[0] >> e[1] >> e[2];
            exit_portal = e;
        }
        in >> n;
        gateways.resize(n);
        for (auto& g : gateways) in >> g;
        in >> n;
        placed_gateways.resize(n);
        for (auto& g : placed_gateways) in >> g.x >> g.y >> g.z >> g.has_exit >> g.exact >> g.ex >> g.ey >> g.ez;
        if (stage != Respawn::NONE) stage = Respawn::NONE;  // respawn crystals are not persisted: restart the sequence by hand
        dirty = false;
    }
};

inline bool fight_active() { return fight != nullptr; }
inline int fight_alive_crystals() { return fight ? fight->alive_crystals : 0; }
inline bool fight_previously_killed() { return fight && fight->previously_killed; }
inline void fight_update_dragon(EnderDragon& d) {
    if (fight) fight->update_dragon(d);
}
inline void fight_dragon_killed(EnderDragon& d) {
    if (fight) fight->set_dragon_killed(d);
}
inline void fight_crystal_destroyed(Entity& c, const Dmg& d) {
    if (fight) fight->on_crystal_destroyed(static_cast<EndCrystal&>(c), d);
}

// ---------- TheEndGatewayBlockEntity: exits to the outer islands ----------
inline bool full_block(int s) { return block_of(s).collision; }  // ponytail: isCollisionShapeFullBlock ~ has collision
inline std::array<int, 3> find_tallest_block(int ax, int ay, int az, int dist, bool allow_bedrock) {
    std::optional<std::array<int, 3>> t;
    for (int xd = -dist; xd <= dist; xd++)
        for (int zd = -dist; zd <= dist; zd++) {
            if (xd == 0 && zd == 0 && !allow_bedrock) continue;
            for (int y = MIN_Y + HEIGHT - 1; y > (t ? (*t)[1] : MIN_Y); y--) {
                int s = get_block(ax + xd, y, az + zd);
                if (full_block(s) && (allow_bedrock || s != blocks::bedrock)) { t = std::array<int, 3>{ax + xd, y, az + zd}; break; }
            }
        }
    return t ? *t : std::array<int, 3>{ax, ay, az};
}
inline bool chunk_empty(double x, double z) {
    int cx = Mth::floor(x / 16.0), cz = Mth::floor(z / 16.0);
    for (int bx = 0; bx < 16; bx++)
        for (int bz = 0; bz < 16; bz++)
            if (heightmap(cx * 16 + bx, cz * 16 + bz, false) > MIN_Y) return false;  // ponytail: any block, not just filled sections
    return true;
}
inline std::array<int, 3> gateway_exit(size_t gi) {  // getPortalPosition
    Gateway g = fight->placed_gateways[gi];
    if (!g.has_exit) {
        Vec3 dir = Vec3{double(g.x), 0.0, double(g.z)}.normalize();
        Vec3 t = dir.scale(1024.0);
        for (int lim = 16; !chunk_empty(t.x, t.z) && lim-- > 0;) t = t + dir.scale(-16.0);
        for (int lim = 16; chunk_empty(t.x, t.z) && lim-- > 0;) t = t + dir.scale(16.0);
        int cx = Mth::floor(t.x / 16.0), cz = Mth::floor(t.z / 16.0);
        std::optional<std::array<int, 3>> best;
        double bd = 0;
        for (int bz = cz * 16; bz < cz * 16 + 16; bz++)  // findValidSpawnInChunk
            for (int bx = cx * 16; bx < cx * 16 + 16; bx++)
                for (int by = 30; by < MIN_Y + HEIGHT - 2; by++)
                    if (get_block(bx, by, bz) == blocks::end_stone && !full_block(get_block(bx, by + 1, bz)) && !full_block(get_block(bx, by + 2, bz))) {
                        double d = (bx + 0.5) * (bx + 0.5) + (by + 0.5) * (by + 0.5) + (bz + 0.5) * (bz + 0.5);
                        if (!best || d < bd) best = std::array<int, 3>{bx, by, bz}, bd = d;
                    }
        std::array<int, 3> pos;
        if (!best) {  // EndIslandFeature at y 75
            pos = {Mth::floor(t.x + 0.5), 75, Mth::floor(t.z + 0.5)};
            LegacyRandomSource r(pack_pos(pos[0], pos[1], pos[2]));
            float size = r.nextInt(3) + 4.0F;
            for (int y = 0; size > 0.5F; y--) {
                for (int x = Mth::floor(-size); x <= Mth::d2i(std::ceil(size)); x++)
                    for (int z = Mth::floor(-size); z <= Mth::d2i(std::ceil(size)); z++)
                        if (x * x + z * z <= (size + 1.0F) * (size + 1.0F)) set_block(pos[0] + x, pos[1] + y, pos[2] + z, blocks::end_stone);
                size -= r.nextInt(2) + 0.5F;
            }
        } else pos = *best;
        pos = find_tallest_block(pos[0], pos[1], pos[2], 16, true);
        pos[1] += 10;
        fight->spawn_gateway(pos[0], pos[1], pos[2], std::array<int, 3>{g.x, g.y, g.z}, false, false);
        g.has_exit = true, g.ex = pos[0], g.ey = pos[1], g.ez = pos[2];
        for (auto& o : fight->placed_gateways)  // spawn_gateway may have moved the list
            if (o.x == g.x && o.y == g.y && o.z == g.z) o = g;
        fight->dirty = true;
    }
    if (g.exact) return {g.ex, g.ey, g.ez};
    auto t = find_tallest_block(g.ex, g.ey + 2, g.ez, 5, false);
    return {t[0], t[1] + 1, t[2]};
}

}  // namespace boss
