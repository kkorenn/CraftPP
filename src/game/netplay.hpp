#pragma once
// Connection-level vanilla behaviour (ServerCommonPacketListenerImpl, ServerGamePacketListenerImpl, PlayerList):
// keep-alive + latency, ping, teleport confirmation, movement sanity checks and the floating kick, block-change
// acks, arm swings, client options (skin layers, main hand, chat visibility) as player entity data, abilities,
// difficulty buttons, statistics request, spectator teleport, illegal chat characters. Tick thread only.
#include "survival.hpp"

#include <chrono>

inline int64_t util_millis() {  // Util.getMillis (System.nanoTime based)
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void disconnect(Player& p, const Text& reason) {
    Writer w = pkt(play_cb::disconnect);
    write_nbt(w, reason);
    p.send(w);
    p.out.flush();
    if (p.quit_reason.empty()) p.quit_reason = plain(reason);
    kick(p);
}

// ---------- keep-alive (ServerCommonPacketListenerImpl.keepConnectionAlive) ----------
inline void keepalive_tick(Player& p) {
    auto& n = p.net;
    int64_t now = util_millis();
    if (n.keepalive_time == 0) n.keepalive_time = now;  // the listener starts its clock at join
    if (now - n.keepalive_time < 15000) return;
    if (n.keepalive_pending) return disconnect(p, tr("disconnect.timeout"));
    n.keepalive_pending = true;
    n.keepalive_time = n.keepalive_challenge = now;
    p.send(Writer().varint(play_cb::keep_alive).i64(now));
}

// ---------- entity data the other players render: shared flags, pose, main hand, skin layers ----------
inline void send_player_data(Player& to, const Player& of) {
    const auto& n = of.net;
    to.send(Writer().varint(play_cb::set_entity_data).varint(of.eid)
                .u8(0).varint(0).u8(uint8_t(n.sent_flags))       // DATA_SHARED_FLAGS_ID (byte)
                .u8(6).varint(20).varint(n.sent_pose)            // DATA_POSE
                .u8(15).varint(42).varint(n.main_hand)           // Avatar.DATA_PLAYER_MAIN_HAND (HUMANOID_ARM = 42)
                .u8(16).varint(0).u8(uint8_t(n.sent_skin))       // Avatar.DATA_PLAYER_MODE_CUSTOMISATION
                .u8(0xFF));
}
inline void sync_player_data(Player& p) {
    auto& n = p.net;
    auto& g = p.g;
    bool flying_pose = n.fall_flying && !p.on_ground;
    int flags = (g.sneaking ? 0x02 : 0) | (g.sprinting ? 0x08 : 0) | (flying_pose ? 0x80 : 0) | g.effect_flags;
    int pose = g.dead ? 7                                                                  // DYING
             : flying_pose ? 1                                                             // FALL_FLYING
             : g.sneaking && !n.flying && p.gamemode != 3 ? 5 : 0;                         // CROUCHING / STANDING
    bool respawned = n.was_dead && !g.dead;  // respawn re-added the entity on every client
    n.was_dead = g.dead;
    if (respawned)
        for (auto& q : players)
            if (q.get() != &p && q->net.sent_flags >= 0) send_player_data(p, *q);
    if (!respawned && flags == n.sent_flags && pose == n.sent_pose && n.skin_parts == n.sent_skin) return;
    n.sent_flags = flags, n.sent_pose = pose, n.sent_skin = n.skin_parts;
    for (auto& q : players) send_player_data(*q, p);  // ServerEntity.sendDirtyEntityData: trackers and self
}
inline void net_join(Player& p) {
    for (auto& q : players)
        if (q.get() != &p && q->net.sent_flags >= 0) send_player_data(p, *q);
}

// ClientboundPlayerInfoUpdatePacket(UPDATE_LATENCY) for everyone, every 600 ticks (PlayerList.tick).
inline void latency_tick() {
    static int countdown = 0;
    if (++countdown <= 600 || players.empty()) return;
    countdown = 0;
    Writer w;
    w.varint(play_cb::player_info_update).u8(0x10).varint(int32_t(players.size()));
    for (auto& q : players) w.i64(q->uuid_hi).i64(q->uuid_lo).varint(q->net.latency);
    broadcast(w);
}

// ---------- movement (handleMovePlayer / handlePlayerPositionChange / tickPlayer) ----------
inline float wrap_degrees(float d) {  // Mth.wrapDegrees(float)
    float f = std::fmod(d, 360.0f);
    if (f >= 180.0f) f -= 360.0f;
    if (f < -180.0f) f += 360.0f;
    return f;
}
inline float snap_pitch(float x) { return std::fmod(std::clamp(x, -90.0f, 90.0f), 360.0f); }  // Entity.setXRot

// ServerGamePacketListenerImpl.noBlocksAround: box inflated by 1/16 and stretched 0.55 down is all air.
inline bool no_blocks_around(const Player& p) {
    double h = p.g.sneaking ? 1.5 : 1.8, e = 0.0625;
    int x0 = int(std::floor(p.x - 0.3 - e)), x1 = int(std::floor(p.x + 0.3 + e));
    int y0 = int(std::floor(p.y - e - 0.55)), y1 = int(std::floor(p.y + h + e));
    int z0 = int(std::floor(p.z - 0.3 - e)), z1 = int(std::floor(p.z + 0.3 + e));
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++) {
                int b = y < world.min_y() || y >= world.max_y() ? blocks::void_air : world.get_block(x, y, z);
                if (b != blocks::air && b != blocks::cave_air && b != blocks::void_air) return false;
            }
    return true;
}
inline bool may_fly(const Player& p) { return p.gamemode == 1 || p.gamemode == 3; }

