#pragma once
// Shared server state: config, players, broadcast, persistence, chunk streaming.
#include "gen/ids.hpp"
#include "log.hpp"
#include "net.hpp"
#include "world.hpp"
#include "game/player_state.hpp"
#include "worldgen/random.hpp"
#include "worldgen/material.hpp"
#include "worldgen/overworld.hpp"
#include "worldgen/structure/hooks.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <charconv>
#include <climits>
#include <random>
#include <sstream>
#include <thread>

#include <unistd.h>

// Filled from server.properties by load_properties (paper.hpp), which also supplies vanilla's defaults.
inline std::map<std::string, std::string> props = {
    {"motd", "A Minecraft Server"},
    {"server-port", "25565"},
    {"max-players", "20"},
    {"view-distance", "10"},
    {"gamemode", "survival"},
    {"level-name", "world"},
    {"level-seed", ""},
    {"level-type", "minecraft:normal"},  // [minecraft:]normal | [minecraft:]flat
    {"worldgen-stages", "features"},  // noise | surface | carvers | structures | features (debug: stop after a vanilla stage)
    {"generate-structures", "true"},
};
inline std::atomic<int> next_entity_id{1};
inline std::vector<uint8_t> config_packets;  // data/config.bin: pre-framed registry + tag packets
inline std::vector<uint8_t> commands_packet;
inline volatile std::sig_atomic_t shutdown_requested = 0;
inline int shutdown_pipe[2] = {-1, -1};  // wakes the autosave thread (main.cpp) without polling
inline void request_shutdown_async() {  // any thread or signal handler (write() is async-signal-safe)
    shutdown_requested = 1;
    if (shutdown_pipe[1] >= 0) (void)!write(shutdown_pipe[1], "x", 1);
}
inline bool flat_world = false;
inline std::shared_ptr<structure::StructureWorld> structure_world;  // null: structures off (flat, generate-structures=false)
struct SpawnPos { int x, y, z; };
inline SpawnPos world_spawn{0, -60, 0};  // LevelData respawn position (flat: the old fixed spawn)
inline int64_t hashed_seed = 0;  // BiomeManager.obfuscateSeed(seed), set by setup_world

// WorldOptions.parseSeed: trimmed; empty -> random, Long.parseLong, else String.hashCode.
inline int64_t parse_seed(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    if (s.empty()) return int64_t(std::random_device{}()) << 32 ^ std::random_device{}();
    const char* b = s.data() + (s[0] == '+' && s.size() > 1 && s[1] != '-' ? 1 : 0);
    int64_t v;
    auto [end, ec] = std::from_chars(b, s.data() + s.size(), v);
    if (ec == std::errc() && end == s.data() + s.size()) return v;
    return javaStringHashCode(s);
}

// ---------- players ----------

struct Player {
    int fd;
    std::string name;
    int64_t uuid_hi, uuid_lo;
    int eid;
    int gamemode;
    double x = 0.5, y = -60, z = 0.5;
    float yaw = 0, pitch = 0;
    bool on_ground = true;
    int cx = 0, cz = 0;  // chunk the client's view is centered on
    World* level = &worlds[DIM_OVERWORLD];  // dimension the player is in (game/dimensions.hpp changes it)
    // Entity.portalCooldown + PortalProcessor (game/portal.hpp): portal kind (0 none), entry block, ticks inside.
    int portal_cooldown = 0, portal_kind = 0, portal_x = 0, portal_y = 0, portal_z = 0, portal_time = 0;
    bool portal_this_tick = false;
    bool seen_credits = false, won_game = false;
    using Slot = ItemStack;    // {item, count, damage}
    std::array<Slot, 41> inv;  // vanilla Inventory indices: 0-8 hotbar, 9-35 main, 36-39 armor, 40 offhand
    int selected = 0;          // hotbar slot 0-8
    PlayerGame g;              // survival state, owned by the tick thread (src/game/)
    Outbox out;                // queued outgoing packets, flushed by the tick thread (src/game/tick.hpp)
    Inbox in;                  // serverbound play packets waiting for the tick thread
    ViewTicket view;           // keeps the view square loaded (world.hpp)
    std::vector<std::pair<std::pair<int, int>, std::shared_future<ChunkPtr>>> chunk_queue;  // nearest first
    std::string quit_reason;   // logged as "<name> lost connection: <reason>" (empty = "Disconnected")

