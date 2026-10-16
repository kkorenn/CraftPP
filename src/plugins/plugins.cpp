// Bukkit/Paper plugins on an embedded JVM. Design + threading: docs/plugins.md.
//
//   C++ -> Java: cached jmethodIDs on prism.bridge.Main (request / move / tick), called on the caller's
//                thread (attached on first use, detached when the thread ends).
//   Java -> C++: natives on prism.bridge.Native registered with RegisterNatives (call / notify0 / log /
//                playerPos / sendMessage / packet). The long tail of API calls ("api" with {m, a}) is JSON
//                over byte arrays; hot paths (position, messages, move events, ticks) are typed.
//
// libjvm is dlopen'd only when plugins/*.jar exist, so the binary never links against Java.
#include "plugins.hpp"

#include "../play.hpp"
#include "../worldgen/json.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>

#include <optional>
#include <set>
#include <unordered_map>

#if CRAFTPP_JNI
#include <jni.h>
#endif

namespace plugins {
namespace {

// ---------- text: legacy § codes -> text component ----------

std::string strip_codes(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (uint8_t(s[i]) == 0xC2 && i + 1 < s.size() && uint8_t(s[i + 1]) == 0xA7) {  // "§" in UTF-8
            i += 2;  // skip the code char too
            continue;
        }
        o += s[i];
    }
    return o;
}

// {text:"", extra:[{text, color, bold, ...}, ...]} as network NBT.
void legacy_component(Writer& w, const std::string& s) {
    if (s.find("\xC2\xA7") == std::string::npos) {
        text_component(w, s);
        return;
    }
    static const char* colors[16] = {"black", "dark_blue", "dark_green", "dark_aqua", "dark_red", "dark_purple",
                                     "gold", "gray", "dark_gray", "blue", "green", "aqua", "red", "light_purple",
                                     "yellow", "white"};
    struct Seg { std::string text, color; bool b = false, i = false, u = false, st = false, ob = false; };
    std::vector<Seg> segs(1);
    for (size_t k = 0; k < s.size(); k++) {
        if (uint8_t(s[k]) == 0xC2 && k + 2 < s.size() && uint8_t(s[k + 1]) == 0xA7) {
            char c = char(std::tolower(uint8_t(s[k + 2])));
            k += 2;
            Seg next = segs.back();
            next.text.clear();
            int hex = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (hex >= 0) next = Seg{"", colors[hex]};
            else if (c == 'r') next = Seg{};
            else if (c == 'l') next.b = true;
            else if (c == 'o') next.i = true;
            else if (c == 'n') next.u = true;
            else if (c == 'm') next.st = true;
            else if (c == 'k') next.ob = true;
            else if (c == 'x' && k + 12 < s.size()) {  // §x§R§R§G§G§B§B (Bungee hex)
                std::string rgb = "#";
                for (int j = 0; j < 6; j++) rgb += char(std::tolower(uint8_t(s[k + 3 + j * 3])));
                k += 18;
                next = Seg{"", rgb};
            }
            if (segs.back().text.empty()) segs.back() = next;
            else segs.push_back(next);
            continue;
        }
        segs.back().text += s[k];
    }
    auto nbt_str = [&](const std::string& v) { w.u8(uint8_t(v.size() >> 8)).u8(uint8_t(v.size())); w.bytes(v.data(), v.size()); };
    auto flag = [&](const char* name, bool on) { if (on) { w.u8(0x01); nbt_str(name); w.u8(1); } };
    w.u8(0x0A);
    w.u8(0x08); nbt_str("text"); nbt_str("");
    w.u8(0x09); nbt_str("extra"); w.u8(0x0A).i32(int32_t(segs.size()));
    for (auto& g : segs) {
        w.u8(0x08); nbt_str("text"); nbt_str(g.text);
        if (!g.color.empty()) { w.u8(0x08); nbt_str("color"); nbt_str(g.color); }
        flag("bold", g.b); flag("italic", g.i); flag("underlined", g.u); flag("strikethrough", g.st); flag("obfuscated", g.ob);
        w.u8(0x00);
    }
    w.u8(0x00);
}

void send_legacy(Player& p, const std::string& text, bool action_bar = false) {
    Writer w;
    w.varint(action_bar ? play_cb::set_action_bar_text : play_cb::system_chat);
    legacy_component(w, text);
    if (!action_bar) w.boolean(false);
    p.send(w);
}

void broadcast_legacy(const std::string& text) {
    printf("[chat] %s\n", strip_codes(text).c_str());
    fflush(stdout);
    Writer w;
    w.varint(play_cb::system_chat);
    legacy_component(w, text);
    broadcast(w.boolean(false));
}

void kick_player(Player& p, const std::string& reason) {
    Writer w;
    w.varint(play_cb::disconnect);
    legacy_component(w, reason);
    try { p.send(w); p.out.flush(); } catch (...) {}
    printf("%s was kicked: %s\n", p.name.c_str(), strip_codes(reason).c_str());
    fflush(stdout);
    shutdown(p.fd, SHUT_RDWR);  // the connection thread sees EOF and runs the normal leave path
}

// ---------- JSON helpers ----------

std::string q(const std::string& s) { return "\"" + json_escape(s) + "\""; }
std::string num(double v) {
    if (!std::isfinite(v)) return "0";
    char b[32];
    snprintf(b, sizeof b, "%.17g", v);
    return b;
}
std::string text_arg(const Json& a, std::string_view k, const std::string& def = "") {
    auto* v = a.find(k);
    return v && v->isString() ? v->asString() : def;
}
double num_arg(const Json& a, std::string_view k, double def = 0) {
    auto* v = a.find(k);
    return v && v->isNumber() ? v->asDouble() : def;
}

std::string uuid_str(int64_t hi, int64_t lo) {
    char b[40];
    auto h = uint64_t(hi), l = uint64_t(lo);
    snprintf(b, sizeof b, "%08llx-%04llx-%04llx-%04llx-%012llx", (unsigned long long)(h >> 32),
             (unsigned long long)(h >> 16 & 0xFFFF), (unsigned long long)(h & 0xFFFF), (unsigned long long)(l >> 48),
             (unsigned long long)(l & 0xFFFFFFFFFFFFULL));
    return b;
}
bool parse_uuid(const std::string& s, int64_t& hi, int64_t& lo) {
    std::string hex;
    for (char c : s) if (c != '-') hex += c;
    if (hex.size() != 32) return false;
    hi = int64_t(std::stoull(hex.substr(0, 16), nullptr, 16));
    lo = int64_t(std::stoull(hex.substr(16), nullptr, 16));
    return true;
}
PlayerPtr player_by_uuid(int64_t hi, int64_t lo) {
    for (auto& p : snapshot())
        if (p->uuid_hi == hi && p->uuid_lo == lo) return p;
    return nullptr;
}
PlayerPtr player_arg(const Json& a, std::string_view key = "uuid") {
    int64_t hi, lo;
    auto* v = a.find(key);
    if (v && v->isString() && parse_uuid(v->asString(), hi, lo)) return player_by_uuid(hi, lo);
    return nullptr;
}
Player& require_player(const Json& a) {
    static thread_local PlayerPtr keep;  // keeps the player alive for the duration of the call
    keep = player_arg(a);
    if (!keep) throw std::runtime_error("Player is not online");
    return *keep;
}

// ---------- ops.json (vanilla format) ----------

std::mutex ops_mtx;
std::vector<std::pair<std::string, std::string>> ops;  // (uuid, name)

void load_ops() {
    std::lock_guard lock(ops_mtx);
    ops.clear();
    try {
        Json j = Json::parseFile("ops.json");
        for (auto& e : j.arr) ops.emplace_back(text_arg(e, "uuid"), text_arg(e, "name"));
    } catch (...) {}
}
void save_ops() {
    std::string s = "[\n";
    for (size_t i = 0; i < ops.size(); i++)
        s += "  {\"uuid\": " + q(ops[i].first) + ", \"name\": " + q(ops[i].second) + ", \"level\": 4, \"bypassesPlayerLimit\": false}" +
             (i + 1 < ops.size() ? ",\n" : "\n");
    std::ofstream("ops.json") << s << "]\n";
}
bool is_op(const Player& p) {
    std::string u = uuid_str(p.uuid_hi, p.uuid_lo);
    std::lock_guard lock(ops_mtx);
    for (auto& [id, name] : ops)
        if (id == u || strcasecmp(name.c_str(), p.name.c_str()) == 0) return true;
    return false;
}

std::string player_json(const Player& p) {
    return "{\"uuid\":" + q(uuid_str(p.uuid_hi, p.uuid_lo)) + ",\"name\":" + q(p.name) + ",\"x\":" + num(p.x) +
           ",\"y\":" + num(p.y) + ",\"z\":" + num(p.z) + ",\"yaw\":" + num(p.yaw) + ",\"pitch\":" + num(p.pitch) +
           ",\"gamemode\":" + std::to_string(p.gamemode) + ",\"op\":" + (is_op(p) ? "true" : "false") +
           ",\"health\":20,\"food\":20,\"level\":0,\"exp\":0,\"totalExp\":0,\"ip\":\"127.0.0.1\",\"port\":0" +
           ",\"entityId\":" + std::to_string(p.eid) + ",\"onGround\":" + (p.on_ground ? "true" : "false") +
           ",\"ping\":0,\"heldSlot\":" + std::to_string(p.selected) + "}";
}
std::string player_ref(const Player& p) {
    return "\"uuid\":" + q(uuid_str(p.uuid_hi, p.uuid_lo)) + ",\"name\":" + q(p.name);
}

// ---------- block states (table shipped in the bridge jar: block-states-26.3.txt) ----------

struct BlockInfo {
    std::string name;
    int first = 0, def = 0;
    std::vector<std::pair<std::string, std::vector<std::string>>> props;
};
std::vector<BlockInfo> block_table;
std::unordered_map<std::string, int> block_by_name;
std::vector<std::string> item_names;
std::unordered_map<std::string, int> item_by_name;

void load_block_table(const std::string& text) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        BlockInfo b;
        std::string props;
        ls >> b.name >> b.first >> b.def >> props;
        std::istringstream ps(props);
        for (std::string prop; std::getline(ps, prop, ';');) {
            auto eq = prop.find('=');
            std::vector<std::string> vals;
            std::istringstream vs(prop.substr(eq + 1));
            for (std::string v; std::getline(vs, v, ',');) vals.push_back(v);
            b.props.emplace_back(prop.substr(0, eq), vals);
        }
        block_by_name[b.name] = int(block_table.size());
        block_table.push_back(std::move(b));
    }
}
const BlockInfo* block_of(int state) {
    auto it = std::upper_bound(block_table.begin(), block_table.end(), state,
                               [](int s, const BlockInfo& b) { return s < b.first; });
    return it == block_table.begin() ? nullptr : &*std::prev(it);
}
std::string block_name(int state) {
    auto* b = block_of(state);
    return b ? b->name : "minecraft:air";
}
std::string norm_id(std::string s) {
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s.find(':') == std::string::npos ? "minecraft:" + s : s;
}
// Property value indices of a state (sorted-name order, last property varies fastest).
std::vector<int> decode(const BlockInfo& b, int state) {
    std::vector<int> idx(b.props.size());
    int r = state - b.first;
    for (size_t i = b.props.size(); i-- > 0;) {
        idx[i] = r % int(b.props[i].second.size());
        r /= int(b.props[i].second.size());
    }
    return idx;
}
int encode(const BlockInfo& b, const std::vector<int>& idx) {
    int r = 0;
    for (size_t i = 0; i < b.props.size(); i++) r = r * int(b.props[i].second.size()) + idx[i];
    return b.first + r;
}
// Default state of `name` with `props` overridden; -1 for an unknown block.
int resolve_state(const std::string& name, const std::vector<std::pair<std::string, std::string>>& props = {}) {
    auto it = block_by_name.find(norm_id(name));
    if (it == block_by_name.end()) return -1;
    const BlockInfo& b = block_table[size_t(it->second)];
    auto idx = decode(b, b.def);
    for (auto& [k, v] : props)
        for (size_t i = 0; i < b.props.size(); i++)
            if (b.props[i].first == k) {
                auto& vals = b.props[i].second;
                auto f = std::find(vals.begin(), vals.end(), v);
                if (f == vals.end()) throw std::runtime_error("Invalid value " + v + " for property " + k + " of " + b.name);
                idx[i] = int(f - vals.begin());
            }
    return encode(b, idx);
}
std::string block_data_json(int state) {
    auto* b = block_of(state);
    if (!b) return "null";
    auto idx = decode(*b, state);
    std::string props, allowed;
    for (size_t i = 0; i < b->props.size(); i++) {
        auto& [k, vals] = b->props[i];
        props += (i ? "," : "") + q(k) + ":" + q(vals[size_t(idx[i])]);
        allowed += (i ? "," : "") + q(k) + ":[";
        for (size_t j = 0; j < vals.size(); j++) allowed += (j ? "," : "") + q(vals[j]);
        allowed += "]";
    }
    return "{\"block\":" + q(b->name) + ",\"state\":" + std::to_string(state) + ",\"properties\":{" + props +
           "},\"allowed\":{" + allowed + "}}";
}
int state_from_args(const Json& a) {
    std::vector<std::pair<std::string, std::string>> props;
    if (auto* p = a.find("properties"); p && p->isObject())
        for (auto& [k, v] : p->obj) props.emplace_back(k, v.isString() ? v.asString() : v.str);
    int s = resolve_state(text_arg(a, "block", "minecraft:air"), props);
    if (s < 0) throw std::runtime_error("Unknown block " + text_arg(a, "block"));
    return s;
}

