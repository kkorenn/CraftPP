#pragma once
// Brain AI (net.minecraft.world.entity.ai.Brain + memory, sensing, behavior): memories with expiry, sensors that
// fill them on an interval, activities holding prioritised behaviors with memory entry conditions. Used by
// villager-style mobs (villagers, piglins, axolotls, frogs, goats, wardens...); the first mobs use goals only.
// Usage: give a Mob a `Brain brain;`, add sensors/behaviors in init(), call brain.tick(*this) from
// custom_server_ai_step(). Memory ids are interned names (memory_id("attack_target")).
#include "goals.hpp"

#include <variant>

using MemoryValue = std::variant<std::monostate, int, int64_t, double, bool, Vec3, std::array<int, 3>, std::vector<int>>;
inline int memory_id(const std::string& name) {  // MemoryModuleType registry (interned)
    static std::map<std::string, int> ids;
    auto [it, fresh] = ids.try_emplace(name, int(ids.size()));
    return it->second;
}
enum class MemoryStatus { PRESENT, ABSENT, REGISTERED };

struct Brain;
struct Sensor {  // Sensor: runs every `scan_rate` ticks (randomised start)
    int scan_rate = 20, timer = 0;
    virtual ~Sensor() = default;
    virtual void sense(Mob& m, Brain& b) = 0;
    void tick(Mob& m, Brain& b) {
        if (--timer <= 0) { timer = scan_rate; sense(m, b); }
    }
};
struct Behavior {  // Behavior / BehaviorControl
    enum Status { STOPPED, RUNNING } status = STOPPED;
    std::vector<std::pair<int, MemoryStatus>> entry;  // memory requirements
    int min_duration = 60, max_duration = 60, end_tick = 0;
    virtual ~Behavior() = default;
    virtual bool check_extra_start_conditions(Mob&) { return true; }
    virtual bool can_still_use(Mob&) { return false; }
    virtual void start(Mob&) {}
    virtual void tick(Mob&) {}
    virtual void stop(Mob&) {}
    bool has_required_memories(Brain& b) const;
    bool try_start(Mob& m, Brain& b) {
        if (!has_required_memories(b) || !check_extra_start_conditions(m)) return false;
        status = RUNNING;
        end_tick = int(server_tick) + min_duration + rand_int(max_duration + 1 - min_duration);
        start(m);
        return true;
    }
    void tick_or_stop(Mob& m) {
        if (!(server_tick > end_tick) && can_still_use(m)) tick(m);
        else do_stop(m);
    }
    void do_stop(Mob& m) { status = STOPPED; stop(m); }
};
struct Brain {
    struct Memory { MemoryValue v; int64_t expires = INT64_MAX; };
    std::map<int, Memory> memories;
    std::vector<std::unique_ptr<Sensor>> sensors;
    std::map<std::string, std::vector<std::pair<int, std::unique_ptr<Behavior>>>> activities;  // activity -> (priority, behavior)
    std::vector<std::string> active = {"core", "idle"};
    std::set<std::string> core = {"core"};

    bool has(int id) const { auto it = memories.find(id); return it != memories.end() && it->second.v.index() != 0; }
    template <class T> std::optional<T> get(int id) const {
        auto it = memories.find(id);
        if (it == memories.end()) return std::nullopt;
        if (auto* p = std::get_if<T>(&it->second.v)) return *p;
        return std::nullopt;
    }
    void set(int id, MemoryValue v, int ttl = -1) { memories[id] = {std::move(v), ttl < 0 ? INT64_MAX : server_tick + ttl}; }
    void erase(int id) { memories.erase(id); }
    MemoryStatus status(int id) const { return has(id) ? MemoryStatus::PRESENT : MemoryStatus::ABSENT; }
    void add(const std::string& activity, int prio, std::unique_ptr<Behavior> b) { activities[activity].emplace_back(prio, std::move(b)); }
    void set_active(std::vector<std::string> a) {  // setActiveActivityIfPossible (core always stays)
        active = std::move(a);
        if (std::find(active.begin(), active.end(), "core") == active.end()) active.push_back("core");
    }
    void tick(Mob& m) {  // Brain.tick: forget expired, sensors, start, tick running
        for (auto it = memories.begin(); it != memories.end();)
            it = it->second.expires <= server_tick ? memories.erase(it) : std::next(it);
        for (auto& s : sensors) s->tick(m, *this);
        for (auto& act : active) {  // startEachNonRunningBehavior (priority order)
            auto& list = activities[act];
            std::stable_sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [p, b] : list)
                if (b->status == Behavior::STOPPED) b->try_start(m, *this);
        }
        for (auto& [name, list] : activities)  // tickEachRunningBehavior; behaviors of inactive activities stop
            for (auto& [p, b] : list) {
                if (b->status != Behavior::RUNNING) continue;
                if (std::find(active.begin(), active.end(), name) == active.end()) b->do_stop(m);
                else b->tick_or_stop(m);
            }
    }
};
inline bool Behavior::has_required_memories(Brain& b) const {
    for (auto& [id, st] : entry) {
        if (st == MemoryStatus::PRESENT && !b.has(id)) return false;
        if (st == MemoryStatus::ABSENT && b.has(id)) return false;
    }
    return true;
}

