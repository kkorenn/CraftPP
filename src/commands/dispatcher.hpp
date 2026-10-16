#pragma once
// Brigadier-equivalent command tree, parser, executor, suggestions and the clientbound commands packet.
#include "args.hpp"

#include <deque>
#include <functional>
#include <span>

// ---------- tree: the static vanilla table (gen::CMD_NODES) + a small overlay for plugin roots ----------
// Node ids < N_GEN index the generated table; plugin nodes follow. Only the root's child list is mutable.
inline constexpr int N_GEN = int(std::size(gen::CMD_NODES));
inline std::vector<gen::CmdNode> extra_nodes;          // plugin literal + "args" nodes
inline std::vector<std::vector<int32_t>> extra_kids;
inline std::deque<std::string> name_pool;              // stable storage for plugin node names
inline std::vector<int32_t>& root_kids() {
    static std::vector<int32_t> k(gen::CMD_KIDS + gen::CMD_NODES[0].first, gen::CMD_KIDS + gen::CMD_NODES[0].first + gen::CMD_NODES[0].count);
    return k;
}
inline const gen::CmdNode& node_info(int i) { return i < N_GEN ? gen::CMD_NODES[i] : extra_nodes[size_t(i - N_GEN)]; }
inline std::span<const int32_t> node_kids(int i) {
    if (i == 0) return root_kids();
    if (i >= N_GEN) return extra_kids[size_t(i - N_GEN)];
    return {gen::CMD_KIDS + gen::CMD_NODES[i].first, size_t(gen::CMD_NODES[i].count)};
}
inline const char* node_name(int i) { return node_info(i).name; }
inline int child_named(int parent, const std::string& name) {
    for (int k : node_kids(parent)) if (name == node_name(k)) return k;
    return -1;
}

// ---------- parse results ----------
struct ParsedNode { int node; size_t start, end; };
struct Segment {
    std::string cmd;  // root literal (after following an alias redirect, the target's name)
    int root = 0;     // node this context started at (0 = dispatcher root)
    size_t start = 0, end = 0;
    std::vector<ParsedNode> nodes;
    std::map<std::string, Value> args;
    bool exec = false;
};
struct Parse {
    std::vector<Segment> segs;
    size_t cursor = 0;
    std::vector<CmdError> errors;
    bool done(const std::string& s) const { return cursor >= s.size(); }
};

inline bool can_use(int node, const Source& src) { return src.level >= node_info(node).perm; }

inline Parse parse_nodes(int node, const StringReader& in, Parse ctx, const Source& src) {
    std::vector<CmdError> errors;
    std::vector<Parse> potentials;
    // CommandNode.getRelevantNodes: an exactly matching literal wins, otherwise every argument child.
    size_t e = in.c;
    while (e < in.s.size() && in.s[e] != ' ') e++;
    std::string word = in.s.substr(in.c, e - in.c);
    std::vector<int> relevant;
    bool has_literal = false;
    for (int k : node_kids(node)) {
        if (node_info(k).type == 1) { has_literal = true; if (word == node_name(k)) relevant = {k}; }
    }
    if (relevant.empty())
        for (int k : node_kids(node)) if (node_info(k).type == 2) relevant.push_back(k);
    (void)has_literal;
    for (int child : relevant) {
        if (!can_use(child, src)) continue;
        Parse c = ctx;
        StringReader r = in;
        const auto& info = node_info(child);
        size_t st = r.c;
        try {
            if (info.type == 1) r.c += strlen(info.name);
            else c.segs.back().args[info.name] = parse_argument(info, r, src);
            if (r.can() && r.peek() != ' ') throw r.error("command.expected.separator");
        } catch (CmdError& err) {
            errors.push_back(std::move(err));
            continue;
        }
        Segment& seg = c.segs.back();
        if (node == 0) seg.cmd = info.name;
        seg.nodes.push_back({child, st, r.c});
        seg.end = r.c;
        seg.exec = info.exec;
        if (r.can(info.redirect < 0 ? 2 : 1)) {
            r.skip();
            if (info.redirect >= 0) {
                Segment next;
                next.root = info.redirect;
                next.cmd = info.redirect == 0 ? "" : node_name(info.redirect);
                next.start = next.end = r.c;
                c.segs.push_back(std::move(next));
                return parse_nodes(info.redirect, r, std::move(c), src);
            }
            potentials.push_back(parse_nodes(child, r, std::move(c), src));
        } else {
            c.cursor = r.c;
            c.errors.clear();
            potentials.push_back(std::move(c));
        }
    }
    if (!potentials.empty()) {
        const std::string& s = in.s;
        std::stable_sort(potentials.begin(), potentials.end(), [&](const Parse& a, const Parse& b) {
            if (a.done(s) != b.done(s)) return a.done(s);
            return a.errors.empty() && !b.errors.empty();
        });
        return potentials[0];
    }
    ctx.cursor = in.c;
    ctx.errors = std::move(errors);
    return ctx;
}

