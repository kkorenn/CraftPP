#pragma once
// Text components (as NBT), translations, command errors and the brigadier StringReader.
#include "../gen/command_data.hpp"
#include "nbt.hpp"

#include <cstring>
#include <initializer_list>

// ---------- components ----------
using Text = Nbt;

inline Text lit(const std::string& s) { Text t = Nbt::compound(); t.set("text", Nbt::str(s)); return t; }

struct TArg {  // translation argument: component, string or number
    Text t;
    TArg(Text c) : t(std::move(c)) {}
    TArg(const std::string& s) : t(lit(s)) {}
    TArg(const char* s) : t(lit(s)) {}
    TArg(int v) : t(lit(std::to_string(v))) {}
    TArg(long v) : t(lit(std::to_string(v))) {}
    TArg(long long v) : t(lit(std::to_string(v))) {}
    TArg(unsigned long v) : t(lit(std::to_string(v))) {}
    TArg(double v) : t(lit(fmt_double(v))) {}
    TArg(float v) : t(lit(fmt_double(double(v)))) {}  // ponytail: Java prints floats with float precision
};

inline Text tr(const std::string& key, std::initializer_list<TArg> args = {}) {
    Text t = Nbt::compound();
    t.set("translate", Nbt::str(key));
    if (args.size()) {
        Nbt w = Nbt::make_list();
        for (auto& a : args) w.list.push_back(a.t);
        t.set("with", std::move(w));
    }
    return t;
}
inline Text tr_list(const std::string& key, std::vector<Text> args) {
    Text t = Nbt::compound();
    t.set("translate", Nbt::str(key));
    if (!args.empty()) { Nbt w = Nbt::make_list(); w.list = std::move(args); t.set("with", std::move(w)); }
    return t;
}
inline Text styled(Text t, const std::string& color, bool italic = false) {
    if (t.t == Nbt::String) t = lit(t.s);
    if (!color.empty()) t.set("color", Nbt::str(color));
    if (italic) t.set("italic", Nbt::num(Nbt::Byte, 1));
    return t;
}
inline Text& append(Text& t, Text child) {
    Nbt* e = t.get("extra");
    if (!e) e = &t.set("extra", Nbt::make_list());
    e->list.push_back(std::move(child));
    return t;
}
// Joins with ", " like ComponentUtils.formatList.
inline Text join_list(const std::vector<Text>& parts) {
    Text t = lit("");
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) append(t, lit(", "));
        append(t, parts[k]);
    }
    return t;
}

