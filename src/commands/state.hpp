#pragma once
// World-level state owned by the command system: world clocks, game rules, difficulty, weather, spawn,
// world border, scoreboard/teams/tags, ops/bans/whitelist. Persisted to <world>/craftpp_level.snbt and
// the vanilla-format JSON lists (ops.json, banned-players.json, banned-ips.json, whitelist.json).
#include "../server.hpp"
#include "../worldgen/json.hpp"
#include "core.hpp"

#include <arpa/inet.h>
#include <ctime>
#include <random>
#include <set>
#include <sys/socket.h>

// One lock for all command/world-state mutation. Commands run on connection threads and the console thread.
// ponytail: global lock; becomes unnecessary once everything runs on the survival tick thread.
inline std::recursive_mutex cmd_mtx;
inline std::string server_dir = ".";  // where ops.json etc. live (directory of server.properties)
inline std::mt19937_64 cmd_rng{std::random_device{}()};

inline Writer pkt(int id) { Writer w; w.varint(id); return w; }
inline void send_text(Player& p, const Text& t, bool overlay = false) {
    Writer w = pkt(play_cb::system_chat);
    write_nbt(w, t);
    try { p.send(w.boolean(overlay)); } catch (...) {}
}
inline void broadcast_text(const Text& t) {
    printf("%s\n", plain(t).c_str());
    fflush(stdout);
    for (auto& p : snapshot()) send_text(*p, t);
}

// ---------- world clocks (ServerClockManager) ----------
struct WorldClock {
    const char* id;
    int64_t total = 0;
    float partial = 0, rate = 1;
    bool paused = false;
};
inline std::array<WorldClock, 2> clocks{{{"minecraft:overworld"}, {"minecraft:the_end"}}};  // registry order (sorted ids)
inline int64_t game_time = 0;
inline int64_t tick_count = 0;

inline int game_rule_index(const std::string& id) {
    std::string s = id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id;
    for (size_t i = 0; i < std::size(gen::GAME_RULES); i++)
        if (s == gen::GAME_RULES[i].id) return int(i);
    return -1;
}
inline std::vector<int> game_rule_values = [] {
    std::vector<int> v;
    for (auto& r : gen::GAME_RULES) v.push_back(r.def);
    return v;
}();
inline int rule(const char* id) { return game_rule_values[size_t(game_rule_index(id))]; }

inline Writer set_time_packet(int only_clock = -1) {  // only_clock: -2 = no clocks, -1 = all
    Writer w = pkt(play_cb::set_time);
    w.i64(game_time);
    int n = only_clock == -2 ? 0 : only_clock < 0 ? int(clocks.size()) : 1;
    w.varint(n);
    for (int i = 0; i < int(clocks.size()); i++) {
        if (only_clock == -2 || (only_clock >= 0 && i != only_clock)) continue;
        auto& c = clocks[size_t(i)];
        w.varint(i);  // holder by registry id
        int64_t v = c.total;  // VAR_LONG
        uint64_t u = uint64_t(v);
        do { uint8_t b = u & 0x7F; u >>= 7; w.u8(u ? b | 0x80 : b); } while (u);
        bool stopped = c.paused || !rule("advance_time");
        w.f32(c.partial).f32(stopped ? 0.f : c.rate);
    }
    return w;
}
inline void clock_changed(int i) { broadcast(set_time_packet(i)); }

