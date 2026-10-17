// read_level_data / write_level_data (declared in world.hpp) in the vanilla 26.3 world layout:
//   level.dat                                   Data: Time, spawn, difficulty_settings, Version, ... + CraftPP
//   data/minecraft/game_rules.dat, weather.dat, world_clocks.dat, world_gen_settings.dat
//   dimensions/minecraft/overworld/data/minecraft/world_border.dat
// The key=value map itself is kept losslessly in level.dat's "CraftPP" compound (strings; SNBT values as
// save_level writes them). A world without it (made or last saved by vanilla) is read from the vanilla fields.
// An old text level.dat (key=value lines) is still read.
#include "../play.hpp"
#include "../commands/args.hpp"
#include "nbt_io.hpp"

#include <chrono>
#include <filesystem>

namespace levelio {

inline std::string data_file(const std::string& name) { return world_dir + "/data/minecraft/" + name + ".dat"; }
inline std::string border_file() { return world_dir + "/dimensions/minecraft/overworld/data/minecraft/world_border.dat"; }
inline Nbt data_of(const std::string& path) {  // SavedData file -> its "data" compound
    Nbt root;
    if (!nbtio::read_file(path, root)) return Nbt::compound();
    const Nbt* d = root.get("data");
    return d ? *d : Nbt::compound();
}
inline void write_data(const std::string& path, const Nbt& data) {
    Nbt root = Nbt::compound();
    root.set("data", data);
    root.set("DataVersion", Nbt::num(Nbt::Int, DATA_VERSION));
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    if (!nbtio::write_file(path, root)) log_error("Failed to save %s", path.c_str());
}
inline std::string strip_ns(const std::string& s) { return s.rfind("minecraft:", 0) == 0 ? s.substr(10) : s; }
inline Nbt snbt_or(const std::map<std::string, std::string>& kv, const std::string& k) {
    auto it = kv.find(k);
    if (it == kv.end()) return Nbt::compound();
    try { return parse_snbt(it->second); } catch (...) { return Nbt::compound(); }
}
inline const Nbt* field(const Nbt& n, const char* k) { return n.get(k); }
inline double dbl(const Nbt* n, double d) { return n && n->is_num() ? n->as_double() : d; }
inline int64_t lng(const Nbt* n, int64_t d) { return n && n->is_num() ? int64_t(n->as_double()) : d; }

// Vanilla files -> our keys (a world vanilla created or saved last).
inline std::map<std::string, std::string> from_vanilla(const Nbt& data) {
    std::map<std::string, std::string> kv;
    Nbt gen = data_of(data_file("world_gen_settings"));
    if (const Nbt* seed = gen.get("seed")) kv["level-seed"] = std::to_string(seed->i);
    std::string type = "normal";
    if (const Nbt* dims = gen.get("dimensions"))
        if (const Nbt* ow = dims->get("minecraft:overworld"))
            if (const Nbt* g = ow->get("generator"))
                if (const Nbt* t = g->get("type"); t && strip_ns(t->s) == "flat") type = "flat";
    kv["level-type"] = type;
    if (const Nbt* sp = data.get("spawn")) {
        if (const Nbt* pos = sp->get("pos"); pos && pos->arr.size() == 3) {
            kv["spawn-x"] = std::to_string(pos->arr[0]), kv["spawn-y"] = std::to_string(pos->arr[1]);
            kv["spawn-z"] = std::to_string(pos->arr[2]);
        }
        Nbt a = Nbt::compound();
        a.set("yaw", Nbt::real(Nbt::Float, dbl(sp->get("yaw"), 0)));
        a.set("pitch", Nbt::real(Nbt::Float, dbl(sp->get("pitch"), 0)));
        kv["spawn-angle"] = to_snbt(a);
    }
    kv["game-time"] = to_snbt(Nbt::num(Nbt::Long, lng(data.get("Time"), 0)));
    if (const Nbt* d = data.get("difficulty_settings")) {
        int diff = 1;
        if (const Nbt* n = d->get("difficulty"))
            for (int i = 0; i < 4; i++)
                if (n->s == DIFFICULTY_NAMES[i]) diff = i;
        kv["difficulty"] = to_snbt(Nbt::num(Nbt::Byte, diff));
        kv["difficulty-locked"] = to_snbt(Nbt::num(Nbt::Byte, lng(d->get("locked"), 0)));
    }
    Nbt rules = Nbt::compound();
    for (auto& [k, v] : data_of(data_file("game_rules")).comp) rules.set(strip_ns(k), Nbt::num(Nbt::Int, v.i));
    kv["game-rules"] = to_snbt(rules);
    Nbt w = data_of(data_file("weather")), ow = Nbt::compound();
    ow.set("clear_time", Nbt::num(Nbt::Int, lng(w.get("clear_weather_time"), 0)));
    ow.set("rain_time", Nbt::num(Nbt::Int, lng(w.get("rain_time"), 0)));
    ow.set("thunder_time", Nbt::num(Nbt::Int, lng(w.get("thunder_time"), 0)));
    ow.set("raining", Nbt::num(Nbt::Byte, lng(w.get("raining"), 0)));
    ow.set("thundering", Nbt::num(Nbt::Byte, lng(w.get("thundering"), 0)));
    kv["weather"] = to_snbt(ow);
    kv["world-clocks"] = to_snbt(data_of(data_file("world_clocks")));
    Nbt b = data_of(border_file());
    if (!b.comp.empty()) {
        Nbt ob = Nbt::compound();
        ob.set("center_x", Nbt::real(Nbt::Double, dbl(b.get("center_x"), 0)));
        ob.set("center_z", Nbt::real(Nbt::Double, dbl(b.get("center_z"), 0)));
        ob.set("size", Nbt::real(Nbt::Double, dbl(b.get("size"), 59999968)));
        ob.set("target", Nbt::real(Nbt::Double, dbl(b.get("lerp_target"), dbl(b.get("size"), 59999968))));
        ob.set("lerp_time", Nbt::num(Nbt::Long, lng(b.get("lerp_time"), 0)));
        ob.set("damage_per_block", Nbt::real(Nbt::Double, dbl(b.get("damage_per_block"), 0.2)));
        ob.set("safe_zone", Nbt::real(Nbt::Double, dbl(b.get("safe_zone"), 5)));
        ob.set("warning_blocks", Nbt::num(Nbt::Int, lng(b.get("warning_blocks"), 5)));
        ob.set("warning_time", Nbt::num(Nbt::Int, lng(b.get("warning_time"), 300)));
        kv["world-border"] = to_snbt(ob);
    }
    return kv;
}

// Our keys -> vanilla files (everything vanilla needs to open the world; scoreboard etc. stay CraftPP-only).
inline void write_vanilla(const std::map<std::string, std::string>& kv) {
    auto get = [&](const char* k, const std::string& d) { auto it = kv.find(k); return it == kv.end() ? d : it->second; };
    auto num_of = [&](const char* k, int64_t d) { Nbt n = snbt_or(kv, k); return lng(&n, d); };
    Nbt data = Nbt::compound();
    auto I = [](int64_t v) { return Nbt::num(Nbt::Int, v); };
    auto B = [](int64_t v) { return Nbt::num(Nbt::Byte, v); };
    auto S = [](const std::string& v) { return Nbt::str(v); };
    data.set("DataVersion", I(DATA_VERSION));
    data.set("version", I(19133));  // NBT_VERSION (anvil)
    Nbt ver = Nbt::compound();
    ver.set("Id", I(DATA_VERSION)), ver.set("Name", S(GAME_VERSION)), ver.set("Series", S("main")), ver.set("Snapshot", B(0));
    data.set("Version", ver);
    data.set("LevelName", S(std::filesystem::path(world_dir).filename().string()));  // display name
    data.set("GameType", I(std::max(0, gamemode_id(props["gamemode"]))));
    data.set("Time", Nbt::num(Nbt::Long, num_of("game-time", 0)));
    data.set("LastPlayed", Nbt::num(Nbt::Long, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::system_clock::now().time_since_epoch()).count()));
    Nbt sp = Nbt::compound(), pos;
    pos.t = Nbt::IntArray;
    pos.arr = {std::stoll(get("spawn-x", "0")), std::stoll(get("spawn-y", "64")), std::stoll(get("spawn-z", "0"))};
    Nbt angle = snbt_or(kv, "spawn-angle");
    sp.set("pos", pos);
    sp.set("yaw", Nbt::real(Nbt::Float, dbl(angle.get("yaw"), 0)));
    sp.set("pitch", Nbt::real(Nbt::Float, dbl(angle.get("pitch"), 0)));
    sp.set("dimension", S("minecraft:overworld"));
    data.set("spawn", sp);
    Nbt diff = Nbt::compound();
    int d = int(num_of("difficulty", 1));
    diff.set("difficulty", S(DIFFICULTY_NAMES[std::clamp(d, 0, 3)]));
    diff.set("hardcore", B(props["hardcore"] == "true"));
    diff.set("locked", B(num_of("difficulty-locked", 0)));
    data.set("difficulty_settings", diff);
    data.set("initialized", B(1));
    data.set("allowCommands", B(0));
    data.set("WasModded", B(0));
    Nbt brands = Nbt::make_list();
    brands.list.push_back(S("CraftPP"));
    data.set("ServerBrands", brands);
    Nbt packs = Nbt::compound(), en = Nbt::make_list(), dis = Nbt::make_list();
    en.list.push_back(S("vanilla"));
    for (const char* p : {"minecart_improvements", "redstone_experiments", "trade_rebalance"}) dis.list.push_back(S(p));
    packs.set("Enabled", en), packs.set("Disabled", dis);
    data.set("DataPacks", packs);
    Nbt ours = Nbt::compound();
    for (auto& [k, v] : kv) ours.set(k, S(v));
    data.set("CraftPP", ours);
    Nbt root = Nbt::compound();
    root.set("Data", data);
    std::filesystem::create_directories(world_dir);
    std::string path = world_dir + "/level.dat";
    std::error_code ec;
    if (std::filesystem::exists(path)) std::filesystem::copy_file(path, path + "_old", std::filesystem::copy_options::overwrite_existing, ec);
    if (!nbtio::write_file(path, root)) log_error("Failed to save %s", path.c_str());

