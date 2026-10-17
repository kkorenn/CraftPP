#pragma once
// What surrounds a Paper server jar: the eula.txt gate, a full vanilla server.properties, Paper's first-run
// files (bukkit.yml, spigot.yml, commands.yml, config/paper-*.yml copied from data/paper and never parsed),
// the JSON lists, usercache.json, version_history.json, plugins/; and Paper's /version, /plugins, /tps, /mspt.
#include "play.hpp"

#include <random>

inline const std::string IMPLEMENTATION_VERSION = std::string(GAME_VERSION) + "-CraftPP";

// java.util.Date.toString(), as written into the eula.txt / server.properties headers.
inline std::string java_date() {
    char b[64];
    time_t t = time(nullptr);
    tm lt{};
    localtime_r(&t, &lt);
    strftime(b, sizeof b, "%a %b %d %H:%M:%S %Z %Y", &lt);
    return b;
}

// Double.toString for ordinary magnitudes: shortest digits that round-trip, at least one decimal.
inline std::string java_double(double v) {
    char b[64];
    for (int p = 1; p <= 17; p++) {
        snprintf(b, sizeof b, "%.*f", p, v);
        if (strtod(b, nullptr) == v) break;
    }
    return b;
}

// ---------- java.util.Properties text format ----------
inline bool read_properties(const std::string& path, std::map<std::string, std::string>& out) {
    std::ifstream f(path);
    if (!f) return false;
    auto unescape = [](const std::string& s) {
        std::string o;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
            char c = s[++i];
            if (c == 'u' && i + 4 < s.size()) {
                unsigned cp = unsigned(strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
                i += 4;
                if (cp < 0x80) o += char(cp);
                else if (cp < 0x800) o += char(0xC0 | cp >> 6), o += char(0x80 | (cp & 63));
                else o += char(0xE0 | cp >> 12), o += char(0x80 | ((cp >> 6) & 63)), o += char(0x80 | (cp & 63));
            } else o += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c == 'f' ? '\f' : c;
        }
        return o;
    };
    // ponytail: no backslash line continuations (vanilla never writes them).
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t s = line.find_first_not_of(" \t\f");
        if (s == std::string::npos || line[s] == '#' || line[s] == '!') continue;
        size_t e = s;
        while (e < line.size() && line[e] != '=' && line[e] != ':' && line[e] != ' ' && line[e] != '\t') e += line[e] == '\\' ? 2 : 1;
        e = std::min(e, line.size());
        size_t v = line.find_first_not_of(" \t\f", e);
        if (v != std::string::npos && (line[v] == '=' || line[v] == ':')) v = line.find_first_not_of(" \t\f", v + 1);
        out[unescape(line.substr(s, e - s))] = v == std::string::npos ? "" : unescape(line.substr(v));
    }
    return true;
}
inline std::string escape_property(const std::string& s, bool key) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        switch (c) {
            case '\t': o += "\\t"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\f': o += "\\f"; break;
            case '\\': case '=': case ':': case '#': case '!': o += '\\'; o += c; break;
            case ' ': o += key || i == 0 ? "\\ " : " "; break;
            default: o += c;
        }
    }
    return o;
}