inline Parse parse_command(const std::string& line, size_t start, const Source& src) {
    Parse p;
    p.segs.push_back({});
    p.segs.back().start = p.segs.back().end = start;
    StringReader r{line, start};
    return parse_nodes(0, r, std::move(p), src);
}

// Commands.getParseException + ContextChain.tryFlatten.
inline std::optional<CmdError> parse_error(const Parse& p, const std::string& line) {
    if (!p.done(line)) {
        if (p.errors.size() == 1) return p.errors[0];
        bool empty = p.segs.size() == 1 && p.segs[0].nodes.empty();
        return CmdError{tr(empty ? "command.unknown.command" : "command.unknown.argument"), line, int(p.cursor)};
    }
    if (!p.segs.back().exec) return CmdError{tr("command.unknown.command"), line, int(p.cursor)};
    return std::nullopt;
}

// ---------- execution context passed to command handlers ----------
struct StoreSpec { bool success; std::vector<std::string> holders; std::string objective; };
struct Cmd;
using Handler = std::function<int(Cmd&)>;
inline std::map<std::string, Handler>& command_handlers() { static std::map<std::string, Handler> h; return h; }

struct Cmd {
    Source& src;
    const Segment& seg;
    const std::string& line;

    bool has(const char* name) const { return seg.args.count(name); }
    bool is(const char* literal) const {
        for (auto& n : seg.nodes) if (node_info(n.node).type == 1 && !strcmp(node_name(n.node), literal)) return true;
        return false;
    }
    template <class T> const T& get(const char* name) const { return std::get<T>(seg.args.at(name)); }
    int64_t num(const char* name, int64_t def = 0) const { return has(name) ? std::get<int64_t>(seg.args.at(name)) : def; }
    double real(const char* name, double def = 0) const { return has(name) ? std::get<double>(seg.args.at(name)) : def; }
    bool flag(const char* name, bool def = false) const { return has(name) ? std::get<bool>(seg.args.at(name)) : def; }
    std::string str(const char* name, const std::string& def = "") const { return has(name) ? std::get<std::string>(seg.args.at(name)) : def; }
    Text text(const char* name) const { return std::get<Nbt>(seg.args.at(name)); }

    // EntityArgument.getEntities / getPlayers (throws when nothing matches).
    std::vector<PlayerPtr> players(const char* name, bool allow_empty = false) const {
        auto& sel = get<Selector>(name);
        auto v = select(sel, src);
        if (v.empty() && !allow_empty)
            throw err(sel.players_only_type || sel.by_name ? "argument.entity.notfound.player" : "argument.entity.notfound.entity");
        return v;
    }
    PlayerPtr player(const char* name) const { return players(name)[0]; }
    // getPlayerOrException / getEntityOrException (entity=true) when the argument is omitted.
    std::vector<PlayerPtr> targets_or_self(const char* name, bool entity = false) const {
        if (has(name)) return players(name);
        return {self_player(entity)};
    }
    PlayerPtr self_player(bool entity = false) const {
        if (!src.entity) throw err(entity ? "permissions.requires.entity" : "permissions.requires.player");
        return src.entity;
    }
    Vec3 vec(const char* name) const { return get<Coords>(name).pos(src); }
    void block_pos(const char* name, int& x, int& y, int& z) const {
        get<Coords>(name).block(src, x, y, z);
        if (y < world.min_y() || y >= world.max_y()) throw err("argument.pos.outofworld");
    }
    // ScoreHolderArgument.getNames
    std::vector<std::string> holders(const char* name) const {
        auto& h = get<ScoreHolders>(name);
        std::vector<std::string> out;
        if (h.all) { for (auto& [k, v] : scores) out.push_back(k); }
        else if (h.is_selector) { for (auto& p : select(h.sel, src)) out.push_back(p->name); }
        else out.push_back(h.name);
        if (out.empty()) throw err("argument.scoreHolder.empty");
        return out;
    }
    // GameProfileArgument: online players by selector, or a name (online first, then offline-mode profile).
    std::vector<std::pair<std::string, std::string>> profiles(const char* name) const {  // (name, uuid)
        auto& g = get<GameProfiles>(name);
        std::vector<std::pair<std::string, std::string>> out;
        if (g.is_selector) {
            for (auto& p : select(g.sel, src)) out.emplace_back(p->name, player_uuid(*p));
        } else if (auto p = find_player(g.name)) out.emplace_back(p->name, player_uuid(*p));
        else out.emplace_back(g.name, offline_uuid(g.name));
        if (out.empty()) throw err("argument.player.unknown");
        return out;
    }

