#pragma once
// Shared pieces for container blocks: data/containers.json tables, sounds, block events, directions, the
// slot-list menu base (ChestMenu & co.) and menu opening helpers.
#include "block_entity.hpp"

#include <unordered_map>

namespace cont {

// ---------- data/containers.json (tools/gen_container_data.py) ----------
struct Cooking {
    std::string id;
    std::vector<uint16_t> in;
    ItemStack result;
    float xp = 0;
    int time = 200;
};
struct Fuel {
    int burn[2];      // normal, fast (smoker / blast furnace)
    float speed[2];
};
struct SingleRecipe {  // stonecutting
    std::vector<uint16_t> in;
    ItemStack result;
};
struct Smithing {
    std::vector<uint16_t> t, b, a;
    ItemStack result;  // transform
    std::string pattern;  // trim (result empty)
};
inline std::array<std::vector<Cooking>, 4> cooking;  // smelting, blasting, smoking, campfire_cooking
inline std::unordered_map<int, Fuel> fuels;
inline std::vector<SingleRecipe> stonecutting;
inline std::vector<Smithing> smithing;
inline std::map<std::string, int> sound_ids;
inline std::map<std::string, std::vector<uint16_t>> item_tags;
inline std::map<std::string, int> slot_display_ids;
inline std::vector<std::string> trim_patterns, trim_materials;

inline bool tagged(const char* tag, int item) {
    auto it = item_tags.find(tag);
    return it != item_tags.end() && in_sorted(it->second, item);
}
inline int item_id(const char* name) {
    auto it = item_by_name.find(std::string("minecraft:") + name);
    return it == item_by_name.end() ? -1 : it->second;
}

inline Json data_json;  // freed by containers_init once every table is built
inline bool load_data() {
    Json& j = data_json;
    try {
        j = Json::parseFile("data/containers.json");
    } catch (const std::exception& e) {
        log_error("data/containers.json: %s (run: python3 tools/gen_container_data.py)", e.what());
        return false;
    }
    auto ids = [](const Json& a) {
        std::vector<uint16_t> v;
        for (auto& x : a.arr) v.push_back(uint16_t(x.asInt()));
        return v;
    };
    const char* kinds[4] = {"smelting", "blasting", "smoking", "campfire_cooking"};
    for (int k = 0; k < 4; k++)
        for (auto& r : j["cooking"][kinds[k]].arr)
            cooking[size_t(k)].push_back({r["id"].asString(), ids(r["i"]), {r["r"][0].asInt(), r["r"][1].asInt()},
                                          r["xp"].asFloat(), r["t"].asInt()});
    for (auto& [k, v] : j["fuel"].obj) fuels[std::stoi(k)] = {{v[0].asInt(), v[1].asInt()}, {v[2].asFloat(), v[3].asFloat()}};
    for (auto& r : j["stonecutting"].arr) stonecutting.push_back({ids(r["i"]), {r["r"][0].asInt(), r["r"][1].asInt()}});
    for (auto& r : j["smithing_transform"].arr)
        smithing.push_back({ids(r["t"]), ids(r["b"]), ids(r["a"]), {r["r"][0].asInt(), r["r"][1].asInt()}, ""});
    for (auto& r : j["smithing_trim"].arr) smithing.push_back({ids(r["t"]), ids(r["b"]), ids(r["a"]), {}, r["p"].asString()});
    for (auto& [k, v] : j["sounds"].obj) sound_ids[k] = v.asInt();
    for (auto& [k, v] : j["item_tags"].obj) item_tags[k] = ids(v);
    for (auto& [k, v] : j["recipe_display"].obj) slot_display_ids[k] = v.asInt();
    for (auto& v : j["trim_pattern"].arr) trim_patterns.push_back(v.asString());
    for (auto& v : j["trim_material"].arr) trim_materials.push_back(v.asString());
    return true;
}

// ---------- packets ----------
// ClientboundSoundPacket, source BLOCKS (4) unless given.
inline void play_sound(const char* name, double x, double y, double z, float volume, float pitch, int source = 4) {
    auto it = sound_ids.find(name);
    if (it == sound_ids.end()) return;
    Writer w;
    w.varint(play_cb::sound).varint(it->second + 1).varint(source).i32(int32_t(x * 8)).i32(int32_t(y * 8)).i32(int32_t(z * 8))
        .f32(volume).f32(pitch).i64(int64_t(game_rng()));
    broadcast_near(x, z, w);
}
inline void block_event(int x, int y, int z, int b0, int b1, int state) {
    Writer w;
    w.varint(play_cb::block_event).i64(int64_t(be::key(x, y, z))).u8(uint8_t(b0)).u8(uint8_t(b1)).varint(state_block_index(state));
    broadcast_near(x, z, w);
}
inline void set_state(be::Live& b, int state) {  // Level.setBlock(pos, state, 3) for a property change
    if (b.state == state) return;
    change_block(b.x, b.y, b.z, state);
}

// ---------- directions (bs::Dir: DOWN UP NORTH SOUTH WEST EAST) ----------
using bs::Dir;
inline const char* dir_name(Dir d) {
    static const char* n[6] = {"down", "up", "north", "south", "west", "east"};
    return n[d];
}
inline Dir dir_from(std::string_view s) {
    for (int d = 0; d < 6; d++)
        if (s == dir_name(Dir(d))) return Dir(d);
    return bs::NORTH;
}
inline Dir clockwise(Dir d) {  // around Y
    switch (d) {
        case bs::NORTH: return bs::EAST;
        case bs::EAST: return bs::SOUTH;
        case bs::SOUTH: return bs::WEST;
        case bs::WEST: return bs::NORTH;
        default: return d;
    }
}
inline Dir counter_clockwise(Dir d) { return clockwise(clockwise(clockwise(d))); }
inline Dir horizontal_facing(const Player& p) {  // Direction.fromYRot (the direction the player looks)
    static const Dir by2d[4] = {bs::SOUTH, bs::WEST, bs::NORTH, bs::EAST};
    return by2d[int(std::floor(p.yaw / 90.0 + 0.5)) & 3];
}
inline Dir nearest_looking(const Player& p) {  // Direction.orderedByNearest(entity)[0]
    float pitch = p.pitch * float(M_PI / 180), yaw = -p.yaw * float(M_PI / 180);
    float sp = std::sin(pitch), cp = std::cos(pitch), sy = std::sin(yaw), cy = std::cos(yaw);
    bool east = sy > 0, up = sp < 0, south = cy > 0;
    float xy = east ? sy : -sy, yy = up ? -sp : sp, zy = south ? cy : -cy;
    float xd = xy * cp, zd = zy * cp;
    Dir ax = east ? bs::EAST : bs::WEST, ay = up ? bs::UP : bs::DOWN, az = south ? bs::SOUTH : bs::NORTH;
    if (xy > zy) return yy > xd ? ay : ax;
    return yy > zd ? ay : az;
}
inline Dir facing(int state) { return dir_from(bs::R().value(state, "facing")); }
inline int with(int state, const char* prop, std::string_view v) {
    int s = bs::R().with(state, prop, v);
    return s < 0 ? state : s;
}

// Player.isWithinBlockInteractionRange(pos, 4.0); Container.stillValidBlockEntity adds "not removed".
inline bool in_range(const Player& p, int x, int y, int z) {
    double ex = p.x, ey = p.y + 1.62, ez = p.z;
    double dx = std::max({x - ex, 0.0, ex - (x + 1)}), dy = std::max({y - ey, 0.0, ey - (y + 1)}),
           dz = std::max({z - ez, 0.0, ez - (z + 1)});
    double r = (p.gamemode == 1 ? 5.0 : 4.5) + 4.0;
    return dx * dx + dy * dy + dz * dz < r * r;
}
inline bool in_range(const Player& p, const be::Live& b) { return !b.removed && in_range(p, b.x, b.y, b.z); }

// ---------- slot-list menus ----------
// A menu over container slots that live elsewhere (block entities, the ender chest). `holders` keep the
// block entities alive and count as openers.
struct SlotsMenu : Menu {
    std::vector<ItemStack*> slots;
    std::vector<std::shared_ptr<be::Live>> holders;
    bool count_check = false;  // quickMoveStack returns EMPTY when nothing moved (dispenser, furnace, ...)
    int size() const override { return int(slots.size()); }
    ItemStack& at(int i) override { return *slots[size_t(i)]; }
    void changed(Player&, int) override {
        for (auto& h : holders) h->set_changed();
    }
    bool still_valid(Player& p) override {
        for (auto& h : holders)
            if (!in_range(p, *h)) return false;
        return true;
    }
    // ChestMenu.quickMoveStack shape: container -> inventory (backwards), inventory -> container.
    ItemStack quick_move(Player& p, int i) override {
        int n = size();
        SlotRef r = menu_slot(p, i);
        if (r.s->empty()) return {};
        ItemStack stack = *r.s, clicked = stack;
        bool ok = i < n ? move_to(p, stack, n, n + 36, true) : move_to(p, stack, 0, n, false);
        if (!ok) return {};
        *r.s = stack;
        slot_changed(p, r);
        if (count_check && stack.count == clicked.count) return {};
        return clicked;
    }
    void removed(Player& p) override {
        for (auto& h : holders)
            if (!h->removed) h->openers--, h->closed(p);
    }
};
inline std::vector<uint8_t> title_of(const be::Live& b, const char* key) {
    return b.custom_name.empty() ? comp::text_translate(key) : b.custom_name;
}
inline void open_slots_menu(Player& p, std::shared_ptr<SlotsMenu> m, const char* menu, std::vector<uint8_t> title) {
    m->type = menu_ids[menu];
    open_menu(p, m, title);
    for (auto& h : m->holders) h->openers++, h->opened(p);
}

}  // namespace cont
