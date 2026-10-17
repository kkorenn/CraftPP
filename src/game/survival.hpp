#pragma once
// Survival: block breaking (hardness/tools), placement cost, vitals (health/food/exhaustion), fall + void
// damage, death and respawn, eating, item drops (Q). Tick thread only.
//
// API: hurt(p, amount, "fall"|"out_of_world"|"starve"|"generic"...) -> bool, heal(p, amount),
//      add_exhaustion(p, amount), kill(p, damage_type), respawn(p).
#include "inventory.hpp"
#include "loot.hpp"
#include "../mobs/api.hpp"

#include <unistd.h>

// ---------- NBT text: {translate:key, with:[{text:arg}...]} ----------
inline void nbt_str(Writer& w, const std::string& s) { w.u8(uint8_t(s.size() >> 8)).u8(uint8_t(s.size())); w.bytes(s.data(), s.size()); }
inline Writer& translate_component(Writer& w, const std::string& key, std::initializer_list<std::string> args) {
    w.u8(0x0A);
    w.u8(0x08); nbt_str(w, "translate"); nbt_str(w, key);
    if (args.size()) {
        w.u8(0x09); nbt_str(w, "with"); w.u8(0x0A).i32(int32_t(args.size()));
        for (auto& a : args) { w.u8(0x08); nbt_str(w, "text"); nbt_str(w, a); w.u8(0); }
    }
    return w.u8(0);
}

inline int difficulty() { return world_difficulty; }  // 0 peaceful .. 3 hard (commands/state.hpp, /difficulty)
inline bool survival_like(const Player& p) { return p.gamemode == 0 || p.gamemode == 2; }

// ---------- vitals ----------
inline void send_health(Player& p) {
    p.send(Writer().varint(play_cb::set_health).f32(p.g.health).varint(p.g.food).f32(p.g.saturation));
    p.g.sent_health = p.g.health, p.g.sent_food = p.g.food, p.g.sent_sat_zero = p.g.saturation == 0;
}
inline void send_experience(Player& p) {
    p.send(Writer().varint(play_cb::set_experience).f32(p.g.xp_progress).varint(p.g.xp_level).varint(p.g.xp_total));
}
inline void add_exhaustion(Player& p, float a) {
    if (survival_like(p)) p.g.exhaustion = std::min(p.g.exhaustion + a, 40.0f);
}
inline void heal(Player& p, float a) {
    if (!p.g.dead && a > 0) p.g.health = std::min(p.g.health + a, 20.0f);
}

inline void kill(Player& p, const std::string& type);
inline void kill(Player& p, const DamageSource& s);

// LivingEntity.getDamageAfterArmorAbsorb: Player.hurtArmor (durability) then CombatRules.getDamageAfterAbsorb.
inline float armor_absorb(Player& p, float damage) {
    float armor = 0, toughness = 0;
    if (damage > 0) {
        int wear = int(std::max(1.0f, damage / 4.0f));
        for (int slot : {36, 37, 38, 39}) {  // FEET, LEGS, CHEST, HEAD
            ItemStack& s = p.inv[size_t(slot)];
            if (s.empty()) continue;
            const ItemInfo& it = item_info[size_t(s.item)];
            if (it.equip != slot || !it.damage_on_hurt || it.max_damage <= 0) continue;
            if ((s.damage += wear) >= it.max_damage) {
                s = {};
                broadcast_near(p.x, p.z, entity_event_packet(p.eid, 52 - (slot - 36)));  // FEET 52 .. HEAD 49
            }
        }
    }
    for (int slot : {36, 37, 38, 39}) {
        const ItemStack& s = p.inv[size_t(slot)];
        if (s.empty() || item_info[size_t(s.item)].equip != slot) continue;
        armor += item_info[size_t(s.item)].armor, toughness += item_info[size_t(s.item)].toughness;
    }
    armor = std::clamp(armor, 0.0f, 30.0f), toughness = std::clamp(toughness, 0.0f, 20.0f);
    float t = 2.0f + toughness / 4.0f;
    float real = std::clamp(armor - damage / t, armor * 0.2f, 20.0f);
    return damage * (1.0f - real / 25.0f);
}