    // world_gen_settings: the generator vanilla must continue with (flat = the default superflat preset).
    Nbt gen = Nbt::compound(), dims = Nbt::compound();
    gen.set("bonus_chest", B(0));
    gen.set("seed", Nbt::num(Nbt::Long, std::stoll(get("level-seed", "0"))));
    gen.set("generate_structures", B(props["generate-structures"] != "false"));
    auto dim = [&](const char* type, Nbt generator) {
        Nbt e = Nbt::compound();
        e.set("generator", generator), e.set("type", S(type));
        return e;
    };
    auto noise = [&](const char* settings, const char* preset) {
        Nbt g = Nbt::compound(), bs = Nbt::compound();
        g.set("settings", S(settings));
        if (preset) bs.set("preset", S(preset)), bs.set("type", S("minecraft:multi_noise"));
        else bs.set("type", S("minecraft:the_end"));
        g.set("biome_source", bs), g.set("type", S("minecraft:noise"));
        return g;
    };
    if (get("level-type", "normal") == "flat") {
        Nbt g = Nbt::compound(), st = Nbt::compound(), layers = Nbt::make_list();
        for (auto [block, h] : {std::pair{"minecraft:bedrock", 1}, {"minecraft:dirt", 2}, {"minecraft:grass_block", 1}}) {
            Nbt l = Nbt::compound();
            l.set("block", S(block)), l.set("height", I(h));
            layers.list.push_back(l);
        }
        st.set("biome", S("minecraft:plains")), st.set("features", B(0)), st.set("lakes", B(0));
        st.set("layers", layers), st.set("structure_overrides", S("minecraft:villages"));
        g.set("settings", st), g.set("type", S("minecraft:flat"));
        dims.set("minecraft:overworld", dim("minecraft:overworld", g));
    } else {
        dims.set("minecraft:overworld", dim("minecraft:overworld", noise("minecraft:overworld", "minecraft:overworld")));
    }
    dims.set("minecraft:the_nether", dim("minecraft:the_nether", noise("minecraft:nether", "minecraft:nether")));
    dims.set("minecraft:the_end", dim("minecraft:the_end", noise("minecraft:end", nullptr)));
    gen.set("dimensions", dims);
    write_data(data_file("world_gen_settings"), gen);

