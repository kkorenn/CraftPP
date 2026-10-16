#pragma once
// Play state: join sequence, packet loop, packet handler registry.
#include "blocks.hpp"
#include "commands.hpp"

#include <functional>
#include <poll.h>

using PacketHandler = std::function<void(Player&, Reader&, int id)>;

// Serverbound play packet id -> handler. Modules add handlers with on() at startup (before accept loop).
inline std::map<int, PacketHandler>& handlers() {
    static std::map<int, PacketHandler> h;
    return h;
}
inline void on(int id, PacketHandler h) { handlers()[id] = std::move(h); }

#include "game/game.hpp"  // tick thread, survival gameplay

inline void join(const PlayerPtr& pp) {
    Player& p = *pp;
    int v = view_distance();
    p.send(Writer()
               .varint(play_cb::login)
               .i32(p.eid)
               .boolean(false)  // hardcore
               .varint(1).str("minecraft:overworld")
               .varint(std::stoi(props["max-players"]))
               .varint(v)
               .varint(v)       // simulation distance
               .boolean(rule("reduced_debug_info"))
               .boolean(!rule("immediate_respawn"))  // show death screen
               .boolean(rule("limited_crafting"))
               .varint(dimension_types::overworld)
               .str("minecraft:overworld")
               .i64(0)          // hashed seed
               .varint(p.gamemode)
               .varint(0)       // previous gamemode: none
               .boolean(false)  // debug world
               .boolean(flat_world)
               .boolean(false)  // no last death location
               .varint(0)       // portal cooldown
               .varint(63)      // sea level
               .boolean(false)  // online mode
               .boolean(false));  // enforces secure chat
    commands_on_join(p);  // commands tree, op level, border, time, spawn, difficulty, weather, scoreboard
    p.send(Writer().varint(play_cb::game_event).u8(13).f32(0));  // level chunks load start
    update_view(p, int(std::floor(p.x)) >> 4, int(std::floor(p.z)) >> 4, true);
    p.send(Writer().varint(play_cb::player_position).varint(next_teleport_id(p))
               .f64(p.x).f64(p.y).f64(p.z).f64(0).f64(0).f64(0).f32(p.yaw).f32(p.pitch).i32(0));
    for (size_t i = 0; i < p.inv.size(); i++)
        if (p.inv[i].item >= 0)
            p.send(Writer().varint(play_cb::set_player_inventory).varint(int32_t(i))
                       .varint(p.inv[i].count).varint(p.inv[i].item).varint(0).varint(0));
    p.send(Writer().varint(play_cb::set_held_slot).varint(p.selected));

}

// Network thread: join, then hand packets to the tick thread until the connection closes (game/tick.hpp).
inline void play(const PlayerPtr& pp) {
    join(pp);
    run_play_connection(pp, register_player, unregister_player);
}