// Player.hurtServer + LivingEntity.hurtServer/actuallyHurt for players: difficulty scaling, invulnerability frames,
// armour/toughness (player_damage_after_armor, src/mobs), absorption-free health loss. Returns whether damage applied.
inline bool hurt(Player& p, const DamageSource& s, float amount) {
    if (p.g.dead || amount <= 0) return false;
    if (!survival_like(p) && !s.is(DT_BYPASSES_INVULNERABILITY)) return false;
    if ((s.is(DT_IS_DROWNING) && !rule("drowning_damage")) || (s.is(DT_IS_FALL) && !rule("fall_damage")) ||
        (s.is(DT_IS_FIRE) && !rule("fire_damage")) || (s.is(DT_IS_FREEZING) && !rule("freeze_damage")))
        return false;
    if (s.scales_with_difficulty()) {
        int d = difficulty();
        if (d == 0) amount = 0;
        else if (d == 1) amount = std::min(amount / 2.0f + 1.0f, amount);
        else if (d == 3) amount = amount * 3.0f / 2.0f;
        if (amount == 0) return false;
    }
    float applied;
    if (p.g.invulnerable > 10 && !s.is(DT_BYPASSES_COOLDOWN)) {
        if (amount <= p.g.last_hurt) return false;
        applied = amount - p.g.last_hurt;
        p.g.last_hurt = amount;
    } else {
        p.g.last_hurt = amount;
        p.g.invulnerable = 20;
        applied = amount;
    }
    applied = player_damage_after_armor(p, s, applied);
    // ponytail: enchantment protection, resistance and absorption are not applied yet.
    auto dt = damage_types.find(s.name());
    if (dt != damage_types.end()) add_exhaustion(p, dt->second.exhaustion);
    p.g.health = std::max(0.0f, p.g.health - applied);
    Writer w;
    w.varint(play_cb::damage_event).varint(p.eid).varint(s.type).varint(s.causing ? s.causing->id + 1 : 0)
        .varint(s.direct ? s.direct->id + 1 : 0);
    if (s.source_pos) w.boolean(true).f64(s.source_pos->x).f64(s.source_pos->y).f64(s.source_pos->z);
    else w.boolean(false);
    broadcast_near(p.x, p.z, w);
    if (p.g.invulnerable == 20 && !s.is(DT_NO_KNOCKBACK)) player_knockback_from(p, s);
    if (p.g.health <= 0) kill(p, s);
    return true;
}
inline bool hurt(Player& p, float amount, const std::string& type) { return hurt(p, DamageSource::of(type), amount); }

inline void drop_all(Player& p) {  // Inventory.dropAll (keepInventory off)
    for (auto& s : p.inv)
        if (!s.empty()) { player_drop(p, s, true); s = {}; }
    for (auto* s : {&p.g.carried}) if (!s->empty()) { player_drop(p, *s, true); *s = {}; }
    for (auto& s : p.g.craft2) if (!s.empty()) { player_drop(p, s, true); s = {}; }
    for (auto& s : p.g.craft3) if (!s.empty()) { player_drop(p, s, true); s = {}; }
    p.g.result = {};
}

inline void kill(Player& p, const std::string& type) { kill(p, DamageSource::of(type)); }
// Killer name for death messages: a player's name, a mob's custom name or its type translation key.
inline Writer& death_component(Writer& w, const std::string& key, const Player& p, const Entity* killer) {
    w.u8(0x0A);
    w.u8(0x08); nbt_str(w, "translate"); nbt_str(w, key);
    w.u8(0x09); nbt_str(w, "with"); w.u8(0x0A).i32(killer ? 2 : 1);
    w.u8(0x08); nbt_str(w, "text"); nbt_str(w, p.name); w.u8(0);
    if (killer) {
        if (const Player* kp = killer->as_player_const()) { w.u8(0x08); nbt_str(w, "text"); nbt_str(w, kp->name); }
        else if (killer->has_custom_name()) { w.u8(0x08); nbt_str(w, "text"); nbt_str(w, killer->custom_name); }
        else { w.u8(0x08); nbt_str(w, "translate"); nbt_str(w, "entity.minecraft." + killer->info().id); }
        w.u8(0);
    }
    return w.u8(0);
}
inline void kill(Player& p, const DamageSource& s) {
    if (p.g.dead) return;
    p.g.dead = true;
    p.g.health = 0;
    p.g.use_ticks = 0;
    p.g.digging = p.g.delayed = false;
    auto dt = damage_types.find(s.name());
    std::string key = "death.attack.generic";
    // DamageSource.getLocalizedDeathMessage: the killer (causing, else direct entity) is the second argument
    const Entity* killer = s.causing ? s.causing : s.direct;
    if (killer && killer->as_player_const() == &p) killer = nullptr;
    if (dt != damage_types.end()) {
        key = "death.attack." + dt->second.msg;
        if (dt->second.fall_variants && p.g.fall_distance > 5) key = "death.fell.accident.generic", killer = nullptr;
    }
    Writer msg;
    if (!rule("show_death_messages")) msg.u8(0x08).u8(0).u8(0);  // CommonComponents.EMPTY
    else if (killer) death_component(msg, key, p, killer);
    else translate_component(msg, key, {p.name});
    std::vector<uint8_t> component(msg.b);
    Writer k;
    k.varint(play_cb::player_combat_kill).varint(p.eid).bytes(component);
    p.send(k);
    if (rule("show_death_messages")) {
        Writer chat;
        chat.varint(play_cb::system_chat).bytes(component).boolean(false);
        for (auto& q : players) q->send(chat);
        log_info("%s", plain(tr(key, {p.name})).c_str());  // vanilla logs the death message like other system chat
    }
    broadcast_near(p.x, p.z, entity_event_packet(p.eid, 3), &p);
    if (p.g.container) close_container(p, true);
    if (rule("keep_inventory")) {
        return_grid(p);
    } else {
        drop_all(p);
        int xp = std::min(p.g.xp_level * 7, 100);  // Player.getBaseExperienceReward
        if (xp > 0 && survival_like(p)) spawn_xp_orbs(p.x, p.y, p.z, xp);
        p.g.xp_level = p.g.xp_total = 0, p.g.xp_progress = 0;
    }
    send_health(p);
}