// ---------- weather (ServerLevel.advanceWeatherCycle) ----------
struct Weather {
    int clear_time = 0, rain_time = 0, thunder_time = 0;
    bool raining = false, thundering = false;
    float rain_level = 0, thunder_level = 0;
};
inline Weather weather;
inline Writer game_event(int ev, float v) { return std::move(pkt(play_cb::game_event).u8(uint8_t(ev)).f32(v)); }
inline int uniform(int lo, int hi) { return lo + int(cmd_rng() % uint64_t(hi - lo + 1)); }
inline void set_weather(int clear, int rain, bool raining, bool thundering) {
    weather.clear_time = clear;
    weather.rain_time = rain;
    weather.thunder_time = rain;
    weather.raining = raining;
    weather.thundering = thundering;
}
inline void advance_weather() {
    auto& w = weather;
    bool was_raining = w.rain_level > 0.2f;
    if (rule("advance_weather")) {
        if (w.clear_time > 0) {
            w.clear_time--;
            w.thunder_time = w.thundering ? 0 : 1;
            w.rain_time = w.raining ? 0 : 1;
            w.thundering = w.raining = false;
        } else {
            if (w.thunder_time > 0) { if (--w.thunder_time == 0) w.thundering = !w.thundering; }
            else w.thunder_time = w.thundering ? uniform(3600, 15600) : uniform(12000, 180000);
            if (w.rain_time > 0) { if (--w.rain_time == 0) w.raining = !w.raining; }
            else w.rain_time = w.raining ? uniform(12000, 24000) : uniform(12000, 180000);
        }
    }
    float orain = w.rain_level, othunder = w.thunder_level;
    w.thunder_level = std::clamp(w.thunder_level + (w.thundering ? 0.01f : -0.01f), 0.f, 1.f);
    w.rain_level = std::clamp(w.rain_level + (w.raining ? 0.01f : -0.01f), 0.f, 1.f);
    if (orain != w.rain_level) broadcast(game_event(7, w.rain_level));
    if (othunder != w.thunder_level) broadcast(game_event(8, w.thunder_level));
    bool raining = w.rain_level > 0.2f;
    if (was_raining != raining) {
        broadcast(game_event(raining ? 1 : 2, 0));
        broadcast(game_event(7, w.rain_level));
        broadcast(game_event(8, w.thunder_level));
    }
}

// ---------- difficulty, spawn, border ----------
inline int difficulty = 2;  // peaceful easy normal hard
inline bool difficulty_locked = false;
inline const char* DIFFICULTY_NAMES[] = {"peaceful", "easy", "normal", "hard"};
inline Writer difficulty_packet() { return std::move(pkt(play_cb::change_difficulty).varint(difficulty).boolean(difficulty_locked)); }

struct Spawn { int x = 0, y = -60, z = 0; float yaw = 0, pitch = 0; };
inline Spawn world_spawn;
inline uint64_t pack_pos(int x, int y, int z) {
    return (uint64_t(int64_t(x) & 0x3FFFFFF) << 38) | (uint64_t(int64_t(z) & 0x3FFFFFF) << 12) | uint64_t(int64_t(y) & 0xFFF);
}
inline Writer spawn_packet() {
    return std::move(pkt(play_cb::set_default_spawn_position).str("minecraft:overworld")
                         .i64(int64_t(pack_pos(world_spawn.x, world_spawn.y, world_spawn.z)))
                         .f32(world_spawn.yaw).f32(world_spawn.pitch));
}

struct Border {
    double cx = 0, cz = 0, from = 59999968, to = 59999968;
    int64_t lerp_ms = 0, lerp_start = 0;  // wall-clock lerp (vanilla lerps over game ticks)
    double damage = 0.2, buffer = 5;
    int warn_dist = 5, warn_time = 15;
    double size() const {
        if (!lerp_ms) return to;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        double t = std::min(1.0, double(now - lerp_start) / double(lerp_ms));
        return from + (to - from) * t;
    }
    int64_t remaining_ms() const {
        if (!lerp_ms) return 0;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return std::max<int64_t>(0, lerp_start + lerp_ms - now);
    }
};
inline Border border;
inline void write_varlong(Writer& w, int64_t v) {
    uint64_t u = uint64_t(v);
    do { uint8_t b = u & 0x7F; u >>= 7; w.u8(u ? b | 0x80 : b); } while (u);
}
inline Writer border_init_packet() {
    Writer w = pkt(play_cb::initialize_border);
    w.f64(border.cx).f64(border.cz).f64(border.size()).f64(border.to);
    write_varlong(w, border.remaining_ms());
    return std::move(w.varint(29999984).varint(border.warn_dist).varint(border.warn_time));
}