    // Queues a packet; it goes out at the end of the tick (or on the next flush()). Never blocks on the socket.
    void send(const Writer& body) {  // framed straight into the outbox (net.hpp)
        std::lock_guard lock(out.m);
        if (!out.closed) frame_packet(out.buf, body.b.data(), body.b.size(), compression_threshold >= 0);
    }
    void send_raw_bytes(const std::vector<uint8_t>& b) { out.push_framed(reframe(b, compression_threshold >= 0)); }
    void message(const std::string& text, const std::string& color = "") {
        Writer w;
        w.varint(play_cb::system_chat);
        text_component(w, text, color);
        send(w.boolean(false));
    }
};
using PlayerPtr = std::shared_ptr<Player>;

// ---------- spawn (MinecraftServer.setInitialSpawn, PlayerSpawnFinder) ----------
// ponytail: block shapes approximated: every non-air, non-fluid block is a full cube (true for noise + material
// rule output); add real collision shapes with features (leaves, snow layers, plants).
inline bool is_fluid(uint16_t b) {
    return (b >= blocks::water && b <= blocks::water + 15) || (b >= blocks::lava && b <= blocks::lava + 15);
}
inline bool is_solid(uint16_t b) { return b != blocks::air && b != blocks::cave_air && b != blocks::void_air && !is_fluid(b); }

// PlayerSpawnFinder.getLevelRespawnPos: y to stand on at column (x, z), or INT_MIN.
inline int level_respawn_y(int x, int z) {
    ChunkPtr cp = worlds[DIM_OVERWORLD].get_ptr(x >> 4, z >> 4);
    Chunk& c = *cp;
    std::lock_guard lock(c.m);
    auto at = [&](int y) { return y >= MIN_Y + HEIGHT ? uint16_t(blocks::air) : c.get(x & 15, y, z & 15); };
    int top = MIN_Y, surface = MIN_Y, ocean = MIN_Y;  // MOTION_BLOCKING, WORLD_SURFACE, OCEAN_FLOOR heights
    for (int y = MIN_Y + HEIGHT - 1; y >= MIN_Y && ocean == MIN_Y; y--) {
        uint16_t b = at(y);
        if (surface == MIN_Y && b != blocks::air && b != blocks::cave_air && b != blocks::void_air) surface = y + 1;
        if (top == MIN_Y && (is_solid(b) || is_fluid(b))) top = y + 1;
        if (is_solid(b)) ocean = y + 1;
    }
    if (surface <= top && surface > ocean) return INT_MIN;
    for (int y = top + 1; y >= MIN_Y; y--) {
        uint16_t b = at(y);
        if (is_fluid(b)) break;
        if (is_solid(b)) return y + 1;
    }
    return INT_MIN;
}

inline SpawnPos find_world_spawn(worldgen::TerrainGenerator& gen) {
    auto [sx, sz] = gen.findSpawnPosition();
    int cx = sx >> 4, cz = sz >> 4;
    SpawnPos spawn{cx * 16 + 8, 64, cz * 16 + 8};  // ChunkGenerator.getSpawnHeight = 64
    for (int i = 0, x = 0, z = 0, dx = 0, dz = -1; i < 121; i++) {
        if (x >= -5 && x <= 5 && z >= -5 && z <= 5) {
            for (int bx = (cx + x) * 16; bx < (cx + x) * 16 + 16; bx++)  // getSpawnPosInChunk
                for (int bz = (cz + z) * 16; bz < (cz + z) * 16 + 16; bz++)
                    if (int y = level_respawn_y(bx, bz); y != INT_MIN) return {bx, y, bz};
        }
        if (x == z || (x < 0 && x == -z) || (x > 0 && x == 1 - z)) {
            int old = dx;
            dx = -dz, dz = old;
        }
        x += dx, z += dz;
    }
    return spawn;
}

