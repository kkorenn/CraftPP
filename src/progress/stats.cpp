// Statistics (ServerStatsCounter): per-player counters across the 9 StatType registries, persisted in
// <world>/players/stats/<uuid>.json and sent on client_command REQUEST_STATS (play_cb::award_stats).
#include "progress.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>

namespace progress {

namespace {
struct StatReg {
    const char* key;
    const char* reg;
};
constexpr StatReg STAT_REGS[9] = {
    {"minecraft:mined", "block"},         {"minecraft:crafted", "item"},
    {"minecraft:used", "item"},           {"minecraft:broken", "item"},
    {"minecraft:picked_up", "item"},      {"minecraft:dropped", "item"},
    {"minecraft:killed", "entity_type"},  {"minecraft:killed_by", "entity_type"},
    {"minecraft:custom", "custom_stat"},
};

std::string entry_name(int type, int id) {
    if (id < 0) return "";
    switch (type) {
        case MINED:
            return size_t(id) < std::size(gen::BLOCKS) ? "minecraft:" + std::string(gen::BLOCKS[id].name) : "";
        case CRAFTED:
        case USED:
        case BROKEN:
        case PICKED_UP:
        case DROPPED:
            return size_t(id) < item_info.size() ? item_info[size_t(id)].name : "";
        case KILLED:
        case KILLED_BY:
        case CUSTOM: {
            const char* rname = type == CUSTOM ? "minecraft:custom_stat" : "minecraft:entity_type";
            for (auto& r : gen::REGISTRIES)
                if (std::string(r.name) == rname)
                    return id < r.count ? "minecraft:" + std::string(r.entries[id]) : "";
            return "";
        }
    }
    return "";
}
}  // namespace

int custom_stat(const char* name) { return reg_id("custom_stat", name); }

void award_stat(Player& p, int type, int id, int amount) {
    if (type < 0 || type > CUSTOM || id < 0 || amount == 0) return;
    State& s = st(p);
    uint32_t k = (uint32_t(type) << 24) | (uint32_t(id) & 0xFFFFFFu);
    int64_t next = std::clamp<int64_t>(int64_t(s.stats[k]) + amount, 0, INT32_MAX);
    s.stats[k] = int(next);
    s.dirty.insert(k);
}

void moved(Player& p, double ox, double oy, double oz, bool was_on_ground) {
    if (p.gamemode == 3 || p.g.dead) return;
    double dx = p.x - ox, dy = p.y - oy, dz = p.z - oz;
    int cm = int(std::llround(std::sqrt(dx * dx + dy * dy + dz * dz) * 100.0));
    int hcm = int(std::llround(std::hypot(dx, dz) * 100.0));
    if (p.net.flying) {
        if (cm > 0) custom(p, "fly_one_cm", cm);
        return;
    }
    if (p.on_ground) {
        if (hcm > 0) {
            if (p.g.sprinting) custom(p, "sprint_one_cm", hcm);
            else if (p.g.sneaking) custom(p, "crouch_one_cm", hcm);
            else custom(p, "walk_one_cm", hcm);
        }
    } else {
        if (was_on_ground && dy > 0.0) custom(p, "jump", 1);
        if (hcm > 0) custom(p, "fly_one_cm", hcm);
    }
}

void send_stats(Player& p) {
    State& s = st(p);
    Writer w;
    w.varint(play_cb::award_stats).varint(int(s.dirty.size()));
    for (uint32_t k : s.dirty) {
        auto it = s.stats.find(k);
        int v = it == s.stats.end() ? 0 : it->second;
        w.varint(int(k >> 24)).varint(int(k & 0xFFFFFFu)).varint(v);
    }
    s.dirty.clear();
    p.send(w);
}

void load_stats_file(Player& p) {
    State& s = st(p);
    std::string path = world_dir + "/players/stats/" + uuid_str(p) + ".json";
    if (!std::filesystem::is_regular_file(path)) return;
    try {
        Json j = Json::parseFile(path);
        const Json* st_obj = j.find("stats");
        if (!st_obj || !st_obj->isObject()) return;
        for (int t = 0; t < 9; t++) {
            const Json* group = st_obj->find(STAT_REGS[t].key);
            if (!group || !group->isObject()) continue;
            for (auto& [name, val] : group->obj) {
                int id = reg_id(STAT_REGS[t].reg, name);
                if (id < 0) continue;
                uint32_t k = (uint32_t(t) << 24) | (uint32_t(id) & 0xFFFFFFu);
                s.stats[k] = val.asInt();
                s.dirty.insert(k);
            }
        }
    } catch (const std::exception& e) {
        log_error("Couldn't parse player statistics in %s: %s", path.c_str(), e.what());
    }
}

void save_stats_file(const Player& p) {
    if (!p.prog) return;
    const State& s = *p.prog;
    std::string out = "{\n  \"stats\": {";
    bool first_group = true;
    for (int t = 0; t < 9; t++) {
        std::vector<std::pair<std::string, int>> entries;
        for (auto& [k, v] : s.stats) {
            if (int(k >> 24) != t || v <= 0) continue;
            std::string name = entry_name(t, int(k & 0xFFFFFFu));
            if (!name.empty()) entries.emplace_back(std::move(name), v);
        }
        if (entries.empty()) continue;
        std::sort(entries.begin(), entries.end());
        out += first_group ? "\n    " : ",\n    ";
        first_group = false;
        out += json_quote(STAT_REGS[t].key) + ": {";
        for (size_t i = 0; i < entries.size(); i++) {
            out += (i == 0 ? "\n      " : ",\n      ") + json_quote(entries[i].first) + ": " + std::to_string(entries[i].second);
        }
        out += "\n    }";
    }
    out += "\n  },\n  \"DataVersion\": " + std::to_string(DATA_VERSION) + "\n}\n";
    std::string dir = world_dir + "/players/stats";
    std::filesystem::create_directories(dir);
    std::string path = dir + "/" + uuid_str(p) + ".json", tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << out;
        if (!f) {
            log_error("Couldn't save player statistics to %s", path.c_str());
            return;
        }
    }
    std::filesystem::rename(tmp, path);
}

void stats_init() {
    PacketHandler prev_cc = handlers()[play_sb::client_command];
    on(play_sb::client_command, [prev_cc](Player& p, Reader& in, int id) {
        Reader peek = in;
        if (peek.varint() == 1) {
            send_stats(p);
            return;
        }
        if (prev_cc) prev_cc(p, in, id);
    });
    on_player_tick([](Player& p) {
        if (p.gamemode != 3) custom(p, "play_time", 1);
        custom(p, "total_world_time", 1);
        if (!p.g.dead) {
            custom(p, "time_since_death", 1);
            custom(p, "time_since_rest", 1);
        }
        if (p.g.sneaking && !p.g.dead) custom(p, "sneak_time", 1);
    });
}

}  // namespace progress
