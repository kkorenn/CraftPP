#pragma once
// Network security and external server protocols (ServerLoginPacketListenerImpl, ServerConfigurationPacketListenerImpl,
// ServerGamePacketListenerImpl, QueryThreadGs4, RconThread/RconClient):
//   - Online-mode login (RSA-1024ClientboundHelloPacket / ServerboundKeyPacket, AES-128/CFB8, Mojang hasJoined)
//   - Signed chat (RemoteChatSession, ProfilePublicKey validation, SignedMessageChain SHA256withRSA, LastSeenMessages)
//   - Resource pack negotiation (ClientboundResourcePackPushPacket / ServerboundResourcePackPacket)
//   - Server status favicon (server-icon.png, 64x64 PNG)
//   - GS4 UDP status/rules Query (enable-query, query.port)
//   - Source RCON TCP server (enable-rcon, rcon.port, rcon.password)
#include "../paper.hpp"
#include "crypto.hpp"
#include "http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace netsec {

// ---------- helpers ----------
inline void put_be32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
inline void put_be64(std::vector<uint8_t>& b, uint64_t v) {
    put_be32(b, uint32_t(v >> 32));
    put_be32(b, uint32_t(v));
}
inline bool parse_uuid_str(const std::string& s, int64_t& hi, int64_t& lo) {
    std::string hex;
    for (char c : s) {
        if (c == '-') continue;
        if (!isxdigit((unsigned char)c)) return false;
        hex += c;
    }
    if (hex.size() != 32) return false;
    hi = int64_t(strtoull(hex.substr(0, 16).c_str(), nullptr, 16));
    lo = int64_t(strtoull(hex.substr(16, 16).c_str(), nullptr, 16));
    return true;
}
inline void name_uuid_from_bytes(const std::string& s, int64_t& hi, int64_t& lo) {
    auto d = md5(s);
    d[6] = uint8_t((d[6] & 0x0f) | 0x30);
    d[8] = uint8_t((d[8] & 0x3f) | 0x80);
    hi = 0, lo = 0;
    for (int i = 0; i < 8; i++) hi = (hi << 8) | d[size_t(i)];
    for (int i = 8; i < 16; i++) lo = (lo << 8) | d[size_t(i)];
}
inline bool enforces_secure_chat() {
    return props["online-mode"] == "true" && props["enforce-secure-profile"] != "false";
}

// ---------- favicon (MinecraftServer.loadStatusIcon) ----------
inline std::string load_status_icon() {
    std::string p1 = server_dir + "/server-icon.png";
    std::string p2 = world_dir + "/icon.png";
    std::string path = std::filesystem::is_regular_file(p1) ? p1 : std::filesystem::is_regular_file(p2) ? p2 : "";
    if (path.empty()) return "";
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    static const uint8_t magic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (b.size() < 24 || memcmp(b.data(), magic, 8) != 0 || memcmp(b.data() + 12, "IHDR", 4) != 0) {
        log_error("Couldn't load server icon: invalid PNG header");
        return "";
    }
    uint32_t w = (uint32_t(b[16]) << 24) | (uint32_t(b[17]) << 16) | (uint32_t(b[18]) << 8) | b[19];
    uint32_t h = (uint32_t(b[20]) << 24) | (uint32_t(b[21]) << 16) | (uint32_t(b[22]) << 8) | b[23];
    if (w != 64 || h != 64) {
        log_error("Couldn't load server icon: Invalid world icon size [%u, %u], but expected [64, 64]", w, h);
        return "";
    }
    return "data:image/png;base64," + crypto::base64(b);
}

// ---------- online-mode login (ServerLoginPacketListenerImpl) ----------
inline const crypto::RsaKey& server_rsa_key() {
    static std::once_flag once;
    static crypto::RsaKey key;
    std::call_once(once, [] { key = crypto::rsa_generate(1024); });
    return key;
}

inline std::string peer_ip_only(int fd) {
    sockaddr_in a{};
    socklen_t n = sizeof a;
    char ip[INET_ADDRSTRLEN] = "";
    if (getpeername(fd, (sockaddr*)&a, &n) == 0) inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    return ip;
}