std::string item_json(const Player::Slot& s) {
    if (s.item < 0 || s.item >= int(item_names.size())) return "null";
    return "{\"item\":" + q(item_names[size_t(s.item)]) + ",\"count\":" + std::to_string(s.count) + "}";
}

// ---------- shared plugin state ----------

std::atomic<bool> running{false};
std::mutex state_mtx;
std::map<std::string, bool> listeners;           // hookable event -> some plugin listens (bridge "listeners" notify)
std::set<std::string> labels;                    // plugin command roots (name, aliases, plugin:name)
std::vector<std::pair<std::string, std::string>> plugin_list;  // (name, version)
std::map<std::string, std::string> plugin_data;  // persistent data blobs (session only)

bool has(const std::string& hook) {
    std::lock_guard lock(state_mtx);
    auto it = listeners.find(hook);
    return it != listeners.end() && it->second;
}
bool is_label(const std::string& line) {
    std::string label = line.substr(0, line.find(' '));
    for (auto& c : label) c = char(std::tolower(uint8_t(c)));
    std::lock_guard lock(state_mtx);
    return labels.count(label) > 0;
}

void log_line(const std::string& level, const std::string& logger, const std::string& msg) {
    std::istringstream in(strip_codes(msg));
    for (std::string line; std::getline(in, line);) {
        std::string prefix = level == "INFO" ? "" : level + ": ";
        if (!logger.empty()) prefix += "[" + logger + "] ";
        fputs((prefix + line + "\n").c_str(), level == "ERROR" ? stderr : stdout);
    }
    fflush(stdout);
}

// ---------- JVM ----------

#if CRAFTPP_JNI
JavaVM* vm = nullptr;
jclass main_cls;
jmethodID m_request, m_move, m_tick, m_exit, m_dump_cds;