// Core handlers: movement, inventory, blocks, chat, commands.
inline void register_core_handlers() {
    on(play_sb::move_player_pos, [](Player& p, Reader& in, int id) {
        (void)id;
        if (id == play_sb::move_player_pos || id == play_sb::move_player_pos_rot) {
            p.x = in.f64(); p.y = in.f64(); p.z = in.f64();
        }
        if (id == play_sb::move_player_rot || id == play_sb::move_player_pos_rot) {
            p.yaw = in.f32(); p.pitch = in.f32();
        }
        p.on_ground = in.u8() & 1;
        // ponytail: client is trusted (no anti-cheat / collision checks).
        int ncx = int(std::floor(p.x)) >> 4, ncz = int(std::floor(p.z)) >> 4;
        if (ncx != p.cx || ncz != p.cz) update_view(p, ncx, ncz, false);
        broadcast_position(p);
    });
    on(play_sb::move_player_pos_rot, [](Player& p, Reader& in, int id) {
        (void)id;
        if (id == play_sb::move_player_pos || id == play_sb::move_player_pos_rot) {
            p.x = in.f64(); p.y = in.f64(); p.z = in.f64();
        }
        if (id == play_sb::move_player_rot || id == play_sb::move_player_pos_rot) {
            p.yaw = in.f32(); p.pitch = in.f32();
        }
        p.on_ground = in.u8() & 1;
        // ponytail: client is trusted (no anti-cheat / collision checks).
        int ncx = int(std::floor(p.x)) >> 4, ncz = int(std::floor(p.z)) >> 4;
        if (ncx != p.cx || ncz != p.cz) update_view(p, ncx, ncz, false);
        broadcast_position(p);
    });
    on(play_sb::move_player_rot, [](Player& p, Reader& in, int id) {
        (void)id;
        if (id == play_sb::move_player_pos || id == play_sb::move_player_pos_rot) {
            p.x = in.f64(); p.y = in.f64(); p.z = in.f64();
        }
        if (id == play_sb::move_player_rot || id == play_sb::move_player_pos_rot) {
            p.yaw = in.f32(); p.pitch = in.f32();
        }
        p.on_ground = in.u8() & 1;
        // ponytail: client is trusted (no anti-cheat / collision checks).
        int ncx = int(std::floor(p.x)) >> 4, ncz = int(std::floor(p.z)) >> 4;
        if (ncx != p.cx || ncz != p.cz) update_view(p, ncx, ncz, false);
        broadcast_position(p);
    });
    on(play_sb::move_player_status_only, [](Player& p, Reader& in, int id) {
        (void)id;
        if (id == play_sb::move_player_pos || id == play_sb::move_player_pos_rot) {
            p.x = in.f64(); p.y = in.f64(); p.z = in.f64();
        }
        if (id == play_sb::move_player_rot || id == play_sb::move_player_pos_rot) {
            p.yaw = in.f32(); p.pitch = in.f32();
        }
        p.on_ground = in.u8() & 1;
        // ponytail: client is trusted (no anti-cheat / collision checks).
        int ncx = int(std::floor(p.x)) >> 4, ncz = int(std::floor(p.z)) >> 4;
        if (ncx != p.cx || ncz != p.cz) update_view(p, ncx, ncz, false);
        broadcast_position(p);
    });
    on(play_sb::set_carried_item, [](Player& p, Reader& in, int id) {
        (void)id;
        int slot = int16_t(in.u16());
        if (slot >= 0 && slot < 9) p.selected = slot;
    });
    on(play_sb::set_creative_mode_slot, [](Player& p, Reader& in, int id) {
        (void)id;
        int inv = menu_to_inv(int16_t(in.u16()));
        int count = in.varint();
        int item = count > 0 ? in.varint() : -1;  // component patch ignored
        if (inv >= 0 && p.gamemode == 1)
            p.inv[size_t(inv)] = item >= 0 && item < ITEM_COUNT ? Player::Slot{item, count} : Player::Slot{};
    });
    on(play_sb::player_action, [](Player& p, Reader& in, int id) {
        (void)id;
        int action = in.varint();
        int x, y, z;
        read_block_pos(in, x, y, z);
        in.u8();  // face
        int seq = in.varint();
        // Creative breaks on start; survival on finish.
        // ponytail: client's dig timing is trusted and nothing drops; add hardness/tool checks + item drops with survival.
        if ((action == 0 && p.gamemode == 1) || (action == 3 && p.gamemode != 1)) change_block(x, y, z, blocks::air);
        if (action <= 3) p.send(Writer().varint(play_cb::block_changed_ack).varint(seq));
    });
    on(play_sb::pick_item_from_block, [](Player& p, Reader& in, int id) {
        (void)id;
        int x, y, z;
        read_block_pos(in, x, y, z);
        // ponytail: no reach check and includeData (ctrl+pick block entity NBT) ignored.
        int item = pick_item(world.get_block(x, y, z));
        if (item >= 0) pick_block(p, item);
    });
    on(play_sb::use_item_on, [](Player& p, Reader& in, int id) {
        (void)id;
        int hand = in.varint();
        int x, y, z;
        read_block_pos(in, x, y, z);
        int face = in.varint();
        in.f32(); in.f32(); in.f32(); in.u8(); in.u8();  // cursor, inside, world border
        int seq = in.varint();
        const auto& slot = p.inv[hand == 1 ? 40 : size_t(p.selected)];
        int state = slot.item >= 0 ? ITEM_TO_BLOCK[slot.item] : -1;
        if (state >= 0 && p.gamemode != 3) {
            // ponytail: places the block's default state (no facing/axis/half); needs per-block placement rules.
            if (!replaceable(world.get_block(x, y, z))) {
                static const int off[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
                if (face < 0 || face > 5) face = 1;
                x += off[face][0]; y += off[face][1]; z += off[face][2];
            }
            if (replaceable(world.get_block(x, y, z))) change_block(x, y, z, state);
        }
        p.send(Writer().varint(play_cb::block_changed_ack).varint(seq));
    });
    on(play_sb::chat, [](Player& p, Reader& in, int id) {
        (void)id;
        std::string msg = in.str(256);
        broadcast_message("<" + p.name + "> " + msg);
    });
    on(play_sb::chat_command, [](Player& p, Reader& in, int id) {
        (void)id;
        std::string cmd = in.str(32767);
        log_info("%s issued server command: /%s", p.name.c_str(), cmd.c_str());
        run_command(&p, cmd);
    });
    on(play_sb::chat_command_signed, [](Player& p, Reader& in, int id) {
        (void)id;
        std::string cmd = in.str(32767);
        log_info("%s issued server command: /%s", p.name.c_str(), cmd.c_str());
        run_command(&p, cmd);
    });
}
