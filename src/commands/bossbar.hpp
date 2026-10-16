#pragma once
// /bossbar (BossBarCommands + CustomBossEvents). Bars persist in level.dat key "bossbars".
#include "vanilla.hpp"

struct BossBar {
    std::string id;
    Text name;
    int64_t uh = 0, ul = 0;  // event uuid
    int value = 0, max = 100, color = 6, style = 0;  // color white, style progress
    bool visible = true;
    std::set<std::string> players;  // player uuids (dashed)
};
inline std::map<std::string, BossBar> bossbars;
inline const char* BAR_COLORS[] = {"pink", "blue", "red", "green", "yellow", "purple", "white"};
inline const char* BAR_STYLES[] = {"progress", "notched_6", "notched_10", "notched_12", "notched_20"};

inline Writer bar_packet(const BossBar& b, int op) {
    Writer w = pkt(play_cb::boss_event);
    w.i64(b.uh).i64(b.ul).varint(op);
    float progress = b.max > 0 ? std::clamp(float(b.value) / float(b.max), 0.f, 1.f) : 0.f;
    if (op == 0) { write_nbt(w, b.name); w.f32(progress).varint(b.color).varint(b.style).u8(0); }
    else if (op == 2) w.f32(progress);
    else if (op == 3) write_nbt(w, b.name);
    else if (op == 4) w.varint(b.color).varint(b.style);
    return w;
}
inline void bar_send(const BossBar& b, int op) {
    if (!b.visible) return;
    for (auto& p : snapshot()) if (b.players.count(player_uuid(*p))) p->send(bar_packet(b, op));
}
inline Text bar_text(const BossBar& b) {
    Text n = lit("[");
    append(n, b.name);
    append(n, lit("]"));
    Nbt hover = Nbt::compound();
    hover.set("action", Nbt::str("show_text"));
    hover.set("value", lit(b.id));
    n.set("hover_event", hover);
    return n;
}
inline void bossbars_on_join(Player& p) {
    for (auto& [id, b] : bossbars)
        if (b.visible && b.players.count(player_uuid(p))) p.send(bar_packet(b, 0));
}

inline Nbt bossbars_nbt() {
    Nbt l = Nbt::make_list();
    for (auto& [id, b] : bossbars) {
        Nbt e = Nbt::compound();
        e.set("id", Nbt::str(id));
        e.set("name", b.name);
        e.set("value", Nbt::num(Nbt::Int, b.value));
        e.set("max", Nbt::num(Nbt::Int, b.max));
        e.set("color", Nbt::num(Nbt::Int, b.color));
        e.set("style", Nbt::num(Nbt::Int, b.style));
        e.set("visible", Nbt::num(Nbt::Byte, b.visible));
        Nbt ps = Nbt::make_list();
        for (auto& u : b.players) ps.list.push_back(Nbt::str(u));
        e.set("players", ps);
        l.list.push_back(e);
    }
    return l;
}
inline void bossbars_from(const Nbt& l) {
    for (auto& e : l.list) {
        BossBar b;
        b.id = e.get("id")->s;
        if (auto* n = e.get("name")) b.name = *n;
        auto I = [&](const char* k, int d) { auto* v = e.get(k); return v && v->is_num() ? int(v->i) : d; };
        b.value = I("value", 0); b.max = I("max", 100); b.color = I("color", 6); b.style = I("style", 0); b.visible = I("visible", 1);
        if (auto* ps = e.get("players")) for (auto& u : ps->list) b.players.insert(u.s);
        b.uh = int64_t(cmd_rng()); b.ul = int64_t(cmd_rng());
        bossbars[b.id] = b;
    }
}

