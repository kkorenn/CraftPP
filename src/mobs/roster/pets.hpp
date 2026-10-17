#pragma once
// Wolf, Cat, Ocelot, Parrot (animal.wolf / feline / parrot): taming, sitting, following and defending the owner.
#include "common.hpp"

// ---------- Wolf ----------
struct Wolf : TamableAnimal {
    bool is_food(const ItemStack& s) const override { return item_in_tag(s.item, "wolf_food"); }
    std::string ambient_sound() const override {
        if (rand_int(3) == 0) return is_tame() && health() < 20.0f ? "minecraft:entity.wolf.whine" : "minecraft:entity.wolf.pant";
        return "minecraft:entity.wolf.ambient";
    }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.wolf.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.wolf.death"; }
    float sound_volume() const override { return 0.4f; }
    void apply_taming_side_effects() override {  // Wolf.applyTamingSideEffects
        if (is_tame()) {
            set_attr_base(attr::max_health, 40.0);
            set_health(40.0f);
        } else {
            set_attr_base(attr::max_health, 8.0);
        }
    }
    bool wants_to_attack(LivingEntity& t, LivingEntity& owner) override {  // Wolf.wantsToAttack
        if (t.type == entity_types::creeper || t.type == entity_types::ghast || t.type == entity_types::armor_stand) return false;
        if (auto* w = dynamic_cast<Wolf*>(&t)) return !w->is_tame() || w->owner() != &owner;
        if (t.as_player() && owner.as_player() && !rule("pvp")) return false;
        if (auto* tam = dynamic_cast<TamableAnimal*>(&t); tam && tam->is_tame()) return false;
        return true;
    }
    void register_goals() override {  // Wolf.registerGoals
        goal_selector.add(1, std::make_unique<FloatGoal>(*this));
        goal_selector.add(1, std::make_unique<PanicGoal>(*this, 1.5));  // WolfPanicGoal
        goal_selector.add(2, std::make_unique<SitWhenOrderedToSitGoal>(*this));
        goal_selector.add(3, std::make_unique<AvoidEntityGoal>(*this, [](LivingEntity& e) { return e.type == entity_types::llama || e.type == entity_types::trader_llama; }, 24.0f, 1.5, 1.5));
        goal_selector.add(4, std::make_unique<LeapAtTargetGoal>(*this, 0.4f));
        goal_selector.add(5, std::make_unique<MeleeAttackGoal>(*this, 1.0, true));
        goal_selector.add(6, std::make_unique<FollowOwnerGoal>(*this, 1.0, 10.0f, 2.0f));
        goal_selector.add(7, std::make_unique<BreedGoal>(*this, 1.0));
        goal_selector.add(8, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
        goal_selector.add(10, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(10, std::make_unique<RandomLookAroundGoal>(*this));
        target_selector.add(1, std::make_unique<OwnerHurtByTargetGoal>(*this));
        target_selector.add(2, std::make_unique<OwnerHurtTargetGoal>(*this));
        target_selector.add(3, std::make_unique<HurtByTargetGoal>(*this));
        // NonTameRandomTargetGoal: wild wolves hunt sheep, rabbits and foxes; NearestAttackableTarget skeletons.
        auto prey = [this](LivingEntity& e) {
            return !is_tame() && (e.type == entity_types::sheep || e.type == entity_types::rabbit || e.type == entity_types::fox);
        };
        target_selector.add(5, std::make_unique<NearestAttackableTargetGoal>(*this, false, prey));
        target_selector.add(7, std::make_unique<NearestAttackableTargetGoal>(*this, false, [](LivingEntity& e) {
            return e.type == entity_types::skeleton || e.type == entity_types::stray || e.type == entity_types::wither_skeleton || e.type == entity_types::bogged; }));
    }
    bool can_mate(Animal& o) override {  // Wolf.canMate: both tame, neither sitting
        auto* w = dynamic_cast<Wolf*>(&o);
        return w && w != this && is_tame() && w->is_tame() && !in_sitting_pose() && !w->in_sitting_pose() && is_in_love() && w->is_in_love();
    }
    std::unique_ptr<Entity> breed_offspring(AgeableMob& partner) override {  // Wolf.getBreedOffspring: owner carries over
        auto e = create_entity(type);
        auto* pup = static_cast<Wolf*>(e.get());
        auto& p = static_cast<Wolf&>(partner);
        int64_t hi = owner_hi || owner_lo ? owner_hi : p.owner_hi, lo = owner_hi || owner_lo ? owner_lo : p.owner_lo;
        if (hi || lo) {
            pup->set_owner(hi, lo);
            pup->set_tame(true, true);
        }
        return e;
    }
    bool hurt_server(const DamageSource& s, float dmg) override {
        if (!TamableAnimal::hurt_server(s, dmg)) return false;
        ordered_to_sit = false;  // Wolf.hurtServer: stands up
        return true;
    }
    int mob_interact(Player& p, int hand) override {  // Wolf.mobInteract
        ItemStack& s = held_stack(p, hand);
        if (is_tame()) {
            if (is_food(s) && health() < max_health()) {  // TamableAnimal.feed(2, 2)
                int n = item_info[size_t(s.item)].nutrition;
                use_player_item(p, hand);
                heal(n > 0 ? 2.0f * float(n) : 2.0f);
                return IR_SUCCESS;
            }
            int r = TamableAnimal::mob_interact(p, hand);  // breeding food
            if (r != IR_SUCCESS && is_owned_by(p)) {
                set_ordered_to_sit(!ordered_to_sit);
                jumping = false;
                navigation->stop();
                set_target(nullptr);
                return IR_SUCCESS;
            }
            return r;
        }
        if (s.item == item_by_name["minecraft:bone"] && !target()) {  // tryToTame
            use_player_item(p, hand);
            if (rand_int(3) == 0) {
                tame(p);
                navigation->stop();
                set_target(nullptr);
                set_ordered_to_sit(true);
                broadcast_event(7);
            } else {
                broadcast_event(6);
            }
            return IR_SUCCESS;
        }
        return TamableAnimal::mob_interact(p, hand);
    }
};

// ---------- Cat / Ocelot ----------
struct Cat : TamableAnimal {
    bool is_food(const ItemStack& s) const override { return item_in_tag(s.item, "cat_food"); }
    std::string ambient_sound() const override {
        if (!is_tame()) return "minecraft:entity.cat.stray_ambient";
        return rand_int(4) == 0 ? "minecraft:entity.cat.purreow" : "minecraft:entity.cat.ambient";
    }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.cat.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.cat.death"; }
    void register_goals() override {  // Cat.registerGoals (minus lie-on-bed / sit-on-block / gifts)
        goal_selector.add(1, std::make_unique<FloatGoal>(*this));
        goal_selector.add(1, std::make_unique<PanicGoal>(*this, 1.5));
        goal_selector.add(2, std::make_unique<SitWhenOrderedToSitGoal>(*this));
        goal_selector.add(4, std::make_unique<TemptGoal>(*this, 0.6, [](const ItemStack& s) { return item_in_tag(s.item, "cat_food"); }, true));
        goal_selector.add(6, std::make_unique<FollowOwnerGoal>(*this, 1.0, 10.0f, 5.0f));
        goal_selector.add(9, std::make_unique<LeapAtTargetGoal>(*this, 0.3f));
        goal_selector.add(10, std::make_unique<MeleeAttackGoal>(*this, 1.0, true));  // OcelotAttackGoal
        goal_selector.add(11, std::make_unique<BreedGoal>(*this, 0.8));
        goal_selector.add(12, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 0.8, 1.0000001E-5f));
        goal_selector.add(13, std::make_unique<LookAtPlayerGoal>(*this, 10.0f));
        target_selector.add(1, std::make_unique<NearestAttackableTargetGoal>(*this, false, [this](LivingEntity& e) {
            return !is_tame() && (e.type == entity_types::rabbit || e.type == entity_types::turtle); }));
    }
    int mob_interact(Player& p, int hand) override {  // Cat.mobInteract
        ItemStack& s = held_stack(p, hand);
        if (is_tame()) {
            if (is_owned_by(p) && !is_food(s)) {
                set_ordered_to_sit(!ordered_to_sit);
                return IR_SUCCESS;
            }
            if (is_food(s) && health() < max_health()) {
                use_player_item(p, hand);
                heal(float(std::max(1, item_info[size_t(s.item)].nutrition)));
                return IR_SUCCESS;
            }
            return TamableAnimal::mob_interact(p, hand);
        }
        if (is_food(s)) {  // tryToTame
            use_player_item(p, hand);
            if (rand_int(3) == 0) {
                tame(p);
                set_ordered_to_sit(true);
                broadcast_event(7);
            } else {
                broadcast_event(6);
            }
            return IR_SUCCESS;
        }
        return TamableAnimal::mob_interact(p, hand);
    }
    bool can_mate(Animal& o) override {
        auto* c = dynamic_cast<Cat*>(&o);
        return c && c != this && is_tame() && c->is_tame() && !in_sitting_pose() && !c->in_sitting_pose() && is_in_love() && c->is_in_love();
    }
};

struct Ocelot : Animal {
    bool trusting() const { return data.get(edata::Ocelot::TRUSTING, 0); }
    bool is_food(const ItemStack& s) const override { return item_in_tag(s.item, "ocelot_food"); }
    std::string ambient_sound() const override { return "minecraft:entity.ocelot.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.ocelot.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.ocelot.death"; }
    void register_goals() override {
        goal_selector.add(1, std::make_unique<FloatGoal>(*this));
        goal_selector.add(3, std::make_unique<TemptGoal>(*this, 0.6, [](const ItemStack& s) { return item_in_tag(s.item, "ocelot_food"); }, true));
        goal_selector.add(7, std::make_unique<LeapAtTargetGoal>(*this, 0.3f));
        goal_selector.add(8, std::make_unique<MeleeAttackGoal>(*this, 1.0, true));
        goal_selector.add(9, std::make_unique<BreedGoal>(*this, 0.8));
        goal_selector.add(10, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 0.8, 1.0000001E-5f));
        goal_selector.add(11, std::make_unique<LookAtPlayerGoal>(*this, 10.0f));
        goal_selector.add(4, std::make_unique<AvoidEntityGoal>(*this, [this](LivingEntity& e) { return !trusting() && e.as_player(); }, 16.0f, 0.8, 1.33));
        target_selector.add(1, std::make_unique<NearestAttackableTargetGoal>(*this, false, [](LivingEntity& e) {
            return e.type == entity_types::chicken || e.type == entity_types::turtle; }));
    }
    int mob_interact(Player& p, int hand) override {  // Ocelot.mobInteract: trust instead of taming
        ItemStack& s = held_stack(p, hand);
        if (!trusting() && is_food(s)) {
            use_player_item(p, hand);
            if (rand_int(3) == 0) {
                data.set_bool(edata::Ocelot::TRUSTING, true);
                broadcast_event(41);
            } else {
                broadcast_event(40);
            }
            return IR_SUCCESS;
        }
        return Animal::mob_interact(p, hand);
    }
    void save_extra(Nbt& o) const override { Animal::save_extra(o); o.set("Trusting", Nbt::num(Nbt::Byte, trusting())); }
    void load_extra(const Nbt& in) override { Animal::load_extra(in); data.set_bool(edata::Ocelot::TRUSTING, nbt_num(in, "Trusting", 0) != 0); }
};

