#pragma once
// Survival: block breaking (hardness/tools), placement cost, vitals (health/food/exhaustion), fall + void
// damage, death and respawn, eating, item drops (Q). Tick thread only.
//
// API: hurt(p, amount, "fall"|"out_of_world"|"starve"|"generic"...) -> bool, heal(p, amount),
//      add_exhaustion(p, amount), kill(p, damage_type), respawn(p).
#include "inventory.hpp"
#include "loot.hpp"

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

// LivingEntity.hurtServer (no armor/effects yet). Returns whether damage was applied.
inline bool hurt(Player& p, float amount, const std::string& type) {
    if (p.g.dead || amount <= 0) return false;
    bool bypass_invuln = type == "out_of_world" || type == "generic_kill";
    if (!survival_like(p) && !bypass_invuln) return false;
    float applied;
    if (p.g.invulnerable > 10) {
        if (amount <= p.g.last_hurt) return false;
        applied = amount - p.g.last_hurt;
        p.g.last_hurt = amount;
    } else {
        p.g.last_hurt = amount;
        p.g.invulnerable = 20;
        applied = amount;
    }
    // ponytail: armor, enchantment protection and absorption are not applied yet.
    auto dt = damage_types.find(type);
    if (dt != damage_types.end()) add_exhaustion(p, dt->second.exhaustion);
    p.g.health = std::max(0.0f, p.g.health - applied);
    Writer w;
    w.varint(play_cb::damage_event).varint(p.eid).varint(damage_type_id(type)).varint(0).varint(0).boolean(false);
    broadcast_near(p.x, p.z, w);
    if (p.g.health <= 0) kill(p, type);
    return true;
}

inline void drop_all(Player& p) {  // Inventory.dropAll (keepInventory off)
    for (auto& s : p.inv)
        if (!s.empty()) { player_drop(p, s, true); s = {}; }
    for (auto* s : {&p.g.carried}) if (!s->empty()) { player_drop(p, *s, true); *s = {}; }
    for (auto& s : p.g.craft2) if (!s.empty()) { player_drop(p, s, true); s = {}; }
    for (auto& s : p.g.craft3) if (!s.empty()) { player_drop(p, s, true); s = {}; }
    p.g.result = {};
}

inline void kill(Player& p, const std::string& type) {
    if (p.g.dead) return;
    p.g.dead = true;
    p.g.health = 0;
    p.g.use_ticks = 0;
    p.g.digging = p.g.delayed = false;
    auto dt = damage_types.find(type);
    std::string key = "death.attack.generic";
    if (dt != damage_types.end()) {
        key = "death.attack." + dt->second.msg;
        if (dt->second.fall_variants && p.g.fall_distance > 5) key = "death.fell.accident.generic";
    }
    Writer msg;
    translate_component(msg, key, {p.name});
    std::vector<uint8_t> component(msg.b);
    Writer k;
    k.varint(play_cb::player_combat_kill).varint(p.eid).bytes(component);
    p.send(k);
    Writer chat;
    chat.varint(play_cb::system_chat).bytes(component).boolean(false);
    for (auto& q : players) q->send(chat);
    log_info("%s", plain(tr(key, {p.name})).c_str());  // vanilla logs the death message like other system chat
    broadcast_near(p.x, p.z, entity_event_packet(p.eid, 3), &p);
    if (p.g.container) close_container(p, true);
    drop_all(p);
    p.g.xp_level = p.g.xp_total = 0, p.g.xp_progress = 0;  // ponytail: no xp orbs dropped yet
    send_health(p);
}