inline void register_bossbar() {
    reg("bossbar", [](Cmd& c) {
        if (c.is("list")) {
            if (bossbars.empty()) { c.success(tr("commands.bossbar.list.bars.none"), false); return 0; }
            std::vector<Text> names;
            for (auto& [id, b] : bossbars) names.push_back(bar_text(b));
            c.success(tr("commands.bossbar.list.bars.some", {int(bossbars.size()), join_list(names)}), false);
            return int(bossbars.size());
        }
        std::string id = c.str("id");
        if (c.is("add")) {
            if (bossbars.count(id)) throw err("commands.bossbar.create.failed", {id});
            BossBar b;
            b.id = id;
            b.name = c.text("name");
            b.uh = int64_t(cmd_rng());
            b.ul = int64_t(cmd_rng());
            bossbars[id] = b;
            c.success(tr("commands.bossbar.create.success", {bar_text(b)}), true);
            return int(bossbars.size());
        }
        auto it = bossbars.find(id);
        if (it == bossbars.end()) throw err("commands.bossbar.unknown", {id});
        BossBar& b = it->second;
        auto online = [&] {
            std::vector<PlayerPtr> v;
            for (auto& p : snapshot()) if (b.players.count(player_uuid(*p))) v.push_back(p);
            return v;
        };
        if (c.is("remove")) {
            bar_send(b, 1);
            Text t = bar_text(b);
            bossbars.erase(it);
            c.success(tr("commands.bossbar.remove.success", {t}), true);
            return int(bossbars.size());
        }
        if (c.is("get")) {
            if (c.is("max")) { c.success(tr("commands.bossbar.get.max", {bar_text(b), b.max}), true); return b.max; }
            if (c.is("value")) { c.success(tr("commands.bossbar.get.value", {bar_text(b), b.value}), true); return b.value; }
            if (c.is("visible")) {
                c.success(tr(b.visible ? "commands.bossbar.get.visible.visible" : "commands.bossbar.get.visible.hidden", {bar_text(b)}), true);
                return int(b.visible);
            }
            auto on = online();
            if (on.empty()) c.success(tr("commands.bossbar.get.players.none", {bar_text(b)}), true);
            else {
                std::vector<Text> names;
                for (auto& p : on) names.push_back(player_name(*p));
                c.success(tr("commands.bossbar.get.players.some", {bar_text(b), int(on.size()), join_list(names)}), true);
            }
            return int(on.size());
        }
        // set
        if (c.is("name")) {
            Text n = c.text("name");
            if (n == b.name) throw err("commands.bossbar.set.name.unchanged");
            b.name = n;
            bar_send(b, 3);
            c.success(tr("commands.bossbar.set.name.success", {bar_text(b)}), true);
            return 0;
        }
        if (c.is("color") || c.is("style")) {
            bool color = c.is("color");
            int v = 0;
            for (int i = 0; i < (color ? 7 : 5); i++) if (c.is(color ? BAR_COLORS[i] : BAR_STYLES[i])) v = i;
            int& field = color ? b.color : b.style;
            if (field == v) throw err(color ? "commands.bossbar.set.color.unchanged" : "commands.bossbar.set.style.unchanged");
            field = v;
            bar_send(b, 4);
            c.success(tr(color ? "commands.bossbar.set.color.success" : "commands.bossbar.set.style.success", {bar_text(b)}), true);
            return 0;
        }
        if (c.is("value") || c.is("max")) {
            bool max = c.is("max");
            int v = int(c.num(max ? "max" : "value"));
            int& field = max ? b.max : b.value;
            if (field == v) throw err(max ? "commands.bossbar.set.max.unchanged" : "commands.bossbar.set.value.unchanged");
            field = v;
            bar_send(b, 2);
            c.success(tr(max ? "commands.bossbar.set.max.success" : "commands.bossbar.set.value.success", {bar_text(b), v}), true);
            return v;
        }
        if (c.is("visible")) {
            bool v = c.flag("visible");
            if (b.visible == v) throw err(v ? "commands.bossbar.set.visibility.unchanged.visible" : "commands.bossbar.set.visibility.unchanged.hidden");
            if (!v) bar_send(b, 1);
            b.visible = v;
            if (v) bar_send(b, 0);
            c.success(tr(v ? "commands.bossbar.set.visible.success.visible" : "commands.bossbar.set.visible.success.hidden", {bar_text(b)}), true);
            return 0;
        }
        // players [targets]
        std::set<std::string> want;
        std::vector<PlayerPtr> targets = c.has("targets") ? c.players("targets", true) : std::vector<PlayerPtr>{};
        for (auto& p : targets) want.insert(player_uuid(*p));
        if (want == b.players) throw err("commands.bossbar.set.players.unchanged");
        for (auto& p : snapshot()) {
            bool had = b.players.count(player_uuid(*p)), has = want.count(player_uuid(*p));
            if (b.visible && had && !has) p->send(bar_packet(b, 1));
            if (b.visible && !had && has) p->send(bar_packet(b, 0));
        }
        b.players = want;
        if (targets.empty()) c.success(tr("commands.bossbar.set.players.success.none", {bar_text(b)}), true);
        else {
            std::vector<Text> names;
            for (auto& p : targets) names.push_back(player_name(*p));
            c.success(tr("commands.bossbar.set.players.success.some", {bar_text(b), int(targets.size()), join_list(names)}), true);
        }
        return int(targets.size());
    });
}