// PlayerList.respawn at the world spawn (no beds/anchors yet).
inline void respawn(Player& p) {
    if (!p.g.dead) return;
    p.g = [&] {
        PlayerGame g;
        g.next_container = p.g.next_container;
        g.loaded = true;
        g.xp_level = p.g.xp_level, g.xp_total = p.g.xp_total, g.xp_progress = p.g.xp_progress;
        return g;
    }();
    World* died_in = p.level;
    place_new_player(p);  // overworld spawn
    World* spawn = p.level;
    p.level = died_in;
    move_to_level(p, spawn, p.x, p.y, p.z, p.yaw, p.pitch, 0);
}

// FoodData.tick + peaceful regeneration (Player.aiStep).
inline void food_tick(Player& p) {
    auto& g = p.g;
    int diff = difficulty();
    if (g.exhaustion > 4) {
        g.exhaustion -= 4;
        if (g.saturation > 0) g.saturation = std::max(g.saturation - 1, 0.0f);
        else if (diff != 0) g.food = std::max(g.food - 1, 0);
    }
    bool hurt_ = g.health > 0 && g.health < 20;
    bool regen = rule("natural_health_regeneration");
    if (regen && g.saturation > 0 && hurt_ && g.food >= 20) {
        if (++g.food_timer >= 10) {
            float spent = std::min(g.saturation, 6.0f);
            heal(p, spent / 6.0f);
            add_exhaustion(p, spent);
            g.food_timer = 0;
        }
    } else if (regen && g.food >= 18 && hurt_) {
        if (++g.food_timer >= 80) { heal(p, 1); add_exhaustion(p, 6); g.food_timer = 0; }
    } else if (g.food <= 0) {
        if (++g.food_timer >= 80) {
            if (g.health > 10 || diff == 3 || (g.health > 1 && diff == 2)) hurt(p, 1, "starve");
            g.food_timer = 0;
        }
    } else {
        g.food_timer = 0;
    }
    if (diff == 0 && regen) {
        if (g.game_ticks % 20 == 0) {
            if (g.health < 20) heal(p, 1);
            if (g.saturation < 20) g.saturation += 1;
        }
        if (g.food < 20 && g.game_ticks % 10 == 0) g.food++;
    }
}

// ---------- block breaking (ServerPlayerGameMode) ----------
inline ItemStack& held(Player& p) { return p.inv[size_t(p.selected)]; }

inline bool eye_in_water(const Player& p) {
    int b = world.get_block(int(std::floor(p.x)), int(std::floor(p.y + 1.62)), int(std::floor(p.z)));
    return block_index(b) == block_index(blocks::water);
}
inline bool correct_tool(Player& p, int state) {  // Player.hasCorrectToolForDrops
    return !block_of(state).requires_tool || tool_correct(held(p).item, state);
}
// BlockBehaviour.getDestroyProgress: fraction of the block broken per tick.
inline float destroy_progress(Player& p, int state) {
    float h = block_of(state).hardness;
    if (h < 0) return 0;
    float speed = tool_speed(held(p).item, state);
    if (eye_in_water(p)) speed *= 0.2f;
    if (!p.on_ground) speed /= 5.0f;
    if (h == 0) return 1.0f;
    return speed / h / (correct_tool(p, state) ? 30.0f : 100.0f);
}

inline void hurt_tool(Player& p, int amount) {  // ItemStack.hurtAndBreak (no unbreaking yet)
    ItemStack& t = held(p);
    if (t.empty() || !survival_like(p)) return;
    int maxd = item_info[size_t(t.item)].max_damage;
    if (maxd <= 0) return;
    if ((t.damage += amount) >= maxd) {
        t = {};
        broadcast_near(p.x, p.z, entity_event_packet(p.eid, 47));  // main hand break animation + sound
    }
}

// Level.levelEvent(source, type, pos, data): players within 64 blocks, except the source.
inline void level_event(int type, int x, int y, int z, int data, const Player* except) {
    Writer w;
    w.varint(play_cb::level_event).i32(type).i64(int64_t(pack_pos(x, y, z))).i32(data).boolean(false);
    for (auto& q : players) {
        double dx = x - q->x, dy = y - q->y, dz = z - q->z;
        if (q.get() != except && dx * dx + dy * dy + dz * dz < 64.0 * 64.0) q->send(w);
    }
}
// ServerLevel.destroyBlockProgress: crack stage for everyone within 32 blocks but the breaker.
inline void destroy_block_progress(const Player& p, int x, int y, int z, int stage) {
    Writer w;
    w.varint(play_cb::block_destruction).varint(p.eid).i64(int64_t(pack_pos(x, y, z))).u8(uint8_t(stage));
    for (auto& q : players) {
        double dx = x - q->x, dy = y - q->y, dz = z - q->z;
        if (q->eid != p.eid && dx * dx + dy * dy + dz * dz < 1024.0) q->send(w);
    }
}

