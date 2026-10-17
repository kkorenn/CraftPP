#pragma once
// MobEffectInstance (net.minecraft.world.effect): one active effect on a living entity. Behaviour: effects.hpp.
#include <cstdint>
#include <memory>

namespace eff {
inline int speed = -1, slowness = -1, haste = -1, mining_fatigue = -1, strength = -1, instant_health = -1,
           instant_damage = -1, jump_boost = -1, nausea = -1, regeneration = -1, resistance = -1, fire_resistance = -1,
           water_breathing = -1, invisibility = -1, blindness = -1, night_vision = -1, hunger = -1, weakness = -1,
           poison = -1, wither = -1, health_boost = -1, absorption = -1, saturation = -1, glowing = -1, levitation = -1,
           luck = -1, unluck = -1, slow_falling = -1, conduit_power = -1, dolphins_grace = -1,
           bad_omen = -1, hero_of_the_village = -1, darkness = -1, trial_omen = -1, raid_omen = -1, wind_charged = -1,
           weaving = -1, oozing = -1, infested = -1, breath_of_the_nautilus = -1;
}

struct EffectInstance {
    int id = 0;          // minecraft:mob_effect registry id
    int duration = 0;    // ticks, -1 = infinite
    int amp = 0;         // 0..255
    bool ambient = false, visible = true, icon = true;
    std::unique_ptr<EffectInstance> hidden;  // weaker/longer effect restored when this one runs out

    EffectInstance() = default;
    EffectInstance(int i, int d, int a = 0, bool amb = false, bool vis = true, bool ic = true)
        : id(i), duration(d), amp(a < 0 ? 0 : a > 255 ? 255 : a), ambient(amb), visible(vis), icon(ic) {}
    EffectInstance(const EffectInstance& o) { *this = o; }
    EffectInstance(EffectInstance&&) = default;
    EffectInstance& operator=(EffectInstance&&) = default;
    EffectInstance& operator=(const EffectInstance& o) {
        if (this == &o) return *this;
        id = o.id, duration = o.duration, amp = o.amp, ambient = o.ambient, visible = o.visible, icon = o.icon;
        hidden = o.hidden ? std::make_unique<EffectInstance>(*o.hidden) : nullptr;
        return *this;
    }
    bool infinite() const { return duration == -1; }
    bool ends_within(int t) const { return !infinite() && duration <= t; }
    bool shorter_than(const EffectInstance& o) const { return !infinite() && (duration < o.duration || o.infinite()); }
    void set_details_from(const EffectInstance& o) {
        duration = o.duration, amp = o.amp, ambient = o.ambient, visible = o.visible, icon = o.icon;
    }
    // MobEffectInstance.update: merge a new instance of the same effect; true when something visible changed.
    bool update(const EffectInstance& t) {
        bool changed = false;
        if (t.amp > amp) {
            if (t.shorter_than(*this)) {
                auto prev = std::move(hidden);
                hidden = std::make_unique<EffectInstance>();
                hidden->id = id;
                hidden->set_details_from(*this);
                hidden->hidden = std::move(prev);
            }
            amp = t.amp, duration = t.duration;
            changed = true;
        } else if (shorter_than(t)) {
            if (t.amp == amp) duration = t.duration, changed = true;
            else if (!hidden) hidden = std::make_unique<EffectInstance>(t);
            else hidden->update(t);
        }
        if ((!t.ambient && ambient) || changed) ambient = t.ambient, changed = true;
        if (t.visible != visible) visible = t.visible, changed = true;
        if (t.icon != icon) icon = t.icon, changed = true;
        return changed;
    }
    void tick_down() {
        if (hidden) hidden->tick_down();
        if (!infinite() && duration != 0) duration--;
    }
    bool downgrade_to_hidden() {
        if (duration == 0 && hidden) {
            set_details_from(*hidden);
            hidden = std::move(hidden->hidden);
            return true;
        }
        return false;
    }
};