template <class WaitFn, class DisconnectFn>
inline bool authenticate_login(int fd, std::vector<uint8_t>& pkt, std::string& name, int64_t& uuid_hi, int64_t& uuid_lo,
                               std::vector<ProfileProperty>& out_props, WaitFn&& wait_for_fn, DisconnectFn&& disconnect_fn) {
    if (props["online-mode"] != "true") {
        // Offline mode: vanilla UUIDUtil.createOfflineProfile(name) if the client sent a zero UUID, else client's UUID.
        return true;
    }
    const auto& rsa = server_rsa_key();
    uint8_t challenge[4];
    crypto::random_bytes(challenge, 4);
    Writer hello;
    hello.varint(login_cb::hello)
        .str("")
        .varint(int32_t(rsa.der.size())).bytes(rsa.der)
        .varint(4).bytes(challenge, 4)
        .boolean(true);
    send_packet(fd, hello);

    Reader key_pkt = wait_for_fn(fd, pkt, login_sb::key);
    int32_t klen = key_pkt.varint();
    if (klen <= 0 || klen > 512) throw std::runtime_error("Protocol error");
    crypto::Bytes enc_secret = key_pkt.bytes(size_t(klen));
    int32_t clen = key_pkt.varint();
    if (clen <= 0 || clen > 512) throw std::runtime_error("Protocol error");
    crypto::Bytes enc_chal = key_pkt.bytes(size_t(clen));

    auto dec_chal = crypto::rsa_decrypt_pkcs1(rsa, enc_chal);
    auto secret = crypto::rsa_decrypt_pkcs1(rsa, enc_secret);
    if (!dec_chal || dec_chal->size() != 4 || memcmp(dec_chal->data(), challenge, 4) != 0 || !secret || secret->size() != 16)
        throw std::runtime_error("Protocol error");

    start_encryption(secret->data());

    auto digest = crypto::Sha1().update(std::string("")).update(*secret).update(rsa.der).final();
    std::string server_id = crypto::minecraft_digest_hex(digest.data(), digest.size());
    std::string base = props.count("session-server-url") && !props["session-server-url"].empty()
                           ? props["session-server-url"]
                           : "https://sessionserver.mojang.com/session/minecraft/hasJoined";
    std::string url = base + "?username=" + http::url_encode(name) + "&serverId=" + http::url_encode(server_id);
    if (props["prevent-proxy-connections"] == "true") {
        std::string ip = peer_ip_only(fd);
        if (!ip.empty()) url += "&ip=" + http::url_encode(ip);
    }
    http::Response resp = http::get(url, 10);
    if (resp.status == 200 && !resp.body.empty()) {
        try {
            Json j = Json::parse(resp.body);
            auto* id_j = j.find("id");
            auto* nm_j = j.find("name");
            if (!id_j || !id_j->isString() || !parse_uuid_str(id_j->str, uuid_hi, uuid_lo))
                throw std::runtime_error("bad session id");
            if (nm_j && nm_j->isString() && !nm_j->str.empty()) name = nm_j->str;
            if (auto* pr_j = j.find("properties"); pr_j && pr_j->type == Json::Array) {
                for (const auto& elem : pr_j->arr) {
                    auto* pn = elem.find("name");
                    auto* pv = elem.find("value");
                    auto* ps = elem.find("signature");
                    if (pn && pn->isString() && pv && pv->isString())
                        out_props.push_back({pn->str, pv->str, ps && ps->isString() ? ps->str : ""});
                }
            }
            log_info("UUID of player %s is %s", name.c_str(), uuid_dashed(uuid_hi, uuid_lo).c_str());
            return true;
        } catch (...) {
            disconnect_fn(fd, "Failed to verify username!", name);
            log_error("Username '%s' tried to join with an invalid session", name.c_str());
            return false;
        }
    } else if (resp.status >= 200 && resp.status < 500) {
        disconnect_fn(fd, "Failed to verify username!", name);
        log_error("Username '%s' tried to join with an invalid session", name.c_str());
        return false;
    } else {
        disconnect_fn(fd, "Authentication servers are down. Please try again later, sorry!", name);
        log_error("Couldn't verify username because servers are unavailable");
        return false;
    }
}