// ---------- scoreboard ----------
struct NumberFormat { int type = -1; Nbt data; };  // -1 none, 0 blank, 1 styled(style), 2 fixed(component)
struct Objective {
    std::string name, criteria = "dummy";
    Text display;
    bool hearts = false, auto_update = false;
    NumberFormat fmt;
};
struct Score { int value = 0; bool locked = true; Text display; bool has_display = false; NumberFormat fmt; };
struct Team {
    std::string name;
    Text display, prefix = lit(""), suffix = lit("");
    int color = -1;  // TeamColor id, -1 = none
    bool friendly_fire = true, see_invisible = true;
    int nametag = 0, deathmsg = 0, collision = 0;
    std::vector<std::string> members;
};
inline std::map<std::string, Objective> objectives;
inline std::map<std::string, std::map<std::string, Score>> scores;  // holder -> objective -> score
inline std::array<std::string, 19> display_slots;
inline std::map<std::string, Team> teams;
inline std::map<std::string, std::set<std::string>> entity_tags;  // player uuid hex -> tags

inline const char* TEAM_COLORS[] = {"black", "dark_blue", "dark_green", "dark_aqua", "dark_red", "dark_purple", "gold", "gray",
                                    "dark_gray", "blue", "green", "aqua", "red", "light_purple", "yellow", "white"};
inline const char* VISIBILITY[] = {"always", "never", "hideForOtherTeams", "hideForOwnTeam"};
inline const char* COLLISION[] = {"always", "never", "pushOtherTeams", "pushOwnTeam"};

inline void write_number_format(Writer& w, const NumberFormat& f) {
    w.boolean(f.type >= 0);
    if (f.type < 0) return;
    w.varint(f.type);
    if (f.type >= 1) write_nbt(w, f.data);
}
inline Writer objective_packet(const Objective& o, int method) {  // 0 add, 1 remove, 2 change
    Writer w = pkt(play_cb::set_objective);
    w.str(o.name).u8(uint8_t(method));
    if (method != 1) {
        write_nbt(w, o.display);
        w.varint(o.hearts ? 1 : 0);
        write_number_format(w, o.fmt);
    }
    return w;
}
inline Writer score_packet(const std::string& holder, const std::string& obj, const Score& s) {
    Writer w = pkt(play_cb::set_score);
    w.str(holder).str(obj).varint(s.value).boolean(s.has_display);
    if (s.has_display) write_nbt(w, s.display);
    write_number_format(w, s.fmt);
    return w;
}
inline Writer reset_score_packet(const std::string& holder, const std::string* obj) {
    Writer w = pkt(play_cb::reset_score);
    w.str(holder).boolean(obj != nullptr);
    if (obj) w.str(*obj);
    return w;
}
inline Writer display_packet(int slot) { return std::move(pkt(play_cb::set_display_objective).varint(slot).str(display_slots[size_t(slot)])); }
inline Writer team_packet(const Team& t, int method, const std::vector<std::string>& players = {}) {
    // 0 add, 1 remove, 2 change, 3 join, 4 leave
    Writer w = pkt(play_cb::set_player_team);
    w.str(t.name).u8(uint8_t(method));
    if (method == 0 || method == 2) {
        write_nbt(w, t.display);
        write_nbt(w, t.prefix);
        write_nbt(w, t.suffix);
        w.varint(t.nametag).varint(t.collision).boolean(t.color >= 0);
        if (t.color >= 0) w.varint(t.color);
        w.u8(uint8_t((t.friendly_fire ? 1 : 0) | (t.see_invisible ? 2 : 0)));
    }
    if (method == 0 || method == 3 || method == 4) {
        const auto& list = method == 0 ? t.members : players;
        w.varint(int32_t(list.size()));
        for (auto& m : list) w.str(m);
    }
    return w;
}
inline Team* team_of(const std::string& holder) {
    for (auto& [n, t] : teams)
        if (std::find(t.members.begin(), t.members.end(), holder) != t.members.end()) return &t;
    return nullptr;
}
inline bool objective_displayed(const std::string& name) {
    return std::find(display_slots.begin(), display_slots.end(), name) != display_slots.end();
}
inline void set_score(const std::string& holder, const std::string& obj, int value) {
    auto& s = scores[holder][obj];
    s.value = value;
    if (objective_displayed(obj)) broadcast(score_packet(holder, obj, s));
}

