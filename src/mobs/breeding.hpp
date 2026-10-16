#pragma once
// Breeding, ageing and taming state machines (AgeableMob, Animal, TamableAnimal, AbstractHorse temper).
// Plain value types embedded in a mob; the mob calls these and turns the returned flags into packets
// (entity_event 18 hearts / 7 tame / 6 smoke, DATA_BABY_ID metadata) and spawns (baby, XP orb).
#include "../game/entity.hpp"
#include "roster_data.hpp"

// AgeableMob: age < 0 baby (counts up to 0), > 0 breeding cooldown (counts down).
struct AgeState {
    int age = 0, forced_age = 0, forced_age_timer = 0;
    bool age_locked = false;

    bool baby() const { return age < 0; }
    bool can_age_up() const { return baby() && !age_locked; }
    // Returns true when the baby flag flipped (resync DATA_BABY_ID, refresh dimensions).
    bool set_age(int a) {
        bool flip = (age < 0) != (a < 0);
        age = a;
        return flip;
    }
    bool age_up(int seconds, bool forced) {  // AgeableMob.ageUp
        int old = age, a = age + seconds * 20;
        if (a > 0) a = 0;
        bool flip = set_age(a);
        if (forced) {
            forced_age += a - old;
            if (forced_age_timer == 0) forced_age_timer = 40;
        }
        if (age == 0) flip |= set_age(forced_age);
        return flip;
    }
    // AgeableMob.aiStep (server, alive). Returns baby flag flip; *particle = happy villager particle this tick.
    bool tick(bool* particle = nullptr) {
        if (particle) *particle = forced_age_timer > 0 && forced_age_timer % 4 == 0;
        if (forced_age_timer > 0) forced_age_timer--;
        if (can_age_up()) return set_age(age + 1);
        if (age > 0) return set_age(age - 1);
        return false;
    }
};
inline constexpr int BABY_START_AGE = -24000, PARENT_AGE_AFTER_BREEDING = 6000, IN_LOVE_TIME = 600;
inline int speed_up_seconds_when_feeding(int ticks_until_adult) { return int(float(ticks_until_adult / 20) * 0.1f); }

// Animal love mode.
struct LoveState {
    int in_love = 0;
    int64_t cause_uh = 0, cause_ul = 0;  // loveCause player UUID (0,0 = none)

    bool can_fall_in_love() const { return in_love <= 0; }
    bool is_in_love() const { return in_love > 0; }
    void set_in_love(int64_t uh, int64_t ul) { in_love = IN_LOVE_TIME, cause_uh = uh, cause_ul = ul; }  // + entity_event 18
    void reset() { in_love = 0; }
    // Animal.aiStep: returns true on ticks that show a heart particle (client-side in vanilla; optional).
    bool tick(const AgeState& a) {
        if (a.age != 0) in_love = 0;
        if (in_love > 0) return --in_love % 10 == 0;
        return false;
    }
};

enum class FeedResult { pass, in_love, aged_up };
// Animal.mobInteract food branch. On anything but pass the caller consumes one item (usePlayerItem, not in
// creative), plays the eat sound and, for in_love, broadcasts entity_event 18. *flip = baby flag changed.
inline FeedResult animal_feed(const RosterType& t, AgeState& a, LoveState& l, int item, int64_t uh, int64_t ul,
                              bool* flip = nullptr) {
    if (!t.is_food(item)) return FeedResult::pass;
    if (a.age == 0 && l.can_fall_in_love()) {
        l.set_in_love(uh, ul);
        return FeedResult::in_love;
    }
    if (a.can_age_up()) {
        bool f = a.age_up(speed_up_seconds_when_feeding(-a.age), true);
        if (flip) *flip = f;
        return FeedResult::aged_up;
    }
    return FeedResult::pass;
}
// Animal.canMate (same entity type stands in for getClass(); callers exclude self).
inline bool can_mate(int type_a, const LoveState& a, int type_b, const LoveState& b) {
    return type_a == type_b && a.is_in_love() && b.is_in_love();
}
// Animal.finalizeSpawnChildFromBreeding: parents' cooldown + love reset; returns XP orb value (1..7).
// The caller spawns the baby (AgeState{BABY_START_AGE}) at parent A, broadcasts entity_event 18 for A
// and spawns the orb when mobDrops is on.
inline int finish_breeding(AgeState& a, LoveState& la, AgeState& b, LoveState& lb) {
    a.set_age(PARENT_AGE_AFTER_BREEDING), b.set_age(PARENT_AGE_AFTER_BREEDING);
    la.reset(), lb.reset();
    return rand_int(7) + 1;
}