// ---------- Parrot ----------
struct Parrot : TamableAnimal {
    double default_gravity() const override { return on_ground ? TamableAnimal::default_gravity() : 0.02; }  // flaps down slowly
    bool cause_fall_damage(double, float, const DamageSource&) override { return false; }
    bool is_food(const ItemStack&) const override { return false; }  // parrots don't breed
    std::string ambient_sound() const override { return "minecraft:entity.parrot.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.parrot.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.parrot.death"; }
    void register_goals() override {  // ponytail: FlyingPathNavigation / shoulder riding not ported; walks
        goal_selector.add(0, std::make_unique<PanicGoal>(*this, 1.25));
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        goal_selector.add(1, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(2, std::make_unique<SitWhenOrderedToSitGoal>(*this));
        goal_selector.add(2, std::make_unique<FollowOwnerGoal>(*this, 1.0, 5.0f, 1.0f));
        goal_selector.add(3, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
    }
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override {
        data.set_int(edata::Parrot::VARIANT, rand_int(5));
        return TamableAnimal::finalize_spawn(r, g ? g : new AgeableMobGroupData(false));
    }
    int mob_interact(Player& p, int hand) override {  // Parrot.mobInteract
        ItemStack& s = held_stack(p, hand);
        if (!is_tame() && item_in_tag(s.item, "parrot_food")) {
            use_player_item(p, hand);
            play_sound("minecraft:entity.parrot.eat", 1.0f, 1.0f + (rand_float() - rand_float()) * 0.2f);
            if (rand_int(10) == 0) {
                tame(p);
                broadcast_event(7);
            } else {
                broadcast_event(6);
            }
            return IR_SUCCESS;
        }
        if (item_in_tag(s.item, "parrot_poisonous_food")) {  // ponytail: no poison effect yet; vanilla kills via Poison 900
            use_player_item(p, hand);
            hurt_server(DamageSource::of("player_attack", player_entity(p)), 3.4028235E38f);
            return IR_SUCCESS;
        }
        if (is_tame() && is_owned_by(p) && on_ground) {
            set_ordered_to_sit(!ordered_to_sit);
            return IR_SUCCESS;
        }
        return TamableAnimal::mob_interact(p, hand);
    }
};
