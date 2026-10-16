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

#include <pthread.h>
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
    log_info("Tick stats: %lld ticks, mspt avg %.3f max %.3f, peak RSS %ld KiB", (long long)server_tick, mspt_avg,
             mspt_max, rss_kib());
}

// Paper's /tps and /mspt (tick thread only): seconds per 20-tick sample over the last 15 minutes (time-weighted
// like Paper's RollingAverage, starting at 20 TPS) and the duration of each of the last 1200 ticks.
inline std::array<double, 900> tps_section_s = [] { std::array<double, 900> a; a.fill(1.0); return a; }();
inline std::array<float, 1200> tick_ms{};
inline int64_t tps_sections = 0;
inline std::chrono::steady_clock::time_point tps_section_start = std::chrono::steady_clock::now();
inline void record_tick(double ms) {
    tick_ms[size_t((server_tick + 1199) % 1200)] = float(ms);  // server_tick already counts this tick
    if (server_tick % 20) return;
    auto now = std::chrono::steady_clock::now();
    tps_section_s[size_t(tps_sections++ % 900)] = std::chrono::duration<double>(now - tps_section_start).count();
    tps_section_start = now;
}
inline std::array<double, 3> tps_averages() {  // last 1m, 5m, 15m
    std::array<double, 3> out{};
    int n[3] = {60, 300, 900};
    for (int k = 0; k < 3; k++) {
        double sum = 0;
        for (int i = 1; i <= n[k]; i++) sum += tps_section_s[size_t((tps_sections - i + 900 * 4) % 900)];
        out[size_t(k)] = 20.0 * n[k] / sum;
    }
    return out;
}
struct TickTimes { double avg, min, max; };
inline TickTimes tick_times(int n) {  // last n ticks (Paper: 100 = 5s, 200 = 10s, 1200 = 1m)
    n = int(std::min<int64_t>(n, server_tick));
    if (n <= 0) return {0, 0, 0};
    TickTimes t{0, 1e9, 0};
    for (int i = 1; i <= n; i++) {
        double v = tick_ms[size_t((server_tick - i) % 1200)];
        t.avg += v, t.min = std::min(t.min, v), t.max = std::max(t.max, v);
    }
    t.avg /= n;
    return t;
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
    for (auto& f : run) {
        try {
            f();
        } catch (const std::exception& e) {  // one failing task (e.g. a save on a full disk) must not stop the server
            log_error("Task failed: %s", e.what());
        }
    }
    run.clear();
    for (auto& pp : players) {  // tick thread is the only writer of `players`, so no lock or copy needed
        Player& p = *pp;
        {
            std::lock_guard lock(p.in.m);
            pkts.swap(p.in.q);
            p.in.bytes = 0;
        }
        for (auto& pkt : pkts) {
            try {
                dispatch_packet(p, pkt);
            } catch (const std::exception& e) {
                log_warn("%s: bad packet (%s), disconnecting", p.name.c_str(), e.what());
                p.quit_reason = std::string("Internal Exception: ") + e.what();
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
        if (pp->out.pending() > (32u << 20)) { pp->quit_reason = "Too far behind (32 MiB unsent)"; kick(*pp); }
        pp->out.flush();
    }
    server_tick++;
}

// Detached thread with an explicit stack size (std::thread gets 512 KiB on macOS, 8 MiB on Linux).
inline void start_thread(size_t stack, std::function<void()> fn) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    auto* arg = new std::function<void()>(std::move(fn));
    pthread_t t;
    int err = pthread_create(&t, &attr, [](void* a) -> void* {
        std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(a));
        (*f)();
        return nullptr;
    }, arg);
    pthread_attr_destroy(&attr);
    if (err) { delete arg; throw std::system_error(err, std::generic_category(), "pthread_create"); }
}

inline void start_tick_thread() {
    // Commands and packet handlers run here; 8 MiB like a Linux main thread, so recursive parsers
    // (SNBT up to Tag.MAX_DEPTH) have room on every platform.
    start_thread(8u << 20, [] {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::milliseconds(50);
        auto next = clock::now();
        auto pause_after = std::chrono::seconds(std::stoi(props.count("pause-when-empty-seconds")
                                                              ? props["pause-when-empty-seconds"] : "60"));
        auto last_busy = clock::now();
        for (;;) {
            if (!players.empty()) last_busy = clock::now();
            static bool pause_logged = false;
            if (!players.empty()) pause_logged = false;
            if (players.empty() && clock::now() - last_busy >= pause_after) {
                if (!pause_logged) {
                    pause_logged = true;
                    log_info("Server empty for %lld seconds, pausing", (long long)pause_after.count());
                }
                std::unique_lock lock(tick_detail::m);
                tick_detail::cv.wait(lock, [] { return !tick_detail::tasks.empty(); });
                next = tps_section_start = clock::now();  // paused time is not lag
            }
            auto t0 = clock::now();
            tick_once();
            double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
            mspt_avg = mspt_avg == 0 ? ms : mspt_avg * 0.99 + ms * 0.01;
            if (ms > mspt_max) mspt_max = ms;
            record_tick(ms);
            next += period;
            auto now = clock::now();
            if (now > next + std::chrono::seconds(2)) next = now;  // way behind: skip ticks instead of bursting
            std::this_thread::sleep_until(next);
        }
    });
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
        // Backpressure: a client sending faster than the tick thread handles waits (TCP pushes back on it)
        // instead of growing the queue without bound; 30 s without progress drops the connection.
        int waited = 0;
        while (pp->in.pending() > (1u << 20) && waited++ < 600) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (waited > 600) break;
        std::lock_guard lock(pp->in.m);
        pp->in.bytes += pkt.size();
        pp->in.q.push_back(std::move(pkt));
        pkt = {};
    }
    std::promise<void> done;
    run_on_tick([&] {
        try {
            if (std::find(players.begin(), players.end(), pp) != players.end()) {
                for (auto f : tick_detail::leave_hooks) f(*pp);
                on_unregister(pp);
            }
        } catch (const std::exception& e) {
            log_error("%s: leave failed: %s", pp->name.c_str(), e.what());
            std::lock_guard lock(players_mtx);
            std::erase(players, pp);
        }
        done.set_value();  // always: the network thread waits for it
    });
    done.get_future().wait();
    pp->out.flush();  // best effort: last packets (e.g. a disconnect message)
}
