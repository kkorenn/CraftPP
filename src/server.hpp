#pragma once
// Shared server state: config, players, broadcast, persistence, chunk streaming.
#include "gen/ids.hpp"
#include "log.hpp"
#include "net.hpp"
#include "world.hpp"
#include "gen/command_data.hpp"
#include "storage/nbt_io.hpp"
#include "game/player_state.hpp"
#include "progress/api.hpp"
#include "worldgen/block_state.hpp"
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
#include <future>
#include <charconv>
#include <climits>
#include <unordered_map>
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
inline std::atomic<int> shutdown_requested{0};  // lock-free: safe from signal handlers and any thread
inline int shutdown_pipe[2] = {-1, -1};  // wakes the autosave thread (main.cpp) without polling
inline void request_shutdown_async() {  // any thread or signal handler (write() is async-signal-safe)
    shutdown_requested = 1;
    if (shutdown_pipe[1] >= 0) (void)!write(shutdown_pipe[1], "x", 1);
}
inline bool flat_world = false;
inline std::array<std::shared_ptr<structure::StructureWorld>, DIM_COUNT> structure_worlds;  // null: structures off (flat, generate-structures=false)
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

struct ProfileProperty {
    std::string name, value, sig;
};

struct Player {
    int fd;
    std::string name;
    int64_t uuid_hi, uuid_lo;
    std::vector<ProfileProperty> properties;
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
    std::shared_ptr<progress::State> prog;  // advancements, recipe book, stats (src/progress/)
    NetState net;              // keep-alive, teleport confirmation, client options (game/netplay.hpp)
    Outbox out;                // queued outgoing packets, flushed by the tick thread (src/game/tick.hpp)
    Inbox in;                  // serverbound play packets waiting for the tick thread
    ViewTicket view;           // keeps the view square loaded (world.hpp)
    std::vector<std::pair<std::pair<int, int>, std::shared_future<ChunkPtr>>> chunk_queue;  // nearest first
    std::string quit_reason;   // logged as "<name> lost connection: <reason>" (empty = "Disconnected")

