#include "play.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <iostream>
#include <iterator>

static void load_properties(const char* path) {
    std::ifstream f(path);
    if (!f) {  // write defaults so the user has something to edit
        std::ofstream o(path);
        for (auto& [k, v] : props) o << k << '=' << v << '\n';
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq != std::string::npos) props[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

static std::string status_json(int protocol) {
    // Echo a matching protocol so the list shows the MOTD; mismatched clients see the required version.
    return "{\"version\":{\"name\":\"CraftPP " + std::string(GAME_VERSION) + "\",\"protocol\":" +
           std::to_string(protocol == PROTOCOL_VERSION ? protocol : PROTOCOL_VERSION) +
           "},\"players\":{\"max\":" + std::to_string(std::stoi(props["max-players"])) +
           ",\"online\":" + std::to_string(snapshot().size()) +
           "},\"description\":{\"text\":\"" + json_escape(props["motd"]) + "\"}}";
}

static void disconnect_login(int fd, const std::string& msg) {
    send_packet(fd, Writer().varint(login_cb::login_disconnect).str("{\"text\":\"" + json_escape(msg) + "\"}"));
}

// Wait for a specific packet id, ignoring others (client_information, brand, etc.).
static Reader wait_for(int fd, std::vector<uint8_t>& pkt, int id) {
    for (;;) {
        if (!read_packet(fd, pkt)) throw std::runtime_error("disconnected");
        Reader r(pkt);
        if (r.varint() == id) return r;
    }
}

static void handle(int fd) {
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

        // Login (offline mode: trust the client's name + uuid, no encryption).
        Reader hello = wait_for(fd, pkt, login_sb::hello);
        std::string name = hello.str(16);
        int64_t uuid_hi = hello.i64(), uuid_lo = hello.i64();
        if (auto msg = login_check(name, uuid_hi, uuid_lo, fd); !msg.empty()) return disconnect_login(fd, msg);
        if (find_player(name)) return disconnect_login(fd, "You are already logged in");
        if (int(snapshot().size()) >= std::stoi(props["max-players"])) return disconnect_login(fd, "The server is full!");
        send_packet(fd, Writer()
                            .varint(login_cb::login_finished)
                            .i64(uuid_hi).i64(uuid_lo).str(name).varint(0)  // profile, no properties
                            .i64(0).i64(0));                                 // session id
        wait_for(fd, pkt, login_sb::login_acknowledged);

        // Configuration.
        send_packet(fd, Writer().varint(config_cb::custom_payload).str("minecraft:brand").str("CraftPP"));
        send_packet(fd, Writer().varint(config_cb::update_enabled_features).varint(1).str("minecraft:vanilla"));
        send_packet(fd, Writer()
                            .varint(config_cb::select_known_packs)
                            .varint(1).str("minecraft").str("core").str(GAME_VERSION));
        wait_for(fd, pkt, config_sb::select_known_packs);
        send_raw(fd, config_packets.data(), config_packets.size());
        send_packet(fd, Writer().varint(config_cb::finish_configuration));
        wait_for(fd, pkt, config_sb::finish_configuration);

        player = std::make_shared<Player>();
        player->fd = fd;
        player->name = name;
        player->uuid_hi = uuid_hi;
        player->uuid_lo = uuid_lo;
        player->eid = next_entity_id++;
        player->gamemode = std::max(0, gamemode_id(props["gamemode"]));
        if (!load_player(*player)) place_new_player(*player);
        printf("%s logged in with entity id %d\n", name.c_str(), player->eid);
        play(player);
    } catch (const std::exception& e) {
        fprintf(stderr, "client error: %s\n", e.what());
    }
    if (player) {
        save_player(*player);
        bool registered = false;
        {
            std::lock_guard lock(players_mtx);
            auto it = std::find(players.begin(), players.end(), player);
            if (it != players.end()) { players.erase(it); registered = true; }
        }
        if (registered) {
            broadcast(Writer().varint(play_cb::remove_entities).varint(1).varint(player->eid));
            broadcast(Writer().varint(play_cb::player_info_remove).varint(1).i64(player->uuid_hi).i64(player->uuid_lo));
            broadcast_message(player->name + " left the game", "yellow");
        }
    }
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int) { shutdown_requested = 1; });
    signal(SIGTERM, [](int) { shutdown_requested = 1; });
    load_properties(argc > 1 ? argv[1] : "server.properties");
    world_dir = props["level-name"];
    server_dir = std::filesystem::path(argc > 1 ? argv[1] : "server.properties").parent_path().string();
    if (server_dir.empty()) server_dir = ".";
    setup_world();
    std::ifstream cfg("data/config.bin", std::ios::binary);
    if (!cfg) {
        fprintf(stderr, "missing data/config.bin, run: python3 tools/gen_data.py\n");
        return 1;
    }
    config_packets.assign(std::istreambuf_iterator<char>(cfg), {});
    build_commands_packet();
    register_core_handlers();
    register_command_handlers();
    plugins::start();  // Bukkit plugins (no-op without plugins/*.jar); wraps handlers, hooks the tick
    on_tick(advance_time);  // world clocks, weather, border (commands/system.hpp)
    start_game();  // tick thread (game/tick.hpp)
    int port = std::stoi(props["server-port"]);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (sockaddr*)&addr, sizeof addr) < 0 || listen(srv, 64) < 0) {
        perror("bind/listen");
        return 1;
    }
    printf("CraftPP (%s, protocol %d) listening on port %d - MOTD: %s\n", GAME_VERSION, PROTOCOL_VERSION, port,
           props["motd"].c_str());
    fflush(stdout);

    std::thread([] {  // console commands
        for (std::string line; std::getline(std::cin, line);) {
            if (line == "stop") { shutdown_requested = 1; return; }
            if (!line.empty()) run_on_tick([line] {
                std::string l = line[0] == '/' ? line.substr(1) : line;
                if (!plugins::console_command(l)) run_command(nullptr, l);
            });
        }
    }).detach();

    std::thread([] {  // autosave every 5 s; on stop/Ctrl+C save everything and exit
        for (int tick = 0;; tick++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (shutdown_requested == 1) {
                shutdown_requested = 2;
                plugins::stop();  // onDisable before the world is saved
                request_stop();  // saves on the tick thread, then exits
            }
            if (tick % 25 == 24 && !autosave_off) {
                world.save_dirty();
                run_on_tick(save_level);
            }
        }
    }).detach();

    for (;;) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        // ponytail: thread per connection; move to an event loop once player counts matter.
        std::thread([fd] { handle(fd); close(fd); }).detach();
    }
}
