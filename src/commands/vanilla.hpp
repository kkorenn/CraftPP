#pragma once
// Vanilla command implementations (net.minecraft.server.commands.*), keyed by root literal.
#include "dispatcher.hpp"
#include "locate.hpp"

// Survival module API (src/game/, included after this header).
inline void kick(Player& p);
inline bool give_item(Player& p, int item, int count, int damage);
inline int clear_inventory(Player& p, int item, int max);
inline void kill(Player& p, const std::string& type);
inline bool hurt(Player& p, float amount, const std::string& type);
inline void send_experience(Player& p);

// ---------- helpers ----------
inline std::string fmt_f(double v, int prec) {  // String.format(Locale.ROOT, "%.<prec>f")
    char b[64];
    snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}
inline Text item_display(int item) {  // ItemStack.getDisplayName: [Name]
    Text t = lit("[");
    append(t, tr(gen::ITEM_NAME_KEY[item]));
    append(t, lit("]"));
    return t;
}
inline Text gamemode_text(int g) { static const char* n[] = {"survival", "creative", "adventure", "spectator"}; return tr(std::string("gameMode.") + n[g]); }

// CommandResponseTracker: "single" message with the target's name, "multiple" with the count.
template <class FS, class FM>
inline int respond(Cmd& c, const std::vector<PlayerPtr>& t, bool broadcast_admins, FS single, FM multiple) {
    c.success(t.size() == 1 ? single(player_name(*t[0])) : multiple(int(t.size())), broadcast_admins);
    return int(t.size());
}

// Block edits: one section_blocks_update per touched section (single changes use block_update).
struct BlockBatch {
    std::map<std::tuple<int, int, int>, std::vector<std::pair<uint16_t, int>>> sections;  // section -> (local pos, state)
    int changed = 0;
    bool set(int x, int y, int z, int state) {
        if (y < world.min_y() || y >= world.max_y() || world.get_block(x, y, z) == state) return false;
        world.set_block(x, y, z, uint16_t(state));
        sections[{x >> 4, y >> 4, z >> 4}].push_back({uint16_t((x & 15) << 8 | (z & 15) << 4 | (y & 15)), state});
        changed++;
        return true;
    }
    void flush() {
        for (auto& [sec, list] : sections) {
            auto [sx, sy, sz] = sec;
            if (list.size() == 1) {
                auto [local, state] = list[0];
                broadcast_level(block_update(sx * 16 + (local >> 8), sy * 16 + (local & 15), sz * 16 + (local >> 4 & 15), state), cur_level);
                continue;
            }
            Writer w = pkt(play_cb::section_blocks_update);
            int ys = sy;
            w.i64(int64_t((uint64_t(int64_t(sx) & 0x3FFFFF) << 42) | (uint64_t(int64_t(sz) & 0x3FFFFF) << 20) | (uint64_t(int64_t(ys) & 0xFFFFF))));
            w.varint(int32_t(list.size()));
            for (auto [local, state] : list) write_varlong(w, int64_t(state) << 12 | local);
            broadcast_level(w, cur_level);
        }
        sections.clear();
    }
};

// ---------- registration ----------
inline void reg(const char* name, Handler h) { command_handlers()[name] = std::move(h); }

inline void sync_time_all(int clock) { clock_changed(clock); }

