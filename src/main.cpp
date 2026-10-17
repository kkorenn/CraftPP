#include "paper.hpp"
#include "net/netsec.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <iterator>

static std::string status_json(int protocol) {
    std::string motd;
    {
        std::lock_guard lock(cmd_mtx);
        motd = props["motd"];
    }
    std::string icon = netsec::load_status_icon();
    // Echo a matching protocol so the list shows the MOTD; mismatched clients see the required version.
    return "{\"version\":{\"name\":\"CraftPP " + std::string(GAME_VERSION) + "\",\"protocol\":" +
           std::to_string(protocol == PROTOCOL_VERSION ? protocol : PROTOCOL_VERSION) +
           "},\"players\":{\"max\":" + std::to_string(std::stoi(props["max-players"])) +
           ",\"online\":" + std::to_string(snapshot().size()) +
           "},\"description\":{\"text\":\"" + json_escape(motd) + "\"}" +
           (icon.empty() ? "" : ",\"favicon\":\"" + json_escape(icon) + "\"") +
           ",\"enforcesSecureChat\":" + (netsec::enforces_secure_chat() ? "true" : "false") + "}";
}

// "/ip:port" like Java's InetSocketAddress (or vanilla's placeholder when log-ips=false).
static std::string peer_address(int fd) {
    if (props["log-ips"] == "false") return "<ip address withheld>";
    sockaddr_in a{};
    socklen_t n = sizeof a;
    char ip[INET_ADDRSTRLEN] = "?";
    if (getpeername(fd, (sockaddr*)&a, &n) == 0) inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    return std::string("/") + ip + ":" + std::to_string(ntohs(a.sin_port));
}

static void disconnect_login(int fd, const std::string& msg, const std::string& name = "") {
    send_packet(fd, Writer().varint(login_cb::login_disconnect).str("{\"text\":\"" + json_escape(msg) + "\"}"));
    if (name.empty()) return;  // e.g. wrong protocol version: vanilla only logs it at debug level
    std::string who = name + " (" + peer_address(fd) + ")";
    log_info("Disconnecting %s: %s", who.c_str(), msg.c_str());
    log_info("%s lost connection: %s", who.c_str(), msg.c_str());
}

// Wait for a specific packet id, ignoring others (brand, etc.). Configuration's client_information is kept in `info`.
static Reader wait_for(int fd, std::vector<uint8_t>& pkt, int id, std::vector<uint8_t>* info = nullptr) {
    for (;;) {
        if (!read_packet(fd, pkt)) throw std::runtime_error("disconnected");
        Reader r(pkt);
        int got = r.varint();
        if (got == id) return r;
        if (info && got == config_sb::client_information) *info = pkt;
    }
}

