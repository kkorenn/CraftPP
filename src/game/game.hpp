#pragma once
// Survival gameplay entry point: included from play.hpp after the handler registry.
// Threading model: game/tick.hpp. APIs for other modules: give_item/clear (inventory.hpp), spawn_item,
// hurt/heal (survival.hpp), run_on_tick/on_tick/on_player_* (tick.hpp).
#include "survival.hpp"
#include "../mobs/roster.hpp"
#include "../plugins/plugins.hpp"

#include <unistd.h>

inline void dispatch_packet(Player& p, const std::vector<uint8_t>& pkt) {
    Reader in(pkt);
    int id = in.varint();
    if (auto h = handlers().find(id); h != handlers().end()) h->second(p, in, id);
}

// Tick thread: make a joined player visible (tab list + entities both ways).
inline void register_player(const PlayerPtr& pp) {
    Player& p = *pp;
    std::vector<PlayerPtr> all = players;
    all.push_back(pp);
    p.send(info_add(all));
    for (auto& q : players) p.send(spawn_entity(*q));
    {
        std::lock_guard lock(players_mtx);
        players.push_back(pp);
    }
    broadcast(info_add({pp}), &p);
    broadcast(spawn_entity(p), &p);
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
    plugins::announce_quit(*pp);  // PlayerQuitEvent may rewrite/suppress it
}

inline void keepalive_tick(Player& p) {
    if (++p.g.keepalive_timer < 200) return;  // every 10 s
    p.g.keepalive_timer = 0;
    p.send(Writer().varint(play_cb::keep_alive).i64(server_tick));
}

// Called once from main() after register_core_handlers().
inline void start_game() {
    game_init();
    roster_init();
    register_game_handlers();
    on_player_tick(keepalive_tick);
    start_tick_thread();
}

// Any thread: save everything on the tick thread, then exit.
inline void request_stop() {
    run_on_tick([] {
        printf("Stopping server, saving...\n");
        save_all();
        save_level();  // world clocks, game rules, scoreboard (commands/state.hpp)
        print_tick_stats();
        fflush(stdout);
        _exit(0);
    });
}
