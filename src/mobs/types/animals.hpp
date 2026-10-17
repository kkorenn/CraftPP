#pragma once
// Cow, Pig, Sheep, Chicken (net.minecraft.world.entity.animal.*).
#include "../ai/goals.hpp"

inline int biome_at_pos(int x, int y, int z) {
    Chunk* c = loaded_chunk(x >> 4, z >> 4);
    if (!c || y < c->min_y || y >= c->min_y + c->height()) return 0;
    int qy = (y - c->min_y) >> 2;
    return c->get_biome((x & 15) >> 2, qy, (z & 15) >> 2);
}
// Farm animal variants (registry order: cold 0, temperate 1, warm 2) chosen by the spawn biome tags.
inline int farm_variant_at(int x, int y, int z) {
    int b = biome_at_pos(x, y, z);
    if (biome_in_tag(b, "spawns_warm_variant_farm_animals")) return 2;
    if (biome_in_tag(b, "spawns_cold_variant_farm_animals")) return 0;
    return 1;
}
inline bool held_matches(Player& p, int hand, const char* tag) { return item_in_tag(p.inv[hand ? 40u : size_t(p.selected)].item, tag); }
inline ItemStack& held_stack(Player& p, int hand) { return p.inv[hand ? 40u : size_t(p.selected)]; }