// Every key the 26.3 dedicated server writes, with its default (DedicatedServerProperties).
// Deliberate difference: online-mode defaults to false because CraftPP never authenticates.
inline const std::pair<const char*, const char*> VANILLA_PROPERTIES[] = {
    {"accepts-transfers", "false"}, {"allow-flight", "false"}, {"broadcast-console-to-ops", "true"},
    {"broadcast-rcon-to-ops", "true"}, {"bug-report-link", ""}, {"chat-spam-threshold-seconds", "10"},
    {"command-spam-threshold-seconds", "10"}, {"difficulty", "easy"}, {"enable-code-of-conduct", "false"},
    {"enable-jmx-monitoring", "false"}, {"enable-query", "false"}, {"enable-rcon", "false"}, {"enable-status", "true"},
    {"enforce-secure-profile", "true"}, {"enforce-whitelist", "false"}, {"entity-broadcast-range-percentage", "100"},
    {"force-gamemode", "false"}, {"function-permission-level", "2"}, {"gamemode", "survival"},
    {"generate-structures", "true"}, {"generator-settings", "{}"}, {"hardcore", "false"},
    {"hide-online-players", "false"}, {"initial-disabled-packs", ""}, {"initial-enabled-packs", "vanilla"},
    {"level-name", "world"}, {"level-seed", ""}, {"level-type", "minecraft:normal"}, {"log-ips", "true"},
    {"management-server-allowed-origins", ""}, {"management-server-enabled", "false"},
    {"management-server-host", "localhost"}, {"management-server-port", "0"}, {"management-server-secret", ""},
    {"management-server-tls-enabled", "true"}, {"management-server-tls-keystore", ""},
    {"management-server-tls-keystore-password", ""}, {"max-chained-neighbor-updates", "1000000"},
    {"max-players", "20"}, {"max-tick-time", "60000"}, {"max-world-size", "29999984"},
    {"motd", "A Minecraft Server"}, {"network-compression-threshold", "256"}, {"online-mode", "false"},
    {"op-permission-level", "4"}, {"pause-when-empty-seconds", "60"}, {"player-idle-timeout", "0"},
    {"prevent-proxy-connections", "false"}, {"query.port", "25565"}, {"rate-limit", "0"}, {"rcon.password", ""},
    {"rcon.port", "25575"}, {"region-file-compression", "deflate"}, {"require-resource-pack", "false"},
    {"resource-pack", ""}, {"resource-pack-id", ""}, {"resource-pack-prompt", ""}, {"resource-pack-sha1", ""},
    {"server-ip", ""}, {"server-port", "25565"}, {"simulation-distance", "10"}, {"spawn-protection", "16"},
    {"status-heartbeat-interval", "0"}, {"sync-chunk-writes", "true"}, {"text-filtering-config", ""},
    {"text-filtering-version", "0"}, {"use-native-transport", "true"}, {"view-distance", "10"}, {"white-list", "true"},
};

// Like vanilla: read server.properties, and (re)write it, sorted, when it is missing or lacks a key.
// Unknown keys are kept. Only the keys CraftPP implements change anything; the rest are carried along.
inline void load_properties(const std::string& path) {
    std::map<std::string, std::string> file;
    bool complete = read_properties(path, file);
    for (auto& [k, v] : VANILLA_PROPERTIES) {
        if (file.count(k)) continue;
        complete = false;
        std::string val = v;
        if (!strcmp(k, "management-server-secret")) {  // SecurityConfig.generateSecretKey
            static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
            std::random_device rd;
            for (int i = 0; i < 40; i++) val += a[rd() % (sizeof a - 1)];
        }
        file[k] = val;
    }
    for (auto& [k, v] : file) props[k] = v;
    if (complete) return;
    std::ofstream o(path);
    o << "#Minecraft server properties\n#" << java_date() << '\n';
    for (auto& [k, v] : file) o << escape_property(k, true) << '=' << escape_property(v, false) << '\n';
}

// ---------- eula.txt (server/Eula.java + Main) ----------
inline bool check_eula(const std::string& dir) {
    std::string path = dir == "." ? "eula.txt" : dir + "/eula.txt";
    std::map<std::string, std::string> kv;
    if (!read_properties(path, kv)) {
        log_warn("Failed to load %s", path.c_str());
        std::ofstream o(path);
        o << "#By changing the setting below to TRUE you are indicating your agreement to our EULA "
             "(https://aka.ms/MinecraftEULA).\n#"
          << java_date() << "\neula=false\n";
    }
    std::string v = kv.count("eula") ? kv["eula"] : "false";
    for (auto& c : v) c = char(tolower((unsigned char)c));
    if (v == "true") return true;
    log_info("You need to agree to the EULA in order to run the server. Go to eula.txt for more info.");
    return false;
}

// ---------- Paper's first-run layout ----------
inline void ensure_paper_files(const std::string& dir) {
    std::error_code ec;
    for (const char* d : {"plugins", "config"}) std::filesystem::create_directories(dir + "/" + d, ec);
    for (const char* f : {"bukkit.yml", "spigot.yml", "commands.yml", "config/paper-global.yml", "config/paper-world-defaults.yml"})
        if (!std::filesystem::exists(dir + "/" + f, ec)) std::filesystem::copy_file(std::string("data/paper/") + f, dir + "/" + f, ec);
    for (const char* f : {"ops.json", "banned-players.json", "banned-ips.json", "whitelist.json", "usercache.json"})
        if (!std::filesystem::exists(dir + "/" + f, ec)) std::ofstream(dir + "/" + f) << "[]";
    if (!std::filesystem::exists(dir + "/version_history.json", ec))
        std::ofstream(dir + "/version_history.json")
            << "{\"currentVersion\":\"" << IMPLEMENTATION_VERSION << " (MC: " << GAME_VERSION << ")\"}";
}