inline void net_player_tick(Player& p) {  // tickPlayer bookkeeping + keep-alive
    auto& n = p.net;
    n.ticks++;
    n.first_x = p.x, n.first_y = p.y, n.first_z = p.z;  // resetPosition
    n.known_move_packets = n.move_packets;
    if (n.floating && !p.g.dead) {
        if (++n.floating_ticks > 80) {  // getMaximumFlyingTicks with the default gravity 0.08
            log_warn("%s was kicked for floating too long!", p.name.c_str());
            n.floating = false, n.floating_ticks = 0;
            return disconnect(p, tr("multiplayer.disconnect.flying"));
        }
    } else {
        n.floating = false, n.floating_ticks = 0;
    }
    keepalive_tick(p);
}
inline void net_player_sync(Player& p) {
    if (p.net.ack_upto > -1) {
        p.send(Writer().varint(play_cb::block_changed_ack).varint(p.net.ack_upto));
        p.net.ack_upto = -1;
    }
    sync_player_data(p);
}

inline void wrap_movement() {
    for (int id : {play_sb::move_player_pos, play_sb::move_player_pos_rot, play_sb::move_player_rot,
                   play_sb::move_player_status_only}) {
        PacketHandler prev = handlers()[id];
        on(id, [prev](Player& p, Reader& in, int id) {
            auto& n = p.net;
            bool has_pos = id == play_sb::move_player_pos || id == play_sb::move_player_pos_rot;
            bool has_rot = id == play_sb::move_player_rot || id == play_sb::move_player_pos_rot;
            double x = p.x, y = p.y, z = p.z;
            float yaw = p.yaw, pitch = p.pitch;
            if (has_pos) x = in.f64(), y = in.f64(), z = in.f64();
            if (has_rot) yaw = in.f32(), pitch = in.f32();
            uint8_t flags = in.u8();
            if (std::isnan(x) || std::isnan(y) || std::isnan(z) || !std::isfinite(yaw) || !std::isfinite(pitch))
                return disconnect(p, tr("multiplayer.disconnect.invalid_player_movement"));
            if (n.awaiting) {  // updateAwaitingTeleport: only the rotation counts until the client confirms
                if (n.ticks - n.awaiting_since > 20) teleport(p, n.await_x, n.await_y, n.await_z);
                p.yaw = wrap_degrees(yaw), p.pitch = snap_pitch(wrap_degrees(pitch));
                return;
            }
            if (n.ticks == 0) n.first_x = p.x, n.first_y = p.y, n.first_z = p.z;  // resetPosition before the first tick
            x = std::clamp(x, -3.0E7, 3.0E7), y = std::clamp(y, -2.0E7, 2.0E7), z = std::clamp(z, -3.0E7, 3.0E7);
            yaw = wrap_degrees(yaw), pitch = snap_pitch(wrap_degrees(pitch));
            int delta = ++n.move_packets - n.known_move_packets;
            if (delta > 5) delta = 1;
            if (rule("player_movement_check") && (!n.fall_flying || rule("elytra_movement_check")) && !p.g.dead) {
                double dx = x - n.first_x, dy = y - n.first_y, dz = z - n.first_z;
                // ponytail: expectedDist (server-side velocity) is taken as 0; we don't simulate player motion.
                if (dx * dx + dy * dy + dz * dz > (n.fall_flying ? 300.0 : 100.0) * delta) {
                    log_warn("%s moved too quickly! %s,%s,%s", p.name.c_str(), fmt_double(dx).c_str(),
                             fmt_double(dy).c_str(), fmt_double(dz).c_str());
                    return teleport(p, p.x, p.y, p.z);
                }
            }
            Writer w;  // the clamped / wrapped packet for the inner handlers (fall damage, plugins' move event)
            w.f64(x).f64(y).f64(z).f32(yaw).f32(pitch).u8(flags);
            Reader r(w.b);
            prev(p, r, play_sb::move_player_pos_rot);
            if (p.on_ground) n.fall_flying = false;
            n.floating = p.gamemode != 3 && props["allow-flight"] != "true" && !may_fly(p) && !n.fall_flying &&
                         no_blocks_around(p);
        });
    }
}

