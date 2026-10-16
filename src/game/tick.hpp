#pragma once
// ============================================================================================================
// Threading model (mirrors vanilla's "everything on the server thread"):
//
//  * Tick thread (start_tick_thread): the single owner of game state. Runs at 20 TPS: run queued tasks ->
//    drain every player's inbox and run its packet handlers -> tick hooks (entities, players) -> per-player
//    sync (inventory/health) -> flush every player's outbox. Packet handlers registered with on() therefore
//    always run on the tick thread, as do run_on_tick() tasks, console commands and saving.
//    Pauses (sleeps on a condition variable, zero CPU) once nobody has been online for
//    pause-when-empty-seconds (vanilla default 60); a run_on_tick() call or a join wakes it.
//  * Network threads (one per connection, main.cpp handle()): login + configuration, then join() (sends the
//    login packet and the initial chunks before the player is visible to the tick thread), then only read
//    frames and push them into Player::in. They never touch game state after join().
//  * No writer threads: Player::send() only appends to Player::out; at the end of every tick the tick thread
//    flushes each outbox with non-blocking send()s (usually one syscall per player per tick). What the socket
//    doesn't take stays queued for the next tick, so a slow client can't stall the tick; one that falls
//    32 MiB behind is disconnected.
//  * Shared structures with their own locks: World/Chunk (chunks are streamed from join() on network threads),
//    `players` (written only on the tick thread, under players_mtx; network threads read it for login checks).
//
// Rule for other modules: touch Player / entity / world-edit state only from the tick thread. From any other
// thread, use run_on_tick([]{ ... }).
// ============================================================================================================
#include "../server.hpp"

#include <sys/resource.h>
#include <sys/socket.h>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>

inline int64_t server_tick = 0;  // ticks run so far (tick thread)

namespace tick_detail {
inline std::mutex m;
inline std::condition_variable cv;
inline std::vector<std::function<void()>> tasks;
inline std::vector<void (*)()> tick_hooks;
inline std::vector<void (*)(Player&)> player_tick_hooks, player_sync_hooks, join_hooks, leave_hooks;
}  // namespace tick_detail

// Runs `f` on the tick thread at the start of the next tick (wakes a paused server). Callable from any thread.
inline void run_on_tick(std::function<void()> f) {
    {
        std::lock_guard lock(tick_detail::m);
        tick_detail::tasks.push_back(std::move(f));
    }
    tick_detail::cv.notify_one();
}

// Hook registration: call during startup (before the accept loop), from the main thread.
inline void on_tick(void (*f)()) { tick_detail::tick_hooks.push_back(f); }                     // once per tick
inline void on_player_tick(void (*f)(Player&)) { tick_detail::player_tick_hooks.push_back(f); } // per player, per tick
inline void on_player_sync(void (*f)(Player&)) { tick_detail::player_sync_hooks.push_back(f); } // end of tick, before flush
inline void on_player_join(void (*f)(Player&)) { tick_detail::join_hooks.push_back(f); }       // after the player is visible
inline void on_player_leave(void (*f)(Player&)) { tick_detail::leave_hooks.push_back(f); }     // before removal

// Disconnects a player: its network thread sees EOF and the normal leave path runs.
inline void kick(Player& p) { shutdown(p.fd, SHUT_RDWR); }

// ---------- tick stats ----------
inline double mspt_avg = 0, mspt_max = 0;  // exponential average / max over the run (ms per tick)
inline long rss_kib() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return u.ru_maxrss / 1024;  // bytes on macOS
#else
    return u.ru_maxrss;  // KiB on Linux
#endif
}
inline void print_tick_stats() {
    printf("Tick stats: %lld ticks, mspt avg %.3f max %.3f, peak RSS %ld KiB\n", (long long)server_tick, mspt_avg,
           mspt_max, rss_kib());
    fflush(stdout);
}

// ---------- the loop ----------
inline void dispatch_packet(Player& p, const std::vector<uint8_t>& pkt);  // play.hpp handler registry

inline void tick_once() {
    using namespace tick_detail;
    static std::vector<std::function<void()>> run;
    static std::vector<std::vector<uint8_t>> pkts;
    {
        std::lock_guard lock(m);
        run.swap(tasks);
    }
    for (auto& f : run) f();
    run.clear();
    for (auto& pp : players) {  // tick thread is the only writer of `players`, so no lock or copy needed
        Player& p = *pp;
        {
            std::lock_guard lock(p.in.m);
            pkts.swap(p.in.q);
        }
        for (auto& pkt : pkts) {
            try {
                dispatch_packet(p, pkt);
            } catch (const std::exception& e) {
                fprintf(stderr, "%s: bad packet (%s), disconnecting\n", p.name.c_str(), e.what());
                kick(p);
                break;
            }
        }
        pkts.clear();
    }
    for (auto f : tick_hooks) f();
    for (auto& pp : players)
        for (auto f : player_tick_hooks) f(*pp);
    for (auto& pp : players) {
        for (auto f : player_sync_hooks) f(*pp);
        if (pp->out.pending() > (32u << 20)) kick(*pp);
        pp->out.flush();
    }
    server_tick++;
}

inline void start_tick_thread() {
    std::thread([] {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::milliseconds(50);
        auto next = clock::now();
        auto pause_after = std::chrono::seconds(std::stoi(props.count("pause-when-empty-seconds")
                                                              ? props["pause-when-empty-seconds"] : "60"));
        auto last_busy = clock::now();
        for (;;) {
            if (!players.empty()) last_busy = clock::now();
            if (players.empty() && clock::now() - last_busy >= pause_after) {
                std::unique_lock lock(tick_detail::m);
                tick_detail::cv.wait(lock, [] { return !tick_detail::tasks.empty(); });
                next = clock::now();
            }
            auto t0 = clock::now();
            tick_once();
            double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
            mspt_avg = mspt_avg == 0 ? ms : mspt_avg * 0.99 + ms * 0.01;
            if (ms > mspt_max) mspt_max = ms;
            next += period;
            auto now = clock::now();
            if (now > next + std::chrono::seconds(2)) next = now;  // way behind: skip ticks instead of bursting
            std::this_thread::sleep_until(next);
        }
    }).detach();
}

// Network-thread side of a play connection: everything after join().
inline void run_play_connection(const PlayerPtr& pp, void (*on_register)(const PlayerPtr&),
                                void (*on_unregister)(const PlayerPtr&)) {
    pp->out.fd = pp->fd;
    pp->out.flush();  // start on the login + chunks queued by join(); the tick thread sends the rest
    run_on_tick([pp, on_register] {
        on_register(pp);
        for (auto f : tick_detail::join_hooks) f(*pp);
    });
    std::vector<uint8_t> pkt;
    while (read_packet(pp->fd, pkt)) {
        std::lock_guard lock(pp->in.m);
        pp->in.q.push_back(std::move(pkt));
        pkt = {};
    }
    std::promise<void> done;
    run_on_tick([&] {
        if (std::find(players.begin(), players.end(), pp) != players.end()) {
            for (auto f : tick_detail::leave_hooks) f(*pp);
            on_unregister(pp);
        }
        done.set_value();
    });
    done.get_future().wait();
    pp->out.flush();  // best effort: last packets (e.g. a disconnect message)
}