static void handle(int fd) {
    conn_compressed = false;
    conn_rbuf = {};
    conn_dec.reset();
    conn_enc.reset();
    std::vector<uint8_t> pkt;
    PlayerPtr player;
    try {
        if (!read_packet(fd, pkt)) return;
        Reader r(pkt);
        if (r.varint() != 0x00) return;  // Handshake
        int32_t protocol = r.varint();
        r.str(255);  // server address
        r.u16();     // port
        int32_t next = r.varint();

        if (next == 1) {  // Status
            while (read_packet(fd, pkt)) {
                Reader q(pkt);
                int32_t id = q.varint();
                if (id == 0x00) {
                    send_packet(fd, Writer().varint(0x00).str(status_json(protocol)));
                } else if (id == 0x01) {
                    send_packet(fd, Writer().varint(0x01).i64(q.i64()));
                    break;
                }
            }
            return;
        }
        if (next != 2 && next != 3) return;
        if (protocol != PROTOCOL_VERSION) return disconnect_login(fd, std::string("CraftPP needs Minecraft ") + GAME_VERSION);

        // Login (offline mode or online-mode RSA/AES-CFB8 + Mojang hasJoined).
        Reader hello = wait_for(fd, pkt, login_sb::hello);
        std::string name = hello.str(16);
        if (!valid_player_name(name)) throw std::runtime_error("Invalid characters in username");
        int64_t uuid_hi = hello.i64(), uuid_lo = hello.i64();
        std::vector<ProfileProperty> profile_props;
        if (!netsec::authenticate_login(fd, pkt, name, uuid_hi, uuid_lo, profile_props,
                                        [](int f, std::vector<uint8_t>& p, int id) { return wait_for(f, p, id); },
                                        [](int f, const std::string& m, const std::string& n) { disconnect_login(f, m, n); }))
            return;
        if (auto msg = login_check(name, uuid_hi, uuid_lo, fd); !msg.empty()) return disconnect_login(fd, msg, name);
        // PlayerList.disconnectAllPlayersWithProfile, then WAITING_FOR_DUPE_DISCONNECT (600 ticks) until they are
        // saved and gone. Offline uuids come from the client, so the same name counts as the same profile too.
        for (int t = 0;; t++) {
            std::vector<PlayerPtr> dupes;
            for (auto& q : snapshot())
                if ((q->uuid_hi == uuid_hi && q->uuid_lo == uuid_lo) || q->name == name) dupes.push_back(q);
            if (dupes.empty()) break;
            if (t == 0)
                run_on_tick([dupes] { for (auto& q : dupes) disconnect(*q, tr("multiplayer.disconnect.duplicate_login")); });
            if (t == 600) return disconnect_login(fd, "Took too long to log in", name);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (int(snapshot().size()) >= std::stoi(props["max-players"])) return disconnect_login(fd, "The server is full!", name);
        start_compression(fd, login_cb::login_compression);  // net.hpp, network-compression-threshold
        Writer lf;
        lf.varint(login_cb::login_finished).i64(uuid_hi).i64(uuid_lo).str(name).varint(int32_t(profile_props.size()));
        for (const auto& pr : profile_props) {
            lf.str(pr.name).str(pr.value).boolean(!pr.sig.empty());
            if (!pr.sig.empty()) lf.str(pr.sig);
        }
        lf.i64(0).i64(0);  // session id
        send_packet(fd, lf);
        wait_for(fd, pkt, login_sb::login_acknowledged);

        // Configuration.
        send_packet(fd, Writer().varint(config_cb::custom_payload).str("minecraft:brand").str("CraftPP"));
        send_packet(fd, Writer().varint(config_cb::update_enabled_features).varint(1).str("minecraft:vanilla"));
        send_packet(fd, Writer()
                            .varint(config_cb::select_known_packs)
                            .varint(1).str("minecraft").str("core").str(GAME_VERSION));
        std::vector<uint8_t> client_info;
        wait_for(fd, pkt, config_sb::select_known_packs, &client_info);
        auto cfg = reframe(config_packets, conn_compressed);
        send_raw(fd, cfg.data(), cfg.size());
        if (!netsec::negotiate_resource_pack(fd, pkt, client_info, name)) return;
        send_packet(fd, Writer().varint(config_cb::finish_configuration));
        wait_for(fd, pkt, config_sb::finish_configuration, &client_info);

        player = std::make_shared<Player>();
        player->fd = fd;
        player->name = name;
        player->uuid_hi = uuid_hi;
        player->uuid_lo = uuid_lo;
        player->properties = std::move(profile_props);
        player->out.enc = conn_enc;
        player->eid = next_entity_id++;
        {
            std::lock_guard lock(cmd_mtx);  // /defaultgamemode rewrites props["gamemode"] on the tick thread
            player->gamemode = std::max(0, gamemode_id(props["gamemode"]));
        }
        if (!client_info.empty()) {  // same body as the play packet (ServerboundClientInformationPacket)
            Reader ci(client_info);
            ci.varint();
            read_client_information(*player, ci);
        }
        if (!load_player(*player)) place_new_player(*player);
        record_usercache(name, uuid_dashed(uuid_hi, uuid_lo));
        log_info("%s[%s] logged in with entity id %d at ([%s]%s, %s, %s)", name.c_str(), peer_address(fd).c_str(),
                 player->eid, props["level-name"].c_str(), java_double(player->x).c_str(), java_double(player->y).c_str(),
                 java_double(player->z).c_str());
        play(player);
    } catch (const std::exception& e) {
        if (!player && strcmp(e.what(), "disconnected")) log_warn("Connection error: %s", e.what());
    }
    // play() returns only after the tick thread has run the leave hooks and saved the player (tick.hpp).
}

static void request_shutdown(int) { request_shutdown_async(); }

int main(int argc, char** argv) {
    auto boot = std::chrono::steady_clock::now();
    signal(SIGPIPE, SIG_IGN);
    if (pipe(shutdown_pipe) != 0) shutdown_pipe[0] = shutdown_pipe[1] = -1;
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) signal(sig, request_shutdown);  // all save first, like the JVM's hook
    std::string props_path = argc > 1 ? argv[1] : "server.properties";
    server_dir = std::filesystem::path(props_path).parent_path().string();
    if (server_dir.empty()) server_dir = ".";
    logging::open(server_dir + "/logs");
    utsname u{};
    uname(&u);
    log_info("[bootstrap] Running CraftPP (C++%ld) on %s %s (%s)", __cplusplus / 100 % 100, u.sysname, u.release, u.machine);
    log_info("[bootstrap] Loading CraftPP %s for Minecraft %s", IMPLEMENTATION_VERSION.c_str(), GAME_VERSION);
    load_properties(props_path);
    if (!check_eula(server_dir)) return 0;
    ensure_paper_files(server_dir);
    if (!std::filesystem::exists("data/vanilla/minecraft")) {  // Mojang's game data is fetched at runtime, never shipped
        log_info("Downloading Minecraft %s server data from Mojang (tools/setup_vanilla.sh)", GAME_VERSION);
        if (std::system("sh tools/setup_vanilla.sh") != 0 || !std::filesystem::exists("data/vanilla/minecraft")) {
            log_error("Could not set up data/vanilla: run tools/setup_vanilla.sh (needs curl and unzip)");
            return 1;
        }
    }

    log_info("Starting minecraft server version %s", GAME_VERSION);
    log_info("Loading properties");
    log_info("This server is running CraftPP version %s (MC: %s) (Implementing API version %s-R0.1-SNAPSHOT)",
             IMPLEMENTATION_VERSION.c_str(), GAME_VERSION, GAME_VERSION);
    static const char* game_types[] = {"SURVIVAL", "CREATIVE", "ADVENTURE", "SPECTATOR"};
    log_info("Default game type: %s", game_types[std::max(0, gamemode_id(props["gamemode"]))]);
    std::ifstream cfg("data/config.bin", std::ios::binary);
    if (!cfg) {
        log_error("Missing data/config.bin, run: python3 tools/gen_data.py");
        return 1;
    }
    config_packets.assign(std::istreambuf_iterator<char>(cfg), {});

    int port = std::stoi(props["server-port"]);
    const std::string& ip = props["server-ip"];
    log_info("Starting Minecraft server on %s:%d", ip.empty() ? "*" : ip.c_str(), port);
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = INADDR_ANY;
    if (!ip.empty() && inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (sockaddr*)&addr, sizeof addr) < 0 || listen(srv, 64) < 0) {
        log_warn("**** FAILED TO BIND TO PORT!");
        log_warn("The exception was: %s", strerror(errno));
        log_warn("Perhaps a server is already running on that port?");
        logging::close();
        return 1;
    }
    if (props["online-mode"] != "true") {
        log_warn("**** SERVER IS RUNNING IN OFFLINE/INSECURE MODE!");
        log_warn("The server will make no attempt to authenticate usernames. Beware.");
        log_warn("While this makes the game possible to play without internet access, it also opens up the ability for "
                 "hackers to connect with any username they choose.");
        log_warn("To change this, set \"online-mode\" to \"true\" in the server.properties file.");
    }

    log_info("Preparing level \"%s\"", props["level-name"].c_str());
    auto level_start = std::chrono::steady_clock::now();
    world_dir = props["level-name"];
    log_info("Preparing start region for dimension minecraft:overworld");
    setup_world();  // generator + spawn search (generates the spawn chunks)
    auto secs = [](auto since) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count(); };
    log_info("Time elapsed: %lld ms", (long long)(secs(level_start) * 1000));
    log_info("Done preparing level \"%s\" (%.3fs)", props["level-name"].c_str(), secs(level_start));
    build_commands_packet();
    register_core_handlers();
    netsec::register_netsec_handlers();
    register_command_handlers();
    register_paper_commands();
    on_tick(advance_time);  // world clocks, weather, border (commands/system.hpp)
    on_tick(broadcast_light);  // light_update for block edits (src/light)
    start_game();  // tick thread (game/tick.hpp)
    netsec::start_query_thread();
    netsec::start_rcon_thread();
    log_info("Running delayed init tasks");
    log_info("Done (%.3fs)! For help, type \"help\"", secs(boot));

    console::complete = [](const std::string& text) {
        std::promise<std::pair<size_t, std::vector<std::string>>> done;
        auto result = done.get_future();
        run_on_tick([&] {  // suggestions read game state: tick thread only
            auto [start, list] = suggestions(text, console_source());
            std::vector<std::string> out;
            for (auto& s : list) out.push_back(s.text);
            done.set_value({start, out});
        });
        return result.get();
    };
    std::thread([] {
        // A pipe or file ending means whoever drove the console is gone: stop like `stop`. /dev/null (systemd,
        // backgrounded shells) is a character device and ends at once, so it leaves the server running.
        struct stat st{};
        bool stop_on_eof = fstat(0, &st) == 0 && !S_ISCHR(st.st_mode);
        console::run(server_dir + "/.console_history", [](const std::string& raw) {
            std::string line = raw;
            line.erase(0, line.find_first_not_of(' '));
            while (!line.empty() && line.back() == ' ') line.pop_back();
            if (!line.empty() && line[0] == '/') line.erase(0, 1);  // Paper accepts both
            if (line.empty()) return;
            run_on_tick([line] { if (!plugins::console_command(line)) run_command(nullptr, line); });
        }, [stop_on_eof] { if (stop_on_eof) request_shutdown_async(); });
    }).detach();

    std::thread([] {  // autosave every 5 s; on stop/Ctrl+C save everything and exit
        auto next = std::chrono::steady_clock::now();
        for (int tick = 24;; tick += 25) {  // `tick` counts the old 200 ms steps: tick % 25 == 24 every 5 s
            next += std::chrono::seconds(5);
            for (;;) {  // sleep until the next autosave or a shutdown request, whichever comes first
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(next - std::chrono::steady_clock::now());
                if (shutdown_requested == 1 || left.count() <= 0) break;
                pollfd pfd{shutdown_pipe[0], POLLIN, 0};
                if (poll(&pfd, 1, int(left.count()) + 1) > 0) { char b[16]; (void)!read(shutdown_pipe[0], b, sizeof b); }
            }
            if (shutdown_requested == 1) {
                shutdown_requested = 2;
                log_info("Stopping server");
                plugins::stop();  // onDisable before the world is saved
                request_stop();  // saves on the tick thread, then exits
            }
            if (tick % 25 == 24 && !autosave_off && shutdown_requested == 0) {
                for (auto& w : worlds) w.save_dirty();
                if (tick % 1500 == 1499) run_on_tick(save_level);  // level.dat: vanilla autosave period, 6000 ticks
            }
        }
    }).detach();

    for (;;) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) {  // e.g. EMFILE: the pending connection stays queued, so back off instead of spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        // Vanilla's 30 s ReadTimeoutHandler; the send timeout bounds blocking sends during login/configuration.
        timeval timeout{30, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        // ponytail: thread per connection; move to an event loop once player counts matter.
        try {
            std::thread([fd] { handle(fd); close(fd); }).detach();
        } catch (const std::system_error& e) {  // out of threads: drop this connection, keep serving
            log_warn("Connection refused: %s", e.what());
            close(fd);
        }
    }
}