struct ThreadEnv {
    JNIEnv* env = nullptr;
    bool attached = false;
    ~ThreadEnv() { if (attached && vm) vm->DetachCurrentThread(); }
};
thread_local ThreadEnv tenv;

JNIEnv* env() {
    if (tenv.env) return tenv.env;
    JNIEnv* e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_8) != JNI_OK) {
        vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&e), nullptr);
        tenv.attached = true;
    }
    tenv.env = e;
    return e;
}

jbyteArray to_bytes(JNIEnv* e, const std::string& s) {
    jbyteArray a = e->NewByteArray(jsize(s.size()));
    e->SetByteArrayRegion(a, 0, jsize(s.size()), reinterpret_cast<const jbyte*>(s.data()));
    return a;
}
std::string from_bytes(JNIEnv* e, jbyteArray a) {
    if (!a) return {};
    std::string s(size_t(e->GetArrayLength(a)), '\0');
    e->GetByteArrayRegion(a, 0, jsize(s.size()), reinterpret_cast<jbyte*>(s.data()));
    return s;
}
// Pending Java exception -> C++ string (and cleared).
std::string take_exception(JNIEnv* e) {
    jthrowable t = e->ExceptionOccurred();
    if (!t) return {};
    e->ExceptionClear();
    jclass obj = e->FindClass("java/lang/Object");
    jstring s = jstring(e->CallObjectMethod(t, e->GetMethodID(obj, "toString", "()Ljava/lang/String;")));
    const char* c = s ? e->GetStringUTFChars(s, nullptr) : nullptr;
    std::string r = c ? c : "unknown Java exception";
    if (c) e->ReleaseStringUTFChars(s, c);
    return r;
}

// C++ -> bridge request (Main.request). Throws std::runtime_error with the Java exception text.
Json request(const std::string& type, const std::string& json = "") {
    if (!vm) throw std::runtime_error("plugins are not running");
    JNIEnv* e = env();
    e->PushLocalFrame(16);
    auto r = jbyteArray(e->CallStaticObjectMethod(main_cls, m_request, to_bytes(e, type), json.empty() ? nullptr : to_bytes(e, json)));
    std::string err = take_exception(e), out = err.empty() ? from_bytes(e, r) : "";
    e->PopLocalFrame(nullptr);
    if (!err.empty()) throw std::runtime_error(err);
    return out.empty() ? Json{} : Json::parse(out);
}

std::string resource(const std::string& name) {
    JNIEnv* e = env();
    e->PushLocalFrame(8);
    jmethodID m = e->GetStaticMethodID(main_cls, "resource", "(Ljava/lang/String;)[B");
    auto r = jbyteArray(e->CallStaticObjectMethod(main_cls, m, e->NewStringUTF(name.c_str())));
    std::string out = take_exception(e).empty() ? from_bytes(e, r) : "";
    e->PopLocalFrame(nullptr);
    return out;
}
std::vector<std::string> registry_names(const std::string& registry) {
    JNIEnv* e = env();
    e->PushLocalFrame(16);
    jmethodID m = e->GetStaticMethodID(main_cls, "registryNames", "(Ljava/lang/String;)[Ljava/lang/String;");
    auto arr = jobjectArray(e->CallStaticObjectMethod(main_cls, m, e->NewStringUTF(registry.c_str())));
    std::vector<std::string> out;
    if (take_exception(e).empty() && arr)
        for (jsize i = 0, n = e->GetArrayLength(arr); i < n; i++) {
            auto s = jstring(e->GetObjectArrayElement(arr, i));
            const char* c = s ? e->GetStringUTFChars(s, nullptr) : nullptr;
            out.push_back(c ? c : "");
            if (c) e->ReleaseStringUTFChars(s, c);
            e->DeleteLocalRef(s);
        }
    e->PopLocalFrame(nullptr);
    return out;
}
#else
Json request(const std::string&, const std::string& = "") { throw std::runtime_error("built without JNI"); }
#endif

// Event round trip; null Json when the bridge failed (logged).
Json event(const std::string& hook, const std::string& payload) {
    try {
        return request("event", "{\"e\":" + q(hook) + ",\"p\":" + payload + "}");
    } catch (const std::exception& e) {
        log_line("ERROR", "", "Plugin event " + hook + " failed: " + e.what());
        return Json{};
    }
}
bool cancelled(const Json& r) { auto* c = r.find("cancelled"); return c && c->isBool() && c->asBool(); }

// ---------- commands ----------

void reply_to(Player* p, const std::string& text) {
    if (p) send_legacy(*p, text);
    else log_line("INFO", "", text);
}

void refresh_labels() {
    try {
        Json r = request("visible", "{\"uuid\":\"00000000-0000-0000-0000-000000000000\",\"name\":\"CONSOLE\",\"op\":true}");
        std::lock_guard lock(state_mtx);
        if (auto* l = r.find("labels"))
            for (auto& v : l->arr) labels.insert(v.asString());
    } catch (const std::exception& e) {
        log_line("WARN", "", std::string("Could not list plugin commands: ") + e.what());
    }
}

// /plugins and /version (Paper's own commands, answered from the bridge's plugin list).
bool server_command(Player* p, const std::string& line) {
    std::string label = line.substr(0, line.find(' '));
    if (label == "plugins" || label == "pl") {
        std::string s;
        for (auto& [n, v] : plugin_list) s += (s.empty() ? "" : "\xC2\xA7" "f, ") + std::string("\xC2\xA7" "a") + n;
        reply_to(p, "\xC2\xA7" "fServer Plugins (" + std::to_string(plugin_list.size()) + "):");
        if (!s.empty()) reply_to(p, "\xC2\xA7" "6Bukkit Plugins:\n\xC2\xA7" "f - " + s);
        return true;
    }
    if (label == "version" || label == "ver" || label == "about") {
        reply_to(p, std::string("This server is running CraftPP version 26.3-CraftPP (MC: ") + GAME_VERSION +
                        ") (Implementing API version 26.3-R0.1-SNAPSHOT)");
        return true;
    }
    return false;
}

// Plugin command through Bukkit's command map; false when no plugin owns the label.
bool dispatch(Player* p, const std::string& line) {
    if (!is_label(line)) return false;
    try {
        Json r = request("dispatch", "{\"sender\":" + (p ? q(uuid_str(p->uuid_hi, p->uuid_lo)) : std::string("null")) +
                                         ",\"line\":" + q(line) + "}");
        auto* h = r.find("handled");
        return h && h->isBool() && h->asBool();
    } catch (const std::exception& e) {
        log_line("ERROR", "", "Plugin command /" + line + " failed: " + e.what());
        reply_to(p, "\xC2\xA7" "cAn internal error occurred while attempting to perform this command");
        return true;
    }
}

// A player's command line: PlayerCommandPreprocessEvent, then plugin commands, then `fallback` (builtins).
void player_command(Player& p, std::string line, const std::function<void(const std::string&)>& fallback) {
    if (has("command")) {
        Json r = event("command", "{" + player_ref(p) + ",\"line\":" + q("/" + line) + "}");
        if (cancelled(r)) return;
        if (auto* l = r.find("line"); l && l->isString()) line = l->asString()[0] == '/' ? l->asString().substr(1) : l->asString();
    }
    if (server_command(&p, line) || dispatch(&p, line)) {
        printf("%s issued server command: /%s\n", p.name.c_str(), line.c_str());
        fflush(stdout);
        return;
    }
    fallback(line);
}