    if (kv.count("game-rules")) {
        Nbt rules = Nbt::compound(), ours_r = snbt_or(kv, "game-rules");
        for (auto& g : gen::GAME_RULES) {
            const Nbt* v = ours_r.get(g.id);
            int64_t x = v && v->is_num() ? int64_t(v->as_double()) : g.def;
            rules.set(std::string("minecraft:") + g.id, g.is_int ? I(x) : B(x != 0));
        }
        write_data(data_file("game_rules"), rules);
    }
    if (kv.count("weather")) {
        Nbt w = snbt_or(kv, "weather"), o = Nbt::compound();
        o.set("clear_weather_time", I(lng(w.get("clear_time"), 0)));
        o.set("rain_time", I(lng(w.get("rain_time"), 0)));
        o.set("thunder_time", I(lng(w.get("thunder_time"), 0)));
        o.set("raining", B(lng(w.get("raining"), 0)));
        o.set("thundering", B(lng(w.get("thundering"), 0)));
        write_data(data_file("weather"), o);
    }
    if (kv.count("world-clocks")) write_data(data_file("world_clocks"), snbt_or(kv, "world-clocks"));
    if (kv.count("world-border")) {
        Nbt b = snbt_or(kv, "world-border"), o = Nbt::compound();
        auto D = [](double v) { return Nbt::real(Nbt::Double, v); };
        o.set("center_x", D(dbl(b.get("center_x"), 0))), o.set("center_z", D(dbl(b.get("center_z"), 0)));
        o.set("damage_per_block", D(dbl(b.get("damage_per_block"), 0.2))), o.set("safe_zone", D(dbl(b.get("safe_zone"), 5)));
        o.set("warning_blocks", I(lng(b.get("warning_blocks"), 5))), o.set("warning_time", I(lng(b.get("warning_time"), 300)));
        o.set("size", D(dbl(b.get("size"), 59999968))), o.set("lerp_time", Nbt::num(Nbt::Long, lng(b.get("lerp_time"), 0)));
        o.set("lerp_target", D(dbl(b.get("target"), dbl(b.get("size"), 59999968))));
        write_data(border_file(), o);
    }
}

}  // namespace levelio

std::map<std::string, std::string> read_level_data() {
    std::string path = world_dir + "/level.dat";
    Nbt root;
    if (nbtio::read_file(path, root)) {
        const Nbt* data = root.get("Data");
        if (!data) return {};
        std::map<std::string, std::string> kv;
        if (const Nbt* ours = data->get("CraftPP"))
            for (auto& [k, v] : ours->comp) kv[k] = v.s;
        else
            kv = levelio::from_vanilla(*data);
        return kv;
    }
    std::map<std::string, std::string> kv;  // text level.dat (before region files)
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);)
        if (auto eq = line.find('='); eq != std::string::npos && line[0] != '#') kv[line.substr(0, eq)] = line.substr(eq + 1);
    return kv;
}

void write_level_data(const std::map<std::string, std::string>& changes) {
    auto kv = read_level_data();
    auto old = kv;
    for (auto& [k, v] : changes) kv[k] = v;
    Nbt root;
    bool current = nbtio::read_file(world_dir + "/level.dat", root) && root.get("Data") && root.get("Data")->get("CraftPP");
    if (kv == old && current) return;  // unchanged: spare the SD card
    levelio::write_vanilla(kv);
}