    void send(const Text& t) const {  // CommandSource.sendSystemMessage
        if (src.silent) return;
        if (src.sender) send_text(*src.sender, t);
        else log_info("%s", plain(t, true).c_str());
    }
    // CommandSourceStack.sendSuccess
    void success(const Text& t, bool broadcast_admins) const {
        if (src.silent) return;
        bool accepts = !src.sender || rule("send_command_feedback");
        if (accepts) send(t);
        if (!broadcast_admins) return;
        Text b = styled(tr("chat.type.admin", {src.display(), t}), "gray", true);
        if (rule("send_command_feedback"))
            for (auto& p : snapshot())
                if (p != src.sender && ops.find(player_uuid(*p), p->name)) send_text(*p, b);
        if (src.sender && rule("log_admin_commands")) log_info("%s", plain(b, true).c_str());
    }
};
inline Text not_supported(const std::string& cmd) { return lit("/" + cmd + " is not supported yet"); }

// ---------- execute chain (ExecuteCommand modifiers) ----------
inline std::vector<Source> apply_execute(const Segment& seg, const Source& in, const std::string& line, std::vector<StoreSpec>& stores);
inline bool execute_condition(Cmd& c);  // vanilla_execute.hpp

struct ExecState { int forks = 0; };

inline void send_failure(const Source& src, const Text& t) {
    if (src.silent) return;
    Text red = styled(lit(""), "red");
    append(red, t);
    if (src.sender) send_text(*src.sender, red);
    else log_info("%s", plain(red, true).c_str());
}
inline void send_error(const Source& src, const CmdError& e, const std::string& shown_line) {
    send_failure(src, e.msg);
    if (e.cursor < 0 || e.input.empty()) return;
    int cursor = std::min(int(e.input.size()), e.cursor);
    Text ctx = styled(lit(""), "gray");
    Nbt click = Nbt::compound();
    click.set("action", Nbt::str("suggest_command"));
    click.set("command", Nbt::str("/" + shown_line));
    ctx.set("click_event", click);
    if (cursor > 10) append(ctx, lit("..."));
    append(ctx, lit(e.input.substr(size_t(std::max(0, cursor - 10)), size_t(cursor - std::max(0, cursor - 10)))));
    if (size_t(cursor) < e.input.size()) {
        Text rest = styled(lit(e.input.substr(size_t(cursor))), "red");
        rest.set("underlined", Nbt::num(Nbt::Byte, 1));
        append(ctx, rest);
    }
    append(ctx, styled(tr("command.context.here"), "red", true));
    if (src.sender) send_text(*src.sender, ctx);
    else log_info("%s", plain(ctx, true).c_str());
}

