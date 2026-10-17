#pragma once
// NBT value, SNBT parser (vanilla SnbtGrammar subset), network writer, SNBT printer.
#include "../protocol.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

struct Nbt {
    enum Type : uint8_t { End, Byte, Short, Int, Long, Float, Double, ByteArray, String, List, Compound, IntArray, LongArray };
    Type t = End;
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::vector<Nbt> list;                             // List
    std::vector<std::pair<std::string, Nbt>> comp;     // Compound (insertion order)
    std::vector<int64_t> arr;                          // Byte/Int/LongArray

    static Nbt str(std::string v);
    static Nbt num(Type t, int64_t v);
    static Nbt real(Type t, double v);
    static Nbt compound();
    static Nbt make_list();

    bool is_num() const { return t >= Byte && t <= Double; }
    double as_double() const { return t == Float || t == Double ? d : double(i); }
    const Nbt* get(const std::string& k) const;
    Nbt* get(const std::string& k);
    Nbt& set(const std::string& k, Nbt v);
    bool operator==(const Nbt& o) const;
};
// Defined after the struct: libstdc++ rejects instantiating pair<string, Nbt> while Nbt is incomplete.
inline Nbt Nbt::str(std::string v) { Nbt n; n.t = String; n.s = std::move(v); return n; }
inline Nbt Nbt::num(Type t, int64_t v) { Nbt n; n.t = t; n.i = v; return n; }
inline Nbt Nbt::real(Type t, double v) { Nbt n; n.t = t; n.d = v; return n; }
inline Nbt Nbt::compound() { Nbt n; n.t = Compound; return n; }
inline Nbt Nbt::make_list() { Nbt n; n.t = List; return n; }
inline const Nbt* Nbt::get(const std::string& k) const {
    for (auto& [key, v] : comp) if (key == k) return &v;
    return nullptr;
}
inline Nbt* Nbt::get(const std::string& k) {
    for (auto& [key, v] : comp) if (key == k) return &v;
    return nullptr;
}
inline Nbt& Nbt::set(const std::string& k, Nbt v) {
    if (Nbt* e = get(k)) return *e = std::move(v);
    comp.emplace_back(k, std::move(v));
    return comp.back().second;
}
inline bool Nbt::operator==(const Nbt& o) const {
    return t == o.t && i == o.i && d == o.d && s == o.s && list == o.list && comp == o.comp && arr == o.arr;
}

// ---- network (unnamed root) ----
inline void nbt_string(Writer& w, const std::string& s) {
    // ponytail: plain UTF-8, not modified UTF-8 (differs only for NUL and 4-byte chars).
    w.u8(uint8_t(s.size() >> 8)).u8(uint8_t(s.size()));
    w.bytes(s.data(), s.size());
}
inline void nbt_payload(Writer& w, const Nbt& n) {
    switch (n.t) {
        case Nbt::Byte: w.u8(uint8_t(n.i)); break;
        case Nbt::Short: w.i16(int16_t(n.i)); break;
        case Nbt::Int: w.i32(int32_t(n.i)); break;
        case Nbt::Long: w.i64(n.i); break;
        case Nbt::Float: w.f32(float(n.d)); break;
        case Nbt::Double: w.f64(n.d); break;
        case Nbt::String: nbt_string(w, n.s); break;
        case Nbt::List:
            w.u8(n.list.empty() ? 0 : n.list[0].t).i32(int32_t(n.list.size()));
            for (auto& e : n.list) nbt_payload(w, e);
            break;
        case Nbt::Compound:
            for (auto& [k, v] : n.comp) { w.u8(v.t); nbt_string(w, k); nbt_payload(w, v); }
            w.u8(0);
            break;
        case Nbt::ByteArray: w.i32(int32_t(n.arr.size())); for (auto v : n.arr) w.u8(uint8_t(v)); break;
        case Nbt::IntArray: w.i32(int32_t(n.arr.size())); for (auto v : n.arr) w.i32(int32_t(v)); break;
        case Nbt::LongArray: w.i32(int32_t(n.arr.size())); for (auto v : n.arr) w.i64(v); break;
        default: break;
    }
}
inline Writer& write_nbt(Writer& w, const Nbt& n) {
    w.u8(n.t);
    nbt_payload(w, n);
    return w;
}

// ---- SNBT printer ----
inline std::string snbt_quote(const std::string& s) {
    char q = s.find('"') != std::string::npos && s.find('\'') == std::string::npos ? '\'' : '"';
    std::string o(1, q);
    for (char c : s) {
        if (c == q || c == '\\') o += '\\';
        o += c;
    }
    return o + q;
}
inline std::string fmt_double(double v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.17g", v);
    for (int p = 6; p <= 17; p++) {  // shortest representation that round-trips (like Java's Double.toString)
        snprintf(buf, sizeof buf, "%.*g", p, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}
inline std::string to_snbt(const Nbt& n) {
    auto key = [](const std::string& k) {
        bool plain = !k.empty();
        for (char c : k) plain &= isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '+';
        return plain ? k : snbt_quote(k);
    };
    std::string o;
    switch (n.t) {
        case Nbt::Byte: return std::to_string(n.i) + "b";
        case Nbt::Short: return std::to_string(n.i) + "s";
        case Nbt::Int: return std::to_string(n.i);
        case Nbt::Long: return std::to_string(n.i) + "L";
        case Nbt::Float: return fmt_double(float(n.d)) + "f";
        case Nbt::Double: return fmt_double(n.d) + "d";
        case Nbt::String: return snbt_quote(n.s);
        case Nbt::List:
            o = "[";
            for (size_t k = 0; k < n.list.size(); k++) o += (k ? "," : "") + to_snbt(n.list[k]);
            return o + "]";
        case Nbt::Compound:
            o = "{";
            for (size_t k = 0; k < n.comp.size(); k++) o += (k ? "," : "") + key(n.comp[k].first) + ":" + to_snbt(n.comp[k].second);
            return o + "}";
        case Nbt::ByteArray: case Nbt::IntArray: case Nbt::LongArray: {
            o = n.t == Nbt::ByteArray ? "[B;" : n.t == Nbt::IntArray ? "[I;" : "[L;";
            const char* suf = n.t == Nbt::ByteArray ? "B" : n.t == Nbt::LongArray ? "L" : "";
            for (size_t k = 0; k < n.arr.size(); k++) o += (k ? "," : "") + std::to_string(n.arr[k]) + suf;
            return o + "]";
        }
        default: return "END";
    }
}