// PlayerSpawnFinder.findSpawn with respawn_radius 10: a random column near the world spawn with room to stand.
inline void place_new_player(Player& p) {
    p.level = &worlds[DIM_OVERWORLD];
    p.x = world_spawn.x + 0.5, p.y = world_spawn.y, p.z = world_spawn.z + 0.5;
    if (flat_world) return;  // fixed spawn: tests rely on it
    auto free_at = [](int x, int y, int z) {  // noCollisionNoLiquid for a player box at the block's bottom center
        for (int yy = y; yy <= y + 1; yy++) {
            if (yy < MIN_Y || yy >= MIN_Y + HEIGHT) continue;
            ChunkPtr c = worlds[DIM_OVERWORLD].get_ptr(x >> 4, z >> 4);
            std::lock_guard lock(c->m);
            uint16_t b = c->get(x & 15, yy, z & 15);
            if (is_solid(b) || is_fluid(b)) return false;
        }
        return true;
    };
    const int r = 10, side = 2 * r + 1, n = side * side, coprime = 17;
    int offset = int(std::random_device{}() % n);
    for (int i = 0; i < n; i++) {
        int v = (offset + coprime * i) % n;
        int x = world_spawn.x + v % side - r, z = world_spawn.z + v / side - r;
        int y = level_respawn_y(x, z);
        if (y != INT_MIN && free_at(x, y, z)) {
            p.x = x + 0.5, p.y = y, p.z = z + 0.5;
            return;
        }
    }
    int y = world_spawn.y;  // fixupSpawnHeight
    while (!free_at(world_spawn.x, y, world_spawn.z) && y < MIN_Y + HEIGHT - 1) y++;
    y--;
    while (free_at(world_spawn.x, y, world_spawn.z) && y > MIN_Y) y--;
    p.y = y + 1;
}

// The world folder remembers its generator: level.dat wins over server.properties once the world exists.
// A world folder with saved chunks but no level.dat predates noise generation: treat it as flat.
// The three vanilla levels (dimension_type/*.json layouts). The overworld keeps CraftPP's <world>/chunks layout; the
// others live where 26.x puts them (DimensionType.getStorageFolder: <world>/dimensions/<namespace>/<path>). Their
// generators compile on first use, so a dimension nobody visits costs no memory or startup time.
inline void setup_dimensions(int64_t seed) {
    worlds[DIM_OVERWORLD].open("minecraft:overworld", dimension_types::overworld, -64, 384, world_dir);
    struct Def { Dim dim; const char* name; int type; const char* settings; double scale; bool sky, ceiling; int sea, logical; };
    for (const Def& d : {Def{DIM_NETHER, "the_nether", dimension_types::the_nether, "nether", 8.0, false, true, 32, 128},
                         Def{DIM_END, "the_end", dimension_types::the_end, "end", 1.0, true, false, 0, 256}}) {
        World& w = worlds[d.dim];
        w.open(std::string("minecraft:") + d.name, d.type, 0, 256, world_dir + "/dimensions/minecraft/" + d.name);
        w.coordinate_scale = d.scale, w.skylight = d.sky, w.ceiling = d.ceiling, w.sea_level = d.sea;
        w.logical_height = d.logical;
        w.fullbright = true;  // ponytail: the light engine is overworld-only (fixed layout); full light here
        auto gen = std::make_shared<std::unique_ptr<worldgen::TerrainGenerator>>();
        auto once = std::make_shared<std::once_flag>();
        bool surface = props["worldgen-stages"] != "noise";
        std::string settings = d.settings;
        w.generator = [=](Chunk& c) {
            std::call_once(*once, [&] {
                *gen = std::make_unique<worldgen::TerrainGenerator>(seed, "vanilla/src/data", surface, settings, 0, 256);
            });
            (*gen)->generate(c);
        };
    }
}
inline void on_player_sync(void (*f)(Player&));  // game/tick.hpp
inline void deliver_chunks(Player& p);