namespace lvl {
inline bool player_destroy(Player& p, int x, int y, int z, int state);  // level/glue.hpp: playerWillDestroy + removeBlock
inline int use_item_on(Player& pl, int hand, int x, int y, int z, int face, float cx, float cy, float cz);  // level/placement.hpp
inline bool use_bucket(Player& pl, ItemStack& slot);
}
// ServerPlayerGameMode.destroyBlock: remove the block, drop loot when harvestable, wear the tool.
// Returns false when the held item can't break blocks (swords etc. in creative).
inline bool destroy_block(Player& p, int x, int y, int z) {
    int state = world.get_block(x, y, z);
    int item = held(p).item;
    if (p.gamemode == 1 && item >= 0 && item_info[size_t(item)].tool >= 0 &&
        !tools[size_t(item_info[size_t(item)].tool)].creative_destroy)
        return false;
    if (state == blocks::air) return true;
    level_event(2001, x, y, z, state, &p);  // Block.spawnDestroyParticles (break particles + sound for others)
    if (!lvl::player_destroy(p, x, y, z, state)) return true;  // other halves of doors/beds/plants, leaves fluids
    if (p.gamemode == 1) return true;
    if (correct_tool(p, state)) {
        static std::vector<ItemStack> drops;
        drops.clear();
        block_drops(state, x, y, z, held(p), drops);
        for (auto& d : drops) pop_resource(x, y, z, d);
    }
    if (block_of(state).hardness != 0) {
        if (item >= 0 && item_info[size_t(item)].tool >= 0) hurt_tool(p, tools[size_t(item_info[size_t(item)].tool)].damage_per_block);
    }
    add_exhaustion(p, 0.005f);
    return true;
}
inline void destroy_and_ack(Player& p, int x, int y, int z) {
    if (!destroy_block(p, x, y, z)) p.send(block_update(x, y, z, world.get_block(x, y, z)));
}

// Player.isWithinBlockInteractionRange(pos, 1.0): eye to the block's box, range 4.5 (creative 5) + 1.
inline bool within_reach(const Player& p, int x, int y, int z) {
    double ex = p.x, ey = p.y + 1.62, ez = p.z;
    double dx = std::max({x - ex, 0.0, ex - (x + 1)}), dy = std::max({y - ey, 0.0, ey - (y + 1)}),
           dz = std::max({z - ez, 0.0, ez - (z + 1)});
    double r = (p.gamemode == 1 ? 5.0 : 4.5) + 1.0;
    return dx * dx + dy * dy + dz * dz < r * r;
}

// DedicatedServer.isUnderSpawnProtection (overworld only, non-ops, only once there are ops).
inline bool under_spawn_protection(const Player& p, int x, int z) {
    int radius = std::atoi(props["spawn-protection"].c_str());
    if (radius <= 0) return false;
    {
        std::lock_guard lock(cmd_mtx);
        if (ops.rows.empty() || ops.find(player_uuid(p), p.name)) return false;
    }
    return std::max(std::abs(x - world_spawn.x), std::abs(z - world_spawn.z)) <= radius;
}
inline void overlay_message(Player& p, const Text& t) { send_text(p, styled(t, "red"), true); }
inline std::string short_pos(int x, int y, int z) {  // BlockPos.toShortString
    return std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z);
}