void chat(const PlayerPtr& pp, const std::string& msg) {
    if (!msg.empty() && msg[0] == '/') {
        player_command(*pp, msg.substr(1), [&](const std::string& l) { run_command(pp.get(), l); });
        return;
    }
    std::string fallback = "<" + pp->name + "> " + msg;
    if (!has("chat") && !has("legacychat")) return broadcast_legacy(fallback);
    Json r = event("chat", "{" + player_ref(*pp) + ",\"message\":" + q(msg) + "}");
    if (cancelled(r)) return;
    auto* line = r.find("line");
    broadcast_legacy(line && line->isString() ? line->asString() : fallback);
}

// Appends plugin roots (literal + greedy "args" asking the server for suggestions, Paper's shape for
// plugin.yml commands) to the Brigadier tree built by commands.hpp. Root must be node 0, root index last.
void patch_command_tree() {
    std::vector<std::string> roots;
    {
        std::lock_guard lock(state_mtx);
        roots.assign(labels.begin(), labels.end());
    }
    if (roots.empty() || commands_packet.empty()) return;
    Reader frame(commands_packet);
    int len = frame.varint();
    size_t body = frame.i;
    if (size_t(len) != commands_packet.size() - body || commands_packet.back() != 0) return;
    if (frame.varint() != play_cb::commands) return;
    int count = frame.varint();
    uint8_t root_flags = frame.u8();
    int nchild = frame.varint();
    std::vector<int> children;
    for (int i = 0; i < nchild; i++) children.push_back(frame.varint());
    if ((root_flags & 3) != 0) return;
    size_t rest = frame.i;
    Writer w;
    w.varint(play_cb::commands).varint(count + 2 * int(roots.size()));
    w.u8(root_flags);
    for (size_t i = 0; i < roots.size(); i++) children.push_back(count + 2 * int(i));
    w.varint(int32_t(children.size()));
    for (int c : children) w.varint(c);
    w.bytes(commands_packet.data() + rest, commands_packet.size() - 1 - rest);  // other nodes, minus root index
    for (size_t i = 0; i < roots.size(); i++) {
        w.u8(0x01 | 0x04).varint(1).varint(count + 2 * int(i) + 1).str(roots[i]);  // literal, executable
        w.u8(0x02 | 0x04 | 0x10).varint(0).str("args").varint(arg_types::string_).varint(2).str("minecraft:ask_server");
    }
    w.varint(0);
    Writer f;
    f.varint(int32_t(w.b.size())).bytes(w.b);
    commands_packet = f.b;
}

// ---------- API calls from plugins (Java -> C++, "api" {m, a}) ----------