inline void setup_world() {
    worlds[DIM_OVERWORLD].start(props.count("worker-threads") ? std::stoi(props["worker-threads"]) : 0);
    compression_threshold = std::stoi(props["network-compression-threshold"]);
    on_player_sync(deliver_chunks);
    auto level = read_level_data();
    std::string type, seed_str;
    if (level.count("level-type") && level.count("level-seed")) {
        type = level["level-type"], seed_str = level["level-seed"];
    } else {
        std::error_code ec;
        bool legacy = std::filesystem::is_directory(world_dir + "/chunks", ec) &&
                      !std::filesystem::is_empty(world_dir + "/chunks", ec);
        std::string want = props["level-type"];
        for (auto& ch : want) ch = char(tolower((unsigned char)ch));
        type = legacy || want == "flat" || want == "minecraft:flat" ? "flat" : "normal";
        if (legacy) log_warn("Existing world without level.dat: keeping it flat");
        seed_str = std::to_string(parse_seed(props["level-seed"]));
        write_level_data({{"level-type", type}, {"level-seed", seed_str}});
    }
    flat_world = type == "flat";
    int64_t seed = parse_seed(seed_str);
    hashed_seed = material::MaterialSystem::obfuscateSeed(seed);
    setup_dimensions(seed);
    if (flat_world) return;
    auto ow = std::make_shared<worldgen::Overworld>(seed, props["worldgen-stages"], "vanilla/src/data", props["worldgen-datapack"]);
    const std::string& stages = props["worldgen-stages"];
    if (props["generate-structures"] != "false" && (stages == "structures" || stages == "features"))
    {
        structure_world = std::make_shared<structure::StructureWorld>(*ow, true);
        worlds[DIM_OVERWORLD].on_saved = [sw = structure_world] { sw->manager().saveDirty(); };
    }
    auto gen = ow->terrain;
    worlds[DIM_OVERWORLD].generator = [ow](Chunk& c) { ow->pipeline.take(c.cx, c.cz, c); };
    light::attach(worlds[DIM_OVERWORLD], ow->pipeline);
    level = read_level_data();
    if (level.count("spawn-x") && level.count("spawn-y") && level.count("spawn-z")) {
        world_spawn = {std::stoi(level["spawn-x"]), std::stoi(level["spawn-y"]), std::stoi(level["spawn-z"])};
    } else {
        world_spawn = find_world_spawn(*gen);
        write_level_data({{"spawn-x", std::to_string(world_spawn.x)}, {"spawn-y", std::to_string(world_spawn.y)},
                          {"spawn-z", std::to_string(world_spawn.z)}});
    }
}


inline std::mutex players_mtx;
inline std::vector<PlayerPtr> players;

inline std::vector<PlayerPtr> snapshot() {
    std::lock_guard lock(players_mtx);
    return players;
}

inline PlayerPtr find_player(const std::string& name) {
    for (auto& p : snapshot())
        if (p->name == name) return p;
    return nullptr;
}

inline void broadcast(const Writer& body, const Player* except = nullptr) {
    thread_local std::vector<uint8_t> f;  // framed (and compressed) once for everyone
    f.clear();
    frame_packet(f, body.b.data(), body.b.size(), compression_threshold >= 0);
    std::lock_guard lock(players_mtx);  // no copy: push_framed only appends to the player's outbox
    for (auto& p : players)
        if (p.get() != except) p->out.push_framed(f);
}

// Sends to every player in `level` (block changes, entities: what vanilla sends per ServerLevel).
inline void broadcast_level(const Writer& body, const World* level, const Player* except = nullptr) {
    std::lock_guard lock(players_mtx);
    for (auto& p : players)
        if (p.get() != except && p->level == level) p->send(body);
}