// Player display name as vanilla decorates it (team prefix/color/suffix).
inline Text player_name(const Player& p) {
    Text n = lit(p.name);
    n.set("insertion", Nbt::str(p.name));
    Team* t = team_of(p.name);
    if (!t) return n;
    if (t->color >= 0) n.set("color", Nbt::str(TEAM_COLORS[t->color]));
    Text o = lit("");
    append(o, t->prefix);
    append(o, n);
    append(o, t->suffix);
    return o;
}

// Sends the scoreboard, teams, time, weather etc. to a joining player.
inline void send_world_state(Player& p) {
    p.send(border_init_packet());
    p.send(set_time_packet());
    p.send(spawn_packet());
    p.send(difficulty_packet());
    if (weather.rain_level > 0.2f) {
        p.send(game_event(1, 0));
        p.send(game_event(7, weather.rain_level));
        p.send(game_event(8, weather.thunder_level));
    }
    for (auto& [n, t] : teams) p.send(team_packet(t, 0));
    std::set<std::string> sent;
    for (int slot = 0; slot < 19; slot++) {
        auto& name = display_slots[size_t(slot)];
        auto it = objectives.find(name);
        if (name.empty() || it == objectives.end()) continue;
        if (sent.insert(name).second) {
            p.send(objective_packet(it->second, 0));
            for (auto& [holder, m] : scores)
                if (auto s = m.find(name); s != m.end()) p.send(score_packet(holder, name, s->second));
        }
        p.send(display_packet(slot));
    }
}

// ---------- persistence: <world>/craftpp_level.snbt ----------
inline Nbt number_format_nbt(const NumberFormat& f) {
    Nbt n = Nbt::compound();
    n.set("type", Nbt::num(Nbt::Int, f.type));
    if (f.type >= 1) n.set("data", f.data);
    return n;
}
inline NumberFormat number_format_from(const Nbt* n) {
    NumberFormat f;
    if (!n) return f;
    if (auto* t = n->get("type")) f.type = int(t->i);
    if (auto* d = n->get("data")) f.data = *d;
    return f;
}

