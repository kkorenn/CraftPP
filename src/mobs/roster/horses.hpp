#pragma once
// AbstractHorse family (horse, donkey, mule, skeleton/zombie horse, camel, llama): temper taming, saddles, chests,
// player riding (set_passengers + client-driven move_vehicle), breeding (horse + donkey = mule).
// ponytail: no horse inventory screen, armour, jumping charge, random stat inheritance or llama caravans yet.
#include "pets.hpp"

struct AbstractHorse : Animal {
    int temper = 0;
    int64_t owner_hi = 0, owner_lo = 0;
    int rider = 0;  // player entity id riding us
    uint8_t hflags() const { return uint8_t(data.get(edata::AbstractHorse::ID_FLAGS, 0)); }
    void set_hflag(int bit, bool v) { data.set_byte(edata::AbstractHorse::ID_FLAGS, v ? hflags() | bit : hflags() & ~bit); }
    bool tamed() const { return hflags() & 2; }
    virtual int max_temper() const { return 100; }
    virtual bool can_wear_saddle() const { return true; }
    bool saddled() const { return !item_in_slot(ES_SADDLE).empty(); }
    void modify_temper(int d) { temper = std::clamp(temper + d, 0, max_temper()); }
    bool is_food(const ItemStack& s) const override { return item_in_tag(s.item, "horse_food"); }
    std::string ambient_sound() const override { return entity_sound(*this, "ambient"); }
    std::string hurt_sound(const DamageSource&) const override { return entity_sound(*this, "hurt"); }
    std::string death_sound() const override { return entity_sound(*this, "death"); }
    float sound_volume() const override { return 0.8f; }
    int ambient_sound_interval() const override { return 400; }
    int max_spawn_cluster_size() const override { return 6; }

