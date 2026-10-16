#pragma once
// Command system glue: world tick (clocks, weather, border), suggestions, packet handlers, join/login hooks.
#include "bossbar.hpp"
#include "execute.hpp"

// play.hpp's packet handler registry (defined after this header is included).
using PacketHandler = std::function<void(Player&, Reader&, int id)>;
inline std::map<int, PacketHandler>& handlers();
inline void on(int id, PacketHandler h);

// ---------- tick hook (MinecraftServer.tickChildren: clocks, time sync, weather, border) ----------
inline void advance_time() {
    std::lock_guard lock(cmd_mtx);
    game_time++;
    if (rule("advance_time"))
        for (auto& c : clocks)
            if (!c.paused) {  // ServerClockInstance.tick
                c.partial += c.rate;
                int full = int(std::floor(c.partial));
                c.partial -= float(full);
                c.total += full;
            }
    if (++tick_count % 20 == 0) broadcast(set_time_packet(-2));  // forceGameTimeSynchronization
    advance_weather();
    border.tick();
}

// ---------- suggestions for nodes with custom (ask_server) providers ----------
inline std::vector<std::string> custom_suggestions(const Segment& seg, int node, const Source& src, const std::string& typed) {
    (void)typed;
    const auto& n = node_info(node);
    std::string name = n.name;
    std::vector<std::string> out;
    auto online = [&] { for (auto& p : snapshot()) out.push_back(p->name); };
    if (n.parser == arg_types::score_holder) {
        online();
        for (auto& [h, m] : scores) out.push_back(h);
        if (n.flags & 1) out.push_back("*");
        return out;
    }
    const std::string& cmd = seg.cmd;
    if (cmd == "op" || (cmd == "whitelist" && seg.nodes.size() > 1 && !strcmp(node_name(seg.nodes[1].node), "add"))) {
        for (auto& p : snapshot()) if (cmd != "op" || !ops.find(player_uuid(*p), p->name)) out.push_back(p->name);
    } else if (cmd == "deop") {
        for (auto& r : ops.rows) out.push_back(r.name);
    } else if (cmd == "whitelist") {
        for (auto& r : whitelist.rows) out.push_back(r.name);
    } else if (cmd == "pardon") {
        for (auto& r : banned_players.rows) out.push_back(r.name);
    } else if (cmd == "pardon-ip") {
        for (auto& r : banned_ips.rows) out.push_back(r.ip);
    } else if (cmd == "tag" && name == "name") {
        // TagCommand: tags of the targets
        auto it = seg.args.find("targets");
        if (it != seg.args.end())
            for (auto& p : select(std::get<Selector>(it->second), src))
                for (auto& t : entity_tags[uuid_hex(*p)]) out.push_back(t);
    } else if (cmd == "time" && name == "timemarker") {
        out = {"minecraft:day", "minecraft:noon", "minecraft:night", "minecraft:midnight"};
    } else if (cmd == "trigger" || n.parser == arg_types::objective) {
        for (auto& [k, o] : objectives) if (cmd != "trigger" || o.criteria == "trigger") out.push_back(k);
    } else if (cmd == "tick") {
        out = {name == "rate" ? "20" : "1t"};
    }
    return out;
}

// ---------- packet handlers ----------
inline void register_command_handlers() {
    // F3+F4 game mode switcher (ServerGamePacketListenerImpl.handleChangeGameMode): needs gamemaster (level 2).
    on(play_sb::change_game_mode, [](Player& p, Reader& in, int) {
        static const char* names[] = {"survival", "creative", "adventure", "spectator"};
        int mode = in.varint();
        if (mode < 0 || mode > 3) return;
        if (perm_level(p) < 2) {
            printf("Player %s tried to change game mode to %s without required permissions\n", p.name.c_str(), names[mode]);
            fflush(stdout);
            return;
        }
        if (auto pp = find_player(p.name)) perform(source_for(pp), std::string("gamemode ") + names[mode]);
    });
    on(play_sb::command_suggestion, [](Player& p, Reader& in, int) {
        int id = in.varint();
        std::string text = in.str(32500);
        auto pp = find_player(p.name);
        if (!pp) return;
        Source src = source_for(pp);
        auto [start, list] = suggestions(text, src);
        Writer w = pkt(play_cb::command_suggestions);
        w.varint(id).varint(int32_t(start)).varint(int32_t(text.size() - start)).varint(int32_t(list.size()));
        for (auto& s : list) {
            w.str(s.text).boolean(s.tooltip.has_value());
            if (s.tooltip) write_nbt(w, *s.tooltip);
        }
        p.send(w);
    });
    // ServerGamePacketListenerImpl.handleSetGameRule / REQUEST_GAMERULE_VALUES (client_command action 2)
    on(play_sb::set_game_rule, [](Player& p, Reader& in, int) {
        if (perm_level(p) < 2) return;
        std::lock_guard lock(cmd_mtx);
        int n = in.varint();
        for (int k = 0; k < n; k++) {
            std::string id = in.str(), value = in.str();
            int i = game_rule_index(id);
            if (i < 0) continue;
            const auto& g = gen::GAME_RULES[i];
            int v;
            if (g.is_int) {
                char* end;
                long x = strtol(value.c_str(), &end, 10);
                if (*end || value.empty() || x < g.min || x > g.max) continue;
                v = int(x);
            } else if (value == "true" || value == "false") v = value == "true";
            else continue;
            game_rule_values[size_t(i)] = v;
            if (std::string(g.id) == "advance_time") broadcast(set_time_packet());
            Text msg = tr("commands.gamerule.set", {g.id, value});
            for (auto& q : snapshot()) if (ops.find(player_uuid(*q), q->name)) send_text(*q, msg);
        }
    });
    auto prev = handlers().count(play_sb::client_command) ? handlers()[play_sb::client_command] : PacketHandler{};
    on(play_sb::client_command, [prev](Player& p, Reader& in, int id) {
        Reader peek = in;
        if (peek.varint() != 2) { if (prev) prev(p, in, id); return; }
        if (perm_level(p) < 2) return;
        std::lock_guard lock(cmd_mtx);
        Writer w = pkt(play_cb::game_rule_values);
        w.varint(int32_t(game_rule_values.size()));
        for (size_t i = 0; i < game_rule_values.size(); i++) {
            const auto& g = gen::GAME_RULES[i];
            w.str(std::string("minecraft:") + g.id).str(g.is_int ? std::to_string(game_rule_values[i]) : game_rule_values[i] ? "true" : "false");
        }
        p.send(w);
    });
}

// ---------- startup / join ----------
inline void init_commands() {
    std::lock_guard lock(cmd_mtx);
    ops.load();
    banned_players.load();
    banned_ips.load();
    whitelist.load();
    load_level();
    register_vanilla_commands();
    register_bossbar();
    commands_packet = commands_packet_for(0);  // legacy global (level-0 tree)
}
// Join: commands tree for the player's permission level + world state (vanilla PlayerList.placeNewPlayer order).
inline void commands_on_join(Player& p) {
    send_commands(p);
    std::lock_guard lock(cmd_mtx);
    send_world_state(p);
    bossbars_on_join(p);
}
