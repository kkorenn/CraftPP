#pragma once
// Shared helpers of the plugin bridge (src/plugins/*.cpp). Defined in plugins.cpp unless noted.
// API routes ("api" {m, a} calls from the bridge) are split by area: api() in plugins.cpp handles
// server/player/world, then falls through to the api_* functions below (nullopt = not mine).
#include "../play.hpp"
#include "../worldgen/json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Entity;

namespace plugins::detail {

// JSON
std::string q(const std::string& s);  // quoted + escaped
std::string num(double v);            // finite number, else 0
std::string text_arg(const Json& a, std::string_view k, const std::string& def = "");
double num_arg(const Json& a, std::string_view k, double def = 0);
inline bool bool_arg(const Json& a, std::string_view k, bool def = false) {
    auto* v = a.find(k);
    return v && v->isBool() ? v->asBool() : def;
}

// players
std::string uuid_str(int64_t hi, int64_t lo);
bool parse_uuid(const std::string& s, int64_t& hi, int64_t& lo);
PlayerPtr player_by_uuid(int64_t hi, int64_t lo);
PlayerPtr player_arg(const Json& a, std::string_view key = "uuid");
Player& require_player(const Json& a);  // throws "Player is not online"
// a["viewers"] (array of uuids) when present, else every online player.
inline std::vector<PlayerPtr> viewers_arg(const Json& a) {
    auto* v = a.find("viewers");
    if (!v || !v->isArray()) return snapshot();
    std::vector<PlayerPtr> out;
    for (auto& e : v->arr) {
        int64_t hi, lo;
        if (e.isString() && parse_uuid(e.asString(), hi, lo))
            if (auto p = player_by_uuid(hi, lo)) out.push_back(p);
    }
    return out;
}

// text: legacy "§" strings -> network NBT text components
void legacy_component(Writer& w, const std::string& s);
void send_legacy(Player& p, const std::string& text, bool action_bar = false);
std::string strip_codes(const std::string& s);

// ids
std::string norm_id(std::string s);  // lower case, "minecraft:" added when there is no namespace
std::string block_name(int state);
int resolve_state(const std::string& name, const std::vector<std::pair<std::string, std::string>>& props = {});
int state_of_text(const std::string& text);  // "minecraft:oak_stairs[facing=north]" -> state id, -1 if unknown
extern std::vector<std::string> item_names;  // minecraft:item registry order
extern std::unordered_map<std::string, int> item_by_name;

// items: bridge JSON {item, count, meta} <-> ItemStack (api_items.cpp); empty stack / "null" for air
ItemStack item_from_json(const Json& j);
std::string item_to_json(const ItemStack& s);

// bridge requests (C++ -> Java); throws with the Java exception text
Json request(const std::string& type, const std::string& json = "");
PacketHandler original(int id);  // the handler registered before plugins wrapped it

// events (bridge round trip; null Json when the bridge failed)
Json event(const std::string& hook, const std::string& payload);
bool cancelled(const Json& r);
bool has(const std::string& hook);  // some plugin listens for the hookable event

// route groups
std::optional<std::string> api_display(const std::string& m, const Json& a);  // api_display.cpp
std::optional<std::string> api_entity(const std::string& m, const Json& a);   // api_entity.cpp
std::optional<std::string> api_inventory(const std::string& m, const Json& a);  // api_inventory.cpp
void install_inventory_handlers();                                             // api_inventory.cpp
std::string entity_snapshot(Entity& e);                                        // api_entity.cpp (entity.state)
void install_event_handlers();                                                 // api_events.cpp
void flush_gone_entities();                                                    // api_events.cpp, once per tick

}  // namespace plugins::detail