// Runs a parsed chain; returns the summed result. Throws CmdError.
inline int run_chain(const Parse& p, const Source& origin, const std::string& line) {
    std::vector<std::pair<Source, std::vector<StoreSpec>>> sources{{origin, {}}};
    for (size_t i = 0; i + 1 < p.segs.size(); i++) {
        const Segment& seg = p.segs[i];
        if (seg.cmd != "execute") continue;  // alias redirect (tp -> teleport) or `return run`
        std::vector<std::pair<Source, std::vector<StoreSpec>>> next;
        for (auto& [s, st] : sources) {
            auto stores = st;
            for (auto& ns : apply_execute(seg, s, line, stores)) next.emplace_back(ns, stores);
        }
        sources = std::move(next);
        if (int(sources.size()) > rule("max_command_forks")) throw err("command.forkLimit", {rule("max_command_forks")});
    }
    const Segment& last = p.segs.back();
    int total = 0;
    auto it = command_handlers().find(last.cmd);
    for (auto& [s, stores] : sources) {
        Source src = s;
        InLevel in_level(level_by_id(src.dimension));  // commands act in the source's level
        Cmd c{src, last, line};
        int result = 0;
        bool ok = true;
        try {
            if (last.cmd == "execute") result = execute_condition(c) ? 1 : 0;  // `execute if ...` as the final node
            else if (it == command_handlers().end()) throw fail(not_supported(last.cmd));
            else result = it->second(c);
        } catch (CmdError&) {
            ok = false;
            if (stores.empty()) throw;
        }
        for (auto& st : stores)
            for (auto& h : st.holders) set_score(h, st.objective, st.success ? (ok && result != 0 ? 1 : 0) : (ok ? result : 0));
        total += result;
    }
    return total;
}

// Commands.performCommand. `line` has no leading slash.
inline int perform(const Source& src, const std::string& line) {
    std::lock_guard lock(cmd_mtx);
    InLevel in_level(level_by_id(src.dimension));
    Parse p = parse_command(line, 0, src);
    if (auto e = parse_error(p, line)) {
        send_error(src, *e, line);
        return 0;
    }
    try {
        return run_chain(p, src, line);
    } catch (CmdError& e) {
        send_error(src, e, line);
    } catch (std::exception& e) {
        send_failure(src, tr("command.failed"));
        log_error("Command exception: /%s: %s", line.c_str(), e.what());
    }
    return 0;
}

// ---------- help (CommandDispatcher.getSmartUsage) ----------
inline std::string usage_text(int n) {
    auto& i = node_info(n);
    return i.type == 1 ? std::string(i.name) : "<" + std::string(i.name) + ">";
}
inline std::optional<std::string> smart_usage(int n, const Source& src, bool optional, bool deep) {
    if (!can_use(n, src)) return std::nullopt;
    const auto& info = node_info(n);
    std::string self = optional ? "[" + usage_text(n) + "]" : usage_text(n);
    bool child_opt = info.exec;
    if (!deep) {
        if (info.redirect >= 0) return self + " " + (info.redirect == 0 ? "..." : "-> " + usage_text(info.redirect));
        std::vector<int> kids;
        for (int k : node_kids(n)) if (can_use(k, src)) kids.push_back(k);
        if (kids.size() == 1) {
            if (auto u = smart_usage(kids[0], src, child_opt, child_opt)) return self + " " + *u;
        } else if (kids.size() > 1) {
            std::vector<std::string> uniq;
            for (int k : kids)
                if (auto u = smart_usage(k, src, child_opt, true); u && std::find(uniq.begin(), uniq.end(), *u) == uniq.end()) uniq.push_back(*u);
            if (uniq.size() == 1) return self + " " + (child_opt ? "[" + uniq[0] + "]" : uniq[0]);
            if (uniq.size() > 1) {
                std::string b = child_opt ? "[" : "(";
                for (size_t k = 0; k < kids.size(); k++) b += (k ? "|" : "") + usage_text(kids[k]);
                return self + " " + b + (child_opt ? "]" : ")");
            }
        }
    }
    return self;
}
inline std::vector<std::string> usage_lines(int node, const Source& src) {
    std::vector<std::string> out;
    bool optional = node_info(node).exec;
    for (int k : node_kids(node))
        if (auto u = smart_usage(k, src, optional, false)) out.push_back(*u);
    return out;
}

// ---------- clientbound commands packet (cached per permission level) ----------
inline std::array<std::vector<uint8_t>, 5> tree_packets;  // framed; empty = stale

inline void write_props(Writer& w, const gen::CmdNode& n) {
    auto flags = [&] { w.u8(uint8_t((n.has_min ? 1 : 0) | (n.has_max ? 2 : 0))); };
    switch (n.parser) {
        case arg_types::integer: flags(); if (n.has_min) w.i32(int32_t(n.min)); if (n.has_max) w.i32(int32_t(n.max)); break;
        case arg_types::long_: flags(); if (n.has_min) w.i64(int64_t(n.min)); if (n.has_max) w.i64(int64_t(n.max)); break;
        case arg_types::float_: flags(); if (n.has_min) w.f32(float(n.min)); if (n.has_max) w.f32(float(n.max)); break;
        case arg_types::double_: flags(); if (n.has_min) w.f64(n.min); if (n.has_max) w.f64(n.max); break;
        case arg_types::string_: w.varint(n.flags); break;
        case arg_types::entity: case arg_types::score_holder: w.u8(uint8_t(n.flags)); break;
        case arg_types::time_: w.i32(int32_t(n.min)); break;
        case arg_types::resource: case arg_types::resource_key: case arg_types::resource_or_tag:
        case arg_types::resource_or_tag_key: case arg_types::resource_selector: w.str(n.registry); break;
        default: break;
    }
}

