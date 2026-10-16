#pragma once
// /execute (ExecuteCommand): modifiers fork/transform the source; if/unless filter it or, as the last node, test.
#include "vanilla.hpp"

inline int biome_at(int x, int y, int z) {
    if (y < MIN_Y || y >= MIN_Y + HEIGHT) return -1;
    ChunkPtr c = world.get_ptr(x >> 4, z >> 4);  // world.hpp: paletted sections
    std::lock_guard lock(c->m);
    return c->get_biome((x & 15) >> 2, (y - MIN_Y) >> 2, (z & 15) >> 2);
}

// The subcommand literal of an execute segment (skipping the leading `execute`) and its position.
inline size_t execute_sub(const Segment& seg) {
    size_t i = 0;
    if (!seg.nodes.empty() && !strcmp(node_name(seg.nodes[0].node), "execute")) i = 1;
    return i;
}

// Condition count for if/unless (createNumericConditionalHandler semantics for counted tests, else 0/1).
inline int condition_count(Cmd& c, const std::string& what) {
    auto& s = c.src;
    if (what == "block") {
        int x, y, z;
        c.block_pos("pos", x, y, z);
        return c.get<BlockSpec>("block").matches(world.get_block(x, y, z));
    }
    if (what == "blocks") {
        int x0, y0, z0, x1, y1, z1, dx, dy, dz;
        c.block_pos("start", x0, y0, z0);
        c.block_pos("end", x1, y1, z1);
        c.block_pos("destination", dx, dy, dz);
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        if (z0 > z1) std::swap(z0, z1);
        int64_t area = int64_t(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1);
        if (area > 32768) throw err("commands.execute.blocks.toobig", {32768, (long long)area});
        bool masked = c.is("masked");
        int count = 0;
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++)
                for (int z = z0; z <= z1; z++) {
                    int a = world.get_block(x, y, z);
                    if (masked && a == blocks::air) continue;
                    if (a != world.get_block(dx + x - x0, dy + y - y0, dz + z - z0)) return 0;
                    count++;
                }
        return count;
    }
    if (what == "entity") return int(c.players("entities", true).size());
    if (what == "score") {
        std::string th = c.holders("target")[0];
        std::string to = c.str("targetObjective");
        if (!objectives.count(to)) throw err("arguments.objective.notFound", {to});
        auto a = scores[th].find(to);
        if (a == scores[th].end()) return 0;
        if (c.is("matches")) return c.get<Range>("range").matches(a->second.value);
        std::string sh = c.holders("source")[0], so = c.str("sourceObjective");
        if (!objectives.count(so)) throw err("arguments.objective.notFound", {so});
        auto b = scores[sh].find(so);
        if (b == scores[sh].end()) return 0;
        int x = a->second.value, y = b->second.value;
        if (c.is("=")) return x == y;
        if (c.is("<")) return x < y;
        if (c.is("<=")) return x <= y;
        if (c.is(">")) return x > y;
        return x >= y;
    }
    if (what == "biome") {
        int x, y, z;
        c.block_pos("pos", x, y, z);
        std::string id = c.str("biome");
        if (id[0] == '#') throw fail(not_supported("execute if biome #tag"));
        return biome_at(x, y, z) == registry_index(*registry("minecraft:worldgen/biome"), id);
    }
    if (what == "dimension") return c.str("dimension") == s.dimension;
    if (what == "loaded") {
        int x, y, z;
        c.block_pos("pos", x, y, z);
        return 1;  // chunks are generated on demand and never unloaded
    }
    throw fail(not_supported("execute if " + what));
}

// Final-node conditional: vanilla's pass/fail messages.
inline bool execute_condition(Cmd& c) {
    size_t i = execute_sub(c.seg);
    bool expected = !strcmp(node_name(c.seg.nodes[i].node), "if");
    std::string what = node_name(c.seg.nodes[i + 1].node);
    bool counted = what == "entity" || what == "blocks";
    int n = condition_count(c, what);
    if (counted && expected) {
        if (n <= 0) throw err("commands.execute.conditional.fail");
        c.success(tr("commands.execute.conditional.pass_count", {n}), false);
        return true;
    }
    if (counted) {
        if (n != 0) throw err("commands.execute.conditional.fail_count", {n});
        c.success(tr("commands.execute.conditional.pass"), false);
        return true;
    }
    if ((n != 0) != expected) throw err("commands.execute.conditional.fail");
    c.success(tr("commands.execute.conditional.pass"), false);
    return true;
}

