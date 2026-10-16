#pragma once
// Command tree (sent to clients) and command execution.
#include "server.hpp"

#include <sstream>

// Brigadier tree sent to clients for tab-completion and client-side parsing.
inline void build_commands_packet() {
    struct Node { uint8_t flags; std::vector<int> children; std::string name; int parser = -1; std::vector<uint8_t> extra; };
    std::vector<Node> n{{0, {}, "", -1, {}}};
    auto lit = [&](int parent, const std::string& name, bool exec) {
        n.push_back({uint8_t(1 | (exec ? 4 : 0)), {}, name, -1, {}});
        n[parent].children.push_back(int(n.size() - 1));
        return int(n.size() - 1);
    };
    auto arg = [&](int parent, const std::string& name, int parser, std::vector<uint8_t> extra, bool exec) {
        n.push_back({uint8_t(2 | (exec ? 4 : 0)), {}, name, parser, std::move(extra)});
        n[parent].children.push_back(int(n.size() - 1));
        return int(n.size() - 1);
    };
    lit(0, "help", true);
    lit(0, "list", true);
    arg(lit(0, "say", false), "message", arg_types::message, {}, true);
    arg(lit(0, "me", false), "action", arg_types::message, {}, true);
    int gm = arg(lit(0, "gamemode", false), "gamemode", arg_types::gamemode, {}, true);
    arg(gm, "target", arg_types::entity, {0x01 | 0x02}, true);  // single player
    int tp = lit(0, "tp", false);
    arg(tp, "location", arg_types::vec3, {}, true);
    int dest = arg(tp, "destination", arg_types::entity, {0x01}, true);
    (void)dest;

    Writer w;
    w.varint(play_cb::commands).varint(int32_t(n.size()));
    for (auto& node : n) {
        w.u8(node.flags).varint(int32_t(node.children.size()));
        for (int c : node.children) w.varint(c);
        if ((node.flags & 3) == 1) w.str(node.name);
        if ((node.flags & 3) == 2) { w.str(node.name).varint(node.parser); w.bytes(node.extra); }
    }
    w.varint(0);  // root index
    Writer frame;
    frame.varint(int32_t(w.b.size())).bytes(w.b);
    commands_packet = frame.b;
}

inline void run_command(Player* p, const std::string& line) {
    auto reply = [&](const std::string& msg, const std::string& color = "") {
        if (p) p->message(msg, color);
        else { printf("%s\n", msg.c_str()); fflush(stdout); }
    };
    std::istringstream in(line);
    std::vector<std::string> a;
    for (std::string s; in >> s;) a.push_back(s);
    if (a.empty()) return;
    std::string who = p ? p->name : "Server";
    auto rest = [&](size_t) {  // text after the command word
        auto pos = line.find_first_not_of(' ', line.find(a[0]) + a[0].size());
        return pos == std::string::npos ? std::string() : line.substr(pos);
    };

    if (a[0] == "help") {
        reply("Commands: /help, /list, /say <msg>, /me <action>, /gamemode <mode> [player], /tp <x y z | player>", "gold");
    } else if (a[0] == "list") {
        auto list = snapshot();
        std::string names;
        for (auto& q : list) names += (names.empty() ? "" : ", ") + q->name;
        reply("There are " + std::to_string(list.size()) + " of a max of " + props["max-players"] +
              " players online: " + names);
    } else if (a[0] == "say" && a.size() > 1) {
        broadcast_message("[" + who + "] " + rest(1));
    } else if (a[0] == "me" && a.size() > 1) {
        broadcast_message("* " + who + " " + rest(1));
    } else if (a[0] == "gamemode" && a.size() > 1) {
        int gm = gamemode_id(a[1]);
        PlayerPtr target = a.size() > 2 ? find_player(a[2]) : nullptr;
        Player* t = a.size() > 2 ? target.get() : p;
        if (gm < 0) return reply("Unknown game mode: " + a[1], "red");
        if (!t) return reply("No player was found", "red");
        set_gamemode(*t, gm);
        t->message(std::string("Your game mode has been updated to ") + gamemode_names[gm] + " Mode");
        if (t != p) reply("Set " + t->name + "'s game mode to " + gamemode_names[gm] + " Mode");
    } else if (a[0] == "tp" && (a.size() == 2 || a.size() == 4)) {
        if (!p) return reply("Only players can teleport", "red");
        if (a.size() == 2) {
            auto t = find_player(a[1]);
            if (!t) return reply("No player was found", "red");
            teleport(*p, t->x, t->y, t->z);
            return reply("Teleported " + p->name + " to " + t->name);
        }
        double base[3] = {p->x, p->y, p->z}, v[3];
        try {
            for (int i = 0; i < 3; i++) {
                const std::string& s = a[i + 1];
                v[i] = s[0] == '~' ? base[i] + (s.size() > 1 ? std::stod(s.substr(1)) : 0) : std::stod(s);
            }
        } catch (...) {
            return reply("Invalid position", "red");
        }
        teleport(*p, v[0], v[1], v[2]);
        char buf[128];
        snprintf(buf, sizeof buf, "Teleported %s to %.2f, %.2f, %.2f", p->name.c_str(), v[0], v[1], v[2]);
        reply(buf);
    } else {
        reply("Unknown or incomplete command: " + line + " (try /help)", "red");
    }
}