inline Nbt level_to_nbt() {
    Nbt r = Nbt::compound();
    r.set("game_time", Nbt::num(Nbt::Long, game_time));
    Nbt cl = Nbt::compound();
    for (auto& c : clocks) {
        Nbt e = Nbt::compound();
        e.set("total_ticks", Nbt::num(Nbt::Long, c.total));
        e.set("partial_tick", Nbt::real(Nbt::Float, c.partial));
        e.set("rate", Nbt::real(Nbt::Float, c.rate));
        e.set("paused", Nbt::num(Nbt::Byte, c.paused));
        cl.set(c.id, e);
    }
    r.set("world_clocks", cl);
    Nbt gr = Nbt::compound();
    for (size_t i = 0; i < game_rule_values.size(); i++)
        gr.set(gen::GAME_RULES[i].id, Nbt::num(Nbt::Int, game_rule_values[i]));
    r.set("game_rules", gr);
    r.set("difficulty", Nbt::num(Nbt::Byte, difficulty));
    r.set("difficulty_locked", Nbt::num(Nbt::Byte, difficulty_locked));
    Nbt w = Nbt::compound();
    w.set("clear_time", Nbt::num(Nbt::Int, weather.clear_time));
    w.set("rain_time", Nbt::num(Nbt::Int, weather.rain_time));
    w.set("thunder_time", Nbt::num(Nbt::Int, weather.thunder_time));
    w.set("raining", Nbt::num(Nbt::Byte, weather.raining));
    w.set("thundering", Nbt::num(Nbt::Byte, weather.thundering));
    r.set("weather", w);
    Nbt sp = Nbt::compound();
    sp.set("x", Nbt::num(Nbt::Int, world_spawn.x));
    sp.set("y", Nbt::num(Nbt::Int, world_spawn.y));
    sp.set("z", Nbt::num(Nbt::Int, world_spawn.z));
    sp.set("yaw", Nbt::real(Nbt::Float, world_spawn.yaw));
    sp.set("pitch", Nbt::real(Nbt::Float, world_spawn.pitch));
    r.set("spawn", sp);
    Nbt b = Nbt::compound();
    b.set("center_x", Nbt::real(Nbt::Double, border.cx));
    b.set("center_z", Nbt::real(Nbt::Double, border.cz));
    b.set("size", Nbt::real(Nbt::Double, border.size()));
    b.set("target", Nbt::real(Nbt::Double, border.to));
    b.set("lerp_time", Nbt::num(Nbt::Long, border.remaining_ms()));
    b.set("damage_per_block", Nbt::real(Nbt::Double, border.damage));
    b.set("safe_zone", Nbt::real(Nbt::Double, border.buffer));
    b.set("warning_blocks", Nbt::num(Nbt::Int, border.warn_dist));
    b.set("warning_time", Nbt::num(Nbt::Int, border.warn_time));
    r.set("world_border", b);
    Nbt objs = Nbt::make_list();
    for (auto& [n, o] : objectives) {
        Nbt e = Nbt::compound();
        e.set("name", Nbt::str(o.name));
        e.set("criteria", Nbt::str(o.criteria));
        e.set("display", o.display);
        e.set("hearts", Nbt::num(Nbt::Byte, o.hearts));
        e.set("auto_update", Nbt::num(Nbt::Byte, o.auto_update));
        e.set("format", number_format_nbt(o.fmt));
        objs.list.push_back(e);
    }
    r.set("objectives", objs);
    Nbt sc = Nbt::make_list();
    for (auto& [holder, m] : scores)
        for (auto& [obj, s] : m) {
            Nbt e = Nbt::compound();
            e.set("holder", Nbt::str(holder));
            e.set("objective", Nbt::str(obj));
            e.set("value", Nbt::num(Nbt::Int, s.value));
            e.set("locked", Nbt::num(Nbt::Byte, s.locked));
            if (s.has_display) e.set("display", s.display);
            e.set("format", number_format_nbt(s.fmt));
            sc.list.push_back(e);
        }
    r.set("scores", sc);
    Nbt ds = Nbt::compound();
    for (int i = 0; i < 19; i++)
        if (!display_slots[size_t(i)].empty()) ds.set(std::to_string(i), Nbt::str(display_slots[size_t(i)]));
    r.set("display_slots", ds);
    Nbt tl = Nbt::make_list();
    for (auto& [n, t] : teams) {
        Nbt e = Nbt::compound();
        e.set("name", Nbt::str(t.name));
        e.set("display", t.display);
        e.set("prefix", t.prefix);
        e.set("suffix", t.suffix);
        e.set("color", Nbt::num(Nbt::Int, t.color));
        e.set("friendly_fire", Nbt::num(Nbt::Byte, t.friendly_fire));
        e.set("see_invisible", Nbt::num(Nbt::Byte, t.see_invisible));
        e.set("nametag", Nbt::num(Nbt::Int, t.nametag));
        e.set("deathmsg", Nbt::num(Nbt::Int, t.deathmsg));
        e.set("collision", Nbt::num(Nbt::Int, t.collision));
        Nbt mem = Nbt::make_list();
        for (auto& m : t.members) mem.list.push_back(Nbt::str(m));
        e.set("members", mem);
        tl.list.push_back(e);
    }
    r.set("teams", tl);
    Nbt tags = Nbt::compound();
    for (auto& [uuid, set] : entity_tags) {
        if (set.empty()) continue;
        Nbt l = Nbt::make_list();
        for (auto& t : set) l.list.push_back(Nbt::str(t));
        tags.set(uuid, l);
    }
    r.set("entity_tags", tags);
    return r;
}

inline Nbt parse_snbt(const std::string& s);  // args.hpp