// Commands.sendCommands: filter by permission, then enumerate breadth-first like ClientboundCommandsPacket.
inline const std::vector<uint8_t>& commands_packet_for(int level) {
    level = std::clamp(level, 0, 4);
    auto& cached = tree_packets[size_t(level)];
    if (!cached.empty()) return cached;
    Source probe;
    probe.level = level;
    std::vector<int> order;
    std::map<int, int> id;
    std::deque<int> q{0};
    while (!q.empty()) {
        int n = q.front();
        q.pop_front();
        if (id.count(n)) continue;
        id[n] = int(order.size());
        order.push_back(n);
        for (int k : node_kids(n)) if (can_use(k, probe)) q.push_back(k);
        int red = node_info(n).redirect;
        if (red >= 0) q.push_back(red);
    }
    Writer w = pkt(play_cb::commands);
    w.varint(int32_t(order.size()));
    for (int n : order) {
        const auto& info = node_info(n);
        uint8_t fl = info.type;
        if (info.exec) fl |= 4;
        if (info.redirect >= 0) fl |= 8;
        if (info.type == 2 && info.suggest) fl |= 16;
        if (info.perm > 0) fl |= 32;
        w.u8(fl);
        std::vector<int> kids;
        for (int k : node_kids(n)) if (can_use(k, probe)) kids.push_back(id[k]);
        w.varint(int32_t(kids.size()));
        for (int k : kids) w.varint(k);
        if (info.redirect >= 0) w.varint(id[info.redirect]);
        if (info.type >= 1) w.str(info.name);
        if (info.type == 2) {
            w.varint(info.parser);
            write_props(w, info);
            if (info.suggest) w.str(gen::SUGGESTION_IDS[info.suggest]);
        }
    }
    w.varint(0);
    Writer frame;
    frame.varint(int32_t(w.b.size())).bytes(w.b);
    cached = std::move(frame.b);
    return cached;
}

inline void send_commands(Player& p) {
    int level = perm_level(p);
    std::lock_guard lock(cmd_mtx);
    p.send_raw_bytes(commands_packet_for(level));
    p.send(pkt(play_cb::entity_event).i32(p.eid).u8(uint8_t(24 + level)));  // op permission level status
}
inline void resend_all_commands() {
    std::lock_guard lock(cmd_mtx);
    for (auto& c : tree_packets) c.clear();
    for (auto& p : snapshot()) try { send_commands(*p); } catch (...) {}
}

// ---------- plugin command API ----------
// Plugins register a root literal; the client sees `<name> [args...]` (greedy string with ask_server suggestions).
// `run` gets the raw argument text split on spaces; `suggest` gets the same with the last (partial) word.
using PluginRun = std::function<int(Source&, const std::vector<std::string>& args)>;
using PluginSuggest = std::function<std::vector<std::string>(Source&, const std::vector<std::string>& args)>;
inline std::map<std::string, PluginSuggest>& plugin_suggesters() { static std::map<std::string, PluginSuggest> m; return m; }

inline std::vector<std::string> split_args(const std::string& s, bool keep_trailing_empty) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == ' ') { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty() || keep_trailing_empty) v.push_back(cur);
    return v;
}