std::string api(const std::string& m, const Json& a) {
    // server
    if (m == "server.players") {
        std::string s = "[";
        for (auto& p : snapshot()) s += (s.size() > 1 ? "," : "") + player_json(*p);
        return s + "]";
    }
    if (m == "server.motd") return q(props["motd"]);
    if (m == "server.broadcast") { broadcast_legacy(text_arg(a, "text")); return "null"; }
    if (m == "server.dispatch") {  // Bukkit.dispatchCommand for a non-plugin command
        std::string line = text_arg(a, "line");
        if (!line.empty() && line[0] == '/') line = line.substr(1);
        PlayerPtr sender = player_arg(a, "sender");
        run_command(sender.get(), line);
        return "true";
    }
    if (m == "server.shutdown") { shutdown_requested = 1; return "null"; }
    if (m == "server.ops") {
        std::lock_guard lock(ops_mtx);
        std::string s = "[";
        for (auto& [u, n] : ops) s += (s.size() > 1 ? "," : "") + q(n);
        return s + "]";
    }
    if (m == "server.whitelist") return "[]";
    if (m == "server.hasWhitelist") return "false";
    if (m == "server.defaultGameMode") return std::to_string(std::max(0, gamemode_id(props["gamemode"])));
    if (m == "server.registries") return "null";  // the bridge uses its bundled 26.3 registry report

    // player
    if (m == "player.state") { auto p = player_arg(a); return p ? player_json(*p) : "null"; }
    if (m == "player.sendMessage") { send_legacy(require_player(a), text_arg(a, "text")); return "null"; }
    if (m == "player.actionBar") { send_legacy(require_player(a), text_arg(a, "text"), true); return "null"; }
    if (m == "player.title") {
        Player& p = require_player(a);
        p.send(Writer().varint(play_cb::set_titles_animation).i32(int(num_arg(a, "fadeIn", 10)))
                   .i32(int(num_arg(a, "stay", 70))).i32(int(num_arg(a, "fadeOut", 20))));
        Writer sub, title;
        legacy_component(sub.varint(play_cb::set_subtitle_text), text_arg(a, "subtitle"));
        legacy_component(title.varint(play_cb::set_title_text), text_arg(a, "title"));
        p.send(sub);
        p.send(title);
        return "null";
    }
    if (m == "player.resetTitle") { require_player(a).send(Writer().varint(play_cb::clear_titles).boolean(true)); return "null"; }
    if (m == "player.kick") { kick_player(require_player(a), text_arg(a, "text", "Kicked from the server.")); return "null"; }
    if (m == "player.teleport") {
        Player& p = require_player(a);
        p.yaw = float(num_arg(a, "yaw", p.yaw));
        p.pitch = float(num_arg(a, "pitch", p.pitch));
        teleport(p, num_arg(a, "x"), num_arg(a, "y"), num_arg(a, "z"));
        return "null";
    }
    if (m == "player.setGameMode") { set_gamemode(require_player(a), std::clamp(int(num_arg(a, "mode")), 0, 3)); return "null"; }
    if (m == "player.setOp") {
        auto p = player_arg(a);
        std::string name = p ? p->name : text_arg(a, "name");
        std::string uuid = p ? uuid_str(p->uuid_hi, p->uuid_lo) : text_arg(a, "uuid");
        bool op = a["op"].asBool();
        std::lock_guard lock(ops_mtx);
        std::erase_if(ops, [&](auto& o) { return o.first == uuid || strcasecmp(o.second.c_str(), name.c_str()) == 0; });
        if (op) ops.emplace_back(uuid, name);
        save_ops();
        return "null";
    }
    if (m == "player.sendBlockChange") {
        int s = resolve_state(text_arg(a, "block", "minecraft:air"));
        require_player(a).send(block_update(int(num_arg(a, "x")), int(num_arg(a, "y")), int(num_arg(a, "z")), std::max(s, 0)));
        return "null";
    }
    if (m == "player.chat") {
        auto p = player_arg(a);
        if (!p) throw std::runtime_error("Player is not online");
        chat(p, text_arg(a, "text"));
        return "null";
    }
    if (m == "player.closeInventory") return "null";  // CraftPP never opens containers
    if (m == "player.inventory") {  // protocol (menu) slot order 0-45
        Player& p = require_player(a);
        std::string s = "{\"heldSlot\":" + std::to_string(p.selected) + ",\"window\":null,\"items\":[";
        for (int slot = 0; slot < 46; slot++) {
            int i = menu_to_inv(slot);
            s += (slot ? "," : "") + (i < 0 ? std::string("null") : item_json(p.inv[size_t(i)]));
        }
        return s + "]}";
    }
    if (m == "player.setSlot") {
        Player& p = require_player(a);
        int i = menu_to_inv(int(num_arg(a, "slot", -1)));
        if (i < 0) throw std::runtime_error("Invalid inventory slot");
        Player::Slot slot;
        if (auto* it = a.find("item"); it && it->isObject()) {
            auto f = item_by_name.find(norm_id(text_arg(*it, "item")));
            if (f != item_by_name.end() && f->second > 0) slot = {f->second, std::max(1, int(num_arg(*it, "count", 1)))};
        }  // ponytail: item meta (name/lore/enchants) is dropped; CraftPP slots hold item + count only
        p.inv[size_t(i)] = slot;
        send_slot(p, i);
        return "null";
    }
    if (m == "player.setHeldSlot") {
        Player& p = require_player(a);
        p.selected = std::clamp(int(num_arg(a, "slot")), 0, 8);
        p.send(Writer().varint(play_cb::set_held_slot).varint(p.selected));
        return "null";
    }
    if (m == "player.setPluginData" || m == "world.setPluginData" || m == "world.setChunkPluginData" || m == "entity.setPluginData") {
        std::lock_guard lock(state_mtx);  // ponytail: kept for the session only; persist with the world format
        plugin_data[m.substr(0, m.find('.')) + text_arg(a, "uuid") + std::to_string(num_arg(a, "x")) +
                    std::to_string(num_arg(a, "z"))] = text_arg(a, "data");
        return "null";
    }
    if (m == "world.pluginData" || m == "world.chunkPluginData") {
        std::lock_guard lock(state_mtx);
        auto it = plugin_data.find("world" + text_arg(a, "uuid") + std::to_string(num_arg(a, "x")) + std::to_string(num_arg(a, "z")));
        return it == plugin_data.end() ? "null" : q(it->second);
    }

    // world
    int x = int(num_arg(a, "x")), y = int(num_arg(a, "y")), z = int(num_arg(a, "z"));
    if (m == "world.block") {
        int s = world.get_block(x, y, z);
        return "{\"block\":" + q(block_name(s)) + ",\"state\":" + std::to_string(s) + "}";
    }
    if (m == "world.blockData" || m == "world.highestBlock") return block_data_json(world.get_block(x, y, z));
    if (m == "world.setBlock" || m == "world.setBlockData") {
        if (y < MIN_Y || y >= MIN_Y + HEIGHT) return "false";
        change_block(x, y, z, state_from_args(a));
        return "true";
    }
    if (m == "world.blockDataOf") {  // "minecraft:oak_stairs[facing=north]"
        std::string text = text_arg(a, "text", "minecraft:air");
        auto br = text.find('[');
        std::vector<std::pair<std::string, std::string>> props;
        if (br != std::string::npos && text.back() == ']') {
            std::istringstream ps(text.substr(br + 1, text.size() - br - 2));
            for (std::string kv; std::getline(ps, kv, ',');)
                if (auto eq = kv.find('='); eq != std::string::npos) props.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        int s = resolve_state(text.substr(0, br), props);
        return s < 0 ? "null" : block_data_json(s);
    }
    if (m == "world.highestY") {
        for (int yy = MIN_Y + HEIGHT - 1; yy >= MIN_Y; yy--)
            if (world.get_block(x, yy, z) != blocks::air) return std::to_string(yy);
        return std::to_string(MIN_Y);
    }
    if (m == "world.spawn") return "{\"x\":" + std::to_string(world_spawn.x) + ",\"y\":" + std::to_string(world_spawn.y) +
                                   ",\"z\":" + std::to_string(world_spawn.z) + "}";
    if (m == "world.save") { world.save_dirty(); return "null"; }
    if (m == "world.lightAt") return "{\"sky\":15,\"block\":0}";  // matches what CraftPP sends: full sky light everywhere
    if (m == "world.border")  // CraftPP sends no border packets, so clients use the vanilla default
        return "{\"centerX\":0,\"centerZ\":0,\"size\":59999968,\"damageAmount\":0.2,\"damageBuffer\":5,\"warningDistance\":5,\"warningTime\":15}";
    if (m == "world.gameRules") return "[]";
    if (m == "world.gameRule") return "null";
    if (m == "entity.list") return "[]";  // CraftPP has no non-player entities yet
    throw std::runtime_error("CraftPP does not implement plugin API call " + m + " yet");
}

// ---------- natives (Java -> C++) ----------

#if CRAFTPP_JNI
void throw_java(JNIEnv* e, const std::string& msg) {
    e->ThrowNew(e->FindClass("java/lang/IllegalStateException"), msg.c_str());
}

jbyteArray JNICALL n_call(JNIEnv* e, jclass, jbyteArray type, jbyteArray json) {
    try {
        std::string t = from_bytes(e, type);
        if (t != "api") throw std::runtime_error("unknown request " + t);
        Json req = Json::parse(from_bytes(e, json));
        return to_bytes(e, api(req["m"].asString(), req.has("a") ? req["a"] : Json{}));
    } catch (const std::exception& ex) {
        throw_java(e, ex.what());
        return nullptr;
    }
}

void JNICALL n_notify(JNIEnv* e, jclass, jbyteArray type, jbyteArray json) {
    try {
        std::string t = from_bytes(e, type);
        if (t == "listeners") {
            Json j = Json::parse(from_bytes(e, json));
            std::lock_guard lock(state_mtx);
            for (auto& [k, v] : j.obj) listeners[k] = v.isBool() && v.asBool();
        }
        // "pipeline" (a plugin injected a Netty handler): serverbound interception is not wired yet.
    } catch (const std::exception& ex) {
        throw_java(e, ex.what());
    }
}

void JNICALL n_log(JNIEnv* e, jclass, jbyteArray level, jbyteArray logger, jbyteArray msg) {
    log_line(from_bytes(e, level), from_bytes(e, logger), from_bytes(e, msg));
}

jboolean JNICALL n_player_pos(JNIEnv* e, jclass, jlong hi, jlong lo, jdoubleArray out) {
    auto p = player_by_uuid(hi, lo);
    if (!p) return JNI_FALSE;
    jdouble v[5] = {p->x, p->y, p->z, p->yaw, p->pitch};
    e->SetDoubleArrayRegion(out, 0, 5, v);
    return JNI_TRUE;
}

void JNICALL n_send_message(JNIEnv* e, jclass, jlong hi, jlong lo, jbyteArray text) {
    if (auto p = player_by_uuid(hi, lo)) send_legacy(*p, from_bytes(e, text));
}

void JNICALL n_packet(JNIEnv* e, jclass, jlong hi, jlong lo, jbyteArray data) {
    auto p = player_by_uuid(hi, lo);
    if (!p) return;
    std::string b = from_bytes(e, data);
    Writer w;
    w.bytes(b.data(), b.size());
    p->send(w);
}

// ---------- JVM discovery + startup ----------

std::map<std::string, std::string> jvm_conf = {
    {"java-home", ""},          // empty: JAVA_HOME, then the usual JDK install folders
    {"max-heap", "256m"},       // -Xmx
    {"initial-heap", "16m"},    // -Xms
    {"c1-only", "true"},        // -XX:TieredStopAtLevel=1: less JIT memory/CPU, plugins rarely need C2
    {"metaspace", "128m"},      // -XX:MaxMetaspaceSize
    {"code-cache", "32m"},      // -XX:ReservedCodeCacheSize
    {"thread-stack", "512k"},   // -Xss
    {"cds", "true"},            // AppCDS archive in cache/, written on the first run
    {"bridge-jar", "build/java/libs/craftpp-bridge.jar"},
    {"extra-args", ""},         // space-separated extra JVM options
};
const char* const CONF_PATH = "plugins/jvm.properties";

void load_jvm_conf() {
    std::ifstream f(CONF_PATH);
    if (!f) {
        std::ofstream o(CONF_PATH);
        o << "# Embedded JVM for Bukkit/Paper plugins (see docs/plugins.md)\n";
        for (auto& [k, v] : jvm_conf) o << k << '=' << v << '\n';
        return;
    }
    for (std::string line; std::getline(f, line);)
        if (auto eq = line.find('='); !line.empty() && line[0] != '#' && eq != std::string::npos)
            jvm_conf[line.substr(0, eq)] = line.substr(eq + 1);
}

int java_major(const std::string& home) {  // from <home>/release: JAVA_VERSION="25.0.1"
    std::ifstream f(home + "/release");
    for (std::string line; std::getline(f, line);)
        if (line.rfind("JAVA_VERSION=\"", 0) == 0) return atoi(line.c_str() + 14);
    return 0;
}
std::string libjvm_of(const std::string& home) {
#ifdef __APPLE__
    std::string p = home + "/lib/server/libjvm.dylib";
#else
    std::string p = home + "/lib/server/libjvm.so";
#endif
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? p : "";
}
std::vector<std::string> subdirs(const std::string& dir, const std::string& suffix) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') out.push_back(dir + "/" + e->d_name + suffix);
        closedir(d);
    }
    std::sort(out.rbegin(), out.rend());  // newest-looking first
    return out;
}
// Paper 26.3's API is compiled for Java 25, so that's the floor.
std::string find_java_home(std::string& seen) {
    std::vector<std::string> c;
    if (!jvm_conf["java-home"].empty()) c.push_back(jvm_conf["java-home"]);
    if (const char* h = getenv("JAVA_HOME")) c.push_back(h);
    for (auto& d : subdirs("/Library/Java/JavaVirtualMachines", "/Contents/Home")) c.push_back(d);
    if (const char* h = getenv("HOME"))
        for (auto& d : subdirs(std::string(h) + "/Library/Java/JavaVirtualMachines", "/Contents/Home")) c.push_back(d);
    for (auto& d : subdirs("/usr/lib/jvm", "")) c.push_back(d);
    c.push_back("/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home");
    for (auto& h : c) {
        int major = java_major(h);
        if (major) seen += h + " (Java " + std::to_string(major) + ") ";
        if (major >= 25 && !libjvm_of(h).empty()) return h;
    }
    return "";
}