// PlayerList.respawn at the world spawn (no beds/anchors yet).
inline void respawn(Player& p) {
    if (!p.g.dead) return;
    p.g = [&] {
        PlayerGame g;
        g.next_container = p.g.next_container;
        g.loaded = true;
        return g;
    }();
    place_new_player(p);
    p.send(Writer().varint(play_cb::respawn).varint(dimension_types::overworld).str("minecraft:overworld").i64(0)
               .varint(p.gamemode).varint(0).boolean(false).boolean(flat_world).boolean(false).varint(0).varint(63)
               .u8(0));
    p.send(Writer().varint(play_cb::game_event).u8(13).f32(0));
    update_view(p, int(std::floor(p.x)) >> 4, int(std::floor(p.z)) >> 4, true);
    p.send(Writer().varint(play_cb::player_position).varint(0)
               .f64(p.x).f64(p.y).f64(p.z).f64(0).f64(0).f64(0).f32(p.yaw).f32(p.pitch).i32(0));
    p.send(Writer().varint(play_cb::set_held_slot).varint(p.selected));
    send_health(p);
    send_experience(p);
    for (auto& e : item_entities) std::erase(e.watchers, p.eid);  // client dropped its entities
    broadcast(remove_entities_packet({p.eid}), &p);
    broadcast(spawn_entity(p), &p);
    for (auto& q : players)
        if (q.get() != &p) p.send(spawn_entity(*q));
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
    if (g.saturation > 0 && hurt_ && g.food >= 20) {
        if (++g.food_timer >= 10) {
            float spent = std::min(g.saturation, 6.0f);
            heal(p, spent / 6.0f);
            add_exhaustion(p, spent);
            g.food_timer = 0;
        }
    } else if (g.food >= 18 && hurt_) {
        if (++g.food_timer >= 80) { heal(p, 1); add_exhaustion(p, 6); g.food_timer = 0; }
    } else if (g.food <= 0) {
        if (++g.food_timer >= 80) {
            if (g.health > 10 || diff == 3 || (g.health > 1 && diff == 2)) hurt(p, 1, "starve");
            g.food_timer = 0;
        }
    } else {
        g.food_timer = 0;
    }
    if (diff == 0) {
        if (hurt_ && server_tick % 20 == 0) heal(p, 1);
        if (g.food < 20 && server_tick % 10 == 0) g.food++;
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

namespace lvl {
inline bool player_destroy(Player& p, int x, int y, int z, int state);  // level/glue.hpp: playerWillDestroy + removeBlock
}
// ServerPlayerGameMode.destroyBlock: remove the block, drop loot when harvestable, wear the tool.
inline void destroy_block(Player& p, int x, int y, int z) {
    int state = world.get_block(x, y, z);
    if (state == blocks::air) return;
    if (!lvl::player_destroy(p, x, y, z, state)) return;
    if (p.gamemode == 1) return;
    if (correct_tool(p, state)) {
        static std::vector<ItemStack> drops;
        drops.clear();
        block_drops(state, x, y, z, held(p), drops);
        for (auto& d : drops) pop_resource(x, y, z, d);
    }
    if (block_of(state).hardness != 0) {
        int item = held(p).item;
        if (item >= 0 && item_info[size_t(item)].tool >= 0) hurt_tool(p, tools[size_t(item_info[size_t(item)].tool)].damage_per_block);
    }
    add_exhaustion(p, 0.005f);
}

// Player.isWithinBlockInteractionRange(pos, 1.0): eye to the block's box, range 4.5 (creative 5) + 1.
inline bool within_reach(const Player& p, int x, int y, int z) {
    double ex = p.x, ey = p.y + 1.62, ez = p.z;
    double dx = std::max({x - ex, 0.0, ex - (x + 1)}), dy = std::max({y - ey, 0.0, ey - (y + 1)}),
           dz = std::max({z - ez, 0.0, ez - (z + 1)});
    double r = (p.gamemode == 1 ? 5.0 : 4.5) + 1.0;
    return dx * dx + dy * dy + dz * dz < r * r;
}

inline void dig_action(Player& p, int action, int x, int y, int z) {
    auto& g = p.g;
    if (p.gamemode == 3 || g.dead) return;
    if (!within_reach(p, x, y, z)) {
        p.send(block_update(x, y, z, world.get_block(x, y, z)));
        return;
    }
    int state = world.get_block(x, y, z);
    if (action == 0) {  // START_DESTROY_BLOCK
        if (p.gamemode == 1) return destroy_block(p, x, y, z);
        if (state == blocks::air) return;
        if (p.gamemode == 2) return p.send(block_update(x, y, z, state));  // ponytail: no can_break checks
        float prog = destroy_progress(p, state);
        if (prog >= 1.0f) return destroy_block(p, x, y, z);
        if (g.digging) p.send(block_update(g.dig_x, g.dig_y, g.dig_z, world.get_block(g.dig_x, g.dig_y, g.dig_z)));
        g.digging = true;
        g.dig_x = x, g.dig_y = y, g.dig_z = z, g.dig_start = int(server_tick), g.dig_state = state;
    } else if (action == 3) {  // STOP_DESTROY_BLOCK
        if (!g.digging || g.dig_x != x || g.dig_y != y || g.dig_z != z) return p.send(block_update(x, y, z, state));
        g.digging = false;
        float prog = destroy_progress(p, state) * float(int(server_tick) - g.dig_start + 1);
        if (prog >= 0.7f) return destroy_block(p, x, y, z);
        if (!g.delayed) {  // client was a bit fast: finish when the server-side progress reaches 1
            g.delayed = true;
            g.dly_x = x, g.dly_y = y, g.dly_z = z, g.dly_start = g.dig_start, g.dly_state = state;
        }
    } else if (action == 1 || action == 2) {  // abort
        if (g.digging) p.send(block_update(g.dig_x, g.dig_y, g.dig_z, world.get_block(g.dig_x, g.dig_y, g.dig_z)));
        g.digging = false;
    }
}
inline void dig_tick(Player& p) {
    auto& g = p.g;
    if (!g.delayed) return;
    int state = world.get_block(g.dly_x, g.dly_y, g.dly_z);
    if (state != g.dly_state) { g.delayed = false; return; }
    if (destroy_progress(p, state) * float(int(server_tick) - g.dly_start + 1) >= 1.0f) {
        g.delayed = false;
        destroy_block(p, g.dly_x, g.dly_y, g.dly_z);
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
                int dmg = int(std::floor(p.g.fall_distance + 1.0E-6 - 3.0));
                if (dmg > 0) hurt(p, float(dmg), "fall");
            }
            p.g.fall_distance = 0;
        }
    }
    if (was_on_ground && !p.on_ground && dy > 0) add_exhaustion(p, p.g.sprinting ? 0.2f : 0.05f);  // jumpFromGround
    double h = std::sqrt((p.x - ox) * (p.x - ox) + (p.z - oz) * (p.z - oz));
    if (p.g.sprinting && h > 0 && h < 10) add_exhaustion(p, 0.1f * float(h));
}