// ServerPlayerGameMode.handleBlockBreakAction. Actions: 0 start, 1 change direction, 2 abort, 3 stop.
inline void dig_action(Player& p, int action, int x, int y, int z) {
    auto& g = p.g;
    if (p.gamemode == 3 || g.dead) return;
    auto abort = [&] {
        g.digging = false;
        if (g.dig_x != x || g.dig_y != y || g.dig_z != z) {
            log_warn("Mismatch in destroy block pos: BlockPos{x=%d, y=%d, z=%d} BlockPos{x=%d, y=%d, z=%d}", g.dig_x,
                     g.dig_y, g.dig_z, x, y, z);
            destroy_block_progress(p, g.dig_x, g.dig_y, g.dig_z, -1);
        }
        destroy_block_progress(p, x, y, z, -1);
    };
    if (!within_reach(p, x, y, z)) {
        if (g.digging && action == 2) abort();
        return;
    }
    int state = world.get_block(x, y, z);
    if (y > world.max_y() - 1) return p.send(block_update(x, y, z, state));
    if (action == 0) {  // START_DESTROY_BLOCK
        if (under_spawn_protection(p, x, z)) return overlay_message(p, tr("build.spawn_protection", {short_pos(x, y, z)}));
        if (p.gamemode == 1) return destroy_and_ack(p, x, y, z);
        if (p.gamemode == 2) return p.send(block_update(x, y, z, state));  // ponytail: no can_break predicate
        g.dig_start = g.game_ticks;
        float prog = state == blocks::air ? 1.0f : destroy_progress(p, state);
        if (state != blocks::air && prog >= 1.0f) return destroy_and_ack(p, x, y, z);
        if (g.digging) p.send(block_update(g.dig_x, g.dig_y, g.dig_z, world.get_block(g.dig_x, g.dig_y, g.dig_z)));
        g.digging = true;
        g.dig_x = x, g.dig_y = y, g.dig_z = z, g.dig_state = state;
        p.net.last_dig_state = int(prog * 10.0f);
        destroy_block_progress(p, x, y, z, p.net.last_dig_state);
    } else if (action == 3) {  // STOP_DESTROY_BLOCK
        if (!g.digging || g.dig_x != x || g.dig_y != y || g.dig_z != z) return;
        if (state == blocks::air) return;
        float prog = destroy_progress(p, state) * float(g.game_ticks - g.dig_start + 1);
        if (prog >= 0.7f) {
            g.digging = false;
            destroy_block_progress(p, x, y, z, -1);
            return destroy_and_ack(p, x, y, z);
        }
        if (!g.delayed) {  // client was a bit fast: finish when the server-side progress reaches 1
            g.digging = false;
            g.delayed = true;
            g.dly_x = x, g.dly_y = y, g.dly_z = z, g.dly_start = g.dig_start;
        }
    } else if (action == 2) {  // ABORT_DESTROY_BLOCK
        abort();
    }
}
// ServerPlayerGameMode.tick
inline void dig_tick(Player& p) {
    auto& g = p.g;
    g.game_ticks++;
    auto progress = [&](int x, int y, int z, int state, int ticks) {  // incrementDestroyProgress
        float prog = destroy_progress(p, state) * float(ticks + 1);
        int stage = int(prog * 10.0f);
        if (stage != p.net.last_dig_state) destroy_block_progress(p, x, y, z, stage), p.net.last_dig_state = stage;
        return prog;
    };
    if (g.delayed) {  // vanilla passes delayedTickStart itself as the tick count (bytecode-checked)
        int state = world.get_block(g.dly_x, g.dly_y, g.dly_z);
        if (state == blocks::air) {
            g.delayed = false;
        } else if (progress(g.dly_x, g.dly_y, g.dly_z, state, g.dly_start) >= 1.0f) {
            g.delayed = false;
            destroy_block(p, g.dly_x, g.dly_y, g.dly_z);
        }
    } else if (g.digging) {
        int state = world.get_block(g.dig_x, g.dig_y, g.dig_z);
        if (state == blocks::air) {
            destroy_block_progress(p, g.dig_x, g.dig_y, g.dig_z, -1);
            p.net.last_dig_state = -1;
            g.digging = false;
        } else {
            int ticks = g.game_ticks - g.dig_start;
            progress(g.dig_x, g.dig_y, g.dig_z, state, ticks);
            // ponytail: levelEvent 2019/2020 needs Direction.ordinal of the hit face; uses the top face (1).
            level_event(ticks % 4 == 0 ? 2020 : 2019, g.dig_x, g.dig_y, g.dig_z, 1, nullptr);
        }
    }
}

// ---------- eating (LivingEntity.startUsingItem / completeUsingItem) ----------
inline void start_using(Player& p, int hand) {
    ItemStack& s = p.inv[hand ? 40u : size_t(p.selected)];
    if (s.empty() || p.g.dead) return;
    const ItemInfo& it = item_info[size_t(s.item)];
    if (it.consume_seconds < 0) return;
    if (it.nutrition >= 0 && !(it.always_eat || p.g.food < 20 || p.gamemode == 1)) return;
    p.g.use_hand = hand;
    p.g.use_ticks = std::max(1, int(it.consume_seconds * 20));
}
inline void finish_using(Player& p) {
    ItemStack& s = p.inv[p.g.use_hand ? 40u : size_t(p.selected)];
    if (s.empty()) return;
    const ItemInfo& it = item_info[size_t(s.item)];
    if (it.nutrition >= 0) {  // FoodData.eat(FoodProperties)
        p.g.food = std::clamp(p.g.food + it.nutrition, 0, 20);
        p.g.saturation = std::clamp(p.g.saturation + it.saturation, 0.0f, float(p.g.food));
    }
    // ponytail: on_consume_effects (golden apple regen, milk clearing effects) need a mob effect system.
    p.send(entity_event_packet(p.eid, 9));  // finished using
    if (p.gamemode == 1) return;
    int rem = it.use_remainder;
    if (--s.count <= 0) s = rem >= 0 ? ItemStack{rem, 1, 0} : ItemStack{};
    else if (rem >= 0) give_item(p, rem, 1);
}
inline void use_tick(Player& p) {
    if (p.g.use_ticks <= 0) return;
    ItemStack& s = p.inv[p.g.use_hand ? 40u : size_t(p.selected)];
    if (s.empty()) { p.g.use_ticks = 0; return; }
    if (--p.g.use_ticks == 0) finish_using(p);
}