    // Queues a packet; it goes out at the end of the tick (or on the next flush()). Never blocks on the socket.
    void send(const Writer& body) {  // framed straight into the outbox (net.hpp)
        std::lock_guard lock(out.m);
        if (!out.closed) {
            size_t from = out.buf.size();
            frame_packet(out.buf, body.b.data(), body.b.size(), compression_threshold >= 0);
            out.encrypt_tail(from);
        }
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
// ponytail: place_new_player's free_at treats every non-air, non-fluid block as a full cube; use collision
// shapes there too when players should spawn among plants.
inline bool is_fluid(uint16_t b) {
    return (b >= blocks::water && b <= blocks::water + 15) || (b >= blocks::lava && b <= blocks::lava + 15);
}
inline bool is_solid(uint16_t b) { return b != blocks::air && b != blocks::cave_air && b != blocks::void_air && !is_fluid(b); }

// PlayerSpawnFinder.getLevelRespawnPos: y to stand on at column (x, z), or INT_MIN. Heightmap predicates as in
// the worldgen pipeline (Heightmap.Types), standing = collision shape's top face is full.
inline int level_respawn_y(int x, int z) {
    const auto& R = bs::R();
    static const bs::Tag& motion = R.tag("minecraft:blocks_motion_in_heightmap");
    ChunkPtr cp = worlds[DIM_OVERWORLD].get_ptr(x >> 4, z >> 4);
    Chunk& c = *cp;
    std::lock_guard lock(c.m);
    auto at = [&](int y) { return y >= MIN_Y + HEIGHT ? int(blocks::air) : int(c.get(x & 15, y, z & 15)); };
    int top = MIN_Y, surface = MIN_Y, ocean = MIN_Y;  // MOTION_BLOCKING, WORLD_SURFACE, OCEAN_FLOOR heights
    for (int y = MIN_Y + HEIGHT - 1; y >= MIN_Y && ocean == MIN_Y; y--) {
        int b = at(y);
        if (surface == MIN_Y && !R.is_air(b)) surface = y + 1;
        if (top == MIN_Y && (R.in(b, motion) || R.has_fluid(b))) top = y + 1;
        if (R.in(b, motion)) ocean = y + 1;
    }
    if (surface <= top && surface > ocean) return INT_MIN;
    for (int y = top + 1; y >= MIN_Y; y--) {
        int b = at(y);
        if (R.has_fluid(b)) break;
        if (R.collision_face_full(b, bs::UP)) return y + 1;
    }
    return INT_MIN;
}

// Brings the chunks a FULL (cx, cz) depends on up to TERRAIN on the worker pool. TERRAIN is per-chunk
// deterministic and most of the cost; features, light and full then run inline as before.
inline void prefetch_terrain(worldgen::Pipeline& pl, int cx, int cz) {
    using worldgen::Status;
    const auto& need = pl.accumulated(Status::FULL);  // status needed at chessboard distance r
    int r = 0;
    while (r + 1 < int(need.size()) && need[size_t(r + 1)] >= Status::TERRAIN) r++;
    std::vector<std::future<void>> jobs;
    for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
            auto job = std::make_shared<std::packaged_task<void()>>([&pl, x = cx + dx, z = cz + dz] { pl.ensure(x, z, Status::TERRAIN); });
            jobs.push_back(job->get_future());
            worlds[DIM_OVERWORLD].pool.submit([job] { (*job)(); });
        }
    for (auto& j : jobs) j.get();
}

inline SpawnPos find_world_spawn(worldgen::Overworld& ow) {
    auto [sx, sz] = ow.terrain->findSpawnPosition();
    int cx = sx >> 4, cz = sz >> 4;
    SpawnPos spawn{cx * 16 + 8, 64, cz * 16 + 8};  // ChunkGenerator.getSpawnHeight = 64
    for (int i = 0, x = 0, z = 0, dx = 0, dz = -1; i < 121; i++) {
        if (x >= -5 && x <= 5 && z >= -5 && z <= 5) {
            prefetch_terrain(ow.pipeline, cx + x, cz + z);
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
        w.open(std::string("minecraft:") + d.name, d.type, 0, 256, world_dir + "/dimensions/minecraft/" + d.name, d.dim);
        w.coordinate_scale = d.scale, w.skylight = d.sky, w.ceiling = d.ceiling, w.sea_level = d.sea;
        w.logical_height = d.logical;
        auto gen = std::make_shared<std::unique_ptr<worldgen::Overworld>>();
        auto once = std::make_shared<std::once_flag>();
        auto initialized = std::make_shared<std::atomic<bool>>(false);
        std::string settings = d.settings, stages = props["worldgen-stages"], pack = props["worldgen-datapack"], folder = w.dir;
        bool structures = props["generate-structures"] != "false" && (stages == "structures" || stages == "features");
        int dim = d.dim;
        auto init = [=] {
            std::call_once(*once, [&] {
                *gen = std::make_unique<worldgen::Overworld>(seed, stages, "data/vanilla", pack, settings, 0, 256);
                (*gen)->pipeline.dimension = dim;
                (*gen)->pipeline.persist = true;
                light::attach(worlds[dim], (*gen)->pipeline);
                if (structures) std::atomic_store(&structure_worlds[dim], std::make_shared<structure::StructureWorld>(**gen, true, dim, folder));
                initialized->store(true, std::memory_order_release);
            });
        };
        w.initialize_generator = init;
        w.generator = [=](Chunk& c) { init(); (*gen)->pipeline.take(c.cx, c.cz, c); };
        w.ticket = [=](int cx, int cz, bool add) { init(); add ? (*gen)->pipeline.ticket(cx, cz) : (*gen)->pipeline.cancel(cx, cz); };
        w.on_saved = [dim] { if (auto sw = std::atomic_load(&structure_worlds[dim])) sw->manager().saveDirty(); };
        w.on_flush = [gen, initialized, dim](bool all) {
            if (!initialized->load(std::memory_order_acquire)) return;  // This dimension has never initialized.
            auto& pipeline = (*gen)->pipeline;
            int margin = int(pipeline.accumulated(worldgen::Status::FULL).size()) + 1;
            pipeline.evict(worlds[dim].near_view(margin), all);
        };
    }
}
inline void on_player_sync(void (*f)(Player&));  // game/tick.hpp
inline void deliver_chunks(Player& p);

inline void setup_world() {
    worlds[DIM_OVERWORLD].start(props.count("worker-threads") ? std::stoi(props["worker-threads"]) : 0);
    compression_threshold = std::stoi(props["network-compression-threshold"]);
    storage::sync_writes = props["sync-chunk-writes"] != "false";
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
    if (flat_world) {
        // Flat worlds skip noise-generator setup, but must still restore a changed world spawn.
        if (level.count("spawn-x") && level.count("spawn-y") && level.count("spawn-z"))
            world_spawn = {std::stoi(level["spawn-x"]), std::stoi(level["spawn-y"]), std::stoi(level["spawn-z"])};
        return;
    }
    auto ow = std::make_shared<worldgen::Overworld>(seed, props["worldgen-stages"], "data/vanilla", props["worldgen-datapack"]);
    const std::string& stages = props["worldgen-stages"];
    if (props["generate-structures"] != "false" && (stages == "structures" || stages == "features"))
    {
        auto sw = std::make_shared<structure::StructureWorld>(*ow, true);
        std::atomic_store(&structure_worlds[DIM_OVERWORLD], sw);
        worlds[DIM_OVERWORLD].on_saved = [sw] { sw->manager().saveDirty(); };
    }
    worlds[DIM_OVERWORLD].generator = [ow](Chunk& c) { ow->pipeline.take(c.cx, c.cz, c); };
    worlds[DIM_OVERWORLD].ticket = [ow](int cx, int cz, bool add) { add ? ow->pipeline.ticket(cx, cz) : ow->pipeline.cancel(cx, cz); };
    light::attach(worlds[DIM_OVERWORLD], ow->pipeline);
    // Proto chunks beyond the generation pyramid are persisted and unloaded.
    ow->pipeline.persist = true;
    int margin = int(ow->pipeline.accumulated(worldgen::Status::FULL).size()) + 1;
    worlds[DIM_OVERWORLD].on_flush = [ow, margin](bool all) { ow->pipeline.evict(worlds[DIM_OVERWORLD].near_view(margin), all); };
    level = read_level_data();
    if (level.count("spawn-x") && level.count("spawn-y") && level.count("spawn-z")) {
        world_spawn = {std::stoi(level["spawn-x"]), std::stoi(level["spawn-y"]), std::stoi(level["spawn-z"])};
    } else {
        world_spawn = find_world_spawn(*ow);
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

inline uint8_t angle(float deg) { return uint8_t(Mth::floor(deg * 256.0f / 360.0f)); }  // Mth.packDegrees

// player_info_update with ADD_PLAYER | UPDATE_GAME_MODE | UPDATE_LISTED | UPDATE_LATENCY | UPDATE_HAT
inline Writer info_add(const std::vector<PlayerPtr>& list) {
    Writer w;
    w.varint(play_cb::player_info_update).u8(0x01 | 0x04 | 0x08 | 0x10 | 0x80).varint(int32_t(list.size()));
    for (auto& p : list) {
        w.i64(p->uuid_hi).i64(p->uuid_lo).str(p->name).varint(int32_t(p->properties.size()));
        for (const auto& pr : p->properties) {
            w.str(pr.name).str(pr.value).boolean(!pr.sig.empty());
            if (!pr.sig.empty()) w.str(pr.sig);
        }
        w.varint(p->gamemode).boolean(true).varint(p->net.latency).boolean(p->net.skin_parts & 0x40);
    }
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

// Other clients learn player moves from the entity tracker (src/mobs/manager.hpp: ChunkMap.TrackedEntity with
// range min(512, view * 16), relative moves every 2 ticks); nothing to send here.
inline void broadcast_position(const Player&) {}

// ---------- player data ----------

inline std::string uuid_hex(const Player& p) {
    char buf[33];
    snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)p.uuid_hi, (unsigned long long)p.uuid_lo);
    return buf;
}

// players/data/<uuid>.dat (vanilla 26.x PlayerDataStorage): gzip NBT like ServerPlayer.saveWithoutId. Old
// playerdata/<uuid>.txt files ("x y z yaw pitch gamemode selected", "slot item count" lines, extras) are read once.
inline std::string player_dat(const Player& p) {
    char b[40];
    uint64_t h = uint64_t(p.uuid_hi), l = uint64_t(p.uuid_lo);
    snprintf(b, sizeof b, "%08x-%04x-%04x-%04x-%012llx", unsigned(h >> 32), unsigned(h >> 16 & 0xFFFF), unsigned(h & 0xFFFF),
             unsigned(l >> 48), (unsigned long long)(l & 0xFFFFFFFFFFFFull));
    return world_dir + "/players/data/" + b + ".dat";
}
inline const char* const EQUIPMENT_SLOTS[5] = {"feet", "legs", "chest", "head", "offhand"};  // inv 36..40

// Data components beyond damage (ItemStack::comp) <-> the "components" compound: src/containers/item_nbt.hpp.
inline void (*item_components_to_nbt)(const ItemStack& s, Nbt& components) = nullptr;
inline void (*item_components_from_nbt)(ItemStack& s, const Nbt& components) = nullptr;
inline Nbt item_nbt(const ItemStack& s) {  // ItemStack.CODEC: {id, count, components}
    Nbt e = Nbt::compound();
    e.set("id", Nbt::str(std::string("minecraft:") + gen::ITEM_NAMES[s.item]));
    e.set("count", Nbt::num(Nbt::Int, s.count));
    if (s.comp && item_components_to_nbt) {
        Nbt c = Nbt::compound();
        if (s.damage) c.set("minecraft:damage", Nbt::num(Nbt::Int, s.damage));
        item_components_to_nbt(s, c);
        if (!c.comp.empty()) e.set("components", c);
    } else if (s.damage) {
        Nbt c = Nbt::compound();
        c.set("minecraft:damage", Nbt::num(Nbt::Int, s.damage));
        e.set("components", c);
    }
    return e;
}
inline ItemStack item_from(const Nbt& e) {
    static const std::unordered_map<std::string, int> ids = [] {
        std::unordered_map<std::string, int> m;
        for (int i = 0; i < ITEM_COUNT; i++) m["minecraft:" + std::string(gen::ITEM_NAMES[i])] = i;
        return m;
    }();
    ItemStack s;
    const Nbt* id = e.get("id");
    auto it = id ? ids.find(id->s.find(':') == std::string::npos ? "minecraft:" + id->s : id->s) : ids.end();
    if (it == ids.end() || it->second == 0) return s;
    s.item = it->second;
    const Nbt* n = e.get("count");
    s.count = n && n->is_num() ? int(n->i) : 1;
    if (const Nbt* c = e.get("components")) {
        if (const Nbt* d = c->get("minecraft:damage"); d && d->is_num()) s.damage = int(d->i);
        if (item_components_from_nbt) item_components_from_nbt(s, *c);
    }
    return s;
}

inline bool load_player_txt(Player& p) {
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
    load_player_extra_txt(p, f);
    return true;
}

inline bool load_player(Player& p) {
    Nbt t;
    if (!nbtio::read_file(player_dat(p), t)) return load_player_txt(p);
    auto D = [](const Nbt* n, double d) { return n && n->is_num() ? n->as_double() : d; };
    if (const Nbt* pos = t.get("Pos"); pos && pos->list.size() == 3)
        p.x = pos->list[0].d, p.y = pos->list[1].d, p.z = pos->list[2].d;
    if (const Nbt* r = t.get("Rotation"); r && r->list.size() == 2) p.yaw = float(r->list[0].d), p.pitch = float(r->list[1].d);
    if (const Nbt* dim = t.get("Dimension"); dim && dim->t == Nbt::String)
        if (World* w = level_by_id(dim->s)) p.level = w;
    p.seen_credits = D(t.get("seenCredits"), 0) != 0;
    p.on_ground = D(t.get("OnGround"), 1) != 0;
    p.gamemode = std::clamp(int(D(t.get("playerGameType"), p.gamemode)), 0, 3);
    p.selected = std::clamp(int(D(t.get("SelectedItemSlot"), 0)), 0, 8);
    if (const Nbt* inv = t.get("Inventory"))
        for (auto& e : inv->list)
            if (int slot = int(D(e.get("Slot"), -1)); slot >= 0 && slot < 36) p.inv[size_t(slot)] = item_from(e);
    if (const Nbt* eq = t.get("equipment"))
        for (int i = 0; i < 5; i++)
            if (const Nbt* e = eq->get(EQUIPMENT_SLOTS[i])) p.inv[size_t(36 + i)] = item_from(*e);
    load_player_extra(p, t);  // game/survival.hpp: vitals, xp
    return true;
}

inline void save_player(const Player& p) {
    Nbt t = Nbt::compound();
    auto list = [](std::initializer_list<Nbt> v) { Nbt l = Nbt::make_list(); l.list = v; return l; };
    auto Dbl = [](double v) { return Nbt::real(Nbt::Double, v); };
    auto Flt = [](double v) { return Nbt::real(Nbt::Float, v); };
    t.set("DataVersion", Nbt::num(Nbt::Int, DATA_VERSION));
    t.set("Pos", list({Dbl(p.x), Dbl(p.y), Dbl(p.z)}));
    t.set("Motion", list({Dbl(0), Dbl(0), Dbl(0)}));
    t.set("Rotation", list({Flt(p.yaw), Flt(p.pitch)}));
    t.set("OnGround", Nbt::num(Nbt::Byte, p.on_ground));
    Nbt uuid;
    uuid.t = Nbt::IntArray;
    uuid.arr = {int32_t(uint64_t(p.uuid_hi) >> 32), int32_t(p.uuid_hi), int32_t(uint64_t(p.uuid_lo) >> 32), int32_t(p.uuid_lo)};
    t.set("UUID", uuid);
    t.set("Dimension", Nbt::str(p.level->id));
    t.set("seenCredits", Nbt::num(Nbt::Byte, p.seen_credits));
    t.set("playerGameType", Nbt::num(Nbt::Int, p.gamemode));
    t.set("SelectedItemSlot", Nbt::num(Nbt::Int, p.selected));
    Nbt inv = Nbt::make_list(), eq = Nbt::compound();
    for (int i = 0; i < 41; i++) {
        const ItemStack& s = p.inv[size_t(i)];
        if (s.empty() || s.item >= ITEM_COUNT) continue;
        Nbt e = item_nbt(s);
        if (i >= 36) { eq.set(EQUIPMENT_SLOTS[i - 36], e); continue; }
        e.set("Slot", Nbt::num(Nbt::Byte, i));
        inv.list.push_back(e);
    }
    t.set("Inventory", inv);
    t.set("equipment", eq);
    save_player_extra(p, t);
    std::filesystem::create_directories(world_dir + "/players/data");
    nbtio::write_file(player_dat(p), t);
}

inline void (*on_save_all)() = nullptr;          // tick thread, before chunks are saved (level tick lists)
inline void (*before_world_save)() = nullptr;    // tick thread: flush live block entities into chunks (src/containers)
inline void save_all() {
    if (on_save_all) on_save_all();
    if (before_world_save) before_world_save();
    for (auto& p : snapshot()) save_player(*p);
    for (auto& w : worlds) w.save_dirty(true);
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
                log_error("chunk %d,%d failed: %s", e.first.first, e.first.second, ex.what());
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
    int v = view_distance();
    std::vector<PlayerPtr> ps;
    for (World& level : worlds) {
        if (level.fullbright) continue;
        auto changes = light::take_changes(level);
        if (changes.empty()) continue;
        if (ps.empty()) ps = snapshot();
        for (auto& ch : changes) {
            Writer w;
            for (auto& p : ps) {
                if (p->level != &level) continue;
                int dx = std::abs(ch.cx - p->cx), dz = std::abs(ch.cz - p->cz);
                if (dx > v || dz > v || (dx < v && dz < v && !ch.all)) continue;
                if (w.b.empty()) light::write_update(level, w.varint(play_cb::light_update), ch);
                p->send(w);
            }
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
    // Decoration order: vanilla generates by ticket level (chessboard distance from the player), FIFO within a
    // level; the view square's chunks plus the ring the client needs as neighbours (v + 1).
    if (p.level->ticket) {
        auto cheb = [&](int x, int z) { return std::max(std::abs(x - ncx), std::abs(z - ncz)); };
        for (int r = 0; r <= v + 1; r++)
            for (int x = ncx - r; x <= ncx + r; x++)
                for (int z = ncz - r; z <= ncz + r; z++)
                    if (cheb(x, z) == r && (initial || std::max(std::abs(x - ocx), std::abs(z - ocz)) > v + 1)) p.level->ticket(x, z, true);
    }
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

// ServerGamePacketListenerImpl.teleport: a new teleport id; movement is ignored until the client accepts it.
inline int next_teleport_id(Player& p) {
    if (++p.net.teleport_id == INT_MAX) p.net.teleport_id = 0;
    p.net.awaiting = true;
    p.net.awaiting_since = p.net.ticks;
    p.net.await_x = p.x, p.net.await_y = p.y, p.net.await_z = p.z;
    return p.net.teleport_id;
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
// `rel`: the teleport's Relative flags (8 Y_ROT, 16 X_ROT: yaw/pitch are deltas; 32..256 DELTA: velocity kept).
inline void move_to_level(Player& p, World* to, double x, double y, double z, float yaw, float pitch, uint8_t keep, int rel = 0);
inline void change_dimension(Player& p, World* to, double x, double y, double z, float yaw, float pitch, int rel = 0) {
    move_to_level(p, to, x, y, z, yaw, pitch, 3, rel);
}

inline void teleport(Player& p, double x, double y, double z) {
    p.x = x; p.y = y; p.z = z;
    p.send(Writer().varint(play_cb::player_position).varint(next_teleport_id(p))
               .f64(x).f64(y).f64(z).f64(0).f64(0).f64(0).f32(p.yaw).f32(p.pitch).i32(0));
    int ncx = int(std::floor(x)) >> 4, ncz = int(std::floor(z)) >> 4;
    if (ncx != p.cx || ncz != p.cz) update_view(p, ncx, ncz, false);
    broadcast_position(p);
}

// Runs a command line (without '/'). `p` is null for the console.
