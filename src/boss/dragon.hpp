#pragma once
// EnderDragon: flight model, multipart hitboxes, node-graph pathfinding, EnderDragonPhaseManager and every
// DragonPhaseInstance (holding pattern, strafe, landing approach, landing, sitting x3, takeoff, charge, dying,
// hover), crystal healing, block breaking, death sequence.
#include "entities.hpp"

namespace boss {

struct DragonFight;
struct EnderDragon;
inline DragonFight* fight = nullptr;           // the End's EnderDragonFight (fight.hpp)
inline int fight_alive_crystals();              // fight.hpp
inline bool fight_previously_killed();          // fight.hpp
inline void fight_update_dragon(EnderDragon& d);  // fight.hpp
inline void fight_dragon_killed(EnderDragon& d);  // fight.hpp
inline bool fight_owns(const EnderDragon& d);     // fight.hpp
inline void fight_origin_into(int* out);         // fight.hpp

// ---------- pathfinder (Node / BinaryHeap / Path) ----------
struct Node {
    int x = 0, y = 0, z = 0;
    float f = 0, g = 0, h = 0;
    Node* came_from = nullptr;
    int heap_idx = -1;
    bool closed = false;
    float dist(const Node& o) const {
        float xd = float(o.x - x), yd = float(o.y - y), zd = float(o.z - z);
        return emth::sqrt(xd * xd + yd * yd + zd * zd);
    }
    float dist_sqr(const Node& o) const {
        float xd = float(o.x - x), yd = float(o.y - y), zd = float(o.z - z);
        return xd * xd + yd * yd + zd * zd;
    }
    bool same(const Node& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct BinaryHeap {
    std::vector<Node*> heap;
    int size = 0;
    void clear() { size = 0; }
    bool empty() const { return size == 0; }
    void insert(Node* n) {
        if (int(heap.size()) == size) heap.push_back(nullptr);
        heap[size_t(size)] = n, n->heap_idx = size;
        up(size++);
    }
    Node* pop() {
        Node* p = heap[0];
        heap[0] = heap[size_t(--size)];
        heap[size_t(size)] = nullptr;
        if (size > 0) down(0);
        p->heap_idx = -1;
        return p;
    }
    void change_cost(Node* n, float c) {
        float old = n->f;
        n->f = c;
        if (c < old) up(n->heap_idx);
        else down(n->heap_idx);
    }
    void up(int i) {
        Node* n = heap[size_t(i)];
        float c = n->f;
        while (i > 0) {
            int pi = (i - 1) >> 1;
            Node* p = heap[size_t(pi)];
            if (!(c < p->f)) break;
            heap[size_t(i)] = p, p->heap_idx = i, i = pi;
        }
        heap[size_t(i)] = n, n->heap_idx = i;
    }
    void down(int i) {
        Node* n = heap[size_t(i)];
        float c = n->f;
        for (;;) {
            int l = 1 + (i << 1), r = l + 1;
            if (l >= size) break;
            Node* ln = heap[size_t(l)];
            float lc = ln->f;
            Node* rn = r >= size ? nullptr : heap[size_t(r)];
            float rc = r >= size ? INFINITY : rn->f;
            if (lc < rc) {
                if (!(lc < c)) break;
                heap[size_t(i)] = ln, ln->heap_idx = i, i = l;
            } else {
                if (!(rc < c)) break;
                heap[size_t(i)] = rn, rn->heap_idx = i, i = r;
            }
        }
        heap[size_t(i)] = n, n->heap_idx = i;
    }
};
struct Path {
    std::vector<std::array<int, 3>> nodes;
    size_t next = 0;
    bool done() const { return next >= nodes.size(); }
    void advance() { next++; }
    const std::array<int, 3>& next_pos() const { return nodes[next]; }
};

struct FlightHistory {  // DragonFlightHistory
    struct Sample { double y; float yrot; };
    std::array<Sample, 64> s{};
    int head = -1;
    void record(double y, float yrot) {
        if (head < 0) s.fill({y, yrot});
        if (++head == 64) head = 0;
        s[size_t(head)] = {y, yrot};
    }
    const Sample& get(int delay) const { return s[size_t((head - delay) & 63)]; }
};

enum Phase { HOLDING_PATTERN, STRAFE_PLAYER, LANDING_APPROACH, LANDING, TAKEOFF, SITTING_FLAMING, SITTING_SCANNING,
             SITTING_ATTACKING, CHARGING_PLAYER, DYING, HOVERING, PHASE_COUNT };
inline const char* PHASE_NAMES[] = {"HoldingPattern", "StrafePlayer", "LandingApproach", "Landing", "Takeoff", "SittingFlaming",
                                    "SittingScanning", "SittingAttacking", "ChargingPlayer", "Dying", "Hover"};

struct EnderDragon;
struct PhaseInstance {  // AbstractDragonPhaseInstance
    EnderDragon& d;
    explicit PhaseInstance(EnderDragon& d) : d(d) {}
    virtual ~PhaseInstance() = default;
    virtual Phase phase() const = 0;
    virtual bool sitting() const { return false; }
    virtual void server_tick() {}
    virtual void on_crystal_destroyed(Player*) {}
    virtual void begin() {}
    virtual void end() {}
    virtual float fly_speed() const { return 0.6f; }
    virtual std::optional<Vec3> target() const { return std::nullopt; }
    virtual float on_hurt(const DamageSource&, float dmg) { return dmg; }
    virtual float turn_speed() const;
};

struct EnderDragon : BossEntity {
    // part order: head, neck, body, tail1-3, wing1, wing2 (ids eid+1..eid+8, assigned by the client the same way)
    struct Part { float w, h; double x = 0, y = 0, z = 0; };
    std::array<Part, 8> parts{{{1, 1}, {3, 3}, {5, 3}, {2, 2}, {2, 2}, {2, 2}, {4, 2}, {4, 2}}};
    enum { HEAD, NECK, BODY, TAIL1, TAIL2, TAIL3, WING1, WING2 };
    FlightHistory history;
    float health = 200, max_health = 200, last_hurt = 0, sitting_damage = 0, yrot_a = 0, flap = 0, oflap = 0;
    int death_time = 0, hurt_time = 0, cooldown = 0, ambient_time = 0;
    bool in_wall = false, dead_flag = false, no_ai = false, linked = false;
    int nearest_crystal = 0;  // entity id
    int fight_origin[3] = {0, 0, 0};
    std::array<std::unique_ptr<PhaseInstance>, PHASE_COUNT> phases;
    PhaseInstance* current = nullptr;
    std::array<Node, 24> nodes;
    bool nodes_ready = false;
    std::array<int, 24> adjacency{};
    BinaryHeap open;
    std::map<int, int64_t> player_last_hurt;  // player eid -> tick hurt by this dragon (getLastHurtByMobTimestamp)

    EnderDragon();
    void set_health(float h) {
        h = std::clamp(h, 0.0f, max_health);
        if (h == health) return;
        health = h;
        data.set_float(edata::LivingEntity::HEALTH, health);
    }
    bool dying() const { return health <= 0 || dead_flag; }
    bool is_alive() const override { return !removed && !dying(); }
    bool fire_immune() const override { return true; }
    int sound_source() const override { return SRC_HOSTILE; }
    PhaseInstance& phase(Phase p);
    void set_phase(Phase p) {  // EnderDragonPhaseManager.setPhase
        if (current && current->phase() == p) return;
        if (current) current->end();
        current = &phase(p);
        data.set_int(edata::EnderDragon::PHASE, int(p));
        current->begin();
    }
    int egg_y(bool no_leaves) const { return heightmap(fight_origin[0], fight_origin[2], no_leaves); }
    Vec3 view_vector(float xrot) const {
        float rx = xrot * float(M_PI / 180.0), ry = -yaw * float(M_PI / 180.0);
        float yc = emth::cos(ry), ys = emth::sin(ry), xc = emth::cos(rx), xs = emth::sin(rx);
        return {double(ys * xc), double(-xs), double(yc * xc)};
    }
    Vec3 head_look_vector();
    AABB part_box(int i) const {
        auto& p = parts[size_t(i)];
        return {p.x - p.w / 2, p.y, p.z - p.w / 2, p.x + p.w / 2, p.y + p.h, p.z + p.w / 2};
    }

    int find_closest_node();
    int find_closest_node(double x, double y, double z);
    std::optional<Path> find_path(int start, int end, std::optional<Node> final_node);

    void tick() override;
    void ai_step();
    void tick_death();
    void tick_part(int i, double ox, double oy, double oz) { parts[size_t(i)].x = pos.x + ox, parts[size_t(i)].y = pos.y + oy, parts[size_t(i)].z = pos.z + oz; }
    bool check_walls(const AABB& bb);
    bool hurt_by(const DamageSource& d, Player* p, float dmg, int part = -1) override;
    bool really_hurt(const DamageSource& d, Player* p, float dmg);
    void kill() override {  // EnderDragon.kill: no death animation, no XP
        remove(Removal::KILLED);
        fight_update_dragon(*this);
        fight_dragon_killed(*this);
    }
    void on_crystal_destroyed(EndCrystal& c, Player* p);
    void save_extra(Nbt& o) const override {
        o.set("Health", Nbt::real(Nbt::Float, health));
        o.set("DragonPhase", Nbt::num(Nbt::Int, current ? int(current->phase()) : int(HOVERING)));
        o.set("DragonDeathTime", Nbt::num(Nbt::Int, death_time));
        o.set("sitting_damage_received", Nbt::real(Nbt::Float, sitting_damage));
    }
    void load_extra(const Nbt& in) override {
        set_health(float(nbt_num(in, "Health", 200)));
        if (in.get("DragonPhase")) set_phase(Phase(std::clamp(int(nbt_num(in, "DragonPhase", 0)), 0, int(PHASE_COUNT) - 1)));
        death_time = int(nbt_num(in, "DragonDeathTime", 0));
        sitting_damage = float(nbt_num(in, "sitting_damage_received", 0));
    }
};

inline float PhaseInstance::turn_speed() const {
    float rot = float(d.vel.horizontal()) + 1.0f, dist = std::min(rot, 40.0f);
    return 0.7f / dist / rot;
}

// Shared by holding pattern / strafe / landing approach: next path node, random height above it.
inline void navigate_next(EnderDragon& d, std::optional<Path>& path, std::optional<Vec3>& target) {
    if (path && !path->done()) {
        auto c = path->next_pos();
        path->advance();
        double ty;
        do ty = c[1] + rand_float() * 20.0f;
        while (ty < c[1]);
        target = Vec3{double(c[0]), ty, double(c[2])};
    }
    (void)d;
}
inline int ring_target(EnderDragon& d, int current, bool& clockwise, bool crystals_alive) {
    int t = current;
    if (rand_int(8) == 0) clockwise = !clockwise, t = current + 6;
    t += clockwise ? 1 : -1;
    if (crystals_alive) {
        t %= 12;
        if (t < 0) t += 12;
    } else {
        t -= 12;
        t &= 7;
        t += 12;
    }
    (void)d;
    return t;
}
inline bool needs_new_target(const EnderDragon& d, const std::optional<Vec3>& t) {
    double dist = t ? t->dist_sqr(d.pos) : 0.0;
    return dist < 100.0 || dist > 22500.0;  // horizontal/verticalCollision: always false (noPhysics)
}

struct StrafePhase;
struct HoldingPatternPhase : PhaseInstance {
    using PhaseInstance::PhaseInstance;
    std::optional<Path> path;
    std::optional<Vec3> tgt;
    bool clockwise = false;
    Phase phase() const override { return HOLDING_PATTERN; }
    void begin() override { path.reset(), tgt.reset(); }
    std::optional<Vec3> target() const override { return tgt; }
    void server_tick() override {
        if (needs_new_target(d, tgt)) find_new_target();
    }
    void strafe(Player& p);
    void find_new_target() {
        if (path && path->done()) {
            int ey = d.egg_y(true);
            int crystals = fight ? fight_alive_crystals() : 0;
            if (rand_int(crystals + 3) == 0) return d.set_phase(LANDING_APPROACH);
            Player* p = nearest_player(d.fight_origin[0], ey, d.fight_origin[2]);  // ignoreLineOfSight, no range
            double ds = p ? dsq(Vec3{d.fight_origin[0] + 0.5, ey + 0.5, d.fight_origin[2] + 0.5}, p->x, p->y, p->z) / 512.0 : 64.0;
            if (p && (rand_int(int(ds + 2.0)) == 0 || rand_int(crystals + 2) == 0)) return strafe(*p);
        }
        if (!path || path->done()) {
            int cur = d.find_closest_node();
            int t = ring_target(d, cur, clockwise, fight && fight_alive_crystals() >= 0);
            path = d.find_path(cur, t, std::nullopt);
            if (path) path->advance();
        }
        navigate_next(d, path, tgt);
    }
    void on_crystal_destroyed(Player* p) override {
        if (p && attackable(*p)) strafe(*p);
    }
};

struct StrafePhase : PhaseInstance {
    using PhaseInstance::PhaseInstance;
    int charge = 0;
    std::optional<Path> path;
    std::optional<Vec3> tgt;
    int attack_eid = -1;  // player entity id
    bool clockwise = false;
    Phase phase() const override { return STRAFE_PLAYER; }
    void begin() override { charge = 0, tgt.reset(), path.reset(), attack_eid = -1; }
    std::optional<Vec3> target() const override { return tgt; }
    void set_target(Player& p) {
        attack_eid = p.eid;
        int cur = d.find_closest_node(), t = d.find_closest_node(p.x, p.y, p.z);
        int fx = int(std::floor(p.x)), fz = int(std::floor(p.z));
        double xd = fx - d.pos.x, zd = fz - d.pos.z, sd = std::sqrt(xd * xd + zd * zd);
        double ho = std::min(0.4F + sd / 80.0 - 1.0, 10.0);
        Node fin;
        fin.x = fx, fin.y = Mth::floor(p.y + ho), fin.z = fz;
        path = d.find_path(cur, t, fin);
        if (path) {
            path->advance();
            navigate_next(d, path, tgt);
        }
    }
    void server_tick() override;
    void find_new_target() {
        if (!path || path->done()) {
            int cur = d.find_closest_node();
            int t = ring_target(d, cur, clockwise, fight && fight_alive_crystals() > 0);
            path = d.find_path(cur, t, std::nullopt);
            if (path) path->advance();
        }
        navigate_next(d, path, tgt);
    }
};
inline void HoldingPatternPhase::strafe(Player& p) {
    d.set_phase(STRAFE_PLAYER);
    static_cast<StrafePhase&>(d.phase(STRAFE_PLAYER)).set_target(p);
}

struct LandingApproachPhase : PhaseInstance {
    using PhaseInstance::PhaseInstance;
    std::optional<Path> path;
    std::optional<Vec3> tgt;
    Phase phase() const override { return LANDING_APPROACH; }
    void begin() override { path.reset(), tgt.reset(); }
    std::optional<Vec3> target() const override { return tgt; }
    void server_tick() override {
        if (!needs_new_target(d, tgt)) return;
        if (!path || path->done()) {
            int cur = d.find_closest_node();
            int ey = d.egg_y(true);
            Player* p = nearest_player(d.fight_origin[0], ey, d.fight_origin[2]);
            int t;
            if (p) {
                Vec3 aim = Vec3{p->x, 0.0, p->z}.normalize();
                t = d.find_closest_node(-aim.x * 40.0, 105.0, -aim.z * 40.0);
            } else t = d.find_closest_node(40.0, ey, 0.0);
            Node fin;
            fin.x = d.fight_origin[0], fin.y = ey, fin.z = d.fight_origin[2];
            path = d.find_path(cur, t, fin);
            if (path) path->advance();
        }
        navigate_next(d, path, tgt);
        if (path && path->done()) d.set_phase(LANDING);
    }
};

struct SittingFlamingPhase;
struct LandingPhase : PhaseInstance {
    using PhaseInstance::PhaseInstance;
    std::optional<Vec3> tgt;
    Phase phase() const override { return LANDING; }
    void begin() override { tgt.reset(); }
    float fly_speed() const override { return 1.5f; }
    float turn_speed() const override {
        float rot = float(d.vel.horizontal()) + 1.0f, dist = std::min(rot, 40.0f);
        return dist / rot;
    }
    std::optional<Vec3> target() const override { return tgt; }
    void server_tick() override;
};

struct SittingPhase : PhaseInstance {  // AbstractDragonSittingPhase
    using PhaseInstance::PhaseInstance;
    bool sitting() const override { return true; }
    // ponytail: arrows and wind charges (set on fire, 0 damage) are not entities here yet
};

struct SittingFlamingPhase : SittingPhase {
    using SittingPhase::SittingPhase;
    int ticks = 0, count = 0;
    int flame = 0;  // entity id
    Phase phase() const override { return SITTING_FLAMING; }
    void begin() override { ticks = 0, count++; }
    void end() override {
        if (::Entity* f = flame ? entity_by_id(flame) : nullptr) f->discard();
        flame = 0;
    }
    void server_tick() override {
        if (++ticks >= 200) return d.set_phase(count >= 4 ? TAKEOFF : SITTING_SCANNING);
        if (ticks != 10) return;
        auto& h = d.parts[EnderDragon::HEAD];
        Vec3 look = Vec3{h.x - d.pos.x, 0.0, h.z - d.pos.z}.normalize();
        double fx = h.x + look.x * 5.0 / 2.0, fz = h.z + look.z * 5.0 / 2.0;
        double iy = h.y + d.parts[EnderDragon::HEAD].h * 0.5, fy = iy;
        int bx = int(std::floor(fx)), bz = int(std::floor(fz)), by = int(std::floor(iy));
        while (is_air(get_block(bx, by, bz))) {
            if (--fy < 0.0) { fy = iy; break; }
            by = int(std::floor(fy));
        }
        fy = std::floor(fy) + 1;
        auto& c = spawn<AreaEffectCloud>(fx, fy, fz);
        c.owned_by_mob = true;
        c.set_radius(5.0f);
        c.duration = 200;
        c.amplifier = 0;
        flame = c.id;
    }
};

struct SittingScanningPhase : SittingPhase {
    using SittingPhase::SittingPhase;
    int time = 0;
    Phase phase() const override { return SITTING_SCANNING; }
    void begin() override { time = 0; }
    void server_tick() override;
};

struct SittingAttackingPhase : SittingPhase {
    using SittingPhase::SittingPhase;
    int ticks = 0;
    Phase phase() const override { return SITTING_ATTACKING; }
    void begin() override { ticks = 0; }
    void server_tick() override {
        if (ticks++ >= 40) d.set_phase(SITTING_FLAMING);
    }
};

struct TakeoffPhase : PhaseInstance {
    using PhaseInstance::PhaseInstance;
    bool first = true;
    std::optional<Path> path;
    std::optional<Vec3> tgt;
    Phase phase() const override { return TAKEOFF; }
    void begin() override { first = true, path.reset(), tgt.reset(); }
    std::optional<Vec3> target() const override { return tgt; }
    void server_tick() override {
        if (!first && path) {
            double ex = d.fight_origin[0] + 0.5, ey = d.egg_y(true) + 0.5, ez = d.fight_origin[2] + 0.5;
            if (!(Vec3{ex, ey, ez}.dist_sqr(d.pos) < 100.0)) d.set_phase(HOLDING_PATTERN);
            return;
        }
        first = false;
        int cur = d.find_closest_node();
        Vec3 look = d.head_look_vector();
        int t = d.find_closest_node(-look.x * 40.0, 105.0, -look.z * 40.0);
        if (fight && fight_alive_crystals() > 0) {
            t %= 12;
            if (t < 0) t += 12;
        } else {
            t -= 12, t &= 7, t += 12;
        }
        path = d.find_path(cur, t, std::nullopt);
        if (path) {
            path->advance();
            if (!path->done()) {
                auto c = path->next_pos();
                path->advance();
                double ty;
                do ty = c[1] + rand_float() * 20.0f;
                while (ty < c[1]);
                tgt = Vec3{double(c[0]), ty, double(c[2])};
            }
        }
    }
};

struct ChargePlayerPhase : PhaseInstance {
    using PhaseInstance::PhaseInstance;
    std::optional<Vec3> tgt;
    int since = 0;
    Phase phase() const override { return CHARGING_PLAYER; }
    void begin() override { tgt.reset(), since = 0; }
    float fly_speed() const override { return 3.0f; }
    std::optional<Vec3> target() const override { return tgt; }
    void server_tick() override {
        if (!tgt) return d.set_phase(HOLDING_PATTERN);
        if (since > 0 && since++ >= 10) return d.set_phase(HOLDING_PATTERN);
        double dist = tgt->dist_sqr(d.pos);
        if (dist < 100.0 || dist > 22500.0) since++;
    }
};

struct DeathPhase : PhaseInstance {
    using PhaseInstance::PhaseInstance;
    std::optional<Vec3> tgt;
    int time = 0;
    Phase phase() const override { return DYING; }
    void begin() override { tgt.reset(), time = 0; }
    float fly_speed() const override { return 3.0f; }
    std::optional<Vec3> target() const override { return tgt; }
    void server_tick() override {
        time++;
        if (!tgt) tgt = Vec3{d.fight_origin[0] + 0.5, double(d.egg_y(false)), d.fight_origin[2] + 0.5};
        double dist = tgt->dist_sqr(d.pos);
        d.set_health(dist < 100.0 || dist > 22500.0 ? 0.0f : 1.0f);
    }
};

struct HoverPhase : PhaseInstance {
    using PhaseInstance::PhaseInstance;
    std::optional<Vec3> tgt;
    Phase phase() const override { return HOVERING; }
    bool sitting() const override { return true; }
    void begin() override { tgt.reset(); }
    float fly_speed() const override { return 1.0f; }
    std::optional<Vec3> target() const override { return tgt; }
    void server_tick() override {
        if (!tgt) tgt = d.pos;
    }
};

inline void LandingPhase::server_tick() {
    if (!tgt) tgt = Vec3{d.fight_origin[0] + 0.5, double(d.egg_y(true)), d.fight_origin[2] + 0.5};
    if (tgt->dist_sqr(d.pos) < 1.0) {
        static_cast<SittingFlamingPhase&>(d.phase(SITTING_FLAMING)).count = 0;
        d.set_phase(SITTING_SCANNING);
    }
}

inline void SittingScanningPhase::server_tick() {
    time++;
    static EnderDragon* self;
    self = &d;
    Player* t = nearest_player(d.pos.x, d.pos.y, d.pos.z, 20.0, [](const Player& p) { return std::abs(p.y - self->pos.y) <= 10.0; });
    if (t) {
        if (time > 25) return d.set_phase(SITTING_ATTACKING);
        Vec3 aim = Vec3{t->x - d.pos.x, 0.0, t->z - d.pos.z}.normalize();
        Vec3 dir = Vec3{emth::sin(d.yaw * float(M_PI / 180.0)), 0.0, -emth::cos(d.yaw * float(M_PI / 180.0))}.normalize();
        float dot = float(dir.dot(aim));
        float angle = float(std::acos(dot) * 180.0F / float(M_PI)) + 0.5F;
        if (angle < 0.0F || angle > 10.0F) {
            auto& h = d.parts[EnderDragon::HEAD];
            double xa = t->x - h.x, za = t->z - h.z;
            double yrd = emth::clamp(emth::wrapDegrees(180.0 - emth::atan2(xa, za) * 180.0F / float(M_PI) - d.yaw), -100.0, 100.0);
            d.yrot_a *= 0.8F;
            float dist = float(std::sqrt(xa * xa + za * za)) + 1.0F, rot = dist;
            if (dist > 40.0F) dist = 40.0F;
            d.yrot_a += float(yrd) * (0.7F / dist / rot);
            d.yaw += d.yrot_a;
        }
    } else if (time >= 100) {
        Player* c = nearest_player(d.pos.x, d.pos.y, d.pos.z, 150.0);
        d.set_phase(TAKEOFF);
        if (c) {
            d.set_phase(CHARGING_PLAYER);
            static_cast<ChargePlayerPhase&>(d.phase(CHARGING_PLAYER)).tgt = Vec3{c->x, c->y, c->z};
        }
    }
}

// Entity.hasLineOfSight (eye to eye, COLLIDER).
inline bool line_of_sight(const EnderDragon& d, const Player& p) {
    return !clip_hits(Vec3{d.pos.x, d.pos.y + d.height * 0.85, d.pos.z}, Vec3{p.x, p.y + 1.62, p.z});
}

inline void StrafePhase::server_tick() {
    Player* attack = player_by_eid(attack_eid);
    if (!attack) return d.set_phase(HOLDING_PATTERN);  // ponytail: vanilla keeps a dead target until its entity is gone
    if (path && path->done()) {
        double xt = attack->x, zt = attack->z, xd = xt - d.pos.x, zd = zt - d.pos.z;
        double dist = std::sqrt(xd * xd + zd * zd);
        double ho = std::min(0.4F + dist / 80.0 - 1.0, 10.0);
        tgt = Vec3{xt, attack->y + ho, zt};
    }
    double dt = tgt ? tgt->dist_sqr(d.pos) : 0.0;
    if (dt < 100.0 || dt > 22500.0) find_new_target();
    if (Vec3{attack->x, attack->y, attack->z}.dist_sqr(d.pos) < 4096.0) {
        if (line_of_sight(d, *attack)) {
            charge++;
            Vec3 aim = Vec3{attack->x - d.pos.x, 0.0, attack->z - d.pos.z}.normalize();
            Vec3 dir = Vec3{emth::sin(d.yaw * float(M_PI / 180.0)), 0.0, -emth::cos(d.yaw * float(M_PI / 180.0))}.normalize();
            float dot = float(dir.dot(aim));
            float ang = float(std::acos(dot) * 180.0F / float(M_PI)) + 0.5F;
            if (charge >= 5 && ang >= 0.0F && ang < 10.0F) {
                Vec3 view = d.view_vector(0);
                auto& h = d.parts[EnderDragon::HEAD];
                double sx = h.x - view.x, sy = h.y + h.h * 0.5 + 0.5, sz = h.z - view.z;
                Vec3 dirv{attack->x - sx, attack->y + 0.9 - sy, attack->z - sz};
                level_event(1017, d.block_x(), d.block_y(), d.block_z(), 0);
                auto fb = std::make_unique<DragonFireball>();
                fb->owner_id = d.id;
                fb->snap_to(sx, sy, sz, d.yaw, d.pitch);
                fb->vel = dirv.normalize().normalize() * 0.1;
                add_entity(std::move(fb));
                charge = 0;
                if (path)
                    while (!path->done()) path->advance();
                d.set_phase(HOLDING_PATTERN);
            }
        } else if (charge > 0) charge--;
    } else if (charge > 0) charge--;
}

// Dragon parts are ids id+1..id+8 on the client (EnderDragon.recreateFromPacket): reserve them.
inline std::unique_ptr<::Entity> make_dragon() {
    auto d = std::make_unique<EnderDragon>();
    d->id = next_entity_id.fetch_add(9);
    return d;
}
inline EnderDragon::EnderDragon() {
    type = entity_types::ender_dragon;
    no_physics = true;
    set_phase(HOVERING);
}
inline PhaseInstance& EnderDragon::phase(Phase p) {
    auto& slot = phases[size_t(p)];
    if (!slot) {
        switch (p) {
            case HOLDING_PATTERN: slot = std::make_unique<HoldingPatternPhase>(*this); break;
            case STRAFE_PLAYER: slot = std::make_unique<StrafePhase>(*this); break;
            case LANDING_APPROACH: slot = std::make_unique<LandingApproachPhase>(*this); break;
            case LANDING: slot = std::make_unique<LandingPhase>(*this); break;
            case TAKEOFF: slot = std::make_unique<TakeoffPhase>(*this); break;
            case SITTING_FLAMING: slot = std::make_unique<SittingFlamingPhase>(*this); break;
            case SITTING_SCANNING: slot = std::make_unique<SittingScanningPhase>(*this); break;
            case SITTING_ATTACKING: slot = std::make_unique<SittingAttackingPhase>(*this); break;
            case CHARGING_PLAYER: slot = std::make_unique<ChargePlayerPhase>(*this); break;
            case DYING: slot = std::make_unique<DeathPhase>(*this); break;
            default: slot = std::make_unique<HoverPhase>(*this); break;
        }
    }
    return *slot;
}

inline Vec3 EnderDragon::head_look_vector() {
    Phase p = current->phase();
    if (p == LANDING || p == TAKEOFF) {
        double ex = fight_origin[0] + 0.5, ey = egg_y(true) + 0.5, ez = fight_origin[2] + 0.5;
        float dist = std::max(float(std::sqrt(dsq(Vec3{ex, ey, ez}, pos.x, pos.y, pos.z))) / 4.0F, 1.0F);
        float yo = 6.0F / dist;
        return view_vector(-yo * 1.5F * 5.0F);
    }
    if (current->sitting()) return view_vector(-45.0F);
    return view_vector(pitch);
}

// ---------- nodes ----------
inline int EnderDragon::find_closest_node() {
    if (!nodes_ready) {
        nodes_ready = true;
        for (int i = 0; i < 24; i++) {
            int ya = 5, nx, nz;
            if (i < 12) {
                nx = Mth::floor(60.0F * emth::cos(2.0F * (float(-M_PI) + float(M_PI / 12) * i)));
                nz = Mth::floor(60.0F * emth::sin(2.0F * (float(-M_PI) + float(M_PI / 12) * i)));
            } else if (i < 20) {
                int m = i - 12;
                nx = Mth::floor(40.0F * emth::cos(2.0F * (float(-M_PI) + float(M_PI / 8) * m)));
                nz = Mth::floor(40.0F * emth::sin(2.0F * (float(-M_PI) + float(M_PI / 8) * m)));
                ya += 10;
            } else {
                int m = i - 20;
                nx = Mth::floor(20.0F * emth::cos(2.0F * (float(-M_PI) + float(M_PI / 4) * m)));
                nz = Mth::floor(20.0F * emth::sin(2.0F * (float(-M_PI) + float(M_PI / 4) * m)));
            }
            nodes[size_t(i)].x = nx, nodes[size_t(i)].z = nz;
            nodes[size_t(i)].y = std::max(73, heightmap(nx, nz, true) + ya);
        }
        adjacency = {6146, 8197, 8202, 16404, 32808, 32848, 65696, 131392, 131712, 263424, 526848, 525313,
                     1581057, 3166214, 2138120, 6373424, 4358208, 12910976, 9044480, 9706496, 15216640, 13688832, 11763712, 8257536};
    }
    return find_closest_node(pos.x, pos.y, pos.z);
}
inline int EnderDragon::find_closest_node(double tx, double ty, double tz) {
    float closest = 10000.0F;
    int idx = 0;
    Node cur;
    cur.x = Mth::floor(tx), cur.y = Mth::floor(ty), cur.z = Mth::floor(tz);
    int start = !fight || fight_alive_crystals() == 0 ? 12 : 0;
    for (int i = start; i < 24; i++) {
        float d = nodes[size_t(i)].dist_sqr(cur);
        if (d < closest) closest = d, idx = i;
    }
    return idx;
}
inline std::optional<Path> EnderDragon::find_path(int start, int endi, std::optional<Node> final_node) {
    for (auto& n : nodes) n.closed = false, n.f = n.g = n.h = 0, n.came_from = nullptr, n.heap_idx = -1;
    Node* from = &nodes[size_t(start)];
    Node* to = &nodes[size_t(endi)];
    from->g = 0, from->h = from->dist(*to), from->f = from->h;
    open.clear();
    open.insert(from);
    Node* closest = from;
    int min_idx = !fight || fight_alive_crystals() == 0 ? 12 : 0;
    auto reconstruct = [](Node* end, std::optional<Node>& fin) {
        Path p;
        Node* n = end;
        std::vector<std::array<int, 3>> rev;
        if (fin) rev.push_back({fin->x, fin->y, fin->z});
        for (; n; n = n->came_from) rev.push_back({n->x, n->y, n->z});
        p.nodes.assign(rev.rbegin(), rev.rend());
        return p;
    };
    while (!open.empty()) {
        Node* o = open.pop();
        if (o->same(*to)) return reconstruct(to, final_node);
        if (o->dist(*to) < closest->dist(*to)) closest = o;
        o->closed = true;
        int xi = int(o - nodes.data());
        for (int i = min_idx; i < 24; i++) {
            if ((adjacency[size_t(xi)] & 1 << i) <= 0) continue;
            Node* a = &nodes[size_t(i)];
            if (a->closed) continue;
            float g = o->g + o->dist(*a);
            if (a->heap_idx < 0 || g < a->g) {
                a->came_from = o, a->g = g, a->h = a->dist(*to);
                if (a->heap_idx >= 0) open.change_cost(a, a->g + a->h);
                else a->f = a->g + a->h, open.insert(a);
            }
        }
    }
    if (closest == from) return std::nullopt;
    return reconstruct(closest, final_node);
}

// ---------- ticking ----------
inline void EnderDragon::tick() {  // LivingEntity.tick -> baseTick + aiStep (tick_count: entity_common_tick)
    if (fight_owns(*this) && !linked) {  // aiStep: adopt the level's fight when our uuid is its dragon
        linked = true;
        fight_origin_into(fight_origin);
    }
    if (hurt_time > 0) hurt_time--;
    if (cooldown > 0) cooldown--;
    if (dying()) tick_death();
    if (removed) return;
    ai_step();
    // Mob.baseTick ambient sound (interval 80)
    if (!dying() && rand_int(1000) < ambient_time++) {
        ambient_time = -80;
        ::play_sound("minecraft:entity.ender_dragon.ambient", SRC_HOSTILE, pos.x, pos.y, pos.z, 5.0f, (rand_float() - rand_float()) * 0.2f + 1.0f);
    }
}

inline void EnderDragon::ai_step() {
    oflap = flap;
    if (dying()) return;  // vanilla: explosion particles only (client side)
    // checkCrystals
    if (nearest_crystal) {
        ::Entity* nc = entity_by_id(nearest_crystal);
        if (!nc || nc->removed) nearest_crystal = 0;
        else if (tick_count % 10 == 0 && health < max_health) set_health(health + 1.0f);
    }
    if (rand_int(10) == 0) {
        int best = 0;
        double bd = DBL_MAX;
        for (auto* c : in_box<EndCrystal>(bb.inflate(32.0)))
            if (double dd = c->dist_sqr(*this); dd < bd) bd = dd, best = c->id;
        nearest_crystal = best;
    }
    float fs = 0.2F / (float(vel.horizontal()) * 10.0F + 1.0F);
    fs *= float(std::pow(2.0, vel.y));
    flap += current->sitting() ? 0.1F : in_wall ? fs * 0.5F : fs;
    yaw = emth::wrapDegrees(yaw);
    if (no_ai) { flap = 0.5F; return; }
    history.record(pos.y, yaw);
    PhaseInstance* ph = current;
    ph->server_tick();
    if (current != ph) ph = current, ph->server_tick();
    if (auto tl = ph->target()) {
        double xdd = tl->x - pos.x, ydd = tl->y - pos.y, zdd = tl->z - pos.z;
        double dist = xdd * xdd + ydd * ydd + zdd * zdd;
        float mx = ph->fly_speed();
        double hd = std::sqrt(xdd * xdd + zdd * zdd);
        if (hd > 0.0) ydd = emth::clamp(ydd / hd, double(-mx), double(mx));
        vel.y += ydd * 0.01;
        yaw = emth::wrapDegrees(yaw);
        Vec3 aim = (*tl - pos).normalize();
        Vec3 dir = Vec3{emth::sin(yaw * float(M_PI / 180.0)), vel.y, -emth::cos(yaw * float(M_PI / 180.0))}.normalize();
        float dot = std::max((float(dir.dot(aim)) + 0.5F) / 1.5F, 0.0F);
        if (std::abs(xdd) > 1.0E-5F || std::abs(zdd) > 1.0E-5F) {
            float yrd = emth::clamp(emth::wrapDegrees(180.0F - float(emth::atan2(xdd, zdd)) * (180.0F / float(M_PI)) - yaw), -50.0F, 50.0F);
            yrot_a *= 0.8F;
            yrot_a = yrot_a + yrd * ph->turn_speed();
            yaw = yaw + yrot_a * 0.1F;
        }
        float span = float(2.0 / (dist + 1.0));
        float speed = 0.06F * (dot * span + (1.0F - span));
        // moveRelative(speed, (0, 0, -1))
        float s = emth::sin(yaw * float(M_PI / 180.0)), c = emth::cos(yaw * float(M_PI / 180.0));
        double mz = -1.0 * speed;
        vel.x += 0.0 * c - mz * s, vel.z += mz * c + 0.0 * s;
        Vec3 m = in_wall ? vel * (0.8F) : vel;
        move(SELF, m);  // noPhysics
        Vec3 actual = vel.normalize();
        double slide = 0.8 + 0.15 * (actual.dot(dir) + 1.0) / 2.0;
        vel = {vel.x * slide, vel.y * 0.91F, vel.z * slide};
    }
    float tilt = float(history.get(5).y - history.get(10).y) * 10.0F * float(M_PI / 180.0);
    float ct = emth::cos(tilt), st = emth::sin(tilt);
    float r1 = yaw * float(M_PI / 180.0), s1 = emth::sin(r1), c1 = emth::cos(r1);
    tick_part(BODY, s1 * 0.5F, 0.0, -c1 * 0.5F);
    tick_part(WING1, c1 * 4.5F, 2.0, s1 * 4.5F);
    tick_part(WING2, c1 * -4.5F, 2.0, s1 * -4.5F);
    if (hurt_time <= 0) {  // knockBack from the wings, hurt from head / neck
        auto& b = parts[BODY];
        for (int wing : {WING1, WING2}) {
            AABB kb = part_box(wing).inflate(4.0, 2.0, 4.0).move(0.0, -2.0, 0.0);
            for (auto& pp : players) {
                if (!here(*pp)) continue;
                Player& p = *pp;
                if (!attackable(p) || !player_box(p).intersects(kb)) continue;
                double xd = p.x - b.x, zd = p.z - b.z, dd = std::max(xd * xd + zd * zd, 0.1);
                push_player(p, xd / dd * 4.0, 0.2F, zd / dd * 4.0);
                auto lh = player_last_hurt.find(p.eid);
                if (!current->sitting() && (lh == player_last_hurt.end() || lh->second < server_tick - 2)) {
                    player_last_hurt[p.eid] = server_tick;
                    hurt_player(p, 5.0f, "mob_attack", true);
                }
            }
        }
        for (int part : {HEAD, NECK}) {
            AABB hb = part_box(part).inflate(1.0, 1.0, 1.0);
            for (auto& pp : players)
                if (attackable(*pp) && player_box(*pp).intersects(hb)) {
                    player_last_hurt[pp->eid] = server_tick;
                    hurt_player(*pp, 10.0f, "mob_attack", true);
                }
        }
    }
    float s2 = emth::sin(yaw * float(M_PI / 180.0) - yrot_a * 0.01F), c2 = emth::cos(yaw * float(M_PI / 180.0) - yrot_a * 0.01F);
    float yoff = current->sitting() ? -1.0F : float(history.get(5).y - history.get(0).y);
    tick_part(HEAD, s2 * 6.5F * ct, yoff + st * 6.5F, -c2 * 6.5F * ct);
    tick_part(NECK, s2 * 5.5F * ct, yoff + st * 5.5F, -c2 * 5.5F * ct);
    auto& p1 = history.get(5);
    for (int i = 0; i < 3; i++) {
        auto& p0 = history.get(12 + i * 2);
        float rot = yaw * float(M_PI / 180.0) + float(emth::wrapDegrees(double(p0.yrot - p1.yrot))) * float(M_PI / 180.0);
        float ss = emth::sin(rot), cc = emth::cos(rot), dd = (i + 1) * 2.0F;
        tick_part(TAIL1 + i, -(s1 * 1.5F + ss * dd) * ct, p0.y - p1.y - (dd + 1.5F) * st + 1.5, (c1 * 1.5F + cc * dd) * ct);
    }
    in_wall = bool(int(check_walls(part_box(HEAD))) | int(check_walls(part_box(NECK))) | int(check_walls(part_box(BODY))));  // all three run
    fight_update_dragon(*this);
}

inline bool EnderDragon::check_walls(const AABB& bb) {
    int x0 = Mth::floor(bb.x0), y0 = Mth::floor(bb.y0), z0 = Mth::floor(bb.z0);
    int x1 = Mth::floor(bb.x1), y1 = Mth::floor(bb.y1), z1 = Mth::floor(bb.z1);
    bool hit = false, destroyed = false;
    bool grief = rule("mob_griefing");
    for (int bx = x0; bx <= x1; bx++)
        for (int by = y0; by <= y1; by++)
            for (int bz = z0; bz <= z1; bz++) {
                int s = get_block(bx, by, bz);
                if (is_air(s) || has_tag(tag_dragon_transparent, s)) continue;
                if (grief && !has_tag(tag_dragon_immune, s)) {
                    set_block(bx, by, bz, blocks::air);  // removeBlock(pos, false): fluids stay
                    destroyed = true;
                } else hit = true;
            }
    if (destroyed)
        level_event(2008, x0 + rand_int(x1 - x0 + 1), y0 + rand_int(y1 - y0 + 1), z0 + rand_int(z1 - z0 + 1), 0);
    return hit;
}

inline bool EnderDragon::hurt_by(const DamageSource& d, Player* p, float dmg, int part) {  // EnderDragon.hurt(level, part, source, damage)
    if (current->phase() == DYING) return false;
    if (part < 0) part = BODY;
    dmg = current->on_hurt(d, dmg);
    if (part != HEAD && part != NECK) dmg = dmg / 4.0F + std::min(dmg, 1.0F);
    if (dmg < 0.01F) return false;
    if (p || d.is(DT_IS_EXPLOSION)) {  // getEntity() instanceof Player || ALWAYS_HURTS_ENDER_DRAGONS (= #is_explosion)
        float before = health;
        really_hurt(d, p, dmg);
        if (current->sitting()) {
            sitting_damage = sitting_damage + before - health;
            if (sitting_damage > 0.25F * max_health) {
                sitting_damage = 0.0F;
                set_phase(TAKEOFF);
            }
        }
    }
    return true;
}

inline bool EnderDragon::really_hurt(const DamageSource& d, Player* p, float dmg) {  // LivingEntity.hurtServer (dragon: no armor/effects)
    if (dying() || (removed)) return false;
    if (dmg < 0) dmg = 0;
    bool full = true;
    if (cooldown > 10) {
        if (dmg <= last_hurt) return false;
        set_health(health - (dmg - last_hurt));
        last_hurt = dmg;
        full = false;
    } else {
        last_hurt = dmg, cooldown = 20;
        set_health(health - dmg);
        hurt_time = 10;
    }
    if (full) {
        Writer w;  // broadcastDamageEvent
        w.varint(play_cb::damage_event).varint(id).varint(d.type)
            .varint(p ? p->eid + 1 : d.causing ? d.causing->id + 1 : 0)
            .varint(d.direct ? d.direct->id + 1 : p ? p->eid + 1 : 0).boolean(false);
        broadcast_near(pos.x, pos.z, w);
        ::play_sound("minecraft:entity.ender_dragon.hurt", SRC_HOSTILE, pos.x, pos.y, pos.z, 5.0f, (rand_float() - rand_float()) * 0.2f + 1.0f);
    }
    if (dying()) {  // die() -> handleKillingBlow
        if (!current->sitting()) {
            set_health(1.0f);
            set_phase(DYING);
        } else dead_flag = true;
        broadcast_event(3);
    }
    return true;
}

inline void EnderDragon::tick_death() {
    fight_update_dragon(*this);
    death_time++;
    int xp = fight && !fight_previously_killed() ? 12000 : 500;
    if (death_time > 150 && death_time % 5 == 0 && rule("mob_drops")) award_xp(pos.x, pos.y, pos.z, Mth::floor(xp * 0.08F));
    if (death_time == 1) level_event(1028, block_x(), block_y(), block_z(), 0, true);
    move(SELF, Vec3{0.0, double(0.1F), 0.0});  // noPhysics
    for (auto& p : parts) p.y += 0.1F;
    if (death_time >= 200) {
        if (rule("mob_drops")) award_xp(pos.x, pos.y, pos.z, Mth::floor(xp * 0.2F));
        fight_dragon_killed(*this);
        remove(Removal::KILLED);
    }
}

inline void EnderDragon::on_crystal_destroyed(EndCrystal& c, Player* p) {
    if (!p) p = nearest_player(c.block_x(), c.block_y(), c.block_z(), 64.0);
    if (c.id == nearest_crystal) hurt_by(DamageSource::of(p ? "player_explosion" : "explosion", nullptr, &c), p, 10.0f, HEAD);
    current->on_crystal_destroyed(p);
}

}  // namespace boss