inline void face_towards(Source& s, double x, double y, double z) {  // EntityAnchorArgument / Vec2 facing
    double ay = s.y + (s.eyes ? 1.62 : 0);
    double dx = x - s.x, dy = y - ay, dz = z - s.z;
    s.pitch = float(-(std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180 / M_PI));
    s.yaw = float(std::atan2(dz, dx) * 180 / M_PI - 90);
}

inline std::vector<Source> apply_execute(const Segment& seg, const Source& in, const std::string& line, std::vector<StoreSpec>& stores) {
    Source s = in;
    Cmd c{s, seg, line};
    size_t i = execute_sub(seg);
    std::string sub = node_name(seg.nodes[i].node);
    if (sub == "run") return {s};
    if (sub == "as") {
        std::vector<Source> out;
        for (auto& p : c.players("targets", true)) { Source t = s; t.entity = p; out.push_back(t); }
        return out;
    }
    if (sub == "at") {
        std::vector<Source> out;
        for (auto& p : c.players("targets", true)) {
            Source t = s;
            t.x = p->x; t.y = p->y; t.z = p->z; t.yaw = p->yaw; t.pitch = p->pitch;
            out.push_back(t);
        }
        return out;
    }
    if (sub == "positioned") {
        if (c.has("pos")) { Vec3 v = c.vec("pos"); s.x = v.x; s.y = v.y; s.z = v.z; s.eyes = false; return {s}; }
        if (c.is("over")) {
            int x = int(std::floor(s.x)), z = int(std::floor(s.z));
            int y = MIN_Y + HEIGHT - 1;
            while (y > MIN_Y && world.get_block(x, y, z) == blocks::air) y--;
            s.y = y + 1;  // ponytail: every heightmap treated as WORLD_SURFACE
            return {s};
        }
        std::vector<Source> out;
        for (auto& p : c.players("targets", true)) { Source t = s; t.x = p->x; t.y = p->y; t.z = p->z; out.push_back(t); }
        return out;
    }
    if (sub == "rotated") {
        if (c.has("rot")) { c.get<Coords>("rot").rotation(s, s.yaw, s.pitch); return {s}; }
        std::vector<Source> out;
        for (auto& p : c.players("targets", true)) { Source t = s; t.yaw = p->yaw; t.pitch = p->pitch; out.push_back(t); }
        return out;
    }
    if (sub == "facing") {
        if (c.has("pos")) { Vec3 v = c.vec("pos"); face_towards(s, v.x, v.y, v.z); return {s}; }
        std::vector<Source> out;
        bool eyes = c.str("anchor", "feet") == "eyes";
        for (auto& p : c.players("targets", true)) { Source t = s; face_towards(t, p->x, p->y + (eyes ? 1.62 : 0), p->z); out.push_back(t); }
        return out;
    }
    if (sub == "anchored") { s.eyes = c.str("anchor") == "eyes"; return {s}; }
    if (sub == "align") {
        std::string ax = c.str("axes");
        if (ax.find('x') != std::string::npos) s.x = std::floor(s.x);
        if (ax.find('y') != std::string::npos) s.y = std::floor(s.y);
        if (ax.find('z') != std::string::npos) s.z = std::floor(s.z);
        return {s};
    }
    if (sub == "on") return {};  // ponytail: players have no vehicle/owner/leash/attacker relations yet
    if (sub == "in") {
        s.dimension = c.str("dimension");
        if (s.dimension != "minecraft:overworld") throw fail(not_supported("execute in " + s.dimension));
        return {s};
    }
    if (sub == "if" || sub == "unless") {
        bool expected = sub == "if";
        int n = condition_count(c, node_name(seg.nodes[i + 1].node));
        if ((n > 0) == expected) return {s};
        return {};
    }
    if (sub == "store") {
        std::string kind = node_name(seg.nodes[i + 2].node);
        if (kind != "score") throw fail(not_supported("execute store " + kind));
        stores.push_back({!strcmp(node_name(seg.nodes[i + 1].node), "success"), c.holders("targets"), c.str("objective")});
        if (!objectives.count(stores.back().objective)) throw err("arguments.objective.notFound", {stores.back().objective});
        return {s};
    }
    throw fail(not_supported("execute " + sub));
}