// ---------- resource packs (ServerResourcePackConfigurationTask) ----------
inline bool negotiate_resource_pack(int fd, std::vector<uint8_t>& pkt, std::vector<uint8_t>& client_info, const std::string& name) {
    const std::string& url = props["resource-pack"];
    if (url.empty()) return true;
    int64_t rp_hi = 0, rp_lo = 0;
    const std::string& id_str = props["resource-pack-id"];
    if (id_str.empty()) {
        name_uuid_from_bytes(url, rp_hi, rp_lo);
    } else if (!parse_uuid_str(id_str, rp_hi, rp_lo)) {
        log_warn("Failed to parse '%s' into UUID", id_str.c_str());
        return true;
    }
    std::string hash = props["resource-pack-sha1"];
    if (hash.size() > 40) hash.resize(40);
    bool required = props["require-resource-pack"] == "true";
    const std::string& prompt = props["resource-pack-prompt"];

    Writer w;
    w.varint(config_cb::resource_pack_push)
        .i64(rp_hi).i64(rp_lo)
        .str(url)
        .str(hash)
        .boolean(required)
        .boolean(!prompt.empty());
    if (!prompt.empty()) text_component(w, prompt);
    send_packet(fd, w);

    for (;;) {
        if (!read_packet(fd, pkt)) throw std::runtime_error("disconnected");
        Reader r(pkt);
        int got = r.varint();
        if (got == config_sb::client_information) {
            client_info = pkt;
            continue;
        }
        if (got != config_sb::resource_pack) continue;
        r.i64();
        r.i64();  // pack UUID
        int action = r.varint();
        if (action == 1 && required) {  // DECLINED
            log_info("Disconnecting %s due to resource pack %s rejection", name.c_str(), uuid_dashed(rp_hi, rp_lo).c_str());
            Writer dc;
            dc.varint(config_cb::disconnect);
            write_nbt(dc, tr("multiplayer.requiredTexturePrompt.disconnect"));
            send_packet(fd, dc);
            return false;
        }
        if (action != 3 && action != 4) break;  // terminal (not ACCEPTED / DOWNLOADED)
    }
    return true;
}

// ---------- signed chat (RemoteChatSession, ProfilePublicKey, SignedMessageChain, LastSeenMessages) ----------
inline const std::vector<crypto::RsaPublic>& services_keys() {
    static std::mutex m;
    static std::vector<crypto::RsaPublic> keys;
    static std::string last_cfg;
    std::lock_guard lock(m);
    std::string cfg = (props.count("services-public-key-der-base64") ? props["services-public-key-der-base64"] : "") + "|" +
                      (props.count("services-public-keys-url") ? props["services-public-keys-url"] : "");
    if (cfg != last_cfg) {
        keys.clear();
        last_cfg = cfg;
        if (props.count("services-public-key-der-base64") && !props["services-public-key-der-base64"].empty()) {
            if (auto der = crypto::base64_decode(props["services-public-key-der-base64"]))
                if (auto pk = crypto::spki_decode(*der)) keys.push_back(std::move(*pk));
        }
        if (keys.empty() && (props.count("services-public-keys-url") || props["online-mode"] == "true")) {
            std::string url = props.count("services-public-keys-url") && !props["services-public-keys-url"].empty()
                                  ? props["services-public-keys-url"]
                                  : "https://api.minecraftservices.com/publickeys";
            http::Response r = http::get(url, 5);
            if (r.status == 200) {
                try {
                    Json j = Json::parse(r.body);
                    if (auto* arr = j.find("playerCertificateKeys"); arr && arr->type == Json::Array)
                        for (const auto& e : arr->arr)
                            if (auto* k = e.find("publicKey"); k && k->isString())
                                if (auto der = crypto::base64_decode(k->str))
                                    if (auto pk = crypto::spki_decode(*der)) keys.push_back(std::move(*pk));
                } catch (...) {}
            }
        }
    }
    return keys;
}

inline int sig_checksum(const std::array<uint8_t, 256>& sig) {  // Arrays.hashCode(byte[])
    int32_t h = 1;
    for (uint8_t b : sig) h = 31 * h + int32_t(int8_t(b));
    return h;
}
inline int8_t last_seen_checksum(const std::vector<std::array<uint8_t, 256>>& entries) {
    int32_t c = 1;
    for (const auto& e : entries) c = 31 * c + sig_checksum(e);
    int8_t b = int8_t(c);
    return b == 0 ? 1 : b;
}

inline bool apply_last_seen_offset(Player& p, int offset) {
    auto& trk = p.net.last_seen_tracked;
    int max_off = int(trk.size()) - 20;
    if (offset < 0 || offset > max_off) return false;
    trk.erase(trk.begin(), trk.begin() + offset);
    return true;
}