// handleAcceptTeleportPacket
inline void accept_teleport(Player& p, Reader& in) {
    int id = in.varint();
    double x = in.f64(), y = in.f64(), z = in.f64();
    float yaw = in.f32(), pitch = in.f32();
    if (id != p.net.teleport_id) return;
    if (!p.net.awaiting || std::isnan(x) || std::isnan(y) || std::isnan(z) || !std::isfinite(yaw) || !std::isfinite(pitch))
        return disconnect(p, tr("multiplayer.disconnect.invalid_player_movement"));
    p.x = p.net.await_x, p.y = p.net.await_y, p.z = p.net.await_z;
    p.net.awaiting = false;
    Writer w;  // then handlePlayerPositionChange(packet position, onGround false)
    w.f64(x).f64(y).f64(z).f32(yaw).f32(pitch).u8(0);
    Reader r(w.b);
    handlers()[play_sb::move_player_pos_rot](p, r, play_sb::move_player_pos_rot);
}

// ServerboundClientInformationPacket body (configuration and play). Any thread before the player is registered.
inline void read_client_information(Player& p, Reader& in) {
    // ponytail: the requested view distance isn't used; chunk streaming always uses the server's.
    in.str(16);  // language
    in.u8();     // view distance
    p.net.chat_visibility = in.varint();
    in.u8();     // chat colors
    p.net.skin_parts = in.u8();
    p.net.main_hand = in.varint();
}

// PlayerList.broadcastChatMessage for an unsigned message (offline mode): logged as "[Not Secure] <name> text",
// sent as ClientboundPlayerChatPacket with chat type minecraft:chat, so clients apply their chat settings.
inline void broadcast_player_chat(Player& from, const std::string& msg) {
    log_info("[Not Secure] <%s> %s", from.name.c_str(), msg.c_str());
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
    Writer tail;  // everything after the per-receiver index
    tail.i64(from.uuid_hi).i64(from.uuid_lo).varint(0)  // sender, link index (unsigned link)
        .boolean(false)                                  // no signature
        .str(msg).i64(now).i64(0).varint(0)              // body: content, timestamp, salt, last seen
        .boolean(false)                                  // unsigned content = content
        .varint(0)                                       // FilterMask.PASS_THROUGH
        .varint(1);                                      // chat type holder: registry id 0 (minecraft:chat) + 1
    write_nbt(tail, player_name(from));
    tail.boolean(false);                                 // no target name
    for (auto& q : players) {
        if (q->net.chat_visibility != 0) continue;       // ChatVisiblity.FULL only (shouldFilterMessageTo aside)
        Writer w;
        w.varint(play_cb::player_chat).varint(q->net.chat_index++).bytes(tail.b);
        q->send(w);
    }
}

inline bool chat_illegal(const std::string& s) {  // isChatMessageIllegal (UTF-16 units; only ASCII ones matter)
    for (unsigned char c : s)
        if (c == 127 || c < 32) return true;
    return s.find("\xC2\xA7") != std::string::npos;  // section sign
}