inline void vitals_tick(Player& p) {
    auto& g = p.g;
    if (g.invulnerable > 0) g.invulnerable--;
    if (g.dead) return;
    if (p.y < MIN_Y - 64) hurt(p, 4, "out_of_world");
    if (survival_like(p)) food_tick(p);
    if (g.health != g.sent_health || g.food != g.sent_food || (g.saturation == 0) != g.sent_sat_zero) send_health(p);
    if (g.container) {  // CraftingMenu.stillValid: table still there and within 8 blocks
        double dx = p.x - (g.table_x + 0.5), dy = p.y - (g.table_y + 0.5), dz = p.z - (g.table_z + 0.5);
        if (block_index(world.get_block(g.table_x, g.table_y, g.table_z)) != block_index(blocks::crafting_table) ||
            dx * dx + dy * dy + dz * dz > 64)
            close_container(p, true);
    }
}

// ---------- placement ----------
inline bool box_hits_player(int x, int y, int z) {  // Level.isUnobstructed for players
    for (auto& q : players) {
        if (q->gamemode == 3 || q->g.dead) continue;
        if (q->x + 0.3 > x && q->x - 0.3 < x + 1 && q->y + 1.8 > y && q->y < y + 1 && q->z + 0.3 > z && q->z - 0.3 < z + 1)
            return true;
    }
    return false;
}

namespace lvl {
inline void use_item_on(Player& pl, int hand, int x, int y, int z, int face, float cx, float cy, float cz);  // level/placement.hpp
inline bool use_bucket(Player& pl, ItemStack& slot);
}
inline void use_item_on(Player& p, int hand, int x, int y, int z, int face, float cx, float cy, float cz) {
    if (p.gamemode == 3 || p.g.dead) return;
    if (!within_reach(p, x, y, z)) return;
    int clicked = world.get_block(x, y, z);
    bool hands_empty = held(p).empty() && p.inv[40].empty();
    if (block_index(clicked) == block_index(blocks::crafting_table) && (!p.g.sneaking || hands_empty) && hand == 0) {
        open_crafting_table(p, x, y, z);
        return;
    }
    if (face < 0 || face > 5) face = 1;
    lvl::use_item_on(p, hand, x, y, z, face, cx, cy, cz);
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
        }
    });
    on(play_sb::use_item_on, [](Player& p, Reader& in, int) {
        int hand = in.varint();
        int x, y, z;
        read_block_pos(in, x, y, z);
        int face = in.varint();
        float cx = in.f32(), cy = in.f32(), cz = in.f32();
        in.u8(); in.u8();
        int seq = in.varint();
        use_item_on(p, hand, x, y, z, face, cx, cy, cz);
        ack_block_changes(p, seq);
    });
    on(play_sb::use_item, [](Player& p, Reader& in, int) {
        int hand = in.varint();
        int seq = in.varint();
        if (in.i + 8 <= in.n) p.yaw = in.f32(), p.pitch = in.f32();  // the view the client used the item with
        if (p.gamemode != 3 && !p.g.dead && lvl::use_bucket(p, p.inv[hand ? 40u : size_t(p.selected)])) {
            ack_block_changes(p, seq);
            return;
        }
        start_using(p, hand);
        ack_block_changes(p, seq);
    });
    on(play_sb::container_click, [](Player& p, Reader& in, int) { handle_container_click(p, in); });
    on(play_sb::container_close, [](Player& p, Reader& in, int) {
        int id = in.varint();
        if (id == p.g.container) close_container(p, false);
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
    on_player_join(game_join);
    on_player_tick(game_player_tick);
    on_tick(tick_entities);
    on_player_sync(sync_menu);
    on_player_leave(game_leave);
}