std::vector<std::string> plugin_jars() {
    std::vector<std::string> jars;
    for (auto& f : subdirs("plugins", ""))
        if (f.size() > 4 && f.compare(f.size() - 4, 4, ".jar") == 0) jars.push_back(f);
    return jars;
}

std::string file_key(const std::string& path) {  // size + mtime: changes whenever the file is rebuilt
    struct stat st{};
    stat(path.c_str(), &st);
    return std::to_string(st.st_size) + "-" + std::to_string(st.st_mtime);
}

// NMS: plugins that reference net/minecraft or org/bukkit/craftbukkit get Mojang's 26.3 server classes
// (downloaded + access-widened once into cache/ by prism.bridge.nms.Prepare in a short-lived child JVM).
std::vector<std::string> nms_classpath(const std::string& java_home, const std::string& bridge,
                                       const std::vector<std::string>& jars) {
    std::string key = file_key(bridge);
    for (auto& j : jars) key += " " + j + ":" + file_key(j);
    std::string cache = "cache/craftpp-nms.txt";
    auto read = [&](bool check) -> std::optional<std::vector<std::string>> {
        std::ifstream f(cache);
        std::string first;
        if (!std::getline(f, first) || (check && first != key)) return std::nullopt;
        std::vector<std::string> cp;
        for (std::string l; std::getline(f, l);) if (!l.empty()) cp.push_back(l);
        return cp;
    };
    if (auto cp = read(true)) return *cp;
    mkdir("cache", 0755);
    {
        std::ofstream k(cache + ".key");
        k << key << '\n';
    }
    std::string cmd = "'" + java_home + "/bin/java' -Xss1M -XX:TieredStopAtLevel=1 -XX:+UseSerialGC -cp '" + bridge +
                      "' prism.bridge.nms.Prepare . " + cache;
    for (auto& j : jars) cmd += " '" + j + "'";
    if (system(cmd.c_str()) != 0) {
        log_line("ERROR", "", "Could not prepare server internals for NMS plugins; they will fail to load");
        return {};
    }
    return read(false).value_or(std::vector<std::string>{});
}