// usercache.json (CachedUserNameToIdResolver): most recent first, one-month expiry, at most 1000 entries.
// ponytail: rewrites the file on every login; fine at hobby-server join rates.
inline void record_usercache(const std::string& name, const std::string& uuid) {
    static std::mutex m;
    std::lock_guard lock(m);
    std::string path = server_dir + "/usercache.json";
    std::vector<std::pair<std::string, Json>> keep;
    try {
        Json j = Json::parseFile(path);
        for (auto& e : j.arr) {
            auto* n = e.find("name");
            auto* u = e.find("uuid");
            if (!n || !u || !n->isString() || !u->isString() || u->str == uuid || strcasecmp(n->str.c_str(), name.c_str()) == 0) continue;
            keep.push_back({n->str, e});
            if (keep.size() >= 999) break;
        }
    } catch (...) {}
    char exp[64];
    time_t t = time(nullptr) + 30L * 24 * 3600;
    tm lt{};
    localtime_r(&t, &lt);
    strftime(exp, sizeof exp, "%Y-%m-%d %H:%M:%S %z", &lt);
    auto q = [](const std::string& s) { return "\"" + json_escape(s) + "\""; };
    std::string out = "[{\"uuid\":" + q(uuid) + ",\"name\":" + q(name) + ",\"expiresOn\":" + q(exp) + "}";
    for (auto& [n, e] : keep) {
        auto* u = e.find("uuid");
        auto* x = e.find("expiresOn");
        out += ",{\"uuid\":" + q(u->str) + ",\"name\":" + q(n) + ",\"expiresOn\":" + q(x && x->isString() ? x->str : exp) + "}";
    }
    std::ofstream(path) << out << "]";
}

// ---------- /version /plugins /tps /mspt ----------
inline void reply_legacy(const Source& src, const std::string& s) {
    if (src.sender) send_text(*src.sender, lit(s));  // the client renders § codes in literal text
    else if (src.rcon_out) *src.rcon_out += s;
    else log_info("%s", s.c_str());
}

#define S "\xC2\xA7"
inline std::string tps_format(double tps) {  // Paper TicksPerSecondCommand.format
    double shown = std::min(std::round(tps * 100.0) / 100.0, 20.0);
    std::string c = tps > 18 ? S "a" : tps > 16 ? S "e" : S "c";
    return c + (tps > 21 ? "*" : "") + java_double(shown);
}
inline std::string mspt_format(const TickTimes& t) {  // Paper MSPTCommand
    auto col = [](double v) { return v >= 50 ? S "c" : v >= 40 ? S "e" : S "a"; };
    char b[128];
    snprintf(b, sizeof b, "%s%.1f" S "e/%s%.1f" S "e/%s%.1f", col(t.avg), t.avg, col(t.min), t.min, col(t.max), t.max);
    return b;
}

inline void register_paper_commands() {
    auto version = [](Source& src, const std::vector<std::string>&) {
        reply_legacy(src, "Checking version, please wait...");
        reply_legacy(src, "This server is running CraftPP version " + IMPLEMENTATION_VERSION + " (MC: " + GAME_VERSION +
                              ") (Implementing API version " + GAME_VERSION + "-R0.1-SNAPSHOT)");
        return 1;
    };
    for (const char* n : {"version", "ver", "about"}) register_plugin_command(n, version);
    auto plugins_cmd = [](Source& src, const std::vector<std::string>&) {
        reply_legacy(src, S "b\xE2\x84\xB9 " S "fServer Plugins (0):");  // the bridge answers itself when plugins load
        return 1;
    };
    for (const char* n : {"plugins", "pl"}) register_plugin_command(n, plugins_cmd);
    register_plugin_command("tps", [](Source& src, const std::vector<std::string>&) {
        auto t = tps_averages();
        reply_legacy(src, S "6TPS from last 1m, 5m, 15m: " + tps_format(t[0]) + S "6, " + tps_format(t[1]) + S "6, " + tps_format(t[2]));
        return 1;
    }, {}, 2);
    register_plugin_command("mspt", [](Source& src, const std::vector<std::string>&) {
        reply_legacy(src, S "6Server tick times " S "e(" S "7avg" S "e/" S "7min" S "e/" S "7max" S "e)" S "6 from last 5s"
                          S "e," S "6 10s" S "e," S "6 1m" S "e:");
        reply_legacy(src, S "6\xE2\x97\xB4 " + mspt_format(tick_times(100)) + S "e, " + mspt_format(tick_times(200)) + S "e, " +
                              mspt_format(tick_times(1200)));
        return 1;
    }, {}, 2);
}
#undef S