inline const char* lang(const char* key) {
    size_t lo = 0, hi = std::size(gen::LANG);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(gen::LANG[mid].key, key);
        if (c == 0) return gen::LANG[mid].value;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

// Plain-text rendering (console, logs): resolves translations with %s / %n$s. With `codes`, colours become
// legacy § codes (the console logger turns them into ANSI on a TTY and strips them from latest.log).
inline std::string plain(const Text& t, bool codes = false, char inherited = 'r') {
    if (t.t == Nbt::String) return t.s;
    if (t.t == Nbt::List) { std::string o; for (auto& e : t.list) o += plain(e, codes, inherited); return o; }
    if (t.t != Nbt::Compound) return to_snbt(t);
    std::string o;
    char color = inherited;
    if (auto* c = t.get("color"); codes && c && c->t == Nbt::String) {
        if (char k = logging::color_code(c->s)) color = k;
    }
    std::string mark = codes ? std::string("\xC2\xA7") + color : "";
    o += mark;
    if (auto* x = t.get("text")) o += x->s;
    else if (auto* k = t.get("translate")) {
        const char* f = lang(k->s.c_str());
        if (!f) { auto* fb = t.get("fallback"); f = fb ? fb->s.c_str() : k->s.c_str(); }
        std::vector<std::string> args;
        if (auto* w = t.get("with")) for (auto& a : w->list) args.push_back(plain(a, codes, color) + mark);
        size_t next = 0;
        for (const char* p = f; *p; p++) {
            if (*p != '%') { o += *p; continue; }
            if (p[1] == '%') { o += '%'; p++; continue; }
            if (p[1] == 's') { if (next < args.size()) o += args[next]; next++; p++; continue; }
            if (isdigit((unsigned char)p[1]) && p[2] == '$' && p[3] == 's') {
                size_t n = size_t(p[1] - '1');
                if (n < args.size()) o += args[n];
                p += 3;
                continue;
            }
            o += *p;
        }
    } else if (auto* sel = t.get("selector")) o += sel->s;
    else if (auto* kb = t.get("keybind")) o += kb->s;
    if (auto* e = t.get("extra")) for (auto& c : e->list) o += plain(c, codes, color) + mark;
    return o;
}

// ---------- errors ----------
struct CmdError {
    Text msg;
    std::string input;
    int cursor = -1;
};
inline CmdError fail(Text t) { return {std::move(t), "", -1}; }
inline CmdError err(const std::string& key, std::initializer_list<TArg> args = {}) { return {tr(key, args), "", -1}; }

// ---------- brigadier StringReader ----------
struct StringReader {
    std::string s;
    size_t c = 0;

    bool can(size_t n = 1) const { return c + n <= s.size(); }
    char peek(size_t o = 0) const { return s[c + o]; }
    char next() { return s[c++]; }
    void skip() { c++; }
    std::string rest() const { return s.substr(c); }
    void skip_ws() { while (can() && peek() == ' ') c++; }

    CmdError error(const std::string& key, std::initializer_list<TArg> args = {}) const { return {tr(key, args), s, int(c)}; }
    CmdError error_at(size_t at, const std::string& key, std::initializer_list<TArg> args = {}) const {
        return {tr(key, args), s, int(at)};
    }

    static bool number_char(char ch) { return (ch >= '0' && ch <= '9') || ch == '.' || ch == '-'; }
    static bool unquoted_char(char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' || ch == '-' ||
               ch == '.' || ch == '+';
    }
    std::string read_number_text() {
        size_t st = c;
        while (can() && number_char(peek())) c++;
        return s.substr(st, c - st);
    }
    int read_int() {
        size_t st = c;
        std::string n = read_number_text();
        if (n.empty()) throw error("parsing.int.expected");
        char* end;
        errno = 0;
        long long v = strtoll(n.c_str(), &end, 10);
        if (*end || errno || v < INT32_MIN || v > INT32_MAX) { c = st; throw error("parsing.int.invalid", {n}); }
        return int(v);
    }
    int64_t read_long() {
        size_t st = c;
        std::string n = read_number_text();
        if (n.empty()) throw error("parsing.long.expected");
        char* end;
        errno = 0;
        long long v = strtoll(n.c_str(), &end, 10);
        if (*end || errno) { c = st; throw error("parsing.long.invalid", {n}); }
        return v;
    }
    double read_double(bool is_float = false) {
        size_t st = c;
        std::string n = read_number_text();
        if (n.empty()) throw error(is_float ? "parsing.float.expected" : "parsing.double.expected");
        char* end;
        double v = strtod(n.c_str(), &end);
        if (*end || n == "-" || n == ".") { c = st; throw error(is_float ? "parsing.float.invalid" : "parsing.double.invalid", {n}); }
        return v;
    }
    std::string read_unquoted() {
        size_t st = c;
        while (can() && unquoted_char(peek())) c++;
        return s.substr(st, c - st);
    }
    std::string read_quoted() {
        if (!can()) return "";
        char q = peek();
        if (q != '"' && q != '\'') throw error("parsing.quote.expected.start");
        skip();
        return read_until(q);
    }
    std::string read_until(char term) {
        std::string o;
        bool esc = false;
        while (can()) {
            char ch = next();
            if (esc) {
                if (ch == term || ch == '\\') { o += ch; esc = false; }
                else { c--; throw error("parsing.quote.escape", {std::string(1, ch)}); }
            } else if (ch == '\\') esc = true;
            else if (ch == term) return o;
            else o += ch;
        }
        throw error("parsing.quote.expected.end");
    }
    std::string read_string() {
        if (!can()) return "";
        if (peek() == '"' || peek() == '\'') { skip(); return read_until(s[c - 1]); }
        return read_unquoted();
    }
    bool read_bool() {
        size_t st = c;
        std::string v = read_string();
        if (v.empty()) throw error("parsing.bool.expected");
        if (v == "true") return true;
        if (v == "false") return false;
        c = st;
        throw error("parsing.bool.invalid", {v});
    }
    void expect(char ch) {
        if (!can() || peek() != ch) throw error("parsing.expected", {std::string(1, ch)});
        skip();
    }
};