// ---------- movement: fall damage, jumps, sprinting exhaustion, void ----------
inline void after_move(Player& p, double ox, double oy, double oz, bool was_on_ground) {
    if (p.g.dead || !p.g.loaded) return;
    double dy = p.y - oy;
    int fx = int(std::floor(p.x)), fy = int(std::floor(p.y)), fz = int(std::floor(p.z));
    int feet = world.get_block(fx, fy, fz);
    int bi = block_index(feet);
    bool in_fluid = is_fluid(uint16_t(feet));
    if (in_fluid || in_sorted(tag_climbable, bi) || in_sorted(tag_fall_reset, bi) || p.gamemode == 1 || p.gamemode == 3) {
        p.g.fall_distance = 0;
    } else {
        if (dy < 0) p.g.fall_distance -= dy;
        if (p.on_ground) {
            if (p.g.fall_distance > 0) {
                // Block.fallOn of the block landed on (getOnPos, 0.2 below the feet): damage multiplier.
                // ponytail: pointed dripstone, farmland trampling and turtle eggs not handled.
                const std::string& on = block_of(world.get_block(fx, int(std::floor(p.y - 0.2)), fz)).name;
                float mult = on == "minecraft:hay_block" || on == "minecraft:honey_block" ? 0.2f
                           : on == "minecraft:slime_block" && !p.g.sneaking ? 0.0f : 1.0f;
                double dist = p.g.fall_distance;
                if ((on.size() > 4 && on.compare(on.size() - 4, 4, "_bed") == 0) || on == "minecraft:shelf_mushroom")
                    dist *= 1.0f - 0.5f;  // Properties.fallDistanceReduction(0.5F)
                int dmg = int(std::floor((dist + 1.0E-6 - 3.0) * mult));
                if (dmg > 0) hurt(p, float(dmg), "fall");
            }
            p.g.fall_distance = 0;
        }
    }
    if (dy > 0) p.g.fall_distance = 0;  // handlePlayerPositionChange: movedUpwards -> resetFallDistance
    if (was_on_ground && !p.on_ground && dy > 0) add_exhaustion(p, p.g.sprinting ? 0.2f : 0.05f);  // jumpFromGround
    // ServerPlayer.checkMovementStatistics (food exhaustion part)
    double dx = p.x - ox, dz = p.z - oz;
    if (dx * dx + dy * dy + dz * dz < 1.0E-10) return;  // didNotMove
    auto cm = [](double d) { return int(std::floor(float(std::sqrt(d)) * 100.0f + 0.5f)); };  // Math.round(float)
    bool eye_water = eye_in_water(p), feet_water = block_index(feet) == block_index(blocks::water);
    if (eye_water) {  // isSwimming or isEyeInFluid(WATER): same rate
        if (int d = cm(dx * dx + dy * dy + dz * dz); d > 0) add_exhaustion(p, 0.01f * float(d) * 0.01f);
    } else if (feet_water) {
        if (int d = cm(dx * dx + dz * dz); d > 0) add_exhaustion(p, 0.01f * float(d) * 0.01f);
    } else if (!in_sorted(tag_climbable, bi) && p.on_ground && p.g.sprinting) {
        if (int d = cm(dx * dx + dz * dz); d > 0) add_exhaustion(p, 0.1f * float(d) * 0.01f);
    }
}

inline void vitals_tick(Player& p) {
    auto& g = p.g;
    if (g.invulnerable > 0) g.invulnerable--;
    if (g.dead) return;
    if (p.y < p.level->min_y - 64) hurt(p, 4, "out_of_world");
    if (survival_like(p)) food_tick(p);
    if (g.health != g.sent_health || g.food != g.sent_food || (g.saturation == 0) != g.sent_sat_zero) send_health(p);
    if (g.container) {  // CraftingMenu.stillValid: table still there and within 8 blocks
        double dx = p.x - (g.table_x + 0.5), dy = p.y - (g.table_y + 0.5), dz = p.z - (g.table_z + 0.5);
        if (block_index(world.get_block(g.table_x, g.table_y, g.table_z)) != block_index(blocks::crafting_table) ||
            dx * dx + dy * dy + dz * dz > 64)
            close_container(p, true);
    }
}

// ServerGamePacketListenerImpl.ackBlockChangesUpTo: one block_changed_ack per tick (game/netplay.hpp sends it).
inline void ack_block_changes(Player& p, int seq) {
    if (seq < 0) throw std::runtime_error("Expected packet sequence nr >= 0");
    p.net.ack_upto = std::max(p.net.ack_upto, seq);
}
// LivingEntity.swing with the default animation (whack, 6 ticks) to everyone tracking the player.
inline void swing(Player& p, int hand, bool to_self) {
    Writer w;
    w.varint(play_cb::swing_animation).varint(p.eid).varint(hand).varint(1).varint(6);
    broadcast_near(p.x, p.z, w, to_self ? nullptr : &p);
}

// ---------- placement ----------
inline bool box_hits_player(int x, int y, int z) {  // Level.isUnobstructed for players
    for (auto& q : players) {
        if (q->gamemode == 3 || q->g.dead || q->level != cur_level) continue;
        if (q->x + 0.3 > x && q->x - 0.3 < x + 1 && q->y + 1.8 > y && q->y < y + 1 && q->z + 0.3 > z && q->z - 0.3 < z + 1)
            return true;
    }
    return false;
}