inline void broadcast_message(const std::string& text, const std::string& color = "") {
    char code = logging::color_code(color);
    log_info("%s%s", code ? (std::string("\xC2\xA7") + code).c_str() : "", text.c_str());
    Writer w;
    w.varint(play_cb::system_chat);
    text_component(w, text, color);
    broadcast(w.boolean(false));
}

inline uint8_t angle(float deg) { return uint8_t(int(std::floor(deg * 256.0f / 360.0f))); }

// player_info_update with ADD_PLAYER | UPDATE_GAME_MODE | UPDATE_LISTED | UPDATE_LATENCY
inline Writer info_add(const std::vector<PlayerPtr>& list) {
    Writer w;
    w.varint(play_cb::player_info_update).u8(0x01 | 0x04 | 0x08 | 0x10).varint(int32_t(list.size()));
    for (auto& p : list)
        w.i64(p->uuid_hi).i64(p->uuid_lo).str(p->name).varint(0).varint(p->gamemode).boolean(true).varint(0);
    return w;
}

inline Writer spawn_entity(const Player& p) {
    return std::move(Writer()
                         .varint(play_cb::add_entity)
                         .varint(p.eid)
                         .i64(p.uuid_hi).i64(p.uuid_lo)
                         .varint(entity_types::player)
                         .f64(p.x).f64(p.y).f64(p.z)
                         .u8(0)  // velocity (LpVec3 zero)
                         .u8(angle(p.pitch)).u8(angle(p.yaw)).u8(angle(p.yaw))
                         .varint(0));
}

inline void broadcast_position(const Player& p) {
    broadcast_level(Writer()
                  .varint(play_cb::teleport_entity)
                  .varint(p.eid)
                  .f64(p.x).f64(p.y).f64(p.z)
                  .f64(0).f64(0).f64(0)
                  .f32(p.yaw).f32(p.pitch)
                  .i32(0)
                  .boolean(p.on_ground),
              p.level, &p);
    broadcast_level(Writer().varint(play_cb::rotate_head).varint(p.eid).u8(angle(p.yaw)), p.level, &p);
}

// ---------- player data ----------

inline std::string uuid_hex(const Player& p) {
    char buf[33];
    snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)p.uuid_hi, (unsigned long long)p.uuid_lo);
    return buf;
}

// playerdata/<uuid>.txt: "x y z yaw pitch gamemode selected [dimension [seen_credits]]" then "slot item count" lines.
inline bool load_player(Player& p) {
    std::ifstream f(world_dir + "/playerdata/" + uuid_hex(p) + ".txt");
    if (!(f >> p.x >> p.y >> p.z >> p.yaw >> p.pitch >> p.gamemode >> p.selected)) return false;
    std::string rest, dim;
    std::getline(f, rest);
    std::istringstream line(rest);
    if (line >> dim)
        if (World* w = level_by_id(dim)) p.level = w;  // unknown dimension: vanilla falls back to the overworld
    line >> p.seen_credits;
    for (int slot, item, count; f >> slot >> item >> count;)
        if (slot >= 0 && slot < int(p.inv.size()) && item >= 0 && item < ITEM_COUNT) p.inv[slot] = {item, count};
    load_player_extra(p, f);  // game/survival.hpp: vitals, xp, tool damage
    return true;
}

inline void save_player(const Player& p) {
    std::filesystem::create_directories(world_dir + "/playerdata");
    std::string path = world_dir + "/playerdata/" + uuid_hex(p) + ".txt";
    {
        std::ofstream f(path + ".tmp");
        f.precision(17);
        f << p.x << ' ' << p.y << ' ' << p.z << ' ' << p.yaw << ' ' << p.pitch << ' ' << p.gamemode << ' '
          << p.selected << ' ' << p.level->id << ' ' << p.seen_credits << '\n';
        for (size_t i = 0; i < p.inv.size(); i++)
            if (p.inv[i].item >= 0) f << i << ' ' << p.inv[i].item << ' ' << p.inv[i].count << '\n';
        save_player_extra(p, f);
    }
    std::filesystem::rename(path + ".tmp", path);
}