// TamableAnimal: owner + sitting. DATA_FLAGS_ID bit 1 = in sitting pose, bit 4 = tame.
struct TameState {
    bool tame = false, ordered_to_sit = false, in_sitting_pose = false;
    int64_t owner_uh = 0, owner_ul = 0;

    bool owned_by(int64_t uh, int64_t ul) const { return tame && owner_uh == uh && owner_ul == ul; }
    uint8_t flags() const { return uint8_t((in_sitting_pose ? 1 : 0) | (tame ? 4 : 0)); }
    void tame_by(int64_t uh, int64_t ul) { tame = true, owner_uh = uh, owner_ul = ul; }
};
// Taming roll per item given: wolf (bone) / cat (cat_food) / ocelot trust: nextInt(3)==0; parrot (seeds): nextInt(10)==0.
// true -> tame (entity_event 7), false -> entity_event 6. Wolf/cat also sit on success.
inline int tame_chance_bound(const RosterType& t) { return t.name == "parrot" ? 10 : 3; }
inline bool try_tame(const RosterType& t, TameState& s, int item, int64_t uh, int64_t ul) {
    if (s.tame || !t.is_tame_item(item)) return false;
    if (rand_int(tame_chance_bound(t)) != 0) return false;
    s.tame_by(uh, ul);
    if (t.name == "wolf" || t.name == "cat") s.ordered_to_sit = s.in_sitting_pose = true;
    return true;
}

// AbstractHorse temper: feeding raises it; RunAroundLikeCrazyGoal rolls it while an untamed horse is ridden.
struct HorseTemper {
    int temper = 0;
    int max = 100;  // getMaxTemper: llama 30, others 100
    void modify(int d) { temper = std::clamp(temper + d, 0, max); }
};
enum class BuckResult { none, tamed, bucked };
// RunAroundLikeCrazyGoal.tick with a player rider; roll_bound = adjustedTickDelay(50) of the goal system
// (25 when the goal runs every other tick).
inline BuckResult horse_buck_tick(HorseTemper& h, TameState& s, int64_t uh, int64_t ul, int roll_bound = 25) {
    if (s.tame || rand_int(roll_bound) != 0) return BuckResult::none;
    if (h.max > 0 && rand_int(h.max) < h.temper) {
        s.tame_by(uh, ul);
        return BuckResult::tamed;
    }
    h.modify(5);
    return BuckResult::bucked;  // eject passengers, makeMad, entity_event 6
}
// AbstractHorse.handleEating for the plain horse family (llama has its own table). Returns item used.
struct HorseFood { int heal_x2, age_up, temper; bool love; };
inline const HorseFood* horse_food(int item) {
    static const std::pair<const char*, HorseFood> table[] = {
        {"minecraft:wheat", {4, 20, 3, false}},          {"minecraft:sugar", {2, 30, 3, false}},
        {"minecraft:hay_block", {40, 180, 0, false}},    {"minecraft:apple", {6, 60, 3, false}},
        {"minecraft:red_mushroom", {6, 0, 3, false}},    {"minecraft:carrot", {6, 60, 3, false}},
        {"minecraft:golden_carrot", {8, 60, 5, true}},   {"minecraft:golden_apple", {20, 240, 10, true}},
        {"minecraft:enchanted_golden_apple", {20, 240, 10, true}}};
    static std::vector<std::pair<int, const HorseFood*>> ids;
    if (ids.empty())
        for (auto& [n, f] : table) ids.emplace_back(item_by_name.at(n), &f);
    for (auto& [id, f] : ids)
        if (id == item) return f;
    return nullptr;
}
// Returns true if the item was used. *heal = health to add; caller applies it (capped at max health).
inline bool horse_eat(int item, AgeState& a, LoveState& l, HorseTemper& h, const TameState& s, bool hurt, float* heal,
                      int64_t uh, int64_t ul) {
    const HorseFood* f = horse_food(item);
    *heal = 0;
    if (!f) return false;
    bool used = false;
    if (f->love && s.tame && a.age == 0 && !l.is_in_love()) used = true, l.set_in_love(uh, ul);
    if (hurt && f->heal_x2 > 0) *heal = float(f->heal_x2) / 2, used = true;
    if (a.baby() && f->age_up > 0 && !a.age_locked) a.age_up(f->age_up, false), used = true;
    if (f->temper > 0 && (used || !s.tame) && h.temper < h.max) h.modify(f->temper), used = true;
    return used;
}