namespace mem {  // common MemoryModuleTypes
inline int walk_target() { static int i = memory_id("walk_target"); return i; }          // Vec3 (speed in a separate memory)
inline int walk_speed() { static int i = memory_id("walk_target_speed"); return i; }     // double
inline int look_target() { static int i = memory_id("look_target"); return i; }          // int entity id or Vec3
inline int attack_target() { static int i = memory_id("attack_target"); return i; }      // int entity id
inline int nearest_player() { static int i = memory_id("nearest_visible_player"); return i; }
inline int hurt_by_entity() { static int i = memory_id("hurt_by_entity"); return i; }
inline int path() { static int i = memory_id("path"); return i; }
inline int attack_cooling_down() { static int i = memory_id("attack_cooling_down"); return i; }
}

// ---------- sensors ----------
struct NearestPlayerSensor : Sensor {  // PlayerSensor: nearest visible player within 16 blocks
    void sense(Mob& m, Brain& b) override {
        auto tc = TargetingConditions::for_non_combat();
        tc.set_range(16.0);
        LivingEntity* p = nearest_player(tc, &m, m.pos.x, m.pos.y, m.pos.z);
        if (p) b.set(mem::nearest_player(), p->id);
        else b.erase(mem::nearest_player());
    }
};
struct HurtBySensor : Sensor {
    HurtBySensor() { scan_rate = 1; }
    void sense(Mob& m, Brain& b) override {
        if (LivingEntity* h = m.last_hurt_by()) b.set(mem::hurt_by_entity(), h->id);
        else b.erase(mem::hurt_by_entity());
    }
};

// ---------- behaviors ----------
struct LookAtTargetSink : Behavior {  // LookAtTargetSink
    LookAtTargetSink() { entry = {{mem::look_target(), MemoryStatus::PRESENT}}; min_duration = 45, max_duration = 90; }
    Brain* brain = nullptr;
    bool can_still_use(Mob& m) override { return brain && brain->has(mem::look_target()) && m.is_alive(); }
    void start(Mob& m) override { (void)m; }
    void tick(Mob& m) override {
        if (!brain) return;
        if (auto id = brain->get<int>(mem::look_target())) {
            if (Entity* e = entity_by_id(*id)) m.look_control->set_look_at(*e);
        } else if (auto v = brain->get<Vec3>(mem::look_target())) {
            m.look_control->set_look_at(v->x, v->y, v->z);
        }
    }
    void stop(Mob& m) override { if (brain) brain->erase(mem::look_target()); (void)m; }
};
struct MoveToTargetSink : Behavior {  // MoveToTargetSink: follows WALK_TARGET with the navigation
    Brain* brain = nullptr;
    MoveToTargetSink() { entry = {{mem::walk_target(), MemoryStatus::PRESENT}}; min_duration = 150, max_duration = 250; }
    bool reached(Mob& m) const {
        auto t = brain->get<Vec3>(mem::walk_target());
        return !t || t->dist_sqr(m.pos) < 1.5;
    }
    bool check_extra_start_conditions(Mob& m) override { return brain && !reached(m); }
    void start(Mob& m) override {
        auto t = brain->get<Vec3>(mem::walk_target());
        m.navigation->move_to(t->x, t->y, t->z, brain->get<double>(mem::walk_speed()).value_or(1.0));
    }
    bool can_still_use(Mob& m) override { return brain->has(mem::walk_target()) && !reached(m) && !m.navigation->is_done(); }
    void stop(Mob& m) override { m.navigation->stop(); if (reached(m)) brain->erase(mem::walk_target()); }
};
struct RandomStrollBehavior : Behavior {  // RandomStroll.stroll: picks a LandRandomPos walk target
    Brain* brain = nullptr;
    float speed;
    explicit RandomStrollBehavior(float s) : speed(s) { entry = {{mem::walk_target(), MemoryStatus::ABSENT}}; min_duration = max_duration = 1; }
    bool check_extra_start_conditions(Mob& m) override { return brain && dynamic_cast<PathfinderMob*>(&m); }
    void start(Mob& m) override {
        if (auto p = randpos::land_pos(static_cast<PathfinderMob&>(m), 10, 7)) {
            brain->set(mem::walk_target(), *p);
            brain->set(mem::walk_speed(), double(speed));
        }
    }
};
struct SetEntityLookTarget : Behavior {  // SetEntityLookTarget (nearest visible player)
    Brain* brain = nullptr;
    SetEntityLookTarget() { entry = {{mem::look_target(), MemoryStatus::ABSENT}, {mem::nearest_player(), MemoryStatus::PRESENT}}; min_duration = max_duration = 1; }
    void start(Mob&) override { if (brain) brain->set(mem::look_target(), *brain->get<int>(mem::nearest_player())); }
};
// Wires a behavior to its brain (behaviors read memories through it).
template <class B> inline std::unique_ptr<B> brain_behavior(Brain& b, std::unique_ptr<B> x) { x->brain = &b; return x; }
