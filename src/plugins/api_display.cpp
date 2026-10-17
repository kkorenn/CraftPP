// Plugin API routes that only send packets: plugin scoreboards and teams, boss bars, sounds, particles.
// Viewer lists come from the bridge (a["viewers"]); without one the packet goes to every online player.
#include "internal.hpp"
#include "../mobs/mobs.hpp"

namespace plugins::detail {
namespace {

void send_all(const std::vector<PlayerPtr>& to, const Writer& w) {
    for (auto& p : to) p->send(w);
}

std::optional<std::string> scoreboard(const std::string& m, const Json& a) {
    auto to = viewers_arg(a);
    std::string op = text_arg(a, "op");
    if (m == "scoreboard.objective") {
        std::string name = text_arg(a, "name");
        if (op == "display") {  // ClientboundSetDisplayObjectivePacket; "" clears the slot
            send_all(to, pkt(play_cb::set_display_objective).varint(int(num_arg(a, "slot"))).str(name));
            return "null";
        }
        int method = op == "add" ? 0 : op == "remove" ? 1 : op == "update" ? 2 : -1;
        if (method < 0) throw std::runtime_error("Unknown objective operation " + op);
        Writer w = pkt(play_cb::set_objective);
        w.str(name).u8(uint8_t(method));
        if (method != 1) {
            legacy_component(w, text_arg(a, "displayName", name));
            w.varint(int(num_arg(a, "renderType"))).boolean(false);  // no number format
        }
        send_all(to, w);
        return "null";
    }
    if (m == "scoreboard.score") {
        std::string entry = text_arg(a, "entry");
        if (bool_arg(a, "reset")) {
            std::string obj = text_arg(a, "objective");
            Writer w = pkt(play_cb::reset_score);
            w.str(entry).boolean(!obj.empty());
            if (!obj.empty()) w.str(obj);
            send_all(to, w);
            return "null";
        }
        Writer w = pkt(play_cb::set_score);
        w.str(entry).str(text_arg(a, "objective")).varint(int(num_arg(a, "value")));
        if (a.has("display") && a["display"].isString()) legacy_component(w.boolean(true), a["display"].asString());
        else w.boolean(false);
        send_all(to, w.boolean(false));  // no number format
        return "null";
    }
    if (m == "scoreboard.team") {
        int method = op == "add" ? 0 : op == "remove" ? 1 : op == "update" ? 2 : op == "join" ? 3 : op == "leave" ? 4 : -1;
        if (method < 0) throw std::runtime_error("Unknown team operation " + op);
        Writer w = pkt(play_cb::set_player_team);
        w.str(text_arg(a, "name")).u8(uint8_t(method));
        if (method == 0 || method == 2) {  // ClientboundSetPlayerTeamPacket.Parameters
            legacy_component(w, text_arg(a, "displayName", text_arg(a, "name")));
            legacy_component(w, text_arg(a, "prefix"));
            legacy_component(w, text_arg(a, "suffix"));
            w.varint(int(num_arg(a, "visibility"))).varint(int(num_arg(a, "collision")));
            int color = int(num_arg(a, "color", -1));
            w.boolean(color >= 0);
            if (color >= 0) w.varint(color);
            w.u8(uint8_t(num_arg(a, "options", 3)));
        }
        if (method == 0 || method == 3 || method == 4) {
            auto* e = a.find("entries");
            w.varint(e && e->isArray() ? int(e->size()) : 0);
            if (e && e->isArray())
                for (auto& v : e->arr) w.str(v.isString() ? v.asString() : "");
        }
        send_all(to, w);
        return "null";
    }
    return std::nullopt;
}

std::optional<std::string> bossbar(const std::string& m, const Json& a) {
    int64_t hi, lo;
    if (!parse_uuid(text_arg(a, "id"), hi, lo)) throw std::runtime_error("Invalid boss bar id");
    auto to = viewers_arg(a);
    auto head = [&](int op) {
        Writer w = pkt(play_cb::boss_event);
        w.i64(hi).i64(lo).varint(op);
        return w;
    };
    float progress = float(std::clamp(num_arg(a, "progress", 1), 0.0, 1.0));
    int color = int(num_arg(a, "color", 6)), style = int(num_arg(a, "style")), flags = int(num_arg(a, "flags"));
    if (m == "bossbar.show") {  // ClientboundBossEventPacket add
        Writer w = head(0);
        legacy_component(w, text_arg(a, "title"));
        send_all(to, w.f32(progress).varint(color).varint(style).u8(uint8_t(flags)));
        return "null";
    }
    if (m == "bossbar.hide") {
        send_all(to, head(1));
        return "null";
    }
    if (m == "bossbar.update") {
        Writer name = head(3);
        legacy_component(name, text_arg(a, "title"));
        Writer bar = head(2), look = head(4), fl = head(5);
        bar.f32(progress);
        look.varint(color).varint(style);
        fl.u8(uint8_t(flags));
        for (auto& p : to) {
            p->send(bar);
            p->send(name);
            p->send(look);
            p->send(fl);
        }
        return "null";
    }
    return std::nullopt;
}

// Particle options as a typed field list built by the bridge from Bukkit's particle data:
// ["i", n] int, ["v", n] varint, ["f", x] float, ["d", x] double, ["pos", x, y, z] block position,
// ["block", "name[props]"] block state id, ["item", {item}] item stack.
void write_options(Writer& w, const Json* opts) {
    if (!opts || !opts->isArray()) return;
    for (auto& f : opts->arr) {
        if (!f.isArray() || f.size() < 2) continue;
        const std::string& t = f[0].asString();
        if (t == "i") w.i32(int32_t(int64_t(f[1].asDouble())));
        else if (t == "v") w.varint(int(f[1].asDouble()));
        else if (t == "f") w.f32(float(f[1].asDouble()));
        else if (t == "d") w.f64(f[1].asDouble());
        else if (t == "pos" && f.size() >= 4) w.i64(int64_t(pack_pos(int(f[1].asDouble()), int(f[2].asDouble()), int(f[3].asDouble()))));
        else if (t == "block") {
            int state = state_of_text(f[1].asString());
            if (state < 0) throw std::runtime_error("Unknown block " + f[1].asString());
            w.varint(state);
        } else if (t == "item") {
            ItemStack stack = item_from_json(f[1]);
            if (stack.empty()) throw std::runtime_error("Item particles need an item");
            write_slot(w, stack);
        } else throw std::runtime_error("Unknown particle option field " + t);
    }
}

std::optional<std::string> effects(const std::string& m, const Json& a) {
    double x = num_arg(a, "x"), y = num_arg(a, "y"), z = num_arg(a, "z");
    if (m == "player.playSound" || m == "world.playSound") {
        // ClientboundSoundPacket with a direct holder (any sound id, including resource-pack ones).
        int source = std::clamp(int(num_arg(a, "category")), 0, 10);
        float volume = float(num_arg(a, "volume", 1)), pitch = float(num_arg(a, "pitch", 1));
        int64_t seed = a.has("seed") ? int64_t(num_arg(a, "seed")) : int64_t(cmd_rng());
        Writer w;
        if (a.has("entity"))  // ClientboundSoundEntityPacket: follows the entity (x/y/z only pick who hears it)
            w = pkt(play_cb::sound_entity).varint(0).str(norm_id(text_arg(a, "sound"))).boolean(false).varint(source)
                    .varint(int(num_arg(a, "entity"))).f32(volume).f32(pitch).i64(seed);
        else
            w = pkt(play_cb::sound).varint(0).str(norm_id(text_arg(a, "sound"))).boolean(false).varint(source)
                    .i32(int32_t(std::floor(x * 8))).i32(int32_t(std::floor(y * 8))).i32(int32_t(std::floor(z * 8)))
                    .f32(volume).f32(pitch).i64(seed);
        if (m == "player.playSound") {
            require_player(a).send(w);
            return "null";
        }
        // World.playSound: players in range (Level.playSound: 16 blocks, or 16 * volume when louder)
        double r = volume > 1 ? 16.0 * volume : 16.0;
        for (auto& p : snapshot())
            if ((p->x - x) * (p->x - x) + (p->y - y) * (p->y - y) + (p->z - z) * (p->z - z) < r * r) p->send(w);
        return "null";
    }
    if (m == "player.stopSound") {
        int source = a.has("category") ? int(num_arg(a, "category")) : -1;
        std::string sound = text_arg(a, "sound");
        Writer w = pkt(play_cb::stop_sound);
        if (source >= 0 && !sound.empty()) w.u8(3).varint(source).str(norm_id(sound));
        else if (source >= 0) w.u8(1).varint(source);
        else if (!sound.empty()) w.u8(2).str(norm_id(sound));
        else w.u8(0);
        require_player(a).send(w);
        return "null";
    }
    if (m == "world.particle") {
        static const auto* types = registry("minecraft:particle_type");
        int id = registry_index(*types, norm_id(text_arg(a, "type")));
        if (id < 0) throw std::runtime_error("Unknown particle " + text_arg(a, "type"));
        float speed = float(num_arg(a, "speed"));
        bool force = bool_arg(a, "force");
        Writer w = pkt(play_cb::level_particles);
        w.varint(id);
        write_options(w, a.find("options"));
        w.boolean(force).boolean(false).f64(x).f64(y).f64(z).f32(float(num_arg(a, "dx"))).f32(float(num_arg(a, "dy")))
            .f32(float(num_arg(a, "dz"))).f32(speed).f32(speed).f32(speed).varint(std::max(0, int(num_arg(a, "count")))).varint(0);
        // ServerLevel.sendParticles: 32 blocks (512 with force), measured from the player's block centre.
        double range = force ? 512 : 32;
        for (auto& p : viewers_arg(a)) {
            double bx = p->x - x, by = p->y - y, bz = p->z - z;
            if (bx * bx + by * by + bz * bz < range * range) p->send(w);
        }
        return "null";
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> api_display(const std::string& m, const Json& a) {
    if (m.rfind("scoreboard.", 0) == 0) return scoreboard(m, a);
    if (m.rfind("bossbar.", 0) == 0) return bossbar(m, a);
    return effects(m, a);
}

}  // namespace plugins::detail