inline std::optional<std::vector<std::array<uint8_t, 256>>> unpack_last_seen(Player& p, Reader& in) {
    int offset = in.varint();
    uint8_t ack[3] = {in.u8(), in.u8(), in.u8()};  // ByteBufCodecs.fixedBitSet(20) = 3 bytes
    int8_t checksum = int8_t(in.u8());
    if (!apply_last_seen_offset(p, offset)) {
        disconnect(p, tr("multiplayer.disconnect.chat_validation_failed"));
        return std::nullopt;
    }
    std::vector<std::array<uint8_t, 256>> entries;
    auto& trk = p.net.last_seen_tracked;
    for (int i = 0; i < 20; i++) {
        bool bit = (ack[i >> 3] & (1u << (i & 7))) != 0;
        if (bit) {
            if (!trk[size_t(i)].present) {
                disconnect(p, tr("multiplayer.disconnect.chat_validation_failed"));
                return std::nullopt;
            }
            trk[size_t(i)].pending = false;
            entries.push_back(trk[size_t(i)].sig);
        } else {
            if (trk[size_t(i)].present && !trk[size_t(i)].pending) {
                disconnect(p, tr("multiplayer.disconnect.chat_validation_failed"));
                return std::nullopt;
            }
            trk[size_t(i)] = {};
        }
    }
    if (checksum != 0 && checksum != last_seen_checksum(entries)) {
        disconnect(p, tr("multiplayer.disconnect.chat_validation_failed"));
        return std::nullopt;
    }
    return entries;
}

inline void track_pending_signature(Player& to, const std::array<uint8_t, 256>& sig) {
    if (!to.net.last_pending_sig || *to.net.last_pending_sig != sig) {
        to.net.last_seen_tracked.push_back({true, true, sig});
        to.net.last_pending_sig = sig;
    }
}

inline void handle_chat_session_update(Player& p, Reader& in) {
    int64_t sid_hi = in.i64(), sid_lo = in.i64();
    int64_t expires_ms = in.i64();
    int32_t klen = in.varint();
    if (klen <= 0 || klen > 512) return disconnect(p, tr("multiplayer.disconnect.invalid_public_key_signature"));
    crypto::Bytes key_der = in.bytes(size_t(klen));
    int32_t slen = in.varint();
    if (slen <= 0 || slen > 4096) return disconnect(p, tr("multiplayer.disconnect.invalid_public_key_signature"));
    crypto::Bytes key_sig = in.bytes(size_t(slen));

    if (p.net.has_chat_session && p.net.chat_key_expires_ms == expires_ms && p.net.chat_key_der == key_der && p.net.chat_key_sig == key_sig)
        return;
    if (p.net.has_chat_session && expires_ms < p.net.chat_key_expires_ms)
        return disconnect(p, tr("multiplayer.disconnect.expired_public_key"));

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    if (expires_ms < now_ms)
        return disconnect(p, tr("multiplayer.disconnect.expired_public_key"));

    auto pub = crypto::spki_decode(key_der);
    if (!pub) return disconnect(p, tr("multiplayer.disconnect.invalid_public_key_signature"));

    const auto& skeys = services_keys();
    if (skeys.empty()) {
        log_warn("Ignoring chat session from %s due to missing Services public key", p.name.c_str());
        return;
    }
    std::vector<uint8_t> payload;
    put_be64(payload, uint64_t(p.uuid_hi));
    put_be64(payload, uint64_t(p.uuid_lo));
    put_be64(payload, uint64_t(expires_ms));
    payload.insert(payload.end(), key_der.begin(), key_der.end());
    auto digest = crypto::Sha1().update(payload).final();
    bool valid = false;
    for (const auto& sk : skeys)
        if (crypto::rsa_verify(sk, crypto::Hash::Sha1, digest.data(), key_sig)) { valid = true; break; }
    if (!valid) return disconnect(p, tr("multiplayer.disconnect.invalid_public_key_signature"));

    p.net.has_chat_session = true;
    p.net.chat_chain_broken = false;
    p.net.chat_session_hi = sid_hi;
    p.net.chat_session_lo = sid_lo;
    p.net.chat_key_expires_ms = expires_ms;
    p.net.last_chat_timestamp_ms = 0;
    p.net.chat_link_index = 0;
    p.net.chat_key = std::move(*pub);
    p.net.chat_key_der = std::move(key_der);
    p.net.chat_key_sig = std::move(key_sig);

    Writer w;
    w.varint(play_cb::player_info_update).u8(0x02).varint(1)
        .i64(p.uuid_hi).i64(p.uuid_lo)
        .boolean(true)
        .i64(sid_hi).i64(sid_lo)
        .i64(expires_ms)
        .varint(int32_t(p.net.chat_key_der.size())).bytes(p.net.chat_key_der)
        .varint(int32_t(p.net.chat_key_sig.size())).bytes(p.net.chat_key_sig);
    broadcast(w);
}

inline void send_decode_failure(Player& p, const char* key) {
    log_warn("Failed to update secure chat state for %s: '%s'", p.name.c_str(), key);
    send_text(p, styled(tr(key), "red"));
}