inline void level_from_nbt(const Nbt& r) {
    auto I = [](const Nbt* n, int64_t d) { return n && n->is_num() ? int64_t(n->as_double()) : d; };
    auto D = [](const Nbt* n, double d) { return n && n->is_num() ? n->as_double() : d; };
    game_time = I(r.get("game_time"), 0);
    if (auto* cl = r.get("world_clocks"))
        for (auto& c : clocks)
            if (auto* e = cl->get(c.id)) {
                c.total = I(e->get("total_ticks"), 0);
                c.partial = float(D(e->get("partial_tick"), 0));
                c.rate = float(D(e->get("rate"), 1));
                c.paused = I(e->get("paused"), 0);
            }
    if (auto* gr = r.get("game_rules"))
        for (auto& [k, v] : gr->comp)
            if (int i = game_rule_index(k); i >= 0) game_rule_values[size_t(i)] = int(v.i);
    difficulty = int(I(r.get("difficulty"), 2));
    difficulty_locked = I(r.get("difficulty_locked"), 0);
    if (auto* w = r.get("weather")) {
        weather.clear_time = int(I(w->get("clear_time"), 0));
        weather.rain_time = int(I(w->get("rain_time"), 0));
        weather.thunder_time = int(I(w->get("thunder_time"), 0));
        weather.raining = I(w->get("raining"), 0);
        weather.thundering = I(w->get("thundering"), 0);
        weather.rain_level = weather.raining ? 1.f : 0.f;
        weather.thunder_level = weather.thundering ? 1.f : 0.f;
    }
    if (auto* sp = r.get("spawn")) {
        world_spawn.x = int(I(sp->get("x"), 0));
        world_spawn.y = int(I(sp->get("y"), -60));
        world_spawn.z = int(I(sp->get("z"), 0));
        world_spawn.yaw = float(D(sp->get("yaw"), 0));
        world_spawn.pitch = float(D(sp->get("pitch"), 0));
    }
    if (auto* b = r.get("world_border")) {
        border.cx = D(b->get("center_x"), 0);
        border.cz = D(b->get("center_z"), 0);
        border.from = D(b->get("size"), 59999968);
        border.to = D(b->get("target"), border.from);
        border.lerp_ms = I(b->get("lerp_time"), 0);
        border.lerp_start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        border.damage = D(b->get("damage_per_block"), 0.2);
        border.buffer = D(b->get("safe_zone"), 5);
        border.warn_dist = int(I(b->get("warning_blocks"), 5));
        border.warn_time = int(I(b->get("warning_time"), 15));
    }
    if (auto* objs = r.get("objectives"))
        for (auto& e : objs->list) {
            Objective o;
            o.name = e.get("name")->s;
            if (auto* c = e.get("criteria")) o.criteria = c->s;
            if (auto* d = e.get("display")) o.display = *d;
            o.hearts = I(e.get("hearts"), 0);
            o.auto_update = I(e.get("auto_update"), 0);
            o.fmt = number_format_from(e.get("format"));
            objectives[o.name] = o;
        }
    if (auto* sc = r.get("scores"))
        for (auto& e : sc->list) {
            Score s;
            s.value = int(I(e.get("value"), 0));
            s.locked = I(e.get("locked"), 1);
            if (auto* d = e.get("display")) { s.display = *d; s.has_display = true; }
            s.fmt = number_format_from(e.get("format"));
            scores[e.get("holder")->s][e.get("objective")->s] = s;
        }
    if (auto* ds = r.get("display_slots"))
        for (auto& [k, v] : ds->comp) {
            int i = atoi(k.c_str());
            if (i >= 0 && i < 19) display_slots[size_t(i)] = v.s;
        }
    if (auto* tl = r.get("teams"))
        for (auto& e : tl->list) {
            Team t;
            t.name = e.get("name")->s;
            if (auto* d = e.get("display")) t.display = *d;
            if (auto* d = e.get("prefix")) t.prefix = *d;
            if (auto* d = e.get("suffix")) t.suffix = *d;
            t.color = int(I(e.get("color"), -1));
            t.friendly_fire = I(e.get("friendly_fire"), 1);
            t.see_invisible = I(e.get("see_invisible"), 1);
            t.nametag = int(I(e.get("nametag"), 0));
            t.deathmsg = int(I(e.get("deathmsg"), 0));
            t.collision = int(I(e.get("collision"), 0));
            if (auto* m = e.get("members")) for (auto& x : m->list) t.members.push_back(x.s);
            teams[t.name] = t;
        }
    if (auto* tags = r.get("entity_tags"))
        for (auto& [uuid, l] : tags->comp)
            for (auto& t : l.list) entity_tags[uuid].insert(t.s);
}

