#pragma once
// Survival gameplay entry point: included from play.hpp after the handler registry.
// Threading model: game/tick.hpp. APIs for other modules: give_item/clear (inventory.hpp), spawn_item,
// hurt/heal (survival.hpp), run_on_tick/on_tick/on_player_* (tick.hpp).
#include "survival.hpp"
#include "../level/glue.hpp"
#include "portal.hpp"
#include "../containers/containers.hpp"
#include "../mobs/roster.hpp"
#include "netplay.hpp"
#include "../plugins/plugins.hpp"
#include "../console.hpp"
#include "../boss/boss.hpp"
#include "../environment/environment.hpp"

#include <unistd.h>

inline void dispatch_packet(Player& p, const std::vector<uint8_t>& pkt) {
    Reader in(pkt);
    int id = in.varint();
    if (auto h = handlers().find(id); h != handlers().end()) h->second(p, in, id);
}

// Tick thread: make a joined player visible (tab list + entities both ways).
inline void register_player(const PlayerPtr& pp) {
    Player& p = *pp;
    for (auto& q : players)  // handleConfigurationFinished: the same profile joined meanwhile
        if ((q->uuid_hi == p.uuid_hi && q->uuid_lo == p.uuid_lo) || q->name == p.name)
            return disconnect(p, tr("multiplayer.disconnect.duplicate_login"));
    std::vector<PlayerPtr> all = players;
    all.push_back(pp);
    p.send(info_add(all));  // entities (players too) are spawned by the tracker (src/mobs/manager.hpp)
    {
        std::lock_guard lock(players_mtx);
        players.push_back(pp);
    }
    broadcast(info_add({pp}), &p);
    plugins::announce_join(p);  // PlayerJoinEvent may rewrite/suppress it
}

// Tick thread: the connection closed.
inline void unregister_player(const PlayerPtr& pp) {
    save_player(*pp);
    {
        std::lock_guard lock(players_mtx);
        std::erase(players, pp);
    }
    broadcast(Writer().varint(play_cb::remove_entities).varint(1).varint(pp->eid));
    broadcast(Writer().varint(play_cb::player_info_remove).varint(1).i64(pp->uuid_hi).i64(pp->uuid_lo));
    log_info("%s lost connection: %s", pp->name.c_str(), pp->quit_reason.empty() ? "Disconnected" : pp->quit_reason.c_str());
    plugins::announce_quit(*pp);  // PlayerQuitEvent may rewrite/suppress it
}

// Called once from main() after register_core_handlers().
inline void start_game() {
    game_init();
    roster_init();
    lvl::start();  // block updates, scheduled/random ticks, fluids (src/level/)
    register_game_handlers();
    containers_init();
    lvl::container_signal = [](int, const lvl::Pos& p) { return be::comparator_signal(p.x, p.y, p.z); };  // comparators read containers
    dimensions_init();  // after the game handlers: wraps client_command
    portal::init();     // wraps use_item_on
    register_net_handlers();  // keep-alive, teleport confirmation, movement checks (netplay.hpp)
    boss::init();  // after the game handlers: it wraps some of them
    env::init();
    progress::init();  // advancements, recipe book, statistics (src/progress/)
    plugins::start();  // Bukkit plugins (no-op without plugins/*.jar): wraps the handlers above, hooks the tick
    start_tick_thread();
}

// Any thread: save everything on the tick thread, then exit (MinecraftServer.stopServer + Paper's wording).
// The caller has already logged "Stopping server" and disabled plugins.
inline void request_stop() {
    run_on_tick([] {
        log_info("Saving players");
        Text reason = tr("multiplayer.disconnect.server_shutdown");
        for (auto& p : snapshot()) {  // PlayerList.removeAll
            save_player(*p);
            Writer w = pkt(play_cb::disconnect);
            write_nbt(w, reason);
            try { p->send(w); p->out.flush(); } catch (...) {}
            log_info("%s lost connection: %s", p->name.c_str(), plain(reason).c_str());
            log_info("%s%s left the game", "\xC2\xA7" "e", p->name.c_str());
        }
        log_info("Saving worlds");
        std::string level = props["level-name"];
        log_info("Saving chunks for level 'ServerLevel[%s]'/minecraft:overworld", level.c_str());
        save_all();
        mobs_save_all();  // entity chunks (src/mobs/manager.hpp)
        storage::close_all();  // queued region writes (entities saved after the chunks)
        save_level();  // world clocks, game rules, scoreboard (commands/state.hpp)
        log_info("ThreadedAnvilChunkStorage (%s): All chunks are saved", level.c_str());
        log_info("ThreadedAnvilChunkStorage: All dimensions are saved");
        console::restore();
        logging::close();
        fflush(stdout);
        _exit(0);
    });
}