inline bool verify_signed_body(Player& p, const std::string& content, int64_t timestamp_ms, int64_t salt,
                               const std::vector<std::array<uint8_t, 256>>& last_seen,
                               const crypto::Bytes& sig, int& out_link_index) {
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    if (p.net.chat_key_expires_ms < now_ms) {
        send_decode_failure(p, "chat.disabled.expiredProfileKey");
        return false;
    }
    if (p.net.chat_chain_broken) {
        send_decode_failure(p, "chat.disabled.chain_broken");
        return false;
    }
    if (timestamp_ms < p.net.last_chat_timestamp_ms) {
        p.net.chat_chain_broken = true;
        send_decode_failure(p, "chat.disabled.out_of_order_chat");
        return false;
    }
    p.net.last_chat_timestamp_ms = timestamp_ms;
    int link_idx = p.net.chat_link_index;

    std::vector<uint8_t> signed_bytes;
    put_be32(signed_bytes, 1);
    put_be64(signed_bytes, uint64_t(p.uuid_hi));
    put_be64(signed_bytes, uint64_t(p.uuid_lo));
    put_be64(signed_bytes, uint64_t(p.net.chat_session_hi));
    put_be64(signed_bytes, uint64_t(p.net.chat_session_lo));
    put_be32(signed_bytes, uint32_t(link_idx));
    put_be64(signed_bytes, uint64_t(salt));
    put_be64(signed_bytes, uint64_t(timestamp_ms / 1000));  // Instant.getEpochSecond()
    put_be32(signed_bytes, uint32_t(content.size()));
    signed_bytes.insert(signed_bytes.end(), content.begin(), content.end());
    put_be32(signed_bytes, uint32_t(last_seen.size()));
    for (const auto& ls : last_seen) signed_bytes.insert(signed_bytes.end(), ls.begin(), ls.end());

    auto digest = crypto::Sha256().update(signed_bytes).final();
    if (!crypto::rsa_verify(p.net.chat_key, crypto::Hash::Sha256, digest.data(), sig)) {
        p.net.chat_chain_broken = true;
        send_decode_failure(p, "chat.disabled.invalid_signature");
        return false;
    }
    p.net.chat_link_index++;
    out_link_index = link_idx;
    return true;
}

inline void handle_signed_or_unsigned_chat(Player& p, Reader& in) {
    std::string msg = in.str(256);
    if (in.i == in.n) {
        if (chat_illegal(msg)) return disconnect(p, tr("multiplayer.disconnect.illegal_characters"));
        if (p.net.chat_visibility == 2) return send_text(p, styled(tr("chat.disabled.options"), "red"));
        if (p.net.has_chat_session || enforces_secure_chat()) return send_decode_failure(p, "chat.disabled.missingProfileKey");
        broadcast_player_chat(p, msg);
        return;
    }
    int64_t timestamp_ms = in.i64();
    int64_t salt = in.i64();
    bool has_sig = in.u8() != 0;
    crypto::Bytes sig;
    if (has_sig) sig = in.bytes(256);
    auto last_seen = unpack_last_seen(p, in);
    if (!last_seen) return;
    if (chat_illegal(msg)) return disconnect(p, tr("multiplayer.disconnect.illegal_characters"));
    if (p.net.chat_visibility == 2) return send_text(p, styled(tr("chat.disabled.options"), "red"));

    if (!p.net.has_chat_session) {
        if (enforces_secure_chat()) return send_decode_failure(p, "chat.disabled.missingProfileKey");
        broadcast_player_chat(p, msg);
        return;
    }
    if (!has_sig) return send_decode_failure(p, "chat.disabled.missingProfileKey");

    int link_idx = 0;
    if (!verify_signed_body(p, msg, timestamp_ms, salt, *last_seen, sig, link_idx)) return;

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    bool expired = now_ms > timestamp_ms + 5 * 60 * 1000;
    if (expired) log_info("[Not Secure] <%s> %s", p.name.c_str(), msg.c_str());
    else log_info("<%s> %s", p.name.c_str(), msg.c_str());

    std::array<uint8_t, 256> sig_arr{};
    memcpy(sig_arr.data(), sig.data(), 256);

    Writer tail;
    tail.i64(p.uuid_hi).i64(p.uuid_lo).varint(link_idx)
        .boolean(true).bytes(sig)
        .str(msg).i64(timestamp_ms).i64(salt)
        .varint(int32_t(last_seen->size()));
    for (const auto& ls : *last_seen) tail.varint(0).bytes(ls.data(), 256);  // FULL_SIGNATURE (-1 + 1 = 0)
    tail.boolean(false)  // no unsigned content override
        .varint(0)       // FilterMask.PASS_THROUGH
        .varint(1);      // chat type 0 (minecraft:chat) + 1
    write_nbt(tail, player_name(p));
    tail.boolean(false);

    for (auto& q : players) {
        if (q->net.chat_visibility != 0) continue;
        track_pending_signature(*q, sig_arr);
        Writer w;
        w.varint(play_cb::player_chat).varint(q->net.chat_index++).bytes(tail.b);
        q->send(w);
    }
}