inline std::string level_path() { return world_dir + "/craftpp_level.snbt"; }
inline void save_level() {
    std::lock_guard lock(cmd_mtx);
    std::filesystem::create_directories(world_dir);
    {
        std::ofstream f(level_path() + ".tmp");
        f << to_snbt(level_to_nbt()) << '\n';
    }
    std::filesystem::rename(level_path() + ".tmp", level_path());
}
inline void load_level() {
    std::ifstream f(level_path());
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        level_from_nbt(parse_snbt(ss.str()));
    } catch (...) {
        fprintf(stderr, "failed to read %s\n", level_path().c_str());
    }
}

// ---------- ops / bans / whitelist (vanilla JSON formats) ----------
inline std::string uuid_dashed(int64_t hi, int64_t lo) {
    char b[40];
    uint64_t h = uint64_t(hi), l = uint64_t(lo);
    snprintf(b, sizeof b, "%08x-%04x-%04x-%04x-%012llx", unsigned(h >> 32), unsigned(h >> 16 & 0xFFFF), unsigned(h & 0xFFFF),
             unsigned(l >> 48), (unsigned long long)(l & 0xFFFFFFFFFFFFull));
    return b;
}

// MD5 (RFC 1321) for offline-mode UUIDs: UUID.nameUUIDFromBytes("OfflinePlayer:" + name).
inline std::array<uint8_t, 16> md5(const std::string& msg) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    std::vector<uint8_t> m(msg.begin(), msg.end());
    uint64_t bits = uint64_t(m.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 0; i < 8; i++) m.push_back(uint8_t(bits >> (8 * i)));
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(m[off + 4 * i]) | uint32_t(m[off + 4 * i + 1]) << 8 | uint32_t(m[off + 4 * i + 2]) << 16 | uint32_t(m[off + 4 * i + 3]) << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            uint32_t tmp = d;
            d = c;
            c = b;
            uint32_t x = a + f + K[i] + w[g];
            int r = R[(i / 16) * 4 + i % 4];
            b = b + (x << r | x >> (32 - r));
            a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    std::array<uint8_t, 16> out;
    for (int i = 0; i < 16; i++) out[size_t(i)] = uint8_t(h[i / 4] >> (8 * (i % 4)));
    return out;
}
inline std::string offline_uuid(const std::string& name) {
    auto d = md5("OfflinePlayer:" + name);
    d[6] = uint8_t((d[6] & 0x0f) | 0x30);
    d[8] = uint8_t((d[8] & 0x3f) | 0x80);
    int64_t hi = 0, lo = 0;
    for (int i = 0; i < 8; i++) hi = hi << 8 | d[size_t(i)];
    for (int i = 8; i < 16; i++) lo = lo << 8 | d[size_t(i)];
    return uuid_dashed(hi, lo);
}