inline void save_all() {
    for (auto& p : snapshot()) save_player(*p);
    for (auto& w : worlds) w.save_dirty();
}

// Creative-menu slot number -> Inventory index (-1 = not a player inventory slot).
inline int menu_to_inv(int slot) {
    if (slot >= 36 && slot <= 44) return slot - 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (slot >= 5 && slot <= 8) return 39 - (slot - 5);
    if (slot == 45) return 40;
    return -1;
}

// ---------- chunks ----------

inline int view_distance() { return std::stoi(props["view-distance"]); }

inline void push_chunk(Player& p, Chunk& c) {
    if (!c.fullbright) light::ensure_lit(*p.level, c);  // 3x3 lit: the light in the frame is final
    p.out.push_framed(*chunk_frame(c));
}
inline void send_chunk(Player& p, int cx, int cz) { push_chunk(p, *p.level->get_ptr(cx, cz)); }

// Tick thread (on_player_sync): send queued chunks whose generation finished, nearest first, a bounded batch per tick.
inline constexpr int CHUNKS_PER_TICK = 32;
inline void deliver_chunks(Player& p) {
    auto& q = p.chunk_queue;
    int sent = 0;
    size_t keep = 0;
    for (auto& e : q) {
        if (sent < CHUNKS_PER_TICK && e.second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                ChunkPtr c = e.second.get();
                if (!sent) p.send(Writer().varint(play_cb::chunk_batch_start));
                push_chunk(p, *c);
                sent++;
            } catch (const ChunkCancelled&) {  // raced with leaving + re-entering the view: ask again
                e.second = p.level->request(e.first.first, e.first.second);
                q[keep++] = std::move(e);
            } catch (const std::exception& ex) {
                fprintf(stderr, "chunk %d,%d failed: %s\n", e.first.first, e.first.second, ex.what());
            }
            continue;
        }
        q[keep++] = std::move(e);
    }
    q.resize(keep);
    if (sent) p.send(Writer().varint(play_cb::chunk_batch_finished).varint(sent));
}

// Tick hook: light changed by block edits -> light_update to players that have the chunk on their view border
// (ChunkHolder.broadcastChanges: clients relight edits themselves, except where a neighbour chunk is missing).
inline void broadcast_light() {
    auto changes = light::take_changes();
    if (changes.empty()) return;
    int v = view_distance();
    auto ps = snapshot();
    for (auto& ch : changes) {
        Writer w;
        for (auto& p : ps) {
            if (p->level != &worlds[DIM_OVERWORLD]) continue;  // the light engine lights the overworld only
            int dx = std::abs(ch.cx - p->cx), dz = std::abs(ch.cz - p->cz);
            if (dx > v || dz > v || (dx < v && dz < v && !ch.all)) continue;
            if (w.b.empty()) light::write_update(w.varint(play_cb::light_update), ch);
            p->send(w);
        }
    }
}