// Adds (or replaces) a root literal `<name> [args]`. Execution/suggestions are handled by the caller
// (the plugin bridge intercepts its labels before the dispatcher) unless `run`/`suggest` are given.
inline void add_command_root(const std::string& name, int permission = 0) {
    std::lock_guard lock(cmd_mtx);
    auto& root = root_kids();
    std::erase_if(root, [&](int k) { return name == node_name(k); });
    const char* nm = name_pool.emplace_back(name).c_str();
    int lit_id = N_GEN + int(extra_nodes.size());
    extra_nodes.push_back({1, uint8_t(permission), 1, 0, -1, 0, 0, 0, 0, 0, -1, 0, 0, nm, ""});
    extra_kids.push_back({lit_id + 1});
    extra_nodes.push_back({2, uint8_t(permission), 1, 1, arg_types::string_, 0, 0, 0, 0, 2, -1, 0, 0, "args", ""});
    extra_kids.push_back({});
    root.push_back(lit_id);
    for (auto& c : tree_packets) c.clear();
}
inline void register_plugin_command(const std::string& name, PluginRun run, PluginSuggest suggest = {}, int permission = 0) {
    std::lock_guard lock(cmd_mtx);
    add_command_root(name, permission);
    command_handlers()[name] = [run](Cmd& c) { return run(c.src, split_args(c.str("args"), false)); };
    if (suggest) plugin_suggesters()[name] = suggest;
    resend_all_commands();
}
inline void unregister_plugin_command(const std::string& name) {
    std::lock_guard lock(cmd_mtx);
    std::erase_if(root_kids(), [&](int k) { return name == node_name(k); });
    command_handlers().erase(name);
    plugin_suggesters().erase(name);
    resend_all_commands();
}

// ---------- suggestions (serverbound command_suggestion -> clientbound command_suggestions) ----------
struct Suggestion { std::string text; std::optional<Text> tooltip; };
inline std::vector<std::string> custom_suggestions(const Segment& seg, int node, const Source& src, const std::string& typed);  // vanilla_cmds

inline std::pair<size_t, std::vector<Suggestion>> suggestions(const std::string& text, const Source& src) {
    std::lock_guard lock(cmd_mtx);
    size_t start0 = !text.empty() && text[0] == '/' ? 1 : 0;
    Parse p = parse_command(text, start0, src);
    size_t cursor = text.size();
    // CommandContextBuilder.findSuggestionContext
    int parent = 0;
    size_t start = start0;
    const Segment* seg = &p.segs[0];
    for (size_t si = 0; si < p.segs.size(); si++) {
        seg = &p.segs[si];
        if (seg->end < cursor) {
            if (si + 1 < p.segs.size()) continue;
            if (!seg->nodes.empty()) { parent = seg->nodes.back().node; start = seg->nodes.back().end + 1; }
            else { parent = seg->root; start = seg->start; }
        } else {
            int prev = seg->root;
            bool found = false;
            for (auto& n : seg->nodes) {
                if (n.start <= cursor && cursor <= n.end) { parent = prev; start = n.start; found = true; break; }
                prev = n.node;
            }
            if (!found) { parent = prev; start = seg->start; }
        }
        break;
    }
    start = std::min(start, cursor);
    std::string typed = text.substr(start);
    std::string lower = typed;
    for (auto& ch : lower) ch = char(tolower((unsigned char)ch));
    std::vector<Suggestion> out;
    for (int k : node_kids(parent)) {
        if (!can_use(k, src)) continue;
        const auto& info = node_info(k);
        if (info.type == 1) {
            if (std::string(info.name).rfind(lower, 0) == 0) out.push_back({info.name, {}});
            continue;
        }
        if (seg->cmd != "" && plugin_suggesters().count(seg->cmd) && !strcmp(info.name, "args")) {
            auto words = split_args(typed, true);
            size_t last = typed.rfind(' ');
            size_t wstart = last == std::string::npos ? 0 : last + 1;
            std::vector<Suggestion> ps;
            for (auto& s : plugin_suggesters()[seg->cmd](const_cast<Source&>(src), words)) ps.push_back({s, {}});
            return {start + wstart, ps};
        }
        for (auto& s : custom_suggestions(*seg, k, src, typed)) {
            std::string ls = s;
            for (auto& ch : ls) ch = char(tolower((unsigned char)ch));
            // SharedSuggestionProvider.matchesSubStr: prefix of the whole or of any '_'/'.'/':'/'/'-separated part
            bool m = ls.rfind(lower, 0) == 0;
            for (size_t i = 0; !m && i < ls.size(); i++)
                if (strchr("_.:/-", ls[i]) && ls.compare(i + 1, lower.size(), lower) == 0) m = true;
            if (m) out.push_back({s, {}});
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.text < b.text; });
    out.erase(std::unique(out.begin(), out.end(), [](auto& a, auto& b) { return a.text == b.text; }), out.end());
    return {start, out};
}