    void tame_with_name(Player& p) {  // AbstractHorse.tameWithName
        owner_hi = p.uuid_hi, owner_lo = p.uuid_lo;
        set_hflag(2, true);
        set_persistence_required();
        broadcast_event(7);
        progress::tame_animal(p, *this);
    }
    PlayerEntity* rider_entity() const { return rider ? static_cast<PlayerEntity*>(entity_by_id(rider)) : nullptr; }
    void send_passengers() { broadcast(set_passengers_packet(id, rider ? std::vector<int>{rider} : std::vector<int>{})); }
    void send_pairing(Player& to) override {
        Animal::send_pairing(to);
        if (rider) to.send(set_passengers_packet(id, {rider}));
    }
    void mount(Player& p) {  // doPlayerRide
        if (rider) return;
        rider = p.eid;
        p.yaw = yaw;
        navigation->stop();
        send_passengers();
    }
    void eject() {
        if (!rider) return;
        rider = 0;
        send_passengers();
    }
    bool controlled() const { return rider && tamed() && saddled(); }  // getControllingPassenger
    // AbstractHorse.handleEating
    bool handle_eating(Player& p, const ItemStack& s) {
        struct Food { const char* item; float heal; int age_up, temper; bool love; };
        static const Food foods[] = {{"wheat", 2, 20, 3, false}, {"sugar", 1, 30, 3, false}, {"hay_block", 20, 180, 0, false},
                                     {"apple", 3, 60, 3, false}, {"red_mushroom", 3, 0, 3, false}, {"carrot", 3, 60, 3, false},
                                     {"golden_carrot", 4, 60, 5, true}, {"golden_apple", 10, 240, 10, true},
                                     {"enchanted_golden_apple", 10, 240, 10, true}};
        const Food* f = nullptr;
        for (auto& x : foods)
            if (s.item == item_by_name["minecraft:" + std::string(x.item)]) f = &x;
        if (!f) return false;
        bool used = false;
        if (f->love && tamed() && age == 0 && !is_in_love()) used = true, set_in_love(&p);
        if (health() < max_health() && f->heal > 0) heal(f->heal), used = true;
        if (is_baby() && f->age_up > 0 && !age_locked()) age_up(f->age_up), used = true;
        if (f->temper > 0 && (used || !tamed()) && temper < max_temper()) modify_temper(f->temper), used = true;
        if (used) play_sound("minecraft:entity.horse.eat", 1.0f, 1.0f + (rand_float() - rand_float()) * 0.2f);
        return used;
    }
    int mob_interact(Player& p, int hand) override {  // AbstractHorse.mobInteract
        ItemStack& s = held_stack(p, hand);
        if (rider || is_baby()) {
            if (is_baby() && handle_eating(p, s)) { use_player_item(p, hand); return IR_SUCCESS; }
            return Animal::mob_interact(p, hand);
        }
        if (!s.empty()) {
            if (handle_eating(p, s)) { use_player_item(p, hand); return IR_SUCCESS; }
            if (s.item == item_by_name["minecraft:saddle"] && can_wear_saddle() && !saddled()) {
                if (!tamed()) { broadcast_event(6); return IR_SUCCESS; }  // makeMad
                set_item_slot(ES_SADDLE, {s.item, 1, 0});
                use_player_item(p, hand);
                play_sound("minecraft:entity.horse.saddle", 0.5f, 1.0f);
                return IR_SUCCESS;
            }
            if (int r = extra_interact(p, hand); r != IR_PASS) return r;
        }
        mount(p);
        return IR_SUCCESS;
    }
    virtual int extra_interact(Player&, int) { return IR_PASS; }
    void register_goals() override {  // AbstractHorse.registerGoals
        goal_selector.add(1, std::make_unique<PanicGoal>(*this, 1.2));
        goal_selector.add(2, std::make_unique<BreedGoal>(*this, 1.0));
        goal_selector.add(4, std::make_unique<FollowParentGoal>(*this, 1.0));
        goal_selector.add(6, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 0.7));
        goal_selector.add(7, std::make_unique<LookAtPlayerGoal>(*this, 6.0f));
        goal_selector.add(8, std::make_unique<RandomLookAroundGoal>(*this));
        goal_selector.add(3, std::make_unique<TemptGoal>(*this, 1.25, [](const ItemStack& s) { return item_in_tag(s.item, "horse_tempt_items"); }, false));
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
    }
    void tick() override {
        if (rider) {
            PlayerEntity* pe = rider_entity();
            if (!pe || pe->removed || pe->p->g.dead || pe->p->g.sneaking || !is_alive()) {
                eject();
            } else if (controlled()) {  // the client drives us (move_vehicle); just keep the rider's view in sync
                old = pos;
                tick_count++;
                return;
            } else if (!tamed() && rand_int(50) == 0) {  // RunAroundLikeCrazyGoal
                Player& p = *pe->p;
                if (max_temper() > 0 && rand_int(max_temper()) < temper) {
                    tame_with_name(p);
                } else {
                    modify_temper(5);
                    eject();
                    broadcast_event(6);
                    make_sound(entity_sound(*this, "angry"));
                }
            }
        }
        Animal::tick();
    }
    void die(const DamageSource& s) override { eject(); Animal::die(s); }
    bool can_mate(Animal& o) override {  // AbstractHorse.canParent: tame, adult, in love
        return &o != this && dynamic_cast<AbstractHorse*>(&o) && tamed() && static_cast<AbstractHorse&>(o).tamed() &&
               !rider && is_in_love() && o.is_in_love() && (o.type == type || mule_parents(type, o.type));
    }
    static bool mule_parents(int a, int b) {
        return (a == entity_types::horse && b == entity_types::donkey) || (a == entity_types::donkey && b == entity_types::horse);
    }
    std::unique_ptr<Entity> breed_offspring(AgeableMob& partner) override {
        return create_entity(mule_parents(type, partner.type) ? entity_types::mule : type);
    }
    void save_extra(Nbt& o) const override {
        Animal::save_extra(o);
        o.set("Tame", Nbt::num(Nbt::Byte, tamed()));
        o.set("Temper", Nbt::num(Nbt::Int, temper));
        if (owner_hi || owner_lo) o.set("Owner", uuid_nbt(owner_hi, owner_lo));
    }
    void load_extra(const Nbt& in) override {
        Animal::load_extra(in);
        set_hflag(2, nbt_num(in, "Tame", 0) != 0);
        temper = int(nbt_num(in, "Temper", 0));
        uuid_from_nbt(in.get("Owner"), owner_hi, owner_lo);
    }
};
struct ChestedHorse : AbstractHorse {  // donkey, mule, llama: a chest on a tamed adult
    bool has_chest() const { return data.get(edata::AbstractChestedHorse::ID_CHEST, 0); }
    int extra_interact(Player& p, int hand) override {
        ItemStack& s = held_stack(p, hand);
        if (s.item == item_by_name["minecraft:chest"] && tamed() && !has_chest()) {
            data.set_bool(edata::AbstractChestedHorse::ID_CHEST, true);
            use_player_item(p, hand);
            play_sound(entity_sound(*this, "chest"), 1.0f, (rand_float() - rand_float()) * 0.2f + 1.0f);
            return IR_SUCCESS;
        }
        return IR_PASS;
    }
    void save_extra(Nbt& o) const override { AbstractHorse::save_extra(o); o.set("ChestedHorse", Nbt::num(Nbt::Byte, has_chest())); }
    void load_extra(const Nbt& in) override { AbstractHorse::load_extra(in); data.set_bool(edata::AbstractChestedHorse::ID_CHEST, nbt_num(in, "ChestedHorse", 0) != 0); }
};
struct Horse : AbstractHorse {
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override {
        data.set_int(edata::Horse::ID_TYPE_VARIANT, rand_int(7) | rand_int(5) << 8);  // Variant | Markings << 8
        // Horse.randomizeAttributes: max health 15..30, jump 0.4..1.0, speed 0.1125..0.3375
        set_attr_base(attr::max_health, double(15.0f + float(rand_int(8)) + float(rand_int(9))));
        set_attr_base(attr::jump_strength, 0.4 + rand_double() * 0.2 + rand_double() * 0.2 + rand_double() * 0.2);
        set_attr_base(attr::movement_speed, (0.45 + rand_double() * 0.3 + rand_double() * 0.3 + rand_double() * 0.3) * 0.25);
        set_health(max_health());
        return AbstractHorse::finalize_spawn(r, g);
    }
};
struct Llama : ChestedHorse {
    int max_temper() const override { return 30; }
    bool can_wear_saddle() const override { return false; }
    bool is_food(const ItemStack& s) const override { return item_in_tag(s.item, "llama_food"); }
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override {
        data.set_int(edata::Llama::VARIANT, rand_int(4));
        data.set_int(edata::Llama::STRENGTH, 1 + rand_int(rand_float() < 0.04f ? 5 : 3));
        return ChestedHorse::finalize_spawn(r, g);
    }
};
struct UndeadHorse : AbstractHorse {  // skeleton / zombie horse: no breeding
    bool is_food(const ItemStack&) const override { return false; }
    bool can_mate(Animal&) override { return false; }
};