// Send chunks entering the view square, forget ones leaving it. Chunks are generated on the worker pool and
// delivered by deliver_chunks() as they finish (vanilla order: nearest first). During join (network thread,
// outbox not live yet) it waits and queues them all before returning, so the client has them on spawn.
inline void update_view(Player& p, int ncx, int ncz, bool initial) {
    int v = view_distance();
    int ocx = p.cx, ocz = p.cz;
    p.cx = ncx;
    p.cz = ncz;
    auto in_old = [&](int x, int z) { return !initial && std::abs(x - ocx) <= v && std::abs(z - ocz) <= v; };
    auto in_new = [&](int x, int z) { return std::abs(x - ncx) <= v && std::abs(z - ncz) <= v; };
    p.view.move(*p.level, ncx, ncz, v);

    p.send(Writer().varint(play_cb::set_chunk_cache_center).varint(ncx).varint(ncz));
    if (!initial)
        for (int x = ocx - v; x <= ocx + v; x++)
            for (int z = ocz - v; z <= ocz + v; z++)
                if (!in_new(x, z))
                    p.send(Writer().varint(play_cb::forget_level_chunk)
                               .i64(int64_t((uint64_t(uint32_t(z)) << 32) | uint32_t(x))));
    auto& q = p.chunk_queue;
    if (initial) q.clear();
    else std::erase_if(q, [&](auto& e) { return !in_new(e.first.first, e.first.second); });
    auto dist = [&](const std::pair<int, int>& k) {
        int dx = k.first - ncx, dz = k.second - ncz;
        return dx * dx + dz * dz;
    };
    std::vector<std::pair<int, int>> add;
    for (int x = ncx - v; x <= ncx + v; x++)
        for (int z = ncz - v; z <= ncz + v; z++)
            if (!in_old(x, z)) add.push_back({x, z});
    std::stable_sort(add.begin(), add.end(), [&](auto& a, auto& b) { return dist(a) < dist(b); });
    for (auto& k : add) q.push_back({k, p.level->request(k.first, k.second)});
    std::stable_sort(q.begin(), q.end(), [&](auto& a, auto& b) { return dist(a.first) < dist(b.first); });

    if (p.out.fd < 0) {  // joining
        p.send(Writer().varint(play_cb::chunk_batch_start));
        for (auto& e : q) push_chunk(p, *e.second.get());
        p.send(Writer().varint(play_cb::chunk_batch_finished).varint(int32_t(q.size())));
        q.clear();
    }
}

inline int gamemode_id(const std::string& s) {
    if (s == "survival" || s == "0") return 0;
    if (s == "creative" || s == "1") return 1;
    if (s == "adventure" || s == "2") return 2;
    if (s == "spectator" || s == "3") return 3;
    return -1;
}
inline const char* gamemode_names[] = {"Survival", "Creative", "Adventure", "Spectator"};
inline void set_gamemode(Player& p, int gm) {
    p.gamemode = gm;
    p.send(Writer().varint(play_cb::game_event).u8(3).f32(float(gm)));
    broadcast(Writer().varint(play_cb::player_info_update).u8(0x04).varint(1)
                  .i64(p.uuid_hi).i64(p.uuid_lo).varint(gm));
}

// CommonPlayerSpawnInfo for the player's level (login and respawn packets).
inline Writer& write_spawn_info(Writer& w, const Player& p) {
    const World& l = *p.level;
    return w.varint(l.type).str(l.id).i64(hashed_seed)
        .varint(p.gamemode)
        .varint(0)       // previous gamemode: none
        .boolean(false)  // debug world
        .boolean(flat_world && &l == &worlds[DIM_OVERWORLD])
        .boolean(false)  // no last death location
        .varint(p.portal_cooldown)
        .varint(l.sea_level);
}

// Moves a player into `to` with a respawn packet (game/dimensions.hpp): ServerPlayer.teleport across levels
// (keep = 3, all data) and PlayerList.respawn (keep = 0 after death, 1 after the end credits).
inline void move_to_level(Player& p, World* to, double x, double y, double z, float yaw, float pitch, uint8_t keep);
inline void change_dimension(Player& p, World* to, double x, double y, double z, float yaw, float pitch) {
    move_to_level(p, to, x, y, z, yaw, pitch, 3);
}

inline void teleport(Player& p, double x, double y, double z) {
    p.x = x; p.y = y; p.z = z;
    p.send(Writer().varint(play_cb::player_position).varint(0)
               .f64(x).f64(y).f64(z).f64(0).f64(0).f64(0).f32(p.yaw).f32(p.pitch).i32(0));
    int ncx = int(std::floor(x)) >> 4, ncz = int(std::floor(z)) >> 4;
    if (ncx != p.cx || ncz != p.cz) update_view(p, ncx, ncz, false);
    broadcast_position(p);
}

// Runs a command line (without '/'). `p` is null for the console.