inline void register_vanilla_commands() {
    register_locate();
    // help
    reg("help", [](Cmd& c) {
        if (!c.has("command")) {
            auto lines = usage_lines(0, c.src);
            for (auto& l : lines) c.success(lit("/" + l), false);
            return int(lines.size());
        }
        std::string cmd = c.str("command");
        Parse p = parse_command(cmd, 0, c.src);
        if (p.segs.back().nodes.empty() && p.segs.size() == 1) throw err("commands.help.failed");
        int last = p.segs.back().nodes.empty() ? p.segs.back().root : p.segs.back().nodes.back().node;
        std::string shown = cmd.substr(0, p.cursor);
        auto lines = usage_lines(last, c.src);
        for (auto& l : lines) c.success(lit("/" + shown + " " + l), false);
        return int(lines.size());
    });

    reg("list", [](Cmd& c) {
        auto list = snapshot();
        std::vector<Text> names;
        for (auto& p : list)
            names.push_back(c.is("uuids") ? tr("commands.list.nameAndId", {player_name(*p), player_uuid(*p)}) : player_name(*p));
        c.success(tr("commands.list.players", {int(list.size()), prop_int("max-players", 20), join_list(names)}), false);
        return int(list.size());
    });

    reg("say", [](Cmd& c) {
        broadcast_text(tr("chat.type.announcement", {c.src.display(), resolve_message(c.str("message"), c.src)}));
        return 1;
    });
    reg("me", [](Cmd& c) {
        broadcast_text(tr("chat.type.emote", {c.src.display(), resolve_message(c.str("action"), c.src)}));
        return 1;
    });
    reg("msg", [](Cmd& c) {
        auto targets = c.players("targets");
        Text msg = resolve_message(c.str("message"), c.src);
        for (auto& t : targets) {
            c.send(styled(tr("commands.message.display.outgoing", {player_name(*t), msg}), "gray", true));
            send_text(*t, styled(tr("commands.message.display.incoming", {c.src.display(), msg}), "gray", true));
        }
        return int(targets.size());
    });
    reg("teammsg", [](Cmd& c) {
        auto self = c.self_player();
        Team* t = team_of(self->name);
        if (!t) throw err("commands.teammsg.failed.noteam");
        Text team_name = t->display;
        Text tn = lit("[");
        append(tn, team_name);
        append(tn, lit("]"));
        Text msg = resolve_message(c.str("message"), c.src);
        for (auto& p : snapshot()) {
            if (team_of(p->name) != t) continue;
            send_text(*p, p == self ? tr("chat.type.team.sent", {tn, c.src.display(), msg}) : tr("chat.type.team.text", {tn, c.src.display(), msg}));
        }
        return 1;
    });

    // teleport / tp
    reg("teleport", [](Cmd& c) {
        auto do_tp = [&](Player& p, double x, double y, double z, std::optional<std::pair<float, float>> rot) {
            if (std::abs(x) > 30000000 || std::abs(z) > 30000000 || y < -20000000 || y >= 20000000) throw err("commands.teleport.invalidPosition");
            if (rot) {
                p.yaw = std::remainder(rot->first, 360.0f);
                p.pitch = std::clamp(std::remainder(rot->second, 360.0f), -90.f, 90.f);
            }
            World* to = level_by_id(c.src.dimension);  // TeleportCommand: into the source's level
            if (p.level != to) change_dimension(p, to, x, y, z, p.yaw, p.pitch);
            else teleport(p, x, y, z);
        };
        auto face = [&](Player& p, Vec3 at) {  // LookAt.perform (feet anchor of the victim's eyes)
            double dx = at.x - p.x, dy = at.y - (p.y + 1.62), dz = at.z - p.z;
            double h = std::sqrt(dx * dx + dz * dz);
            p.pitch = float(-(std::atan2(dy, h) * 180 / M_PI));
            p.yaw = float(std::atan2(dz, dx) * 180 / M_PI - 90);
            teleport(p, p.x, p.y, p.z);
        };
        std::vector<PlayerPtr> targets = c.has("targets") ? c.players("targets") : std::vector<PlayerPtr>{c.self_player(true)};
        if (c.has("destination")) {
            auto dest = c.player("destination");
            for (auto& t : targets) {
                if (t->level != dest->level) change_dimension(*t, dest->level, dest->x, dest->y, dest->z, dest->yaw, dest->pitch);
                else do_tp(*t, dest->x, dest->y, dest->z, std::make_pair(dest->yaw, dest->pitch));
            }
            return respond(c, targets, true, [&](Text n) { return tr("commands.teleport.success.entity.single", {n, player_name(*dest)}); },
                           [&](int k) { return tr("commands.teleport.success.entity.multiple", {k, player_name(*dest)}); });
        }
        Vec3 pos = c.vec("location");
        for (auto& t : targets) {
            std::optional<std::pair<float, float>> rot;
            if (c.has("rotation")) {
                float yaw, pitch;
                Source s = c.src;
                s.yaw = t->yaw;
                s.pitch = t->pitch;
                c.get<Coords>("rotation").rotation(s, yaw, pitch);
                rot = std::make_pair(yaw, pitch);
            }
            do_tp(*t, pos.x, pos.y, pos.z, rot);
            if (c.has("facingLocation")) face(*t, c.vec("facingLocation"));
            if (c.has("facingEntity")) {
                auto e = c.player("facingEntity");
                bool eyes = c.str("facingAnchor", "feet") == "eyes";
                face(*t, {e->x, e->y + (eyes ? 1.62 : 0), e->z});
            }
        }
        auto f6 = [](double v) { return fmt_f(v, 6); };
        return respond(c, targets, true,
                       [&](Text n) { return tr("commands.teleport.success.location.single", {n, f6(pos.x), f6(pos.y), f6(pos.z)}); },
                       [&](int k) { return tr("commands.teleport.success.location.multiple", {k, f6(pos.x), f6(pos.y), f6(pos.z)}); });
    });

    reg("gamemode", [](Cmd& c) {
        int g = int(c.num("gamemode"));
        auto targets = c.targets_or_self("target");
        int n = 0;
        for (auto& t : targets) {
            if (t->gamemode == g) continue;
            set_gamemode(*t, g);
            if (c.src.entity == t) c.success(tr("commands.gamemode.success.self", {gamemode_text(g)}), true);
            else {
                if (rule("send_command_feedback")) send_text(*t, tr("gameMode.changed", {gamemode_text(g)}));
                c.success(tr("commands.gamemode.success.other", {player_name(*t), gamemode_text(g)}), true);
            }
            n++;
        }
        return n;
    });
    reg("defaultgamemode", [](Cmd& c) {
        int g = int(c.num("gamemode"));
        static const char* n[] = {"survival", "creative", "adventure", "spectator"};
        props["gamemode"] = n[g];
        int changed = 0;
        if (prop_bool("force-gamemode", false))
            for (auto& p : snapshot()) if (p->gamemode != g) { set_gamemode(*p, g); changed++; }
        c.success(tr("commands.defaultgamemode.success", {gamemode_text(g)}), true);
        return changed;
    });

    reg("give", [](Cmd& c) {
        auto& it = c.get<ItemSpec>("item");
        int count = int(c.num("count", 1));
        int max = gen::ITEM_MAX_STACK[it.item] * 100;
        if (count > max) throw err("commands.give.failed.toomanyitems", {max, item_display(it.item)});
        auto targets = c.players("targets");
        for (auto& t : targets) {
            give_item(*t, it.item, count, 0);  // drops what doesn't fit
            t->send(pkt(play_cb::sound).varint(0).str("minecraft:entity.item.pickup").boolean(false).varint(7)
                        .i32(int32_t(t->x * 8)).i32(int32_t(t->y * 8)).i32(int32_t(t->z * 8)).f32(0.2f)
                        .f32(((float(cmd_rng() % 1000) / 1000.f - float(cmd_rng() % 1000) / 1000.f) * 0.7f + 1.f) * 2.f)
                        .i64(int64_t(cmd_rng())));
        }
        return respond(c, targets, true, [&](Text n) { return tr("commands.give.success.single", {count, item_display(it.item), n}); },
                       [&](int k) { return tr("commands.give.success.multiple", {count, item_display(it.item), k}); });
    });
    reg("clear", [](Cmd& c) {
        auto targets = c.targets_or_self("targets");
        std::optional<ItemSpec> pred;
        if (c.has("item")) pred = c.get<ItemSpec>("item");
        int max = int(c.num("maxCount", -1));
        int total = 0;
        for (auto& t : targets) {
            if (!pred || (pred->item >= 0 && pred->components.empty())) {
                if (max == 0) {
                    for (auto& st : t->inv) if (!st.empty() && (!pred || st.item == pred->item)) total += st.count;
                } else total += clear_inventory(*t, pred ? pred->item : -1, max);
                continue;
            }
            // tag / '*' predicates: Inventory.clearOrCountMatchingItems over inventory, carried and 2x2 grid
            int removed = 0;
            auto take = [&](ItemStack& st) {
                if (st.empty() || !pred->matches(st.item) || (max > 0 && removed >= max)) return;
                if (max == 0) { removed += st.count; return; }
                int n = max < 0 ? st.count : std::min(st.count, max - removed);
                st.count -= n;
                removed += n;
                if (st.count <= 0) st = {};
            };
            for (auto& st : t->inv) take(st);
            take(t->g.carried);
            for (auto& st : t->g.craft2) take(st);
            total += removed;
        }
        if (total == 0) {
            if (targets.size() == 1) throw err("clear.failed.single", {player_name(*targets[0])});
            throw err("clear.failed.multiple", {int(targets.size())});
        }
        bool test = max == 0;
        c.success(targets.size() == 1
                      ? tr(test ? "commands.clear.test.single" : "commands.clear.success.single", {total, player_name(*targets[0])})
                      : tr(test ? "commands.clear.test.multiple" : "commands.clear.success.multiple", {total, int(targets.size())}),
                  true);
        return total;
    });
    reg("kill", [](Cmd& c) {
        auto targets = c.targets_or_self("targets", true);
        for (auto& t : targets) kill(*t, "generic_kill");
        return respond(c, targets, true, [&](Text n) { return tr("commands.kill.success.single", {n}); },
                       [&](int k) { return tr("commands.kill.success.multiple", {k}); });
    });
    reg("damage", [](Cmd& c) {
        auto t = c.player("target");
        float amount = float(c.real("amount"));
        if (!hurt(*t, amount, short_id(c.str("damageType", "minecraft:generic")))) throw err("commands.damage.invulnerable");
        c.success(tr("commands.damage.success", {amount, player_name(*t)}), true);
        return 1;
    });

    reg("setblock", [](Cmd& c) {
        int x, y, z;
        c.block_pos("pos", x, y, z);
        auto& b = c.get<BlockSpec>("block");
        if (c.is("keep") && world.get_block(x, y, z) != blocks::air) throw err("commands.setblock.failed");
        BlockBatch batch;
        if (c.is("destroy") && world.get_block(x, y, z) != blocks::air) batch.set(x, y, z, blocks::air);  // ponytail: no drops
        bool ok = batch.set(x, y, z, b.state);
        batch.flush();
        if (!ok) throw err("commands.setblock.failed");
        c.success(tr("commands.setblock.success", {x, y, z}), true);
        return 1;
    });
    reg("fill", [](Cmd& c) {
        int x0, y0, z0, x1, y1, z1;
        c.block_pos("from", x0, y0, z0);
        c.block_pos("to", x1, y1, z1);
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        if (z0 > z1) std::swap(z0, z1);
        int64_t area = int64_t(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1);
        int limit = rule("max_block_modifications");
        if (area > limit) throw err("commands.fill.toobig", {limit, (long long)area});
        auto& b = c.get<BlockSpec>("block");
        std::optional<BlockSpec> filter;
        if (c.has("filter")) filter = c.get<BlockSpec>("filter");
        bool hollow = c.is("hollow"), outline = c.is("outline"), destroy = c.is("destroy"), keep = c.is("keep");
        BlockBatch batch;
        int count = 0;
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++)
                for (int z = z0; z <= z1; z++) {
                    int old = world.get_block(x, y, z);
                    if (filter && !filter->matches(old)) continue;
                    if (keep && old != blocks::air) continue;
                    bool edge = x == x0 || x == x1 || y == y0 || y == y1 || z == z0 || z == z1;
                    bool affected = false;
                    if (destroy && old != blocks::air) { batch.set(x, y, z, blocks::air); affected = true; }  // ponytail: no drops
                    int target = b.state;
                    if (!edge && outline) { if (affected) count++; continue; }
                    if (!edge && hollow) target = blocks::air;
                    if (batch.set(x, y, z, target) || affected) count++;
                }
        batch.flush();
        if (count == 0) throw err("commands.fill.failed");
        c.success(tr("commands.fill.success", {count}), true);
        return count;
    });
    reg("clone", [](Cmd& c) {
        if (c.has("sourceDimension") && c.str("sourceDimension") != "minecraft:overworld") throw fail(not_supported("clone (other dimensions)"));
        if (c.has("targetDimension") && c.str("targetDimension") != "minecraft:overworld") throw fail(not_supported("clone (other dimensions)"));
        int x0, y0, z0, x1, y1, z1, dx, dy, dz;
        c.block_pos("begin", x0, y0, z0);
        c.block_pos("end", x1, y1, z1);
        c.block_pos("destination", dx, dy, dz);
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        if (z0 > z1) std::swap(z0, z1);
        int sx = x1 - x0 + 1, sy = y1 - y0 + 1, sz = z1 - z0 + 1;
        int64_t area = int64_t(sx) * sy * sz;
        int limit = rule("max_block_modifications");
        if (area > limit) throw err("commands.clone.toobig", {limit, (long long)area});
        bool overlap = dx <= x1 && dx + sx - 1 >= x0 && dy <= y1 && dy + sy - 1 >= y0 && dz <= z1 && dz + sz - 1 >= z0;
        bool force = c.is("force"), move = c.is("move");
        if (overlap && !force && !move) throw err("commands.clone.overlap");
        if (dy < world.min_y() || dy + sy - 1 >= world.max_y()) throw err("argument.pos.outofworld");
        std::optional<BlockSpec> filter;
        if (c.has("filter")) filter = c.get<BlockSpec>("filter");
        bool masked = c.is("masked");
        std::vector<int> buf(static_cast<size_t>(area));
        for (int x = 0; x < sx; x++)
            for (int y = 0; y < sy; y++)
                for (int z = 0; z < sz; z++) buf[size_t((x * sy + y) * sz + z)] = world.get_block(x0 + x, y0 + y, z0 + z);
        BlockBatch batch;
        int count = 0;
        if (move)
            for (int x = 0; x < sx; x++)
                for (int y = 0; y < sy; y++)
                    for (int z = 0; z < sz; z++) {
                        int st = buf[size_t((x * sy + y) * sz + z)];
                        if ((filter && !filter->matches(st)) || (masked && st == blocks::air)) continue;
                        batch.set(x0 + x, y0 + y, z0 + z, blocks::air);
                    }
        for (int x = 0; x < sx; x++)
            for (int y = 0; y < sy; y++)
                for (int z = 0; z < sz; z++) {
                    int st = buf[size_t((x * sy + y) * sz + z)];
                    if ((filter && !filter->matches(st)) || (masked && st == blocks::air)) continue;
                    batch.set(dx + x, dy + y, dz + z, st);
                    count++;
                }
        batch.flush();
        if (count == 0) throw err("commands.clone.failed");
        c.success(tr("commands.clone.success", {count}), true);
        return count;
    });

    // time (26.3 world clocks)
    reg("time", [](Cmd& c) {
        int ci = 0;
        if (c.has("clock")) {
            std::string id = c.str("clock");
            ci = id == "minecraft:the_end" ? 1 : 0;
        }
        auto& clk = clocks[size_t(ci)];
        std::string cname = clk.id;
        auto wrap = [](int64_t t) { return int(t % 2147483647LL); };
        if (c.is("gametime")) {
            c.success(tr("commands.time.query.gametime", {(long long)game_time}), false);
            return wrap(game_time);
        }
        if (c.is("query")) {
            if (c.has("timeline")) {
                std::string tl = c.str("timeline");
                int64_t period = tl == "minecraft:day" || tl == "minecraft:villager_schedule" ? 24000 : tl == "minecraft:moon" ? 192000 : 0;
                if (ci != 0) throw err("commands.time.wrong_timeline_for_clock", {tl, cname});
                if (c.is("repetition")) {
                    int64_t reps = period ? clk.total / period : 0;
                    c.success(tr("commands.time.query.timeline.repetitions", {tl, (long long)reps}), false);
                    return wrap(reps);
                }
                int64_t cur = period ? clk.total % period : clk.total;
                c.success(tr("commands.time.query.timeline", {tl, (long long)cur}), false);
                return wrap(cur);
            }
            c.success(tr("commands.time.query.absolute", {cname, (long long)clk.total}), false);
            return wrap(clk.total);
        }
        if (c.is("set") && c.has("time")) {
            int64_t t = c.num("time");
            if (clk.total == t) throw err("commands.time.set.already_at_time", {cname, (long long)t});
            clk.total = t;
            clk.partial = 0;
            clock_changed(ci);
            c.success(tr("commands.time.set.absolute", {cname, (long long)t}), true);
            return int(t);
        }
        if (c.is("set")) {  // time marker (timeline/day.json markers, period 24000)
            std::string m = c.str("timemarker");
            static const std::pair<const char*, int> markers[] = {{"minecraft:day", 1000}, {"minecraft:noon", 6000}, {"minecraft:night", 13000},
                                                                  {"minecraft:midnight", 18000}, {"minecraft:roll_village_siege", 18000},
                                                                  {"minecraft:wake_up_from_sleep", 0}};
            int ticks = -1;
            if (ci == 0) for (auto& [k, v] : markers) if (m == k) ticks = v;
            if (ticks < 0) throw err("commands.time.no_time_marker_found", {cname, m});
            if (clk.total % 24000 == ticks) throw err("commands.time.set.already_at_time_marker", {cname, m});
            int64_t d = ticks - clk.total % 24000;
            clk.total += d > 0 ? d : 24000 + d;
            clk.partial = 0;
            clock_changed(ci);
            c.success(tr("commands.time.set.time_marker", {cname, m}), true);
            return wrap(clk.total);
        }
        if (c.is("add")) {
            clk.total = std::max<int64_t>(clk.total + c.num("time"), 0);
            clock_changed(ci);
            c.success(tr("commands.time.set.absolute", {cname, (long long)clk.total}), true);
            return wrap(clk.total);
        }
        if (c.is("pause") || c.is("resume")) {
            bool pause = c.is("pause");
            if (clk.paused == pause) throw err(pause ? "commands.time.pause.already_paused" : "commands.time.pause.already_running", {cname});
            clk.paused = pause;
            clock_changed(ci);
            c.success(tr(pause ? "commands.time.pause" : "commands.time.resume", {cname}), true);
            return 1;
        }
        if (c.is("rate")) {
            float r = float(c.real("rate"));
            if (clk.rate == r) throw err("commands.time.rate.already_same", {cname, r});
            clk.rate = r;
            clock_changed(ci);
            c.success(tr("commands.time.rate", {cname, r}), true);
            return 1;
        }
        return 0;
    });

    reg("weather", [](Cmd& c) {
        int d = int(c.num("duration", -1));
        if (c.is("clear")) { set_weather(d < 0 ? uniform(12000, 180000) : d, 0, false, false); c.success(tr("commands.weather.set.clear"), true); }
        else if (c.is("rain")) { set_weather(0, d < 0 ? uniform(12000, 24000) : d, true, false); c.success(tr("commands.weather.set.rain"), true); }
        else { set_weather(0, d < 0 ? uniform(3600, 15600) : d, true, true); c.success(tr("commands.weather.set.thunder"), true); }
        return d < 0 ? 0 : d;
    });

    reg("difficulty", [](Cmd& c) {
        int want = -1;
        for (int i = 0; i < 4; i++) if (c.is(DIFFICULTY_NAMES[i])) want = i;
        Text name = want >= 0 ? tr(std::string("options.difficulty.") + DIFFICULTY_NAMES[want]) : tr(std::string("options.difficulty.") + DIFFICULTY_NAMES[world_difficulty]);
        if (want < 0) { c.success(tr("commands.difficulty.query", {name}), false); return world_difficulty; }
        if (world_difficulty == want) throw err("commands.difficulty.failure", {DIFFICULTY_NAMES[want]});
        world_difficulty = want;
        broadcast(difficulty_packet());
        c.success(tr("commands.difficulty.success", {name}), true);
        return 0;
    });

    reg("gamerule", [](Cmd& c) {
        std::string id = node_name(c.seg.nodes[1].node);
        int i = game_rule_index(id);
        const auto& g = gen::GAME_RULES[i];
        std::string shown = g.id;
        auto ser = [&](int v) { return g.is_int ? std::to_string(v) : std::string(v ? "true" : "false"); };
        if (!c.has("value")) {
            c.success(tr("commands.gamerule.query", {shown, ser(game_rule_values[size_t(i)])}), false);
            return game_rule_values[size_t(i)];
        }
        int v = g.is_int ? int(c.num("value")) : int(c.flag("value"));
        if (game_rule_values[size_t(i)] == v) throw err("commands.gamerule.not_set", {shown, ser(v)});
        game_rule_values[size_t(i)] = v;
        if (shown == "advance_time") broadcast(set_time_packet());
        if (shown == "reduced_debug_info")
            for (auto& p : snapshot()) p->send(pkt(play_cb::entity_event).i32(p->eid).u8(v ? 22 : 23));
        if (shown == "immediate_respawn") broadcast(game_event(11, v ? 1.f : 0.f));
        if (shown == "limited_crafting") broadcast(game_event(12, v ? 1.f : 0.f));
        c.success(tr("commands.gamerule.set", {shown, ser(v)}), true);
        return v;
    });

    reg("seed", [](Cmd& c) {
        std::string seed = read_level_data()["level-seed"];
        Text s = styled(lit(seed), "green");
        Nbt click = Nbt::compound();
        click.set("action", Nbt::str("copy_to_clipboard"));
        click.set("value", Nbt::str(seed));
        s.set("click_event", click);
        Nbt hover = Nbt::compound();
        hover.set("action", Nbt::str("show_text"));
        hover.set("value", tr("chat.copy.click"));
        s.set("hover_event", hover);
        s.set("insertion", Nbt::str(seed));
        Text br = lit("[");
        append(br, s);
        append(br, lit("]"));
        c.success(tr("commands.seed.success", {br}), false);
        return int(atoll(seed.c_str()));
    });

    reg("setworldspawn", [](Cmd& c) {
        int x, y, z;
        if (c.has("pos")) c.block_pos("pos", x, y, z);
        else { x = int(std::floor(c.src.x)); y = int(std::floor(c.src.y)); z = int(std::floor(c.src.z)); }
        float yaw = 0, pitch = 0;
        if (c.has("rotation")) c.get<Coords>("rotation").rotation(c.src, yaw, pitch);
        world_spawn = {x, y, z};
        spawn_yaw = std::remainder(yaw, 360.f);
        spawn_pitch = std::clamp(pitch, -90.f, 90.f);
        broadcast(spawn_packet());
        save_level();
        c.success(tr("commands.setworldspawn.success", {x, y, z, spawn_yaw, spawn_pitch, "minecraft:overworld"}), true);
        return 1;
    });
    reg("spawnpoint", [](Cmd& c) {
        auto targets = c.targets_or_self("targets");
        int x, y, z;
        if (c.has("pos")) c.block_pos("pos", x, y, z);
        else { x = int(std::floor(c.src.x)); y = int(std::floor(c.src.y)); z = int(std::floor(c.src.z)); }
        float yaw = 0, pitch = 0;
        if (c.has("rotation")) c.get<Coords>("rotation").rotation(c.src, yaw, pitch);
        // ponytail: per-player respawn points are stored by the survival module once respawning exists.
        return respond(c, targets, true,
                       [&](Text n) { return tr("commands.spawnpoint.success.single", {x, y, z, yaw, pitch, "minecraft:overworld", n}); },
                       [&](int k) { return tr("commands.spawnpoint.success.multiple", {x, y, z, yaw, pitch, "minecraft:overworld", k}); });
    });

    // ops / bans / whitelist
    reg("op", [](Cmd& c) {
        int n = 0;
        for (auto& [name, uuid] : c.profiles("targets")) {
            if (ops.find(uuid, name)) continue;
            ListEntry e;
            e.uuid = uuid; e.name = name; e.level = prop_int("op-permission-level", 4);
            ops.rows.push_back(e);
            n++;
            if (auto p = find_player(name)) send_commands(*p);
            c.success(tr("commands.op.success", {name}), true);
        }
        if (!n) throw err("commands.op.failed");
        save_lists();
        return n;
    });
    reg("deop", [](Cmd& c) {
        int n = 0;
        for (auto& [name, uuid] : c.profiles("targets")) {
            ListEntry* e = ops.find(uuid, name);
            if (!e) continue;
            ops.rows.erase(ops.rows.begin() + (e - ops.rows.data()));
            n++;
            if (auto p = find_player(name)) send_commands(*p);
            c.success(tr("commands.deop.success", {name}), true);
        }
        if (!n) throw err("commands.deop.failed");
        save_lists();
        return n;
    });
    auto disconnect = [](Player& p, const Text& reason) {
        Writer w = pkt(play_cb::disconnect);
        write_nbt(w, reason);
        p.send(w);
        p.out.flush();
        p.quit_reason = plain(reason);
        kick(p);
    };
    reg("kick", [disconnect](Cmd& c) {
        auto targets = c.players("targets");
        Text reason = c.has("reason") ? resolve_message(c.str("reason"), c.src) : tr("multiplayer.disconnect.kicked");
        for (auto& t : targets) {
            disconnect(*t, reason);
            c.success(tr("commands.kick.success", {player_name(*t), reason}), true);
        }
        return int(targets.size());
    });
    reg("ban", [disconnect](Cmd& c) {
        Text reason = c.has("reason") ? resolve_message(c.str("reason"), c.src) : lit("Banned by an operator.");
        int n = 0;
        for (auto& [name, uuid] : c.profiles("targets")) {
            if (banned_players.find(uuid, name)) continue;
            ListEntry e;
            e.uuid = uuid; e.name = name; e.created = now_date(); e.source = c.src.name(); e.reason = plain(reason);
            banned_players.rows.push_back(e);
            n++;
            c.success(tr("commands.ban.success", {name, reason}), true);
            if (auto p = find_player(name)) disconnect(*p, tr("multiplayer.disconnect.banned"));
        }
        if (!n) throw err("commands.ban.failed");
        save_lists();
        return n;
    });
    reg("ban-ip", [disconnect](Cmd& c) {
        std::string target = c.str("target"), ip;
        in6_addr tmp;
        if (inet_pton(AF_INET, target.c_str(), &tmp) == 1 || inet_pton(AF_INET6, target.c_str(), &tmp) == 1) ip = target;
        else if (auto p = find_player(target)) ip = peer_ip(p->fd);
        else throw err("commands.banip.invalid");
        if (banned_ips.find_ip(ip)) throw err("commands.banip.failed");
        Text reason = c.has("reason") ? resolve_message(c.str("reason"), c.src) : lit("Banned by an operator.");
        ListEntry e;
        e.ip = ip; e.created = now_date(); e.source = c.src.name(); e.reason = plain(reason);
        banned_ips.rows.push_back(e);
        save_lists();
        std::vector<PlayerPtr> hit;
        for (auto& p : snapshot()) if (peer_ip(p->fd) == ip) hit.push_back(p);
        c.success(tr("commands.banip.success", {ip, reason}), true);
        if (!hit.empty()) {
            std::vector<Text> names;
            for (auto& p : hit) names.push_back(lit(p->name));
            c.success(tr("commands.banip.info", {int(hit.size()), join_list(names)}), true);
        }
        for (auto& p : hit) disconnect(*p, tr("multiplayer.disconnect.ip_banned"));
        return int(hit.size());
    });
    reg("pardon", [](Cmd& c) {
        int n = 0;
        for (auto& [name, uuid] : c.profiles("targets")) {
            ListEntry* e = banned_players.find(uuid, name);
            if (!e) continue;
            banned_players.rows.erase(banned_players.rows.begin() + (e - banned_players.rows.data()));
            n++;
            c.success(tr("commands.pardon.success", {name}), true);
        }
        if (!n) throw err("commands.pardon.failed");
        save_lists();
        return n;
    });
    reg("pardon-ip", [](Cmd& c) {
        std::string ip = c.str("target");
        in6_addr tmp;
        if (inet_pton(AF_INET, ip.c_str(), &tmp) != 1 && inet_pton(AF_INET6, ip.c_str(), &tmp) != 1) throw err("commands.pardonip.invalid");
        ListEntry* e = banned_ips.find_ip(ip);
        if (!e) throw err("commands.pardonip.failed");
        banned_ips.rows.erase(banned_ips.rows.begin() + (e - banned_ips.rows.data()));
        save_lists();
        c.success(tr("commands.pardonip.success", {ip}), true);
        return 1;
    });
    reg("banlist", [](Cmd& c) {
        std::vector<ListEntry*> rows;
        if (!c.is("ips")) for (auto& r : banned_players.rows) rows.push_back(&r);
        if (!c.is("players")) for (auto& r : banned_ips.rows) rows.push_back(&r);
        if (rows.empty()) { c.success(tr("commands.banlist.none"), false); return 0; }
        c.success(tr("commands.banlist.list", {int(rows.size())}), false);
        for (auto* r : rows) c.success(tr("commands.banlist.entry", {r->ip.empty() ? r->name : r->ip, r->source, r->reason}), false);
        return int(rows.size());
    });
    reg("whitelist", [](Cmd& c) {
        if (c.is("on") || c.is("off")) {
            bool on = c.is("on");
            if (prop_bool("white-list", false) == on) throw err(on ? "commands.whitelist.alreadyOn" : "commands.whitelist.alreadyOff");
            props["white-list"] = on ? "true" : "false";
            c.success(tr(on ? "commands.whitelist.enabled" : "commands.whitelist.disabled"), true);
            return 1;
        }
        if (c.is("list")) {
            if (whitelist.rows.empty()) { c.success(tr("commands.whitelist.none"), false); return 0; }
            std::string names;
            for (auto& r : whitelist.rows) names += (names.empty() ? "" : ", ") + r.name;
            c.success(tr("commands.whitelist.list", {int(whitelist.rows.size()), names}), false);
            return int(whitelist.rows.size());
        }
        if (c.is("reload")) { whitelist.load(); c.success(tr("commands.whitelist.reloaded"), true); return 1; }
        bool add = c.is("add");
        int n = 0;
        for (auto& [name, uuid] : c.profiles("targets")) {
            ListEntry* e = whitelist.find(uuid, name);
            if (add == (e != nullptr)) continue;
            if (add) { ListEntry e; e.uuid = uuid; e.name = name; whitelist.rows.push_back(e); }
            else whitelist.rows.erase(whitelist.rows.begin() + (e - whitelist.rows.data()));
            n++;
            c.success(tr(add ? "commands.whitelist.add.success" : "commands.whitelist.remove.success", {name}), true);
        }
        if (!n) throw err(add ? "commands.whitelist.add.failed" : "commands.whitelist.remove.failed");
        save_lists();
        return n;
    });

    reg("stop", [](Cmd& c) {
        c.success(tr("commands.stop.stopping"), true);
        request_shutdown_async();
        return 1;
    });
    reg("save-all", [](Cmd& c) {
        c.success(tr("commands.save.saving"), false);
        save_all();
        save_level();
        c.success(tr("commands.save.success"), true);
        return 1;
    });
    reg("save-on", [](Cmd& c) {
        if (!autosave_off) throw err("commands.save.alreadyOn");
        autosave_off = false;
        c.success(tr("commands.save.enabled"), true);
        return 1;
    });
    reg("save-off", [](Cmd& c) {
        if (autosave_off) throw err("commands.save.alreadyOff");
        autosave_off = true;
        c.success(tr("commands.save.disabled"), true);
        return 1;
    });

    reg("title", [](Cmd& c) {
        auto targets = c.players("targets");
        auto all = [&](const Writer& w) { for (auto& t : targets) t->send(w); };
        const char* key;
        if (c.is("clear")) { all(pkt(play_cb::clear_titles).boolean(false)); key = "cleared"; }
        else if (c.is("reset")) { all(pkt(play_cb::clear_titles).boolean(true)); key = "reset"; }
        else if (c.is("times")) {
            all(pkt(play_cb::set_titles_animation).i32(int32_t(c.num("fadeIn"))).i32(int32_t(c.num("stay"))).i32(int32_t(c.num("fadeOut"))));
            key = "times";
        } else {
            int id = c.is("actionbar") ? play_cb::set_action_bar_text : c.is("subtitle") ? play_cb::set_subtitle_text : play_cb::set_title_text;
            key = c.is("actionbar") ? "show.actionbar" : c.is("subtitle") ? "show.subtitle" : "show.title";
            for (auto& t : targets) {
                Writer w = pkt(id);
                write_nbt(w, c.text("title"));
                t->send(w);
            }
        }
        std::string k = std::string("commands.title.") + key;
        return respond(c, targets, true, [&](Text n) { return tr(k + ".single", {n}); }, [&](int n) { return tr(k + ".multiple", {n}); });
    });
    reg("tellraw", [](Cmd& c) {
        auto targets = c.players("targets");
        for (auto& t : targets) send_text(*t, c.text("message"));
        return int(targets.size());
    });

    static const char* SOURCES[] = {"master", "music", "record", "weather", "block", "hostile", "neutral", "player", "ambient", "voice", "ui"};
    reg("playsound", [](Cmd& c) {
        std::string sound = c.str("sound");
        int source = 0;
        for (int i = 0; i < 11; i++) if (c.is(SOURCES[i])) source = i;
        std::vector<PlayerPtr> targets = c.has("targets") ? c.players("targets") : std::vector<PlayerPtr>{c.self_player()};
        Vec3 pos = c.has("pos") ? c.vec("pos") : Vec3{c.src.x, c.src.y, c.src.z};
        float volume = float(c.real("volume", 1)), pitch = float(c.real("pitch", 1)), minv = float(c.real("minVolume", 0));
        double range = volume > 1 ? 16.0 * volume : 16.0;
        int n = 0;
        for (auto& t : targets) {
            double dx = pos.x - t->x, dy = pos.y - t->y, dz = pos.z - t->z, d2 = dx * dx + dy * dy + dz * dz;
            Vec3 at = pos;
            float vol = volume;
            if (d2 > range * range) {
                if (minv <= 0) continue;
                double d = std::sqrt(d2);
                at = {t->x + dx / d * 2, t->y + dy / d * 2, t->z + dz / d * 2};
                vol = minv;
            }
            t->send(pkt(play_cb::sound).varint(0).str(sound).boolean(false).varint(source).i32(int32_t(at.x * 8)).i32(int32_t(at.y * 8))
                        .i32(int32_t(at.z * 8)).f32(vol).f32(pitch).i64(int64_t(cmd_rng())));
            n++;
        }
        if (!n) throw err("commands.playsound.failed");
        return respond(c, targets, true, [&](Text nm) { return tr("commands.playsound.success.single", {sound, nm}); },
                       [&](int k) { return tr("commands.playsound.success.multiple", {sound, k}); });
    });
    reg("stopsound", [](Cmd& c) {
        auto targets = c.players("targets");
        int source = -1;
        for (int i = 0; i < 11; i++) if (c.is(SOURCES[i])) source = i;
        std::string sound = c.str("sound");
        Writer w = pkt(play_cb::stop_sound);
        if (source >= 0 && !sound.empty()) w.u8(3).varint(source).str(sound);
        else if (source >= 0) w.u8(1).varint(source);
        else if (!sound.empty()) w.u8(2).str(sound);
        else w.u8(0);
        for (auto& t : targets) t->send(w);
        if (source >= 0 && !sound.empty()) c.success(tr("commands.stopsound.success.source.sound", {sound, SOURCES[source]}), true);
        else if (source >= 0) c.success(tr("commands.stopsound.success.source.any", {SOURCES[source]}), true);
        else if (!sound.empty()) c.success(tr("commands.stopsound.success.sourceless.sound", {sound}), true);
        else c.success(tr("commands.stopsound.success.sourceless.any"), true);
        return int(targets.size());
    });
    reg("particle", [](Cmd& c) {
        static const std::string simple =
            " angry_villager bubble sulfur_bubbles noxious_gas noxious_gas_cloud cloud copper_fire_flame crit damage_indicator dripping_lava "
            "falling_lava landing_lava dripping_water falling_water elder_guardian enchanted_hit enchant end_rod explosion_emitter explosion gust "
            "small_gust gust_emitter_large gust_emitter_small sonic_boom firework fishing flame infested cherry_leaves pale_oak_leaves "
            "red_poplar_leaves orange_poplar_leaves yellow_poplar_leaves sculk_soul sculk_charge_pop soul_fire_flame soul happy_villager composter "
            "heart pause_mob_growth reset_mob_growth item_slime item_cobweb item_snowball large_smoke lava mycelium note poof portal rain smoke "
            "white_smoke sneeze spit squid_ink sweep_attack totem_of_undying underwater splash witch bubble_pop current_down bubble_column_up "
            "nautilus dolphin campfire_cosy_smoke campfire_signal_smoke dripping_honey falling_honey landing_honey falling_nectar "
            "falling_spore_blossom ash crimson_spore warped_spore spore_blossom_air dripping_obsidian_tear falling_obsidian_tear "
            "landing_obsidian_tear reverse_portal white_ash small_flame snowflake dripping_dripstone_lava falling_dripstone_lava "
            "dripping_dripstone_water falling_dripstone_water glow_squid_ink glow wax_on wax_off electric_spark scrape egg_crack dust_plume "
            "trial_spawner_detection trial_spawner_detection_ominous vault_connection ominous_spawning raid_omen trial_omen firefly sulfur_cube_goo ";
        std::string id = c.str("name");
        // ponytail: particles with options (dust, block, item...) need their option codecs; only simple types are sent.
        if (simple.find(" " + short_id(id) + " ") == std::string::npos) throw fail(not_supported("particle " + id));
        int pid = registry_index(*registry("minecraft:particle_type"), id);
        Vec3 pos = c.has("pos") ? c.vec("pos") : Vec3{c.src.x, c.src.y, c.src.z};
        Vec3 delta = c.has("delta") ? c.get<Coords>("delta").pos(Source{}) : Vec3{0, 0, 0};
        float speed = float(c.real("speed", 0));
        int count = int(c.num("count", 0));
        bool force = c.is("force");
        std::vector<PlayerPtr> viewers = c.has("viewers") ? c.players("viewers") : snapshot();
        Writer w = pkt(play_cb::level_particles);
        w.varint(pid).boolean(force).boolean(false).f64(pos.x).f64(pos.y).f64(pos.z).f32(float(delta.x)).f32(float(delta.y))
            .f32(float(delta.z)).f32(speed).f32(speed).f32(speed).varint(count).varint(0);
        int n = 0;
        double range = force ? 512 : 32;
        for (auto& p : viewers) {
            double bx = std::floor(p->x) + 0.5 - pos.x, by = std::floor(p->y) + 0.5 - pos.y, bz = std::floor(p->z) + 0.5 - pos.z;
            if (bx * bx + by * by + bz * bz < range * range) { p->send(w); n++; }
        }
        if (!n) throw err("commands.particle.failed");
        c.success(tr("commands.particle.success", {id}), true);
        return n;
    });

    reg("worldborder", [](Cmd& c) {
        auto f2 = [](double v) { return fmt_f(v, 2); };
        if (c.is("get")) {
            c.success(tr("commands.worldborder.get", {fmt_f(border.size(), 0)}), false);
            return int(std::floor(border.size() + 0.5));
        }
        if (c.is("center")) {
            Coords cc = c.get<Coords>("pos");
            Vec3 v = cc.pos(c.src);
            double x = cc.c[0].rel ? c.src.x + cc.c[0].v : cc.c[0].v, z = cc.c[1].rel ? c.src.z + cc.c[1].v : cc.c[1].v;
            (void)v;
            if (border.cx == x && border.cz == z) throw err("commands.worldborder.center.failed");
            if (std::abs(x) > 2.9999984E7 || std::abs(z) > 2.9999984E7) throw err("commands.worldborder.set.failed.far", {2.9999984E7});
            border.cx = x;
            border.cz = z;
            broadcast(pkt(play_cb::set_border_center).f64(x).f64(z));
            c.success(tr("commands.worldborder.center.success", {f2(x), f2(z)}), true);
            return 0;
        }
        if (c.is("damage")) {
            bool amount = c.is("amount");
            double v = c.real(amount ? "damagePerBlock" : "distance");
            double& field = amount ? border.damage : border.buffer;
            if (field == v) throw err(amount ? "commands.worldborder.damage.amount.failed" : "commands.worldborder.damage.buffer.failed");
            field = v;
            c.success(tr(amount ? "commands.worldborder.damage.amount.success" : "commands.worldborder.damage.buffer.success", {f2(v)}), true);
            return int(v);
        }
        if (c.is("warning")) {
            if (c.is("time")) {
                int t = int(c.num("time"));
                if (border.warn_time == t / 20) throw err("commands.worldborder.warning.time.failed");
                border.warn_time = int(t / 20);
                broadcast(pkt(play_cb::set_border_warning_delay).varint(border.warn_time));
                c.success(tr("commands.worldborder.warning.time.success", {f2(t / 20.0)}), true);
                return t;
            }
            int d = int(c.num("distance"));
            if (border.warn_dist == d) throw err("commands.worldborder.warning.distance.failed");
            border.warn_dist = d;
            broadcast(pkt(play_cb::set_border_warning_distance).varint(d));
            c.success(tr("commands.worldborder.warning.distance.success", {d}), true);
            return d;
        }
        double current = border.size();
        double target = c.real("distance") + (c.is("add") ? current : 0);
        int64_t ticks = c.num("time", 0);
        if (c.is("add") && ticks == 0 && c.has("time")) ticks = c.num("time");
        if (current == target) throw err("commands.worldborder.set.failed.nochange");
        if (target < 1) throw err("commands.worldborder.set.failed.small");
        if (target > 5.999997E7) throw err("commands.worldborder.set.failed.big", {5.999997E7f});
        std::string fd = fmt_f(target, 1);
        if (ticks > 0) {
            border.from = current;
            border.to = target;
            border.lerp_total = border.lerp_left = ticks;
            Writer w = pkt(play_cb::set_border_lerp_size);
            w.f64(current).f64(target);
            write_varlong(w, ticks);
            broadcast(w);
            c.success(tr(target > current ? "commands.worldborder.set.grow" : "commands.worldborder.set.shrink", {fd, f2(ticks / 20.0)}), true);
        } else {
            border.from = border.to = target;
            border.lerp_left = 0;
            broadcast(pkt(play_cb::set_border_size).f64(target));
            c.success(tr("commands.worldborder.set.immediate", {fd}), true);
        }
        return int(target - current);
    });

    reg("experience", [](Cmd& c) {
        bool levels = c.is("levels");
        auto send_xp = [](Player& p) { send_experience(p); };
        auto need = [](int lvl) { return lvl >= 30 ? 112 + (lvl - 30) * 9 : lvl >= 15 ? 37 + (lvl - 15) * 5 : 7 + lvl * 2; };  // getXpNeededForNextLevel
        auto give_points = [&](Player& p, int amount) {  // Player.giveExperiencePoints
            p.g.xp_progress += float(amount) / float(need(p.g.xp_level));
            p.g.xp_total = std::clamp(p.g.xp_total + amount, 0, INT32_MAX);
            while (p.g.xp_progress < 0) {
                float f = p.g.xp_progress * float(need(p.g.xp_level));
                if (p.g.xp_level > 0) { p.g.xp_level--; p.g.xp_progress = 1 + f / float(need(p.g.xp_level)); }
                else { p.g.xp_level = 0; p.g.xp_progress = 0; }
            }
            while (p.g.xp_progress >= 1) {
                p.g.xp_progress = (p.g.xp_progress - 1) * float(need(p.g.xp_level));
                p.g.xp_level++;
                p.g.xp_progress /= float(need(p.g.xp_level));
            }
        };
        if (c.is("query")) {
            auto t = c.player("target");
            int v = levels ? t->g.xp_level : int(t->g.xp_progress * float(need(t->g.xp_level)));
            c.success(tr(levels ? "commands.experience.query.levels" : "commands.experience.query.points", {player_name(*t), v}), false);
            return v;
        }
        auto targets = c.players("target");
        int amount = int(c.num("amount"));
        bool set = c.is("set");
        for (auto& t : targets) {
            if (set && levels) { t->g.xp_level = std::max(0, amount); }
            else if (set) {
                if (amount > need(t->g.xp_level)) throw err("commands.experience.set.points.invalid");
                t->g.xp_progress = float(amount) / float(need(t->g.xp_level));
            } else if (levels) { t->g.xp_level = std::max(0, t->g.xp_level + amount); }
            else give_points(*t, amount);
            send_xp(*t);
        }
        std::string k = std::string("commands.experience.") + (set ? "set." : "add.") + (levels ? "levels" : "points") + ".success";
        return respond(c, targets, true, [&](Text n) { return tr(k + ".single", {amount, n}); }, [&](int n) { return tr(k + ".multiple", {amount, n}); });
    });

    reg("tag", [](Cmd& c) {
        auto targets = c.players("targets");
        if (c.is("list")) {
            std::set<std::string> all;
            for (auto& t : targets) for (auto& g : entity_tags[uuid_hex(*t)]) all.insert(g);
            std::vector<Text> names;
            for (auto& g : all) names.push_back(lit(g));
            if (targets.size() == 1) {
                if (all.empty()) c.success(tr("commands.tag.list.single.empty", {player_name(*targets[0])}), false);
                else c.success(tr("commands.tag.list.single.success", {player_name(*targets[0]), int(all.size()), join_list(names)}), false);
            } else if (all.empty()) c.success(tr("commands.tag.list.multiple.empty", {int(targets.size())}), false);
            else c.success(tr("commands.tag.list.multiple.success", {int(targets.size()), int(all.size()), join_list(names)}), false);
            return int(all.size());
        }
        bool add = c.is("add");
        std::string name = c.str("name");
        int n = 0;
        for (auto& t : targets) {
            auto& set = entity_tags[uuid_hex(*t)];
            if (add ? (set.size() < 1024 && set.insert(name).second) : set.erase(name) > 0) n++;
        }
        if (!n) throw err(add ? "commands.tag.add.failed" : "commands.tag.remove.failed");
        std::string k = add ? "commands.tag.add.success" : "commands.tag.remove.success";
        c.success(targets.size() == 1 ? tr(k + ".single", {name, player_name(*targets[0])}) : tr(k + ".multiple", {name, int(targets.size())}), true);
        return n;
    });

    reg("team", [](Cmd& c) {
        auto get_team = [&]() -> Team& {
            std::string n = c.str("team");
            auto it = teams.find(n);
            if (it == teams.end()) throw err("team.notFound", {n});
            return it->second;
        };
        auto team_text = [](const Team& t) {  // PlayerTeam.getFormattedDisplayName
            Text n = lit("[");
            append(n, t.display);
            append(n, lit("]"));
            if (t.color >= 0) n.set("color", Nbt::str(TEAM_COLORS[t.color]));
            Nbt hover = Nbt::compound();
            hover.set("action", Nbt::str("show_text"));
            hover.set("value", lit(t.name));
            n.set("hover_event", hover);
            n.set("insertion", Nbt::str(t.name));
            return n;
        };
        if (c.is("add")) {
            std::string n = c.str("team");
            if (teams.count(n)) throw err("commands.team.add.duplicate");
            Team t;
            t.name = n;
            t.display = c.has("displayName") ? c.text("displayName") : lit(n);
            teams[n] = t;
            broadcast(team_packet(t, 0));
            c.success(tr("commands.team.add.success", {team_text(t)}), true);
            return int(teams.size());
        }
        if (c.is("list")) {
            if (c.has("team")) {
                Team& t = get_team();
                if (t.members.empty()) { c.success(tr("commands.team.list.members.empty", {team_text(t)}), false); return 0; }
                std::vector<Text> names;
                for (auto& m : t.members) names.push_back(lit(m));
                c.success(tr("commands.team.list.members.success", {team_text(t), int(t.members.size()), join_list(names)}), false);
                return int(t.members.size());
            }
            if (teams.empty()) { c.success(tr("commands.team.list.teams.empty"), false); return 0; }
            std::vector<Text> names;
            for (auto& [n, t] : teams) names.push_back(team_text(t));
            c.success(tr("commands.team.list.teams.success", {int(teams.size()), join_list(names)}), false);
            return int(teams.size());
        }
        if (c.is("leave")) {
            auto members = c.holders("members");
            for (auto& m : members)
                if (Team* t = team_of(m)) { std::erase(t->members, m); broadcast(team_packet(*t, 4, {m})); }
            c.success(members.size() == 1 ? tr("commands.team.leave.success.single", {members[0]})
                                          : tr("commands.team.leave.success.multiple", {int(members.size())}), true);
            return int(members.size());
        }
        Team& t = get_team();
        if (c.is("remove")) {
            Text tt = team_text(t);
            broadcast(team_packet(t, 1));
            teams.erase(t.name);
            c.success(tr("commands.team.remove.success", {tt}), true);
            return int(teams.size());
        }
        if (c.is("empty")) {
            if (t.members.empty()) throw err("commands.team.empty.unchanged");
            int n = int(t.members.size());
            broadcast(team_packet(t, 4, t.members));
            t.members.clear();
            c.success(tr("commands.team.empty.success", {n, team_text(t)}), true);
            return n;
        }
        if (c.is("join")) {
            std::vector<std::string> members = c.has("members") ? c.holders("members") : std::vector<std::string>{c.self_player()->name};
            for (auto& m : members) {
                if (Team* old = team_of(m)) { std::erase(old->members, m); broadcast(team_packet(*old, 4, {m})); }
                t.members.push_back(m);
            }
            broadcast(team_packet(t, 3, members));
            c.success(members.size() == 1 ? tr("commands.team.join.success.single", {members[0], team_text(t)})
                                          : tr("commands.team.join.success.multiple", {int(members.size()), team_text(t)}), true);
            return int(members.size());
        }
        // modify
        auto changed = [&] { broadcast(team_packet(t, 2)); };
        if (c.is("displayName")) {
            Text v = c.text("displayName");
            if (t.display == v) throw err("commands.team.option.name.unchanged");
            t.display = v; changed();
            c.success(tr("commands.team.option.name.success", {team_text(t)}), true);
        } else if (c.is("color")) {
            int v = int(c.num("value"));
            if (t.color == v) throw err("commands.team.option.color.unchanged");
            t.color = v; changed();
            if (v < 0) c.success(tr("commands.team.option.color.clear.success", {team_text(t)}), true);
            else c.success(tr("commands.team.option.color.success", {team_text(t), TEAM_COLORS[v]}), true);
        } else if (c.is("friendlyFire")) {
            bool v = c.flag("allowed");
            if (t.friendly_fire == v) throw err(v ? "commands.team.option.friendlyfire.alreadyEnabled" : "commands.team.option.friendlyfire.alreadyDisabled");
            t.friendly_fire = v; changed();
            c.success(tr(v ? "commands.team.option.friendlyfire.enabled" : "commands.team.option.friendlyfire.disabled", {team_text(t)}), true);
        } else if (c.is("seeFriendlyInvisibles")) {
            bool v = c.flag("allowed");
            if (t.see_invisible == v) throw err(v ? "commands.team.option.seeFriendlyInvisibles.alreadyEnabled" : "commands.team.option.seeFriendlyInvisibles.alreadyDisabled");
            t.see_invisible = v; changed();
            c.success(tr(v ? "commands.team.option.seeFriendlyInvisibles.enabled" : "commands.team.option.seeFriendlyInvisibles.disabled", {team_text(t)}), true);
        } else if (c.is("nametagVisibility") || c.is("deathMessageVisibility") || c.is("collisionRule")) {
            bool coll = c.is("collisionRule");
            const char** names = coll ? COLLISION : VISIBILITY;
            int v = 0;
            for (int i = 0; i < 4; i++) if (c.is(names[i])) v = i;
            int& field = coll ? t.collision : c.is("nametagVisibility") ? t.nametag : t.deathmsg;
            std::string opt = coll ? "collisionRule" : c.is("nametagVisibility") ? "nametagVisibility" : "deathMessageVisibility";
            if (field == v) throw err("commands.team.option." + opt + ".unchanged");
            field = v; changed();
            c.success(tr("commands.team.option." + opt + ".success", {team_text(t), tr(std::string(coll ? "team.collision." : "team.visibility.") + names[v])}), true);
        } else if (c.is("prefix") || c.is("suffix")) {
            bool pre = c.is("prefix");
            Text v = c.text(pre ? "prefix" : "suffix");
            (pre ? t.prefix : t.suffix) = v;
            changed();
            c.success(tr(pre ? "commands.team.option.prefix.success" : "commands.team.option.suffix.success", {v}), true);
        }
        return 0;
    });

    reg("scoreboard", [](Cmd& c) {
        auto get_obj = [&](const char* arg) -> Objective& {
            std::string n = c.str(arg);
            auto it = objectives.find(n);
            if (it == objectives.end()) throw err("arguments.objective.notFound", {n});
            return it->second;
        };
        auto obj_text = [](const Objective& o) {  // Objective.getFormattedDisplayName
            Text n = lit("[");
            append(n, o.display);
            append(n, lit("]"));
            Nbt hover = Nbt::compound();
            hover.set("action", Nbt::str("show_text"));
            hover.set("value", lit(o.name));
            n.set("hover_event", hover);
            return n;
        };
        auto writable = [&](Objective& o) {
            static const char* ro[] = {"health", "food", "air", "armor", "xp", "level"};
            for (auto r : ro) if (o.criteria == r) throw err("arguments.objective.readonly", {o.name});
            return &o;
        };
        auto broadcast_score = [](const std::string& h, const std::string& o) {
            if (objective_displayed(o)) broadcast(score_packet(h, o, scores[h][o]));
        };
        if (c.is("objectives")) {
            if (c.is("list")) {
                if (objectives.empty()) { c.success(tr("commands.scoreboard.objectives.list.empty"), false); return 0; }
                std::vector<Text> names;
                for (auto& [n, o] : objectives) names.push_back(obj_text(o));
                c.success(tr("commands.scoreboard.objectives.list.success", {int(objectives.size()), join_list(names)}), false);
                return int(objectives.size());
            }
            if (c.is("add")) {
                std::string n = c.str("objective");
                if (objectives.count(n)) throw err("commands.scoreboard.objectives.add.duplicate");
                Objective o;
                o.name = n;
                o.criteria = c.str("criteria");
                o.display = c.has("displayName") ? c.text("displayName") : lit(n);
                o.hearts = o.criteria == "health";
                objectives[n] = o;
                c.success(tr("commands.scoreboard.objectives.add.success", {obj_text(o)}), true);
                return int(objectives.size());
            }
            if (c.is("setdisplay")) {
                int slot = int(c.num("slot"));
                auto& cur = display_slots[size_t(slot)];
                std::string want = c.has("objective") ? get_obj("objective").name : "";
                if (want.empty()) {
                    if (cur.empty()) throw err("commands.scoreboard.objectives.display.alreadyEmpty");
                } else if (cur == want) throw err("commands.scoreboard.objectives.display.alreadySet");
                std::string old = cur;
                cur = want;
                if (!want.empty() && std::count(display_slots.begin(), display_slots.end(), want) == 1) {
                    broadcast(objective_packet(objectives[want], 0));
                    for (auto& [h, m] : scores) if (m.count(want)) broadcast(score_packet(h, want, m[want]));
                }
                broadcast(display_packet(slot));
                if (!old.empty() && !objective_displayed(old)) broadcast(objective_packet(objectives[old], 1));
                std::string sname = slot < 3 ? (const char*[]){"list", "sidebar", "below_name"}[slot] : std::string("sidebar.team.") + TEAM_COLORS[slot - 3];
                if (want.empty()) c.success(tr("commands.scoreboard.objectives.display.cleared", {sname}), true);
                else c.success(tr("commands.scoreboard.objectives.display.set", {sname, objectives[want].display}), true);
                return 0;
            }
            Objective& o = get_obj("objective");
            if (c.is("remove")) {
                Text ot = obj_text(o);
                if (objective_displayed(o.name)) broadcast(objective_packet(o, 1));
                for (auto& s : display_slots) if (s == o.name) s.clear();
                for (auto& [h, m] : scores) m.erase(o.name);
                c.success(tr("commands.scoreboard.objectives.remove.success", {ot}), true);
                objectives.erase(o.name);
                return int(objectives.size());
            }
            // modify
            if (c.is("displayname")) {
                o.display = c.text("displayName");
                c.success(tr("commands.scoreboard.objectives.modify.displayname", {o.name, obj_text(o)}), true);
            } else if (c.is("rendertype")) {
                o.hearts = c.is("hearts");
                c.success(tr("commands.scoreboard.objectives.modify.rendertype", {obj_text(o)}), true);
            } else if (c.is("displayautoupdate")) {
                o.auto_update = c.flag("value");
                c.success(tr(o.auto_update ? "commands.scoreboard.objectives.modify.displayAutoUpdate.enable"
                                           : "commands.scoreboard.objectives.modify.displayAutoUpdate.disable", {obj_text(o)}), true);
            } else if (c.is("numberformat")) {
                o.fmt = {};
                if (c.is("blank")) o.fmt.type = 0;
                else if (c.is("fixed")) { o.fmt.type = 2; o.fmt.data = c.text("contents"); }
                else if (c.is("styled")) { o.fmt.type = 1; o.fmt.data = c.get<Nbt>("style"); }
                c.success(tr(o.fmt.type < 0 ? "commands.scoreboard.objectives.modify.objectiveFormat.clear"
                                            : "commands.scoreboard.objectives.modify.objectiveFormat.set", {obj_text(o)}), true);
            }
            if (objective_displayed(o.name)) broadcast(objective_packet(o, 2));
            return 0;
        }
        // players
        if (c.is("list")) {
            if (!c.has("target")) {
                std::vector<Text> names;
                for (auto& [h, m] : scores) if (!m.empty()) names.push_back(lit(h));
                if (names.empty()) c.success(tr("commands.scoreboard.players.list.empty"), false);
                else c.success(tr("commands.scoreboard.players.list.success", {int(names.size()), join_list(names)}), false);
                return int(names.size());
            }
            std::string h = c.holders("target")[0];
            auto& m = scores[h];
            if (m.empty()) { c.success(tr("commands.scoreboard.players.list.entity.empty", {h}), false); return 0; }
            c.success(tr("commands.scoreboard.players.list.entity.success", {h, int(m.size())}), false);
            for (auto& [on, s] : m) c.success(tr("commands.scoreboard.players.list.entity.entry", {obj_text(objectives[on]), s.value}), false);
            return int(m.size());
        }
        if (c.is("get")) {
            std::string h = c.holders("target")[0];
            Objective& o = get_obj("objective");
            auto it = scores[h].find(o.name);
            if (it == scores[h].end()) throw err("commands.scoreboard.players.get.null", {o.name, h});
            c.success(tr("commands.scoreboard.players.get.success", {h, it->second.value, obj_text(o)}), false);
            return it->second.value;
        }
        auto holders = c.holders("targets");
        auto many = [&](const std::string& key, std::initializer_list<TArg> single, std::initializer_list<TArg> multiple) {
            c.success(holders.size() == 1 ? tr(key + ".single", single) : tr(key + ".multiple", multiple), true);
        };
        if (c.is("reset")) {
            for (auto& h : holders) {
                if (c.has("objective")) {
                    Objective& o = get_obj("objective");
                    scores[h].erase(o.name);
                    broadcast(reset_score_packet(h, &o.name));
                } else {
                    scores.erase(h);
                    broadcast(reset_score_packet(h, nullptr));
                }
            }
            if (c.has("objective")) many("commands.scoreboard.players.reset.specific", {obj_text(get_obj("objective")), holders[0]}, {obj_text(get_obj("objective")), int(holders.size())});
            else many("commands.scoreboard.players.reset.all", {holders[0]}, {int(holders.size())});
            return int(holders.size());
        }
        if (c.is("enable")) {
            Objective& o = get_obj("objective");
            if (o.criteria != "trigger") throw err("commands.scoreboard.players.enable.invalid");
            int n = 0;
            for (auto& h : holders) {
                auto& s = scores[h][o.name];
                if (s.locked) { s.locked = false; n++; }
            }
            if (!n) throw err("commands.scoreboard.players.enable.failed");
            many("commands.scoreboard.players.enable.success", {obj_text(o), holders[0]}, {obj_text(o), int(holders.size())});
            return n;
        }
        if (c.is("display")) {
            Objective& o = get_obj("objective");
            bool name = c.is("name");
            for (auto& h : holders) {
                auto& s = scores[h][o.name];
                if (name) { s.has_display = c.has("name"); if (s.has_display) s.display = c.text("name"); }
                else {
                    s.fmt = {};
                    if (c.is("blank")) s.fmt.type = 0;
                    else if (c.is("fixed")) { s.fmt.type = 2; s.fmt.data = c.text("contents"); }
                    else if (c.is("styled")) { s.fmt.type = 1; s.fmt.data = c.get<Nbt>("style"); }
                }
                broadcast_score(h, o.name);
            }
            if (name) {
                if (c.has("name")) many("commands.scoreboard.players.display.name.set.success", {c.text("name"), holders[0], obj_text(o)}, {c.text("name"), int(holders.size()), obj_text(o)});
                else many("commands.scoreboard.players.display.name.clear.success", {holders[0], obj_text(o)}, {int(holders.size()), obj_text(o)});
            } else if (c.is("numberformat") && (c.is("blank") || c.is("fixed") || c.is("styled")))
                many("commands.scoreboard.players.display.numberFormat.set.success", {holders[0], obj_text(o)}, {int(holders.size()), obj_text(o)});
            else many("commands.scoreboard.players.display.numberFormat.clear.success", {holders[0], obj_text(o)}, {int(holders.size()), obj_text(o)});
            return int(holders.size());
        }
        if (c.is("operation")) {
            Objective& to = *writable(get_obj("targetObjective"));
            Objective& from = get_obj("sourceObjective");
            auto sources = c.holders("source");
            std::string op = c.str("operation");
            int64_t total = 0;
            for (auto& h : holders) {
                auto& a = scores[h][to.name];
                for (auto& sh : sources) {
                    auto& b = scores[sh][from.name];
                    int x = a.value, y = b.value;
                    if (op == "=") x = y;
                    else if (op == "+=") x = int(uint32_t(x) + uint32_t(y));
                    else if (op == "-=") x = int(uint32_t(x) - uint32_t(y));
                    else if (op == "*=") x = int(uint32_t(x) * uint32_t(y));
                    else if (op == "/=") { if (y != 0) x = int(std::floor(double(x) / y)); }
                    else if (op == "%=") { if (y != 0) x = ((x % y) + y) % y; }
                    else if (op == "<") x = std::min(x, y);
                    else if (op == ">") x = std::max(x, y);
                    else if (op == "><") { a.value = y; b.value = x; broadcast_score(sh, from.name); continue; }
                    a.value = x;
                }
                total += a.value;
                broadcast_score(h, to.name);
            }
            if (holders.size() == 1) c.success(tr("commands.scoreboard.players.operation.success.single", {obj_text(to), holders[0], scores[holders[0]][to.name].value}), true);
            else c.success(tr("commands.scoreboard.players.operation.success.multiple", {obj_text(to), int(holders.size())}), true);
            return int(total);
        }
        Objective& o = *writable(get_obj("objective"));
        int v = int(c.num("score"));
        int64_t total = 0;
        for (auto& h : holders) {
            auto& s = scores[h][o.name];
            if (c.is("set")) s.value = v;
            else if (c.is("add")) s.value = int(uint32_t(s.value) + uint32_t(v));
            else s.value = int(uint32_t(s.value) - uint32_t(v));
            total += s.value;
            broadcast_score(h, o.name);
        }
        std::string verb = c.is("set") ? "set" : c.is("add") ? "add" : "remove";
        std::string key = "commands.scoreboard.players." + verb + ".success";
        if (verb == "set") many(key, {obj_text(o), holders[0], v}, {obj_text(o), int(holders.size()), v});
        else many(key, {v, obj_text(o), holders[0], scores[holders[0]][o.name].value}, {v, obj_text(o), int(holders.size())});
        return int(total);
    });

    reg("trigger", [](Cmd& c) {
        auto self = c.self_player();
        std::string n = c.str("objective");
        auto it = objectives.find(n);
        if (it == objectives.end()) throw err("arguments.objective.notFound", {n});
        Objective& o = it->second;
        if (o.criteria != "trigger") throw err("commands.trigger.failed.invalid");
        auto sit = scores[self->name].find(n);
        if (sit == scores[self->name].end() || sit->second.locked) throw err("commands.trigger.failed.unprimed");
        Score& s = sit->second;
        s.locked = true;
        Text ot = lit("[");
        append(ot, o.display);
        append(ot, lit("]"));
        if (c.is("set")) { s.value = int(c.num("value")); c.success(tr("commands.trigger.set.success", {ot, s.value}), true); }
        else if (c.is("add")) { s.value += int(c.num("value")); c.success(tr("commands.trigger.add.success", {ot, int(c.num("value"))}), true); }
        else { s.value += 1; c.success(tr("commands.trigger.simple.success", {ot}), true); }
        if (objective_displayed(n)) broadcast(score_packet(self->name, n, s));
        return s.value;
    });

    reg("rotate", [](Cmd& c) {
        auto t = c.player("target");
        if (c.has("rotation")) {
            float yaw, pitch;
            Source s = c.src;
            s.yaw = t->yaw;
            s.pitch = t->pitch;
            c.get<Coords>("rotation").rotation(s, yaw, pitch);
            t->yaw = std::remainder(yaw, 360.f);
            t->pitch = std::clamp(std::remainder(pitch, 360.f), -90.f, 90.f);
        } else {
            Vec3 at;
            if (c.has("facingLocation")) at = c.vec("facingLocation");
            else {
                auto e = c.player("facingEntity");
                at = {e->x, e->y + (c.str("facingAnchor", "feet") == "eyes" ? 1.62 : 0), e->z};
            }
            double dx = at.x - t->x, dy = at.y - (t->y + 1.62), dz = at.z - t->z;
            t->pitch = float(-(std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180 / M_PI));
            t->yaw = float(std::atan2(dz, dx) * 180 / M_PI - 90);
        }
        teleport(*t, t->x, t->y, t->z);
        c.success(tr("commands.rotate.success", {player_name(*t)}), true);
        return 1;
    });

    reg("version", [](Cmd& c) {
        c.success(tr("commands.version.header"), false);
        c.success(tr("commands.version.id", {GAME_VERSION}), false);
        c.success(tr("commands.version.name", {GAME_VERSION}), false);
        c.success(tr("commands.version.protocol", {PROTOCOL_VERSION, "0x" + [] { char b[16]; snprintf(b, sizeof b, "%x", PROTOCOL_VERSION); return std::string(b); }()}), false);
        c.success(tr("commands.version.stable.yes"), false);
        return 0;
    });

    reg("random", [](Cmd& c) {
        if (c.is("reset")) throw fail(not_supported("random reset"));  // ponytail: no named random sequences yet
        auto& r = c.get<Range>("range");
        int64_t lo = int64_t(r.min.value_or(INT32_MIN)), hi = int64_t(r.max.value_or(INT32_MAX));
        int64_t span = hi - lo + 1;
        if (span < 1) throw err("commands.random.error.range_too_small");
        if (span > INT32_MAX) throw err("commands.random.error.range_too_large");
        int v = int(lo + int64_t(cmd_rng() % uint64_t(span)));
        if (c.is("roll")) broadcast_text(tr("commands.random.roll", {c.src.display(), v, (long long)lo, (long long)hi}));
        else c.success(tr("commands.random.sample.success", {v}), false);
        return v;
    });

    reg("transfer", [](Cmd& c) {
        std::string host = c.str("hostname");
        int port = int(c.num("port", 25565));
        std::vector<PlayerPtr> targets = c.has("players") ? c.players("players") : std::vector<PlayerPtr>{c.self_player()};
        for (auto& t : targets) t->send(pkt(play_cb::transfer).str(host).varint(port));
        return respond(c, targets, true, [&](Text n) { return tr("commands.transfer.success.single", {n, host, port}); },
                       [&](int k) { return tr("commands.transfer.success.multiple", {k, host, port}); });
    });

    reg("setidletimeout", [](Cmd& c) {
        int m = int(c.num("minutes"));
        props["player-idle-timeout"] = std::to_string(m);  // ponytail: stored only; idle kicking not implemented
        c.success(m > 0 ? tr("commands.setidletimeout.success", {m}) : tr("commands.setidletimeout.success.disabled"), true);
        return m;
    });
    reg("reload", [](Cmd& c) {
        c.success(tr("commands.reload.success"), true);  // data packs are the built-in vanilla pack: nothing to reload
        return 0;
    });

    reg("return", [](Cmd& c) {
        if (c.is("fail")) throw fail(lit(""));
        if (c.is("run")) return 0;
        return int(c.num("value"));
    });

    reg("forceload", [](Cmd& c) {
        // Chunks are never unloaded here, so force-loading is pure bookkeeping (vanilla's messages).
        static std::set<std::pair<int, int>> forced;
        auto col = [&](const char* n, int& cx, int& cz) {
            Coords cc = c.get<Coords>(n);
            int x = int(std::floor(cc.c[0].rel ? c.src.x + cc.c[0].v : cc.c[0].v)), z = int(std::floor(cc.c[1].rel ? c.src.z + cc.c[1].v : cc.c[1].v));
            cx = x >> 4; cz = z >> 4;
        };
        auto cp = [](int x, int z) { return "[" + std::to_string(x) + ", " + std::to_string(z) + "]"; };
        const std::string dim = "minecraft:overworld";
        if (c.is("query")) {
            if (c.has("pos")) {
                int cx, cz;
                col("pos", cx, cz);
                if (!forced.count({cx, cz})) throw err("commands.forceload.query.failure", {cp(cx, cz), dim});
                c.success(tr("commands.forceload.query.success", {cp(cx, cz), dim}), false);
                return 1;
            }
            if (forced.empty()) { c.success(tr("commands.forceload.added.none", {dim}), false); return 0; }
            std::string list;
            for (auto [x, z] : forced) list += (list.empty() ? "" : ", ") + cp(x, z);
            c.success(forced.size() == 1 ? tr("commands.forceload.list.single", {dim, list}) : tr("commands.forceload.list.multiple", {int(forced.size()), dim, list}), false);
            return int(forced.size());
        }
        if (c.is("all")) { forced.clear(); c.success(tr("commands.forceload.removed.all", {dim}), true); return 0; }
        bool add = c.is("add");
        int ax, az, bx, bz;
        col("from", ax, az);
        if (c.has("to")) col("to", bx, bz); else { bx = ax; bz = az; }
        if (ax > bx) std::swap(ax, bx);
        if (az > bz) std::swap(az, bz);
        int64_t n = int64_t(bx - ax + 1) * (bz - az + 1);
        if (n > 256) throw err("commands.forceload.toobig", {256, (long long)n});
        int changed = 0;
        for (int x = ax; x <= bx; x++)
            for (int z = az; z <= bz; z++) changed += add ? forced.insert({x, z}).second : int(forced.erase({x, z}));
        if (!changed) throw err(add ? "commands.forceload.added.failure" : "commands.forceload.removed.failure");
        std::string k = add ? "commands.forceload.added." : "commands.forceload.removed.";
        if (changed == 1) c.success(tr(k + "single", {cp(ax, az), dim}), true);
        else c.success(tr(k + "multiple", {changed, dim, cp(ax, az), cp(bx, bz)}), true);
        return changed;
    });
}