bool boot_jvm() {
    auto jars = plugin_jars();
    if (jars.empty()) return false;
    load_jvm_conf();
    std::string seen, home = find_java_home(seen);
    if (home.empty()) {
        log_line("ERROR", "", "**** PLUGINS DISABLED: " + std::to_string(jars.size()) +
                                  " plugin jar(s) in plugins/ but no Java 25+ runtime found (" +
                                  (seen.empty() ? "none" : seen) + "). Install a JDK or set JAVA_HOME / java-home in " + CONF_PATH);
        return false;
    }
    std::string bridge = jvm_conf["bridge-jar"];
    struct stat st;
    if (stat(bridge.c_str(), &st) != 0) {
        log_line("ERROR", "", "**** PLUGINS DISABLED: " + bridge + " is missing; build it with tools/build_bridge.sh");
        return false;
    }
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> nms = nms_classpath(home, bridge, jars);
    std::string cp = bridge;
    for (auto& e : nms) cp += ":" + e;

    std::vector<std::string> opts = {
        "-Djava.class.path=" + cp,
        "-Xrs",  // CraftPP owns SIGINT/SIGTERM/SIGHUP/SIGQUIT; the JVM keeps only its internal signals
        "-Xms" + jvm_conf["initial-heap"], "-Xmx" + jvm_conf["max-heap"], "-Xss" + jvm_conf["thread-stack"],
        "-XX:+UseSerialGC", "-XX:MaxMetaspaceSize=" + jvm_conf["metaspace"],
        "-XX:ReservedCodeCacheSize=" + jvm_conf["code-cache"], "-Dfile.encoding=UTF-8", "-Dprismmc.bridge=true",
        "-Xlog:disable", "-Xlog:all=warning:stderr",
    };
    if (jvm_conf["c1-only"] == "true") opts.push_back("-XX:TieredStopAtLevel=1");
    if (!nms.empty()) {
        opts.push_back("-Dprismmc.nms=true");
        opts.push_back("--enable-final-field-mutation=ALL-UNNAMED");
    }
    std::string cds_file, cds_dump;
    if (jvm_conf["cds"] == "true") {
        mkdir("cache", 0755);
        std::string tag = file_key(bridge);
        for (auto& e : nms) tag += "-" + file_key(e);
        cds_file = "cache/craftpp-bridge-" + std::to_string(std::hash<std::string>{}(tag) & 0xFFFFFFFF) + "-" +
                   jvm_conf["c1-only"] + ".jsa";
        if (stat(cds_file.c_str(), &st) == 0) opts.push_back("-XX:SharedArchiveFile=" + cds_file);
        else { opts.push_back("-XX:+RecordDynamicDumpInfo"); cds_dump = cds_file; }
    }
    std::istringstream extra(jvm_conf["extra-args"]);
    for (std::string o; extra >> o;) opts.push_back(o);

    void* lib = dlopen(libjvm_of(home).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) { log_line("ERROR", "", std::string("Could not load libjvm: ") + dlerror()); return false; }
    using CreateFn = jint(JNICALL*)(JavaVM**, void**, void*);
    auto create = reinterpret_cast<CreateFn>(dlsym(lib, "JNI_CreateJavaVM"));
    std::vector<JavaVMOption> jopts;
    for (auto& o : opts) jopts.push_back({const_cast<char*>(o.c_str()), nullptr});
    JavaVMInitArgs args{};
    args.version = JNI_VERSION_1_8;
    args.nOptions = jint(jopts.size());
    args.options = jopts.data();
    args.ignoreUnrecognized = JNI_FALSE;
    JNIEnv* e = nullptr;
    if (!create || create(&vm, reinterpret_cast<void**>(&e), &args) != JNI_OK) {
        log_line("ERROR", "", "JNI_CreateJavaVM failed (" + home + ")");
        vm = nullptr;
        return false;
    }
    tenv.env = e;  // the creating thread is attached; it detaches in DestroyJavaVM semantics when it exits
    tenv.attached = true;

    jclass native_cls = e->FindClass("prism/bridge/Native");
    main_cls = jclass(e->NewGlobalRef(e->FindClass("prism/bridge/Main")));
    if (!native_cls || !main_cls) { log_line("ERROR", "", "bridge classes missing: " + take_exception(e)); return false; }
    JNINativeMethod natives[] = {
        {const_cast<char*>("call"), const_cast<char*>("([B[B)[B"), reinterpret_cast<void*>(n_call)},
        {const_cast<char*>("notify0"), const_cast<char*>("([B[B)V"), reinterpret_cast<void*>(n_notify)},
        {const_cast<char*>("log"), const_cast<char*>("([B[B[B)V"), reinterpret_cast<void*>(n_log)},
        {const_cast<char*>("playerPos"), const_cast<char*>("(JJ[D)Z"), reinterpret_cast<void*>(n_player_pos)},
        {const_cast<char*>("sendMessage"), const_cast<char*>("(JJ[B)V"), reinterpret_cast<void*>(n_send_message)},
        {const_cast<char*>("packet"), const_cast<char*>("(JJ[B)V"), reinterpret_cast<void*>(n_packet)},
    };
    if (e->RegisterNatives(native_cls, natives, jint(std::size(natives))) != 0) {
        log_line("ERROR", "", "RegisterNatives failed: " + take_exception(e));
        return false;
    }
    m_request = e->GetStaticMethodID(main_cls, "request", "([B[B)[B");
    m_move = e->GetStaticMethodID(main_cls, "move", "(JJDDDFFDDDFF)[D");
    m_tick = e->GetStaticMethodID(main_cls, "tick", "()V");
    m_exit = e->GetStaticMethodID(main_cls, "exit", "(I)V");
    m_dump_cds = e->GetStaticMethodID(main_cls, "dumpCds", "(Ljava/lang/String;)V");
    e->CallStaticVoidMethod(main_cls, e->GetStaticMethodID(main_cls, "boot", "()V"));
    if (auto err = take_exception(e); !err.empty()) { log_line("ERROR", "", "bridge boot failed: " + err); return false; }

    load_block_table(resource("block-states-26.3.txt"));
    item_names = registry_names("minecraft:item");
    for (size_t i = 0; i < item_names.size(); i++) item_by_name[item_names[i]] = int(i);

    log_line("INFO", "", "[PluginInitializerManager] Initializing plugins...");
    std::string cwd = std::filesystem::current_path().string();
    Json init = request("init", "{\"pluginsDir\":" + q(cwd + "/plugins") + ",\"serverDir\":" + q(cwd) +
                                    ",\"name\":\"CraftPP\",\"mcVersion\":" + q(GAME_VERSION) +
                                    ",\"version\":\"26.3-CraftPP\",\"bukkitVersion\":\"26.3-R0.1-SNAPSHOT\",\"motd\":" +
                                    q(props["motd"]) + ",\"maxPlayers\":" + props["max-players"] +
                                    ",\"port\":" + props["server-port"] + ",\"onlineMode\":false,\"viewDistance\":" +
                                    props["view-distance"] + ",\"simulationDistance\":" + props["view-distance"] +
                                    ",\"worldName\":" + q(props["level-name"]) + ",\"seed\":0}");
    Json en = request("enable");
    for (auto* r : {&init, &en})
        if (auto* l = r->find("plugins")) {
            plugin_list.clear();
            for (auto& pl : l->arr) plugin_list.emplace_back(text_arg(pl, "name"), text_arg(pl, "version"));
        }
    {
        std::lock_guard lock(state_mtx);
        if (auto* c = en.find("commands"))
            for (auto& cmd : c->arr) labels.insert(text_arg(cmd, "label"));
    }
    refresh_labels();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    log_line("INFO", "", "[PluginInitializerManager] Initialized " + std::to_string(plugin_list.size()) +
                             " plugin(s) in " + std::to_string(int(secs * 1000)) + " ms (Java " +
                             std::to_string(java_major(home)) + (nms.empty() ? "" : ", NMS") + (cds_dump.empty() && !cds_file.empty() ? ", CDS" : "") + ")");
    for (auto& [n, v] : plugin_list) log_line("INFO", "", " - " + n + " (" + v + ")");
    if (!cds_dump.empty()) {
        e = env();
        e->PushLocalFrame(4);
        e->CallStaticVoidMethod(main_cls, m_dump_cds, e->NewStringUTF(cds_dump.c_str()));
        take_exception(e);
        e->PopLocalFrame(nullptr);
    }
    return true;
}

// Runs `f` on a fresh thread with an 8 MiB stack (HotSpot wants more than macOS's 512 KiB default) and waits.
void with_big_stack(void (*f)()) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    pthread_t t;
    pthread_create(&t, &attr, [](void* fn) -> void* { reinterpret_cast<void (*)()>(fn)(); return nullptr; },
                   reinterpret_cast<void*>(f));
    pthread_join(t, nullptr);
    pthread_attr_destroy(&attr);
}

// PlayerMoveEvent via the typed path. Returns true when the client must be sent elsewhere (dest filled).
bool fire_move(Player& p, double tx, double ty, double tz, float tyaw, float tpitch, double dest[5]) {
    JNIEnv* e = env();
    e->PushLocalFrame(4);
    auto r = jdoubleArray(e->CallStaticObjectMethod(main_cls, m_move, jlong(p.uuid_hi), jlong(p.uuid_lo), p.x, p.y, p.z,
                                                    p.yaw, p.pitch, tx, ty, tz, tyaw, tpitch));
    std::string err = take_exception(e);
    bool moved = err.empty() && r;
    if (moved) e->GetDoubleArrayRegion(r, 0, 5, dest);
    e->PopLocalFrame(nullptr);
    if (!err.empty()) log_line("ERROR", "", "PlayerMoveEvent failed: " + err);
    return moved;
}
#endif  // CRAFTPP_JNI

// ---------- packet handler wrappers ----------

PacketHandler original(int id) {
    auto it = handlers().find(id);
    return it == handlers().end() ? PacketHandler{} : it->second;
}
PlayerPtr ptr_of(Player& p) {
    for (auto& q : snapshot())
        if (q.get() == &p) return q;
    return nullptr;
}