inline void handle_signed_chat_command(Player& p, Reader& in) {
    std::string cmd = in.str(32767);
    if (in.i < in.n) {
        int64_t timestamp_ms = in.i64();
        int64_t salt = in.i64();
        int32_t num_sigs = in.varint();
        if (num_sigs < 0 || num_sigs > 8) return disconnect(p, tr("multiplayer.disconnect.chat_validation_failed"));
        std::vector<std::pair<std::string, crypto::Bytes>> arg_sigs;
        for (int32_t i = 0; i < num_sigs; i++) {
            std::string arg_name = in.str(16);
            crypto::Bytes asig = in.bytes(256);
            arg_sigs.emplace_back(std::move(arg_name), std::move(asig));
        }
        auto last_seen = unpack_last_seen(p, in);
        if (!last_seen) return;
        if (chat_illegal(cmd)) return disconnect(p, tr("multiplayer.disconnect.illegal_characters"));

        if (p.net.has_chat_session && !arg_sigs.empty()) {
            // Find argument values from parsed command and verify each signed argument in order.
            Source src = source_for(find_player(p.name));
            Parse parsed = parse_command(cmd, 0, src);
            for (const auto& [aname, asig] : arg_sigs) {
                std::string aval;
                for (const auto& seg : parsed.segs)
                    for (const auto& nd : seg.nodes)
                        if (node_info(nd.node).type == 2 && aname == node_name(nd.node) && nd.end <= cmd.size())
                            aval = cmd.substr(nd.start, nd.end - nd.start);
                int link_idx = 0;
                if (!verify_signed_body(p, aval, timestamp_ms, salt, *last_seen, asig, link_idx)) return;
            }
        }
    } else if (chat_illegal(cmd)) {
        return disconnect(p, tr("multiplayer.disconnect.illegal_characters"));
    }
    log_info("%s issued server command: /%s", p.name.c_str(), cmd.c_str());
    run_command(&p, cmd);
}

inline void register_netsec_handlers() {
    on(play_sb::chat_session_update, [](Player& p, Reader& in, int) { handle_chat_session_update(p, in); });
    on(play_sb::chat, [](Player& p, Reader& in, int) { handle_signed_or_unsigned_chat(p, in); });
    on(play_sb::chat_command_signed, [](Player& p, Reader& in, int) { handle_signed_chat_command(p, in); });
    on(play_sb::chat_ack, [](Player& p, Reader& in, int) {
        int offset = in.varint();
        if (!apply_last_seen_offset(p, offset)) disconnect(p, tr("multiplayer.disconnect.chat_validation_failed"));
    });
    on(play_sb::resource_pack, [](Player& p, Reader& in, int) {
        int64_t hi = in.i64(), lo = in.i64();
        int action = in.varint();
        if (action == 1 && props["require-resource-pack"] == "true") {
            log_info("Disconnecting %s due to resource pack %s rejection", p.name.c_str(), uuid_dashed(hi, lo).c_str());
            disconnect(p, tr("multiplayer.requiredTexturePrompt.disconnect"));
        }
    });
    on_player_join([](Player& p) {
        for (auto& q : players) {
            if (q.get() != &p && q->net.has_chat_session) {
                Writer w;
                w.varint(play_cb::player_info_update).u8(0x02).varint(1)
                    .i64(q->uuid_hi).i64(q->uuid_lo)
                    .boolean(true)
                    .i64(q->net.chat_session_hi).i64(q->net.chat_session_lo)
                    .i64(q->net.chat_key_expires_ms)
                    .varint(int32_t(q->net.chat_key_der.size())).bytes(q->net.chat_key_der)
                    .varint(int32_t(q->net.chat_key_sig.size())).bytes(q->net.chat_key_sig);
                p.send(w);
            }
        }
    });
}