struct FarmAnimal : Animal {  // shared: variant + sound variant slots, food tag, sound prefix
    virtual const char* food_tag() const = 0;
    virtual const char* sound_name() const = 0;
    virtual int variant_slot() const = 0;
    virtual int variant_ser() const = 0;
    bool is_food(const ItemStack& s) const override { return item_in_tag(s.item, food_tag()); }
    std::string ambient_sound() const override { return std::string("minecraft:entity.") + sound_name() + ".ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return std::string("minecraft:entity.") + sound_name() + ".hurt"; }
    std::string death_sound() const override { return std::string("minecraft:entity.") + sound_name() + ".death"; }
    int variant() const { return int(data.get(variant_slot(), 1)); }
    void set_variant(int v) { data.set(variant_slot(), variant_ser(), v); }
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override {
        set_variant(farm_variant_at(block_x(), block_y(), block_z()));
        return Animal::finalize_spawn(r, g);
    }
    std::string loot_prop(const std::string& k) const override {
        static const char* names[] = {"minecraft:cold", "minecraft:temperate", "minecraft:warm"};
        if (k.find("/variant") != std::string::npos) return names[std::clamp(variant(), 0, 2)];
        return "";
    }
    void save_extra(Nbt& o) const override {
        Animal::save_extra(o);
        static const char* names[] = {"minecraft:cold", "minecraft:temperate", "minecraft:warm"};
        o.set("variant", Nbt::str(names[std::clamp(variant(), 0, 2)]));
    }
    void load_extra(const Nbt& in) override {
        Animal::load_extra(in);
        if (const Nbt* v = in.get("variant"); v && v->t == Nbt::String)
            set_variant(v->s.find("cold") != std::string::npos ? 0 : v->s.find("warm") != std::string::npos ? 2 : 1);
    }
};

struct Cow : FarmAnimal {
    const char* food_tag() const override { return "cow_food"; }
    const char* sound_name() const override { return "cow"; }
    int variant_slot() const override { return edata::Cow::VARIANT; }
    int variant_ser() const override { return edser::COW_VARIANT; }
    float sound_volume() const override { return 0.4f; }
    void register_goals() override {  // AbstractCow.registerGoals
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        goal_selector.add(1, std::make_unique<PanicGoal>(*this, 2.0));
        goal_selector.add(2, std::make_unique<BreedGoal>(*this, 1.0));
        goal_selector.add(3, std::make_unique<TemptGoal>(*this, 1.25, [](const ItemStack& s) { return item_in_tag(s.item, "cow_food"); }, false));
        goal_selector.add(4, std::make_unique<FollowParentGoal>(*this, 1.25));
        goal_selector.add(5, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
        goal_selector.add(6, std::make_unique<LookAtPlayerGoal>(*this, 6.0f));
        goal_selector.add(7, std::make_unique<RandomLookAroundGoal>(*this));
    }
    int mob_interact(Player& p, int hand) override {  // milking
        ItemStack& s = held_stack(p, hand);
        if (s.item == item_by_name["minecraft:bucket"] && !is_baby()) {
            ::play_sound("minecraft:entity.cow.milk", SRC_PLAYERS, p.x, p.y, p.z);
            int milk = item_by_name["minecraft:milk_bucket"];
            if (p.gamemode == 1) { if (count_item(p, milk) == 0) give_item(p, milk, 1); }
            else if (s.count == 1) s = {milk, 1, 0};
            else { s.count--; give_item(p, milk, 1); }
            return IR_SUCCESS;
        }
        return FarmAnimal::mob_interact(p, hand);
    }
};
struct Pig : FarmAnimal {
    const char* food_tag() const override { return "pig_food"; }
    const char* sound_name() const override { return "pig"; }
    int variant_slot() const override { return edata::Pig::VARIANT; }
    int variant_ser() const override { return edser::PIG_VARIANT; }
    void register_goals() override {
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        goal_selector.add(1, std::make_unique<PanicGoal>(*this, 1.25));
        goal_selector.add(3, std::make_unique<BreedGoal>(*this, 1.0));
        int stick = item_by_name["minecraft:carrot_on_a_stick"];
        goal_selector.add(4, std::make_unique<TemptGoal>(*this, 1.2, [stick](const ItemStack& s) { return s.item == stick; }, false));
        goal_selector.add(4, std::make_unique<TemptGoal>(*this, 1.2, [](const ItemStack& s) { return item_in_tag(s.item, "pig_food"); }, false));
        goal_selector.add(5, std::make_unique<FollowParentGoal>(*this, 1.1));
        goal_selector.add(6, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
        goal_selector.add(7, std::make_unique<LookAtPlayerGoal>(*this, 6.0f));
        goal_selector.add(8, std::make_unique<RandomLookAroundGoal>(*this));
    }
    void thunder_hit(Entity* lightning) override {
        if (difficulty() == 0) return;
        auto e = create_entity(entity_types::zombified_piglin);
        if (e) {
            e->snap_to(pos.x, pos.y, pos.z, yaw, pitch);
            if (auto* m = dynamic_cast<Mob*>(e.get())) {
                m->finalize_spawn(SpawnReason::CONVERSION, nullptr);
            }
            if (auto* a = dynamic_cast<AgeableMob*>(e.get())) {
                if (is_baby()) a->set_baby(true);
            }
            remove(Removal::DISCARDED);
            add_entity(std::move(e));
        } else {
            Entity::thunder_hit(lightning);
        }
    }
};
struct Chicken : FarmAnimal {
    int egg_time = 0;
    const char* food_tag() const override { return "chicken_food"; }
    const char* sound_name() const override { return "chicken"; }
    int variant_slot() const override { return edata::Chicken::VARIANT; }
    int variant_ser() const override { return edser::CHICKEN_VARIANT; }
    void init() override {
        FarmAnimal::init();
        egg_time = rand_int(6000) + 6000;
        set_pathfinding_malus(PT_WATER, 0.0f);
    }
    void register_goals() override {
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        goal_selector.add(1, std::make_unique<PanicGoal>(*this, 1.4));
        goal_selector.add(2, std::make_unique<BreedGoal>(*this, 1.0));
        goal_selector.add(3, std::make_unique<TemptGoal>(*this, 1.0, [](const ItemStack& s) { return item_in_tag(s.item, "chicken_food"); }, false));
        goal_selector.add(4, std::make_unique<FollowParentGoal>(*this, 1.1));
        goal_selector.add(5, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
        goal_selector.add(6, std::make_unique<LookAtPlayerGoal>(*this, 6.0f));
        goal_selector.add(7, std::make_unique<RandomLookAroundGoal>(*this));
    }
    bool cause_fall_damage(double, float, const DamageSource&) override { return false; }  // chickens flap down
    void ai_step() override {
        FarmAnimal::ai_step();
        if (!on_ground && vel.y < 0.0) vel.y *= 0.6;
        if (is_alive() && !is_baby() && --egg_time <= 0) {  // dropFromGiftLootTable(CHICKEN_LAY)
            auto it = entity_loot_index.find("gameplay/chicken_lay");
            if (it != entity_loot_index.end()) {
                LootContext ctx;
                ctx.this_entity = this;
                std::vector<ItemStack> out;
                roll_loot(loot_tables[size_t(it->second)], ctx, out);
                for (auto& s : out) spawn_at_location(s);
                if (!out.empty()) play_sound("minecraft:entity.chicken.egg", 1.0f, (rand_float() - rand_float()) * 0.2f + 1.0f);
            }
            egg_time = rand_int(6000) + 6000;
        }
    }
    void save_extra(Nbt& o) const override { FarmAnimal::save_extra(o); o.set("EggLayTime", Nbt::num(Nbt::Int, egg_time)); }
    void load_extra(const Nbt& in) override { FarmAnimal::load_extra(in); egg_time = int(nbt_num(in, "EggLayTime", egg_time)); }
};

inline const char* DYE_NAMES[16] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                    "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
struct Sheep : Animal {
    EatBlockGoal* eat_goal = nullptr;
    bool is_food(const ItemStack& s) const override { return item_in_tag(s.item, "sheep_food"); }
    std::string ambient_sound() const override { return "minecraft:entity.sheep.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.sheep.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.sheep.death"; }
    int color() const { return int(data.get(edata::Sheep::WOOL, 0)) & 15; }
    void set_color(int c) { data.set_byte(edata::Sheep::WOOL, (int(data.get(edata::Sheep::WOOL, 0)) & 0xF0) | (c & 15)); }
    bool sheared() const { return int(data.get(edata::Sheep::WOOL, 0)) & 16; }
    void set_sheared(bool v) { int c = int(data.get(edata::Sheep::WOOL, 0)); data.set_byte(edata::Sheep::WOOL, v ? c | 16 : c & ~16); }
    void register_goals() override {
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        goal_selector.add(1, std::make_unique<PanicGoal>(*this, 1.25));
        goal_selector.add(2, std::make_unique<BreedGoal>(*this, 1.0));
        goal_selector.add(3, std::make_unique<TemptGoal>(*this, 1.1, [](const ItemStack& s) { return item_in_tag(s.item, "sheep_food"); }, false));
        goal_selector.add(4, std::make_unique<FollowParentGoal>(*this, 1.1));
        auto eat = std::make_unique<EatBlockGoal>(*this);
        eat->ate = [this] { ate(); };
        eat_goal = goal_selector.add(5, std::move(eat));
        goal_selector.add(6, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 1.0));
        goal_selector.add(7, std::make_unique<LookAtPlayerGoal>(*this, 6.0f));
        goal_selector.add(8, std::make_unique<RandomLookAroundGoal>(*this));
    }
    void ate() {  // Sheep.ate
        set_sheared(false);
        if (can_age_up()) age_up(60);
    }
    bool ready_for_shearing() const { return !sheared() && !is_baby(); }
    void shear() {  // Sheep.shear: shearing/sheep table, each wool item thrown separately
        play_sound("minecraft:entity.sheep.shear");
        auto it = entity_loot_index.find("shearing/sheep");
        if (it != entity_loot_index.end()) {
            LootContext ctx;
            ctx.this_entity = this;
            std::vector<ItemStack> out;
            roll_loot(loot_tables[size_t(it->second)], ctx, out);
            for (auto& s : out)
                for (int i = 0; i < s.count; i++) {
                    auto& e = spawn_item({s.item, 1, s.damage}, pos.x, pos.y + 1.0, pos.z);
                    e.pickup_delay = 10;
                    e.vel = e.vel + Vec3{(rand_float() - rand_float()) * 0.1f, rand_float() * 0.05f, (rand_float() - rand_float()) * 0.1f};
                }
        }
        set_sheared(true);
    }
    int mob_interact(Player& p, int hand) override {
        ItemStack& s = held_stack(p, hand);
        if (s.item == item_by_name["minecraft:shears"]) {
            if (!ready_for_shearing()) return IR_CONSUME;
            shear();
            if (p.gamemode != 1) {  // hurtAndBreak(1)
                if (++s.damage >= item_info[size_t(s.item)].max_damage) s = {};
            }
            return IR_SUCCESS;
        }
        return Animal::mob_interact(p, hand);
    }
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override {  // SheepColorSpawnRules
        int b = biome_at_pos(block_x(), block_y(), block_z());
        int def = 0, a = 15, b2 = 7, c = 8, d = 12;  // temperate: black gray light_gray brown + common white
        if (biome_in_tag(b, "spawns_warm_variant_farm_animals")) def = 12, a = 7, b2 = 8, c = 0, d = 15;
        else if (biome_in_tag(b, "spawns_cold_variant_farm_animals")) def = 15, a = 8, b2 = 7, c = 0, d = 12;
        int roll = rand_int(100);
        int col = roll < 5 ? a : roll < 10 ? b2 : roll < 15 ? c : roll < 18 ? d : (rand_int(500) == 0 ? 6 : def);
        set_color(col);
        return Animal::finalize_spawn(r, g);
    }
    std::unique_ptr<Entity> breed_offspring(AgeableMob& partner) override {  // DyeColor.getMixedColor (ponytail: crafting-based mix: picks a parent)
        auto e = create_entity(type);
        if (auto* s = dynamic_cast<Sheep*>(e.get())) s->set_color(rand_bool() ? color() : static_cast<Sheep&>(partner).color());
        return e;
    }
    int loot_table() const override {  // entities/sheep/<color> when not sheared, else entities/sheep
        auto it = entity_loot_index.find("entities/sheep");
        return it == entity_loot_index.end() ? -1 : it->second;
    }
    std::string loot_prop(const std::string& k) const override {
        if (k == "sheep/color") return DYE_NAMES[color()];
        if (k == "sheep/sheared") return sheared() ? "true" : "false";
        return "";
    }
    void save_extra(Nbt& o) const override {
        Animal::save_extra(o);
        o.set("Sheared", Nbt::num(Nbt::Byte, sheared()));
        o.set("Color", Nbt::num(Nbt::Byte, color()));
    }
    void load_extra(const Nbt& in) override {
        Animal::load_extra(in);
        set_sheared(nbt_num(in, "Sheared", 0) != 0);
        set_color(int(nbt_num(in, "Color", 0)));
    }
};