void install_handlers() {
    for (int id : {play_sb::chat_command, play_sb::chat_command_signed}) {
        auto orig = original(id);
        on(id, [orig](Player& p, Reader& in, int id) {
            Reader peek = in;
            std::string line = peek.str(32767);
            std::vector<uint8_t> rest(peek.p + peek.i, peek.p + peek.n);  // signature data of the signed variant
            player_command(p, line, [&](const std::string& l) {
                if (!orig) return;
                if (l == line) return orig(p, in, id);
                std::vector<uint8_t> buf = Writer().str(l).b;
                buf.insert(buf.end(), rest.begin(), rest.end());
                Reader r(buf);
                orig(p, r, id);
            });
        });
    }
    on(play_sb::chat, [orig = original(play_sb::chat)](Player& p, Reader& in, int id) {
        if (!has("chat") && !has("legacychat")) { if (orig) orig(p, in, id); return; }
        Reader peek = in;
        std::string msg = peek.str(256);
        // AsyncChatEvent is asynchronous (Paper fires it off the main thread): don't hold the caller.
        std::thread([pp = ptr_of(p), msg] { if (pp) chat(pp, msg); }).detach();
    });
    on(play_sb::player_action, [orig = original(play_sb::player_action)](Player& p, Reader& in, int id) {
        Reader peek = in;
        int action = peek.varint();
        int x, y, z;
        read_block_pos(peek, x, y, z);
        peek.u8();
        int seq = peek.varint();
        bool breaks = (action == 0 && p.gamemode == 1) || (action == 2 && p.gamemode != 1 && p.gamemode != 3);
        if (breaks && has("blockbreak")) {
            int state = world.get_block(x, y, z);
            Json r = event("blockbreak", "{" + player_ref(p) + ",\"x\":" + std::to_string(x) + ",\"y\":" +
                                             std::to_string(y) + ",\"z\":" + std::to_string(z) + ",\"block\":" + q(block_name(state)) + "}");
            if (cancelled(r)) {  // revert the client's predicted break
                p.send(block_update(x, y, z, state));
                p.send(Writer().varint(play_cb::block_changed_ack).varint(seq));
                return;
            }
        }
        if (orig) orig(p, in, id);
    });
    on(play_sb::use_item_on, [orig = original(play_sb::use_item_on)](Player& p, Reader& in, int id) {
        if (has("blockplace")) {
            Reader peek = in;
            int hand = peek.varint();
            int x, y, z;
            read_block_pos(peek, x, y, z);
            int face = peek.varint();
            peek.f32(); peek.f32(); peek.f32(); peek.u8(); peek.u8();
            int seq = peek.varint();
            const auto& slot = p.inv[hand == 1 ? 40 : size_t(p.selected)];
            int state = slot.item >= 0 ? ITEM_TO_BLOCK[slot.item] : -1;
            int ax = x, ay = y, az = z;
            // ponytail: mirrors play.hpp's placement target rule; share one helper once placement grows rules.
            if (state >= 0 && p.gamemode != 3 && !replaceable(world.get_block(x, y, z))) {
                static const int off[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
                if (face < 0 || face > 5) face = 1;
                x += off[face][0]; y += off[face][1]; z += off[face][2];
            }
            if (state >= 0 && p.gamemode != 3 && replaceable(world.get_block(x, y, z))) {
                int replaced = world.get_block(x, y, z);
                auto pos = [](int a, int b, int c, const char* k) {
                    return std::string(",\"") + k[0] + "\":" + std::to_string(a) + ",\"" + k[1] + "\":" + std::to_string(b) +
                           ",\"" + k[2] + "\":" + std::to_string(c);
                };
                Json r = event("blockplace", "{" + player_ref(p) + pos(x, y, z, "xyz") + ",\"ax\":" + std::to_string(ax) +
                                                 ",\"ay\":" + std::to_string(ay) + ",\"az\":" + std::to_string(az) +
                                                 ",\"replaced\":" + q(block_name(replaced)) + ",\"item\":" + q(block_name(state)) + "}");
                if (cancelled(r)) {
                    p.send(block_update(x, y, z, replaced));
                    p.send(Writer().varint(play_cb::block_changed_ack).varint(seq));
                    return;
                }
            }
        }
        if (orig) orig(p, in, id);
    });
#if CRAFTPP_JNI
    for (int id : {play_sb::move_player_pos, play_sb::move_player_pos_rot, play_sb::move_player_rot}) {
        auto orig = original(id);
        on(id, [orig](Player& p, Reader& in, int id) {
            if (!has("move")) { if (orig) orig(p, in, id); return; }
            Reader peek = in;
            double x = p.x, y = p.y, z = p.z;
            float yaw = p.yaw, pitch = p.pitch;
            if (id != play_sb::move_player_rot) { x = peek.f64(); y = peek.f64(); z = peek.f64(); }
            if (id != play_sb::move_player_pos) { yaw = peek.f32(); pitch = peek.f32(); }
            // Paper's throttle: only moves over 1/16 block or 10 degrees reach PlayerMoveEvent.
            double d2 = (x - p.x) * (x - p.x) + (y - p.y) * (y - p.y) + (z - p.z) * (z - p.z);
            double dr = std::fabs(yaw - p.yaw) + std::fabs(pitch - p.pitch);
            double dest[5];
            if ((d2 > 1.0 / 256 || dr > 10) && fire_move(p, x, y, z, yaw, pitch, dest)) {
                p.yaw = float(dest[3]);
                p.pitch = float(dest[4]);
                teleport(p, dest[0], dest[1], dest[2]);
                return;
            }
            if (orig) orig(p, in, id);
        });
    }
#endif
    // Tab completion for plugin commands (their args node asks the server).
    on(play_sb::command_suggestion, [orig = original(play_sb::command_suggestion)](Player& p, Reader& in, int id) {
        Reader peek = in;
        int tid = peek.varint();
        std::string text = peek.str(32500);
        std::string line = !text.empty() && text[0] == '/' ? text.substr(1) : text;
        if (!is_label(line)) { if (orig) orig(p, in, id); return; }
        Json r;
        try {
            r = request("tab", "{\"sender\":" + q(uuid_str(p.uuid_hi, p.uuid_lo)) + ",\"line\":" + q(line) + "}");
        } catch (const std::exception& e) {
            log_line("WARN", "", std::string("Plugin tab completion failed: ") + e.what());
        }
        int start = int(line.rfind(' ') + 1);
        if (auto* s = r.find("start"); s && s->isNumber()) start = s->asInt();
        start += int(text.size() - line.size());
        Writer w;
        w.varint(play_cb::command_suggestions).varint(tid).varint(start).varint(int(text.size()) - start);
        auto* c = r.find("completions");
        w.varint(c ? int(c->size()) : 0);
        if (c)
            for (auto& v : c->arr) w.str(v.isString() ? v.asString() : "").boolean(false);
        p.send(w);
    });
}

}  // namespace

// ---------- public hooks ----------

bool active() { return running; }

void start() {
#if CRAFTPP_JNI
    with_big_stack([] {
        try {
            load_ops();
            running = boot_jvm();
        } catch (const std::exception& e) {
            log_line("ERROR", "", std::string("Could not start plugins: ") + e.what());
            running = false;
        }
    });
    if (!running) return;
    install_handlers();
    patch_command_tree();
    on_tick([] { tick(); });  // Bukkit main thread = the game tick thread (game/tick.hpp)
#else
    DIR* d = opendir("plugins");
    if (d) {
        closedir(d);
        for (auto& f : std::filesystem::directory_iterator("plugins"))
            if (f.path().extension() == ".jar") {
                log_line("ERROR", "", "**** PLUGINS DISABLED: this build has no JNI headers (configure with a JDK installed)");
                break;
            }
    }
#endif
}

void stop() {
    if (!running) return;
    try {
        request("disable");
    } catch (const std::exception& e) {
        log_line("WARN", "", std::string("Plugins did not disable cleanly: ") + e.what());
    }
    running = false;
}

void announce_join(Player& p) {
    std::string fallback = "\xC2\xA7" "e" + p.name + " joined the game";
    if (!running) return broadcast_message(p.name + " joined the game", "yellow");
    if (!has("join")) {
        try { request("player", player_json(p)); } catch (...) {}
        return broadcast_legacy(fallback);
    }
    Json r = event("join", player_json(p));
    if (r.isNull()) return broadcast_legacy(fallback);
    if (auto* m = r.find("message"); m && m->isString()) broadcast_legacy(m->asString());
}

void announce_quit(Player& p) {
    std::string fallback = "\xC2\xA7" "e" + p.name + " left the game";
    if (!running) return broadcast_message(p.name + " left the game", "yellow");
    if (!has("quit")) {
        try { request("playergone", "{" + player_ref(p) + "}"); } catch (...) {}
        return broadcast_legacy(fallback);
    }
    Json r = event("quit", player_json(p));
    if (r.isNull()) return broadcast_legacy(fallback);
    if (auto* m = r.find("message"); m && m->isString()) broadcast_legacy(m->asString());
}

bool console_command(const std::string& line) {
    if (!running) return false;
    return server_command(nullptr, line) || dispatch(nullptr, line);
}

void tick() {
#if CRAFTPP_JNI
    if (!running) return;
    JNIEnv* e = env();
    e->CallStaticVoidMethod(main_cls, m_tick);
    if (auto err = take_exception(e); !err.empty()) log_line("ERROR", "", "plugin tick failed: " + err);
#endif
}

}  // namespace plugins