// The rider's client sends the vehicle position while it controls a tamed, saddled mount.
inline void handle_move_vehicle(Player& p, Reader& in) {
    double x = in.f64(), y = in.f64(), z = in.f64();
    float yr = in.f32(), xr = in.f32();
    in.u8();  // on ground
    PlayerEntity* pe = player_entity(p);
    if (!pe) return;
    AbstractHorse* h = nullptr;
    for_each_entity_in(pe->bb.inflate(4.0), [&](Entity& e) {
        if (auto* a = dynamic_cast<AbstractHorse*>(&e); a && a->rider == pe->id) h = a;
    }, false);
    if (!h || !h->controlled()) return;
    double dx = x - h->pos.x, dy = y - h->pos.y, dz = z - h->pos.z;
    if (dx * dx + dy * dy + dz * dz > 100.0) return;  // ponytail: vanilla's moved-too-quickly / collision checks
    h->set_pos(x, y, z);
    h->yaw = yr, h->pitch = xr, h->y_body_rot = h->y_head_rot = yr;
    p.x = x, p.y = y + h->height * 0.75, p.z = z;
    int ncx = int(std::floor(p.x)) >> 4, ncz = int(std::floor(p.z)) >> 4;
    if (ncx != p.cx || ncz != p.cz) update_view(p, ncx, ncz, false);
}