// ---------- handlers ----------
inline void register_net_handlers() {
    wrap_movement();
    on(play_sb::accept_teleportation, [](Player& p, Reader& in, int) { accept_teleport(p, in); });
    on(play_sb::keep_alive, [](Player& p, Reader& in, int) {
        int64_t id = in.i64();
        auto& n = p.net;
        if (n.keepalive_pending && id == n.keepalive_challenge) {
            n.latency = int((int64_t(n.latency) * 3 + (util_millis() - n.keepalive_time)) / 4);
            n.keepalive_pending = false;
        } else {
            disconnect(p, tr("disconnect.timeout"));
        }
    });
    on(play_sb::ping_request, [](Player& p, Reader& in, int) {
        p.send(Writer().varint(play_cb::pong_response).i64(in.i64()));
    });
    on(play_sb::client_information, [](Player& p, Reader& in, int) {
        bool had_hat = p.net.skin_parts & 0x40;
        read_client_information(p, in);
        if (bool(p.net.skin_parts & 0x40) != had_hat)
            broadcast(Writer().varint(play_cb::player_info_update).u8(0x80).varint(1)
                          .i64(p.uuid_hi).i64(p.uuid_lo).boolean(!had_hat));
    });
    on(play_sb::player_abilities, [](Player& p, Reader& in, int) {
        p.net.flying = (in.u8() & 0x02) && may_fly(p);
    });
    on(play_sb::punch, [](Player& p, Reader&, int) { swing(p, 0, false); });
    on(play_sb::change_difficulty, [](Player& p, Reader& in, int) {
        int d = ((in.varint() % 4) + 4) % 4;  // ByIdMap.OutOfBoundsStrategy.WRAP
        static const char* names[] = {"peaceful", "easy", "normal", "hard"};
        if (perm_level(p) < 2) {  // the display name is logged via Component.toString
            log_warn("Player %s tried to change difficulty to translation{key='options.difficulty.%s', args=[]} without required permissions",
                     p.name.c_str(), names[d]);
            return;
        }
        if (difficulty_locked) return;  // MinecraftServer.setDifficulty(_, false)
        world_difficulty = d;
        broadcast(difficulty_packet());
    });
    on(play_sb::lock_difficulty, [](Player& p, Reader& in, int) {
        bool locked = in.u8();
        if (perm_level(p) < 2) return;
        difficulty_locked = locked;
        broadcast(difficulty_packet());
    });
    PacketHandler prev_cc = handlers()[play_sb::client_command];
    on(play_sb::client_command, [prev_cc](Player& p, Reader& in, int id) {
        Reader peek = in;
        if (peek.varint() == 1) return p.send(Writer().varint(play_cb::award_stats).varint(0));  // REQUEST_STATS
        if (prev_cc) prev_cc(p, in, id);
    });
    PacketHandler prev_pc = handlers()[play_sb::player_command];
    on(play_sb::player_command, [prev_pc](Player& p, Reader& in, int id) {
        Reader peek = in;
        peek.varint();
        if (peek.varint() == 6) {  // START_FALL_FLYING: Player.tryToStartFallFlying (glider in the chest slot)
            const ItemStack& chest = p.inv[38];
            bool glider = !chest.empty() && item_info[size_t(chest.item)].name == "minecraft:elytra" &&
                          chest.damage < item_info[size_t(chest.item)].max_damage - 1;
            p.net.fall_flying = !p.on_ground && !p.net.fall_flying && glider;
        }
        if (prev_pc) prev_pc(p, in, id);
    });
    on(play_sb::teleport_to_entity, [](Player& p, Reader& in, int) {
        int64_t hi = in.i64(), lo = in.i64();
        if (p.gamemode != 3) return;
        // ponytail: only players are spectatable targets until mobs share an entity registry with players.
        for (auto& q : players)
            if (q->uuid_hi == hi && q->uuid_lo == lo) {
                p.yaw = q->yaw, p.pitch = q->pitch;
                return teleport(p, q->x, q->y, q->z);
            }
    });
    for (int id : {play_sb::chat, play_sb::chat_command, play_sb::chat_command_signed}) {
        PacketHandler prev = handlers()[id];
        on(id, [prev](Player& p, Reader& in, int id) {  // tryHandleChat
            Reader peek = in;
            if (chat_illegal(peek.str(32767))) return disconnect(p, tr("multiplayer.disconnect.illegal_characters"));
            if (id == play_sb::chat && p.net.chat_visibility == 2)
                return send_text(p, styled(tr("chat.disabled.options"), "red"));
            prev(p, in, id);
        });
    }
    on_player_tick(net_player_tick);
    on_player_sync(net_player_sync);
    on_player_join(net_join);
    on_tick(latency_tick);
}