// ---------- GS4 Query (QueryThreadGs4) ----------
inline void start_query_thread() {
    if (props["enable-query"] != "true") return;
    int port = std::stoi(props["query.port"]);
    if (port <= 0 || port > 65535) {
        log_warn("Invalid query port %d found in server.properties (queries disabled)", port);
        return;
    }
    std::string sip = props["server-ip"].empty() ? "0.0.0.0" : props["server-ip"];
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    timeval tv{0, 500000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = INADDR_ANY;
    if (sip != "0.0.0.0") inet_pton(AF_INET, sip.c_str(), &addr.sin_addr);
    if (bind(sock, (sockaddr*)&addr, sizeof addr) < 0) {
        log_warn("Unable to initialise query system on %s:%d", sip.c_str(), port);
        close(sock);
        return;
    }
    log_info("Query running on %s:%d", sip.c_str(), port);
    std::thread([sock, sip] {
        struct Challenge {
            uint8_t ident[4];
            int32_t token;
            int64_t time_ms;
        };
        std::map<uint64_t, Challenge> challenges;
        auto addr_key = [](const sockaddr_in& a) {
            return (uint64_t(a.sin_addr.s_addr) << 16) | ntohs(a.sin_port);
        };
        uint8_t buf[1460];
        while (shutdown_requested == 0) {
            sockaddr_in peer{};
            socklen_t plen = sizeof peer;
            ssize_t n = recvfrom(sock, buf, sizeof buf, 0, (sockaddr*)&peer, &plen);
            int64_t now = util_millis();
            std::erase_if(challenges, [now](const auto& kv) { return now - kv.second.time_ms > 30000; });
            if (n < 7 || buf[0] != 0xFE || buf[1] != 0xFD) continue;
            uint8_t type = buf[2];
            uint64_t key = addr_key(peer);
            if (type == 9) {  // Handshake
                uint8_t rnd[3];
                crypto::random_bytes(rnd, 3);
                int32_t tok = (int32_t(rnd[0]) << 16) | (int32_t(rnd[1]) << 8) | rnd[2];
                Challenge ch{{buf[3], buf[4], buf[5], buf[6]}, tok, now};
                challenges[key] = ch;
                std::string ts = std::to_string(tok);
                std::vector<uint8_t> out = {9, buf[3], buf[4], buf[5], buf[6]};
                out.insert(out.end(), ts.begin(), ts.end());
                out.push_back(0);
                sendto(sock, out.data(), out.size(), 0, (sockaddr*)&peer, plen);
            } else if (type == 0 && n >= 11) {  // Stat
                auto it = challenges.find(key);
                int32_t got = (int32_t(buf[7]) << 24) | (int32_t(buf[8]) << 16) | (int32_t(buf[9]) << 8) | buf[10];
                if (it == challenges.end() || it->second.token != got) continue;
                auto pls = snapshot();
                std::vector<uint8_t> out = {0, it->second.ident[0], it->second.ident[1], it->second.ident[2], it->second.ident[3]};
                auto wr_str = [&](const std::string& s) {
                    out.insert(out.end(), s.begin(), s.end());
                    out.push_back(0);
                };
                int srv_port = std::stoi(props["server-port"]);
                std::string host_ip = sip == "0.0.0.0" ? "127.0.0.1" : sip;
                if (n == 15) {  // Full stat (rules)
                    wr_str("splitnum");
                    out.push_back(128);
                    out.push_back(0);
                    const std::pair<const char*, std::string> kv[] = {
                        {"hostname", props["motd"]},
                        {"gametype", "SMP"},
                        {"game_id", "MINECRAFT"},
                        {"version", GAME_VERSION},
                        {"plugins", ""},
                        {"map", props["level-name"]},
                        {"numplayers", std::to_string(pls.size())},
                        {"maxplayers", props["max-players"]},
                        {"hostport", std::to_string(srv_port)},
                        {"hostip", host_ip},
                    };
                    for (const auto& [k, v] : kv) { wr_str(k); wr_str(v); }
                    out.push_back(0);
                    out.push_back(1);
                    wr_str("player_");
                    out.push_back(0);
                    for (const auto& p : pls) wr_str(p->name);
                    out.push_back(0);
                } else {  // Basic stat
                    wr_str(props["motd"]);
                    wr_str("SMP");
                    wr_str(props["level-name"]);
                    wr_str(std::to_string(pls.size()));
                    wr_str(props["max-players"]);
                    out.push_back(uint8_t(srv_port & 0xFF));
                    out.push_back(uint8_t((srv_port >> 8) & 0xFF));
                    wr_str(host_ip);
                }
                sendto(sock, out.data(), out.size(), 0, (sockaddr*)&peer, plen);
            }
        }
        close(sock);
    }).detach();
}

// ---------- RCON (RconThread / RconClient) ----------
inline void start_rcon_thread() {
    if (props["enable-rcon"] != "true") return;
    int port = std::stoi(props["rcon.port"]);
    if (port <= 0 || port > 65535) {
        log_warn("Invalid rcon port %d found in server.properties, rcon disabled!", port);
        return;
    }
    std::string password = props["rcon.password"];
    if (password.empty()) {
        log_warn("No rcon password set in server.properties, rcon disabled!");
        return;
    }
    std::string sip = props["server-ip"].empty() ? "0.0.0.0" : props["server-ip"];
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return;
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = INADDR_ANY;
    if (sip != "0.0.0.0") inet_pton(AF_INET, sip.c_str(), &addr.sin_addr);
    if (bind(srv, (sockaddr*)&addr, sizeof addr) < 0 || listen(srv, 16) < 0) {
        log_warn("Unable to initialise RCON on %s:%d", sip.c_str(), port);
        close(srv);
        return;
    }
    log_info("RCON running on %s:%d", sip.c_str(), port);
    std::thread([srv, password] {
        while (shutdown_requested == 0) {
            pollfd pfd{srv, POLLIN, 0};
            if (poll(&pfd, 1, 500) <= 0) continue;
            int cfd = accept(srv, nullptr, nullptr);
            if (cfd < 0) continue;
            std::thread([cfd, password] {
                auto send_rcon = [cfd](int32_t req_id, int32_t type, const std::string& body) {
                    int32_t len = int32_t(body.size()) + 10;
                    std::vector<uint8_t> pkt(size_t(len + 4));
                    auto put_le = [&](size_t off, int32_t v) {
                        uint32_t u = uint32_t(v);
                        pkt[off] = uint8_t(u);
                        pkt[off + 1] = uint8_t(u >> 8);
                        pkt[off + 2] = uint8_t(u >> 16);
                        pkt[off + 3] = uint8_t(u >> 24);
                    };
                    put_le(0, len);
                    put_le(4, req_id);
                    put_le(8, type);
                    memcpy(pkt.data() + 12, body.data(), body.size());
                    pkt[12 + body.size()] = 0;
                    pkt[13 + body.size()] = 0;
                    (void)!send(cfd, pkt.data(), pkt.size(), 0);
                };
                bool authed = false;
                uint8_t buf[1460];
                for (;;) {
                    ssize_t r = recv(cfd, buf, sizeof buf, 0);
                    if (r < 14) break;
                    auto get_le = [&](size_t off) {
                        return int32_t(uint32_t(buf[off]) | (uint32_t(buf[off + 1]) << 8) |
                                       (uint32_t(buf[off + 2]) << 16) | (uint32_t(buf[off + 3]) << 24));
                    };
                    int32_t pktsize = get_le(0);
                    if (pktsize < 10 || pktsize != r - 4) break;
                    int32_t req_id = get_le(4);
                    int32_t cmd_type = get_le(8);
                    size_t slen = 0;
                    while (12 + slen < size_t(r) && buf[12 + slen] != 0) slen++;
                    std::string payload(reinterpret_cast<const char*>(buf + 12), slen);
                    if (cmd_type == 3) {  // SERVERDATA_AUTH
                        if (!payload.empty() && payload == password) {
                            authed = true;
                            send_rcon(req_id, 2, "");
                        } else {
                            authed = false;
                            send_rcon(-1, 2, "");
                        }
                    } else if (cmd_type == 2) {  // SERVERDATA_EXECCOMMAND
                        if (!authed) {
                            send_rcon(-1, 2, "");
                            continue;
                        }
                        std::string line = payload;
                        if (!line.empty() && line[0] == '/') line.erase(0, 1);
                        std::promise<std::string> done;
                        auto fut = done.get_future();
                        run_on_tick([&done, line] {
                            std::string out;
                            Source src = console_source();
                            src.rcon_out = &out;
                            if (!line.empty()) perform(src, line);
                            done.set_value(std::move(out));
                        });
                        std::string resp = fut.get();
                        size_t off = 0;
                        do {
                            size_t chunk = std::min<size_t>(4096, resp.size() - off);
                            send_rcon(req_id, 0, resp.substr(off, chunk));
                            off += chunk;
                        } while (off < resp.size());
                    } else {
                        char msg[64];
                        snprintf(msg, sizeof msg, "Unknown request %x", unsigned(cmd_type));
                        send_rcon(req_id, 0, msg);
                    }
                }
                close(cfd);
            }).detach();
        }
        close(srv);
    }).detach();
}

}  // namespace netsec