struct ListEntry {  // one row of ops.json / banned-players.json / banned-ips.json / whitelist.json
    std::string uuid, name, ip;
    int level = 0;
    bool bypass = false;
    std::string created, source, expires = "forever", reason;
};
struct UserList {
    const char* file;
    std::vector<ListEntry> rows;
    std::string path() const { return server_dir + "/" + file; }
    ListEntry* find(const std::string& uuid, const std::string& name) {
        for (auto& r : rows) {
            if (!uuid.empty() && r.uuid == uuid) return &r;
            if (!name.empty() && strcasecmp(r.name.c_str(), name.c_str()) == 0) return &r;
        }
        return nullptr;
    }
    ListEntry* find_ip(const std::string& ip) {
        for (auto& r : rows) if (r.ip == ip) return &r;
        return nullptr;
    }
    void load() {
        rows.clear();
        try {
            Json j = Json::parseFile(path());
            for (auto& e : j.arr) {
                ListEntry r;
                auto s = [&](const char* k, std::string& out) { if (auto* v = e.find(k); v && v->isString()) out = v->str; };
                s("uuid", r.uuid); s("name", r.name); s("ip", r.ip); s("created", r.created); s("source", r.source);
                s("expires", r.expires); s("reason", r.reason);
                r.level = e.getInt("level", 4);
                if (auto* v = e.find("bypassesPlayerLimit"); v && v->isBool()) r.bypass = v->b;
                rows.push_back(r);
            }
        } catch (...) {}
    }
    void save(bool ops, bool ban, bool ip) const {
        std::ofstream f(path());
        f << "[";
        for (size_t i = 0; i < rows.size(); i++) {
            auto& r = rows[i];
            f << (i ? "," : "") << "\n  {\n";
            std::vector<std::string> kv;
            auto q = [](const std::string& s) { return "\"" + json_escape(s) + "\""; };
            if (ip) kv.push_back("\"ip\": " + q(r.ip));
            else { kv.push_back("\"uuid\": " + q(r.uuid)); kv.push_back("\"name\": " + q(r.name)); }
            if (ops) { kv.push_back("\"level\": " + std::to_string(r.level)); kv.push_back(std::string("\"bypassesPlayerLimit\": ") + (r.bypass ? "true" : "false")); }
            if (ban) {
                kv.push_back("\"created\": " + q(r.created)); kv.push_back("\"source\": " + q(r.source));
                kv.push_back("\"expires\": " + q(r.expires)); kv.push_back("\"reason\": " + q(r.reason));
            }
            for (size_t k = 0; k < kv.size(); k++) f << "    " << kv[k] << (k + 1 < kv.size() ? ",\n" : "\n");
            f << "  }";
        }
        f << (rows.empty() ? "]\n" : "\n]\n");
    }
};
inline UserList ops{"ops.json"}, banned_players{"banned-players.json"}, banned_ips{"banned-ips.json"}, whitelist{"whitelist.json"};
inline void save_lists() {
    ops.save(true, false, false);
    banned_players.save(false, true, false);
    banned_ips.save(false, true, true);
    whitelist.save(false, false, false);
}
inline std::string now_date() {
    char b[64];
    time_t t = time(nullptr);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %z", localtime(&t));
    return b;
}
inline bool prop_bool(const char* k, bool def) {
    auto it = props.find(k);
    return it == props.end() ? def : it->second == "true";
}
inline int prop_int(const char* k, int def) {
    auto it = props.find(k);
    return it == props.end() || it->second.empty() ? def : atoi(it->second.c_str());
}

inline std::string player_uuid(const Player& p) { return uuid_dashed(p.uuid_hi, p.uuid_lo); }
inline int perm_level(const Player& p) {
    std::lock_guard lock(cmd_mtx);
    ListEntry* e = ops.find(player_uuid(p), p.name);
    return e ? e->level : 0;
}
inline std::string peer_ip(int fd) {
    sockaddr_storage a{};
    socklen_t n = sizeof a;
    if (getpeername(fd, (sockaddr*)&a, &n) != 0) return "";
    char b[64] = "";
    if (a.ss_family == AF_INET) inet_ntop(AF_INET, &((sockaddr_in*)&a)->sin_addr, b, sizeof b);
    else if (a.ss_family == AF_INET6) inet_ntop(AF_INET6, &((sockaddr_in6*)&a)->sin6_addr, b, sizeof b);
    return b;
}

// Login gate (PlayerList.canPlayerLogin): returns the disconnect text, or empty to allow.
inline std::string login_check(const std::string& name, int64_t uuid_hi, int64_t uuid_lo, int fd) {
    std::lock_guard lock(cmd_mtx);
    std::string uuid = uuid_dashed(uuid_hi, uuid_lo);
    if (ListEntry* b = banned_players.find(uuid, name)) {
        std::string msg = plain(tr("multiplayer.disconnect.banned.reason", {b->reason}));
        if (b->expires != "forever") msg += plain(tr("multiplayer.disconnect.banned.expiration", {b->expires}));
        return msg;
    }
    if (prop_bool("white-list", false) && !whitelist.find(uuid, name) && !ops.find(uuid, name))
        return plain(tr("multiplayer.disconnect.not_whitelisted"));
    std::string ip = peer_ip(fd);
    if (ListEntry* b = banned_ips.find_ip(ip)) {
        std::string msg = plain(tr("multiplayer.disconnect.banned_ip.reason", {b->reason}));
        if (b->expires != "forever") msg += plain(tr("multiplayer.disconnect.banned_ip.expiration", {b->expires}));
        return msg;
    }
    return "";
}