// ServerPlayerGameMode.useItemOn. Returns 0 PASS, 1 SUCCESS (swing), 2 CONSUME (no swing).
inline int use_item_on(Player& p, int hand, int x, int y, int z, int face, float cx, float cy, float cz) {
    if (p.g.dead) return 0;
    int clicked = world.get_block(x, y, z);
    bool is_table = block_index(clicked) == block_index(blocks::crafting_table);
    if (p.gamemode == 3) {  // spectators can open menus but not use anything else
        if (!is_table) return 0;
        open_crafting_table(p, x, y, z);
        return 2;
    }
    ItemStack& slot = p.inv[hand == 1 ? 40u : size_t(p.selected)];
    bool have_items = !held(p).empty() || !p.inv[40].empty();
    if (is_table && !(p.g.sneaking && have_items) && hand == 0) {  // useWithoutItem
        open_crafting_table(p, x, y, z);
        return 1;
    }
    if (p.gamemode == 2) return 0;  // !mayBuild; ponytail: can_place_on (adventure predicates) not supported
    return lvl::use_item_on(p, hand, x, y, z, face, cx, cy, cz);  // block use, item useOn, BlockItem.place
}

// ---------- persistence (appended to playerdata/<uuid>.txt; old files simply lack these lines) ----------
// "vitals health food saturation exhaustion fall_distance dead" / "xp level progress total" / "dmg slot damage"
inline void save_player_extra(const Player& p, std::ostream& f) {
    auto& g = p.g;
    f << "vitals " << g.health << ' ' << g.food << ' ' << g.saturation << ' ' << g.exhaustion << ' '
      << g.fall_distance << ' ' << g.dead << '\n';
    f << "xp " << g.xp_level << ' ' << g.xp_progress << ' ' << g.xp_total << '\n';
    for (size_t i = 0; i < p.inv.size(); i++)
        if (!p.inv[i].empty() && p.inv[i].damage) f << "dmg " << i << ' ' << p.inv[i].damage << '\n';
}
inline void load_player_extra(Player& p, std::istream& f) {
    f.clear();
    for (std::string key; f >> key;) {
        if (key == "vitals") f >> p.g.health >> p.g.food >> p.g.saturation >> p.g.exhaustion >> p.g.fall_distance >> p.g.dead;
        else if (key == "xp") f >> p.g.xp_level >> p.g.xp_progress >> p.g.xp_total;
        else if (key == "dmg") {
            int slot, d;
            if (f >> slot >> d && slot >= 0 && slot < int(p.inv.size())) p.inv[size_t(slot)].damage = d;
        } else {
            std::string rest;
            std::getline(f, rest);
        }
        if (!f) return;
    }
}

// ---------- handlers ----------
inline void register_game_handlers() {
    for (int id : {play_sb::move_player_pos, play_sb::move_player_pos_rot, play_sb::move_player_rot,
                   play_sb::move_player_status_only}) {
        PacketHandler prev = handlers()[id];
        on(id, [prev](Player& p, Reader& in, int id) {
            double ox = p.x, oy = p.y, oz = p.z;
            bool og = p.on_ground;
            if (p.g.dead) return;  // vanilla ignores movement while dead
            prev(p, in, id);
            after_move(p, ox, oy, oz, og);
        });
    }
    PacketHandler prev_carried = handlers()[play_sb::set_carried_item];
    on(play_sb::set_carried_item, [prev_carried](Player& p, Reader& in, int id) {
        int before = p.selected;
        prev_carried(p, in, id);
        if (p.selected != before) p.g.use_ticks = 0;
    });
    PacketHandler prev_creative = handlers()[play_sb::set_creative_mode_slot];
    on(play_sb::set_creative_mode_slot, [prev_creative](Player& p, Reader& in, int id) {
        Reader peek = in;
        int slot = int16_t(peek.u16());
        int count = peek.varint();
        int item = count > 0 ? peek.varint() : -1;
        if (slot == -1 && p.gamemode == 1 && item >= 0 && item < ITEM_COUNT) player_drop(p, {item, count, 0}, true);
        prev_creative(p, in, id);
    });
    on(play_sb::player_action, [](Player& p, Reader& in, int) {
        int action = in.varint();
        int x, y, z;
        read_block_pos(in, x, y, z);
        in.u8();
        int seq = in.varint();
        if (action <= 3) {
            dig_action(p, action, x, y, z);
            ack_block_changes(p, seq);
        } else if ((action == 4 || action == 5) && p.gamemode != 3 && !p.g.dead) {  // DROP_ALL_ITEMS / DROP_ITEM
            ItemStack& s = held(p);
            if (s.empty()) return;
            ItemStack d = s;
            if (action == 5) d.count = 1;
            if ((s.count -= d.count) <= 0) s = {};
            player_drop(p, d, false);
        } else if (action == 6) {  // RELEASE_USE_ITEM
            p.g.use_ticks = 0;
        } else if (action == 7 && p.gamemode != 3) {  // SWAP_ITEM_WITH_OFFHAND
            std::swap(held(p), p.inv[40]);
            p.g.use_ticks = 0;
        }
    });
    on(play_sb::use_item_on, [](Player& p, Reader& in, int) {
        int hand = in.varint();
        int x, y, z;
        read_block_pos(in, x, y, z);
        int face = in.varint();
        float cx = in.f32(), cy = in.f32(), cz = in.f32();  // hit location relative to the block
        in.u8(); in.u8();
        int seq = in.varint();
        // ServerGamePacketListenerImpl.handleUseItemOn
        ack_block_changes(p, seq);
        if (!within_reach(p, x, y, z)) return;
        if (std::abs(cx - 0.5) >= 1.0000001 || std::abs(cy - 0.5) >= 1.0000001 || std::abs(cz - 0.5) >= 1.0000001) {
            log_warn("Rejecting UseItemOnPacket from %s: Location (%s, %s, %s) too far away from hit block BlockPos{x=%d, y=%d, z=%d}.",
                     p.name.c_str(), fmt_double(x + double(cx)).c_str(), fmt_double(y + double(cy)).c_str(),
                     fmt_double(z + double(cz)).c_str(), x, y, z);
            return;
        }
        const int max_y = world.max_y() - 1;
        if (y > max_y) return overlay_message(p, tr("build.tooHigh", {max_y}));
        if (y < world.min_y()) return overlay_message(p, tr("build.tooLow", {world.min_y()}));
        static const int off[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
        int f = face >= 0 && face <= 5 ? face : 1;
        if (under_spawn_protection(p, x, z)) {
            overlay_message(p, tr("build.spawn_protection", {short_pos(x, y, z)}));
        } else if (!p.net.awaiting) {
            const ItemStack& used = p.inv[hand == 1 ? 40u : size_t(p.selected)];
            bool placing = !used.empty() && ITEM_TO_BLOCK[used.item] >= 0;  // wasBlockPlacementAttempt
            int result = use_item_on(p, hand, x, y, z, face, cx, cy, cz);
            if (result == 0 && placing && ((f == 1 && y >= max_y) || (f == 0 && y <= world.min_y())))
                overlay_message(p, f == 1 ? tr("build.tooHigh", {max_y}) : tr("build.tooLow", {world.min_y()}));
            else if (result == 1) swing(p, hand, false);
        } else {
            overlay_message(p, tr("build.tooHigh", {max_y}));
        }
        p.send(block_update(x, y, z, world.get_block(x, y, z)));
        int nx = x + off[f][0], ny = y + off[f][1], nz = z + off[f][2];
        p.send(block_update(nx, ny, nz, world.get_block(nx, ny, nz)));
    });
    on(play_sb::use_item, [](Player& p, Reader& in, int) {
        int hand = in.varint();
        int seq = in.varint();
        if (in.i + 8 <= in.n) p.yaw = in.f32(), p.pitch = in.f32();  // the view the client used the item with
        ack_block_changes(p, seq);
        if (p.gamemode != 3 && !p.g.dead && lvl::use_bucket(p, p.inv[hand ? 40u : size_t(p.selected)])) return;
        start_using(p, hand);
    });
    on(play_sb::container_click, [](Player& p, Reader& in, int) { handle_container_click(p, in); });
    on(play_sb::container_close, [](Player& p, Reader& in, int) {
        in.varint();  // vanilla closes whatever menu is open, whatever the id (doCloseContainer)
        close_container(p, false);
    });
    on(play_sb::client_command, [](Player& p, Reader& in, int) {
        if (in.varint() == 0) respawn(p);
    });
    on(play_sb::player_command, [](Player& p, Reader& in, int) {
        in.varint();  // entity id
        int action = in.varint();
        if (action == 1) p.g.sprinting = true;
        else if (action == 2) p.g.sprinting = false;
    });
    on(play_sb::player_input, [](Player& p, Reader& in, int) { p.g.sneaking = in.u8() & 0x20; });
    on(play_sb::player_loaded, [](Player& p, Reader&, int) { p.g.loaded = true; });
}

inline void game_join(Player& p) {
    p.g.remote_valid = false;
    send_health(p);
    send_experience(p);
    if (p.g.dead) {  // logged out on the death screen: show it again
        p.g.dead = false;
        kill(p, "generic");
    }
}
inline void game_player_tick(Player& p) {
    dig_tick(p);
    use_tick(p);
    vitals_tick(p);
}
inline void game_leave(Player& p) {
    return_grid(p);  // grid + cursor back into the inventory before saving
    entities_forget_player(p);
}
inline void game_init() {
    if (!load_game_data()) _exit(1);
    mobs_init();  // entity framework, mobs, spawning (src/mobs/)
    entity_post_tick_hooks.push_back(entity_player_touches);
    on_player_join(game_join);
    on_player_tick(game_player_tick);
    on_tick(tick_entities);
    on_player_sync(sync_menu);
    on_player_leave(game_leave);
}
