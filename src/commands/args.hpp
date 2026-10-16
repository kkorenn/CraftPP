#pragma once
// Command argument types: SNBT, coordinates, block states, items, ranges, entity selectors, and the
// per-parser dispatch used by the command tree (commands/arguments/* in vanilla).
#include "state.hpp"

#include <climits>
#include <optional>
#include <unordered_map>
#include <variant>

// ---------- SNBT (SnbtGrammar subset: compounds, lists, typed arrays, quoted/unquoted strings,
// suffixed/hex/binary numbers, true/false) ----------
inline Nbt read_snbt(StringReader& r, int depth = 0);

inline bool snbt_unquoted_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '+';
}
inline std::optional<Nbt> snbt_number(const std::string& tok) {
    std::string s;
    for (char c : tok) if (c != '_') s += c;
    if (s.empty()) return std::nullopt;
    std::string body = s;
    char suf = 0;
    bool is_unsigned = false;
    char last = char(tolower((unsigned char)body.back()));
    bool hex = body.size() > 2 && body[body[0] == '-' || body[0] == '+' ? 1 : 0] == '0' &&
               tolower((unsigned char)body[body[0] == '-' || body[0] == '+' ? 2 : 1]) == 'x';
    if (!hex && (last == 'b' || last == 's' || last == 'l' || last == 'f' || last == 'd' || last == 'i')) {
        suf = last;
        body.pop_back();
        if (!body.empty() && (tolower((unsigned char)body.back()) == 'u' || tolower((unsigned char)body.back()) == 's') && suf != 'f' && suf != 'd') {
            is_unsigned = tolower((unsigned char)body.back()) == 'u';
            body.pop_back();
        }
    } else if (hex && body.size() > 3 && (last == 'b' || last == 's' || last == 'l' || last == 'i') &&
               (tolower((unsigned char)body[body.size() - 2]) == 'u' || tolower((unsigned char)body[body.size() - 2]) == 's')) {
        suf = last;
        is_unsigned = tolower((unsigned char)body[body.size() - 2]) == 'u';
        body.resize(body.size() - 2);
    }
    if (body.empty()) return std::nullopt;
    char* end;
    bool integral = body.find_first_of(".eE") == std::string::npos || hex;
    if (suf == 'f' || suf == 'd' || !integral) {
        if (hex) return std::nullopt;
        double v = strtod(body.c_str(), &end);
        if (*end || !std::isfinite(v)) return std::nullopt;
        if (suf == 'f') return Nbt::real(Nbt::Float, double(float(v)));
        if (suf == 'd' || !integral) return Nbt::real(Nbt::Double, v);
    }
    int base = 10;
    std::string digits = body;
    bool neg = false;
    if (digits[0] == '-' || digits[0] == '+') { neg = digits[0] == '-'; digits = digits.substr(1); }
    if (digits.size() > 2 && digits[0] == '0' && tolower((unsigned char)digits[1]) == 'x') { base = 16; digits = digits.substr(2); }
    else if (digits.size() > 2 && digits[0] == '0' && tolower((unsigned char)digits[1]) == 'b') { base = 2; digits = digits.substr(2); }
    if (digits.empty()) return std::nullopt;
    errno = 0;
    unsigned long long u = strtoull(digits.c_str(), &end, base);
    if (*end || errno) return std::nullopt;
    int64_t v = neg ? -int64_t(u) : int64_t(u);
    (void)is_unsigned;
    switch (suf) {
        case 'b': return Nbt::num(Nbt::Byte, int8_t(v));
        case 's': return Nbt::num(Nbt::Short, int16_t(v));
        case 'l': return Nbt::num(Nbt::Long, v);
        default:
            if (base == 10 && (v < INT32_MIN || v > INT32_MAX)) return std::nullopt;
            return Nbt::num(Nbt::Int, int32_t(v));
    }
}
inline std::string snbt_key(StringReader& r) {
    r.skip_ws();
    if (r.can() && (r.peek() == '"' || r.peek() == '\'')) return r.read_quoted();
    size_t st = r.c;
    while (r.can() && snbt_unquoted_char(r.peek())) r.skip();
    if (r.c == st) throw r.error("argument.nbt.expected.key");
    return r.s.substr(st, r.c - st);
}
inline Nbt read_snbt(StringReader& r, int depth) {
    if (depth > 512) throw r.error("arguments.nbtpath.too_deep");  // Tag.MAX_DEPTH; also bounds the recursion
    r.skip_ws();
    if (!r.can()) throw r.error("argument.nbt.expected.value");
    char c = r.peek();
    if (c == '{') {
        r.skip();
        Nbt n = Nbt::compound();
        r.skip_ws();
        while (r.can() && r.peek() != '}') {
            size_t st = r.c;
            std::string k = snbt_key(r);
            if (k.empty()) throw r.error_at(st, "snbt.parser.empty_key");
            r.skip_ws();
            r.expect(':');
            n.set(k, read_snbt(r, depth + 1));
            r.skip_ws();
            if (!r.can() || r.peek() != ',') break;
            r.skip();
            r.skip_ws();
        }
        r.skip_ws();
        r.expect('}');
        return n;
    }
    if (c == '[') {
        r.skip();
        if (r.can(2) && r.peek(1) == ';' && strchr("BILbil", r.peek())) {
            char kind = char(toupper((unsigned char)r.peek()));
            r.c += 2;
            Nbt n;
            n.t = kind == 'B' ? Nbt::ByteArray : kind == 'I' ? Nbt::IntArray : Nbt::LongArray;
            r.skip_ws();
            while (r.can() && r.peek() != ']') {
                size_t st = r.c;
                Nbt e = read_snbt(r, depth + 1);
                if (!e.is_num() || e.t == Nbt::Float || e.t == Nbt::Double) throw r.error_at(st, "snbt.parser.invalid_array_element_type");
                n.arr.push_back(e.i);
                r.skip_ws();
                if (!r.can() || r.peek() != ',') break;
                r.skip();
                r.skip_ws();
            }
            r.expect(']');
            return n;
        }
        Nbt n = Nbt::make_list();
        r.skip_ws();
        while (r.can() && r.peek() != ']') {
            n.list.push_back(read_snbt(r, depth + 1));
            r.skip_ws();
            if (!r.can() || r.peek() != ',') break;
            r.skip();
            r.skip_ws();
        }
        r.expect(']');
        return n;
    }
    if (c == '"' || c == '\'') return Nbt::str(r.read_quoted());
    size_t st = r.c;
    while (r.can() && snbt_unquoted_char(r.peek())) r.skip();
    std::string tok = r.s.substr(st, r.c - st);
    if (tok.empty()) throw r.error("argument.nbt.expected.value");
    if (tok == "true") return Nbt::num(Nbt::Byte, 1);
    if (tok == "false") return Nbt::num(Nbt::Byte, 0);
    if (auto n = snbt_number(tok)) return *n;
    if (isdigit((unsigned char)tok[0]) || tok[0] == '+' || tok[0] == '-') {
        r.c = st;
        throw r.error("snbt.parser.invalid_unquoted_start");
    }
    return Nbt::str(tok);
}
inline Nbt parse_snbt(const std::string& s) {
    StringReader r{s};
    Nbt n = read_snbt(r);
    r.skip_ws();
    if (r.can()) throw r.error("argument.nbt.trailing");
    return n;
}

// ComponentSerialization.CODEC applied to a parsed tag: strings -> literal, lists -> first + siblings.
inline Text to_component(const Nbt& n) {
    if (n.t == Nbt::String) return lit(n.s);
    if (n.is_num()) return lit(n.t == Nbt::Float || n.t == Nbt::Double ? fmt_double(n.d) : std::to_string(n.i));
    if (n.t == Nbt::List) {
        if (n.list.empty()) throw err("argument.component.invalid", {"empty list"});
        Text c = to_component(n.list[0]);
        for (size_t k = 1; k < n.list.size(); k++) append(c, to_component(n.list[k]));
        return c;
    }
    if (n.t != Nbt::Compound) throw err("argument.component.invalid", {to_snbt(n)});
    Text c = n;
    if (!c.get("text") && !c.get("translate") && !c.get("score") && !c.get("selector") && !c.get("keybind") && !c.get("nbt") &&
        !c.get("type") && !c.get("object"))
        throw err("argument.component.invalid", {"No key text in MapLike" /* matches DFU message prefix */});
    if (Nbt* e = c.get("extra")) {
        Nbt l = Nbt::make_list();
        for (auto& x : e->list) l.list.push_back(to_component(x));
        *e = l;
    }
    if (Nbt* w = c.get("with")) {
        Nbt l = Nbt::make_list();
        for (auto& x : w->list) l.list.push_back(to_component(x));
        *w = l;
    }
    return c;
}

// ---------- command source (CommandSourceStack) ----------
struct Entity;  // non-player entities live in src/mobs (opaque here; see the entity_* functions below)
Text entity_display_name(const Entity* e);  // mobs.cpp
std::string entity_name_string(const Entity* e);
struct Source {
    PlayerPtr sender;  // who receives feedback; null = server console
    PlayerPtr entity;  // executing entity (@s); null = none
    Entity* mob = nullptr;  // executing non-player entity (execute as @e[type=...]); entity is null then
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;
    bool eyes = false;  // anchor
    int level = 4;
    bool silent = false;
    std::string dimension = "minecraft:overworld";
    std::string name() const { return mob ? entity_name_string(mob) : entity ? entity->name : sender ? sender->name : "Server"; }
    Text display() const { return mob ? entity_display_name(mob) : entity ? player_name(*entity) : sender ? player_name(*sender) : lit("Server"); }
};
inline Source source_for(const PlayerPtr& p) {
    Source s;
    s.sender = s.entity = p;
    s.x = p->x; s.y = p->y; s.z = p->z;
    s.yaw = p->yaw; s.pitch = p->pitch;
    s.level = perm_level(*p);
    return s;
}
inline Source console_source() {
    Source s;
    s.x = world_spawn.x; s.y = world_spawn.y; s.z = world_spawn.z;
    return s;
}

// ---------- coordinates ----------
#include "../game/vec.hpp"  // Vec3
struct Coord { double v = 0; bool rel = false; };
struct Coords {
    Coord c[3];
    bool local = false;
    int n = 3;

    // LocalCoordinates.getPosition / WorldCoordinates.getPosition
    Vec3 pos(const Source& s) const {
        double ay = s.y + (s.eyes ? 1.62 : 0);
        if (local) {
            const double R = M_PI / 180.0;
            double cy = cos((s.yaw + 90) * R), sy = sin((s.yaw + 90) * R);
            double cp = cos(-s.pitch * R), sp = sin(-s.pitch * R);
            double cp2 = cos((-s.pitch + 90) * R), sp2 = sin((-s.pitch + 90) * R);
            Vec3 f{cy * cp, sp, sy * cp}, up{cy * cp2, sp2, sy * cp2};
            Vec3 left{f.y * up.z - f.z * up.y, f.z * up.x - f.x * up.z, f.x * up.y - f.y * up.x};
            left = {-left.x, -left.y, -left.z};
            double l = c[0].v, u = c[1].v, fw = c[2].v;
            return {s.x + f.x * fw + up.x * u + left.x * l, ay + f.y * fw + up.y * u + left.y * l, s.z + f.z * fw + up.z * u + left.z * l};
        }
        return {c[0].rel ? s.x + c[0].v : c[0].v, c[1].rel ? s.y + c[1].v : c[1].v, c[2].rel ? s.z + c[2].v : c[2].v};
    }
    void block(const Source& s, int& x, int& y, int& z) const {
        Vec3 v = pos(s);
        x = Mth::floor(v.x); y = Mth::floor(v.y); z = Mth::floor(v.z);  // BlockPos.containing: saturating
    }
    // Vec2 / rotation: c[0], c[1]
    void rotation(const Source& s, float& yaw, float& pitch) const {
        yaw = float(c[0].rel ? s.yaw + c[0].v : c[0].v);
        pitch = float(c[1].rel ? s.pitch + c[1].v : c[1].v);
    }
};
inline Coord read_coord(StringReader& r, bool is_int, bool center) {
    if (r.can() && r.peek() == '^') throw r.error("argument.pos.mixed");
    if (!r.can()) throw r.error(is_int ? "argument.pos.missing.int" : "argument.pos.missing.double");
    Coord c;
    if (r.peek() == '~') { c.rel = true; r.skip(); }
    if (r.can() && r.peek() != ' ') {
        size_t st = r.c;
        if (c.rel || !is_int) c.v = r.read_double();
        else c.v = r.read_int();
        if (!c.rel && center && r.s.substr(st, r.c - st).find('.') == std::string::npos) c.v += 0.5;
    } else if (!c.rel) throw r.error(is_int ? "argument.pos.missing.int" : "argument.pos.missing.double");
    return c;
}
inline Coords read_coords(StringReader& r, int n, bool is_int, bool center, bool allow_local, const char* incomplete) {
    size_t st = r.c;
    Coords cs;
    cs.n = n;
    if (allow_local && r.can() && r.peek() == '^') {
        cs.local = true;
        for (int i = 0; i < n; i++) {
            if (i) {
                if (!r.can() || r.peek() != ' ') { r.c = st; throw r.error(incomplete); }
                r.skip();
            }
            if (!r.can() || r.peek() != '^') { r.c = st; throw r.error("argument.pos.mixed"); }
            r.skip();
            if (r.can() && r.peek() != ' ') cs.c[i].v = r.read_double();
        }
        return cs;
    }
    for (int i = 0; i < n; i++) {
        if (i) {
            if (!r.can() || r.peek() != ' ') { r.c = st; throw r.error(incomplete); }
            r.skip();
        }
        // Vec2 reads x and z; y center-correction never applies to the 2nd/3rd component of a 3-vector's y.
        bool cc = center && !(n == 3 && i == 1);
        cs.c[i] = read_coord(r, is_int, cc);
    }
    return cs;
}

// ---------- identifiers / registries ----------
inline bool id_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c == '/' || c == '.' || c == '-';
}
inline std::string read_id(StringReader& r) {
    size_t st = r.c;
    while (r.can() && id_char(r.peek())) r.skip();
    std::string s = r.s.substr(st, r.c - st);
    size_t colon = s.find(':');
    if (colon != std::string::npos && (s.find(':', colon + 1) != std::string::npos || s.find('/') < colon)) {
        r.c = st;
        throw r.error("argument.id.invalid");
    }
    if (s.empty()) { r.c = st; throw r.error("argument.id.invalid"); }
    return colon == std::string::npos ? "minecraft:" + s : s;
}
inline std::string short_id(const std::string& id) { return id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id; }
inline const gen::Registry* registry(const std::string& name) {
    for (auto& r : gen::REGISTRIES) if (name == r.name) return &r;
    return nullptr;
}
inline int registry_index(const gen::Registry& r, const std::string& id) {
    if (id.rfind("minecraft:", 0) != 0) return -1;
    std::string s = id.substr(10);
    for (int i = 0; i < r.count; i++) if (s == r.entries[i]) return i;
    return -1;
}
inline int item_id(const std::string& id) {
    static const auto m = [] {
        std::unordered_map<std::string_view, int> m;
        for (int i = 0; i < int(std::size(gen::ITEM_NAMES)); i++) m[gen::ITEM_NAMES[i]] = i;
        return m;
    }();
    if (id.rfind("minecraft:", 0) != 0) return -1;
    auto it = m.find(std::string_view(id).substr(10));
    return it == m.end() ? -1 : it->second;
}
inline int block_id(const std::string& id) {
    static const auto m = [] {
        std::unordered_map<std::string_view, int> m;
        for (int i = 0; i < int(std::size(gen::BLOCKS)); i++) m[gen::BLOCKS[i].name] = i;
        return m;
    }();
    if (id.rfind("minecraft:", 0) != 0) return -1;
    auto it = m.find(std::string_view(id).substr(10));
    return it == m.end() ? -1 : it->second;
}
inline int block_of_state(int state) {
    auto it = std::upper_bound(std::begin(gen::BLOCKS), std::end(gen::BLOCKS), state,
                               [](int s, const gen::BlockInfo& b) { return s < b.first; });
    return int(it - std::begin(gen::BLOCKS)) - 1;
}
inline std::vector<std::string> split_values(const char* csv) {
    std::vector<std::string> v;
    std::string cur;
    for (const char* p = csv;; p++) {
        if (*p == ',' || !*p) { v.push_back(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    return v;
}
// State id <-> per-property value indices (first property most significant, as in blocks.json).
inline std::vector<int> state_values(int state) {
    const auto& b = gen::BLOCKS[block_of_state(state)];
    std::vector<int> v(b.prop_count);
    int rem = state - b.first;
    for (int k = b.prop_count - 1; k >= 0; k--) {
        int n = gen::BLOCK_PROPS[b.prop_off + k].count;
        v[size_t(k)] = rem % n;
        rem /= n;
    }
    return v;
}
inline int state_from_values(int block, const std::vector<int>& v) {
    const auto& b = gen::BLOCKS[block];
    int idx = 0;
    for (int k = 0; k < b.prop_count; k++) idx = idx * gen::BLOCK_PROPS[b.prop_off + k].count + v[size_t(k)];
    return b.first + idx;
}
inline std::string state_string(int state) {
    int bi = block_of_state(state);
    const auto& b = gen::BLOCKS[bi];
    std::string s = std::string("minecraft:") + b.name;
    if (!b.prop_count) return s;
    auto v = state_values(state);
    s += '[';
    for (int k = 0; k < b.prop_count; k++) {
        const auto& p = gen::BLOCK_PROPS[b.prop_off + k];
        s += (k ? "," : "") + std::string(p.name) + "=" + split_values(p.values)[size_t(v[size_t(k)])];
    }
    return s + ']';
}
template <class T> inline const T* find_tag(const T* tags, size_t n, const std::string& id) {
    if (id.rfind("minecraft:", 0) != 0) return nullptr;
    std::string s = id.substr(10);
    for (size_t i = 0; i < n; i++) if (s == tags[i].name) return &tags[i];
    return nullptr;
}

// ---------- block state / predicate (BlockStateParser) ----------
struct BlockSpec {
    int block = -1, state = -1;                   // exact block (state = default + given properties)
    const std::remove_extent_t<decltype(gen::BLOCK_TAGS)>* tag = nullptr;  // #tag (predicates only)
    std::vector<std::pair<std::string, std::string>> props;  // as written (for tag matching)
    std::optional<Nbt> nbt;
    bool matches(int st) const {
        int b = block_of_state(st);
        if (tag) {
            bool in = false;
            for (int i = 0; i < tag->count; i++) in |= gen::BLOCK_TAGS_IDS[tag->off + i] == b;
            if (!in) return false;
        } else if (b != block) return false;
        const auto& bi = gen::BLOCKS[b];
        auto vals = state_values(st);
        for (auto& [k, v] : props) {
            bool ok = !tag;  // tags ignore properties the block doesn't have
            for (int i = 0; i < bi.prop_count; i++)
                if (k == gen::BLOCK_PROPS[bi.prop_off + i].name) ok = split_values(gen::BLOCK_PROPS[bi.prop_off + i].values)[size_t(vals[size_t(i)])] == v;
            if (!ok) return false;
        }
        return true;  // ponytail: block entity NBT not matched (no block entities yet)
    }
};
inline BlockSpec read_block(StringReader& r, bool allow_tag) {
    BlockSpec b;
    size_t st = r.c;
    bool is_tag = r.can() && r.peek() == '#';
    if (is_tag) {
        if (!allow_tag) throw r.error("argument.block.tag.disallowed");
        r.skip();
        std::string id = read_id(r);
        b.tag = find_tag(gen::BLOCK_TAGS, std::size(gen::BLOCK_TAGS), id);
        if (!b.tag) { r.c = st; throw r.error("arguments.block.tag.unknown", {id}); }
    } else {
        std::string id = read_id(r);
        b.block = block_id(id);
        if (b.block < 0) { r.c = st; throw r.error("argument.block.id.invalid", {id}); }
        b.state = gen::BLOCKS[b.block].def;
    }
    std::string bname = is_tag ? "" : "minecraft:" + std::string(gen::BLOCKS[b.block].name);
    if (r.can() && r.peek() == '[') {
        r.skip();
        r.skip_ws();
        std::vector<int> vals = b.block >= 0 ? state_values(b.state) : std::vector<int>{};
        std::set<std::string> seen;
        while (r.can() && r.peek() != ']') {
            r.skip_ws();
            size_t ks = r.c;
            std::string k = r.read_string();
            int pi = -1;
            if (b.block >= 0) {
                const auto& bi = gen::BLOCKS[b.block];
                for (int i = 0; i < bi.prop_count; i++) if (k == gen::BLOCK_PROPS[bi.prop_off + i].name) pi = i;
                if (pi < 0) { r.c = ks; throw r.error("argument.block.property.unknown", {bname, k}); }
            }
            if (!seen.insert(k).second) { r.c = ks; throw r.error("argument.block.property.duplicate", {k, bname}); }
            r.skip_ws();
            if (!r.can() || r.peek() != '=') throw r.error("argument.block.property.novalue", {k, bname});
            r.skip();
            r.skip_ws();
            size_t vs = r.c;
            std::string v = r.read_string();
            if (pi >= 0) {
                auto opts = split_values(gen::BLOCK_PROPS[gen::BLOCKS[b.block].prop_off + pi].values);
                auto it = std::find(opts.begin(), opts.end(), v);
                if (it == opts.end()) { r.c = vs; throw r.error("argument.block.property.invalid", {bname, v, k}); }
                vals[size_t(pi)] = int(it - opts.begin());
            }
            b.props.emplace_back(k, v);
            r.skip_ws();
            if (r.can() && r.peek() == ',') { r.skip(); continue; }
            if (!r.can() || r.peek() != ']') throw r.error("argument.block.property.unclosed");
        }
        if (!r.can()) throw r.error("argument.block.property.unclosed");
        r.skip();
        if (b.block >= 0) b.state = state_from_values(b.block, vals);
    }
    if (r.can() && r.peek() == '{') b.nbt = read_snbt(r);
    return b;
}

// ---------- items (ItemParser / ItemPredicateArgument) ----------
struct ItemSpec {
    int item = -1;
    const std::remove_extent_t<decltype(gen::ITEM_TAGS)>* tag = nullptr;
    bool any = false;      // '*' in predicates
    std::string components;  // raw "[...]" text; ponytail: parsed for syntax only, not applied to stacks
    bool matches(int id) const {
        if (any) return true;
        if (tag) { for (int i = 0; i < tag->count; i++) if (gen::ITEM_TAGS_IDS[tag->off + i] == id) return true; return false; }
        return id == item;
    }
};
inline void skip_bracketed(StringReader& r, char open, char close) {
    int depth = 0;
    while (r.can()) {
        char c = r.peek();
        if (c == '"' || c == '\'') { r.read_quoted(); continue; }
        r.skip();
        if (c == open) depth++;
        else if (c == close && --depth == 0) return;
    }
    throw r.error("parsing.expected", {std::string(1, close)});
}
inline ItemSpec read_item(StringReader& r, bool predicate) {
    ItemSpec it;
    size_t st = r.c;
    if (predicate && r.can() && r.peek() == '*') { r.skip(); it.any = true; }
    else if (predicate && r.can() && r.peek() == '#') {
        r.skip();
        std::string id = read_id(r);
        it.tag = find_tag(gen::ITEM_TAGS, std::size(gen::ITEM_TAGS), id);
        if (!it.tag) { r.c = st; throw r.error("arguments.item.tag.unknown", {id}); }
    } else {
        std::string id = read_id(r);
        it.item = item_id(id);
        if (it.item < 0 || it.item == 0) { r.c = st; throw r.error("argument.item.id.invalid", {id}); }
    }
    if (r.can() && r.peek() == '[') {
        size_t cs = r.c;
        // Validate component values as SNBT: key=value or !key (stacks) / key~value, key|key (predicates).
        skip_bracketed(r, '[', ']');
        it.components = r.s.substr(cs, r.c - cs);
    }
    if (!predicate && r.can() && r.peek() == '{') read_snbt(r);
    return it;
}

// ---------- ranges (MinMaxBounds) ----------
struct Range {
    std::optional<double> min, max;
    bool matches(double v) const { return (!min || v >= *min) && (!max || v <= *max); }
    bool any() const { return !min && !max; }
};
inline Range read_range(StringReader& r, bool ints) {
    if (!r.can()) throw r.error("argument.range.empty");
    size_t st = r.c;
    auto number = [&]() -> std::optional<double> {
        size_t s0 = r.c;
        while (r.can() && (isdigit((unsigned char)r.peek()) || r.peek() == '-' || (r.peek() == '.' && !(r.can(2) && r.peek(1) == '.')))) r.skip();
        std::string t = r.s.substr(s0, r.c - s0);
        if (t.empty()) return std::nullopt;
        if (ints && t.find('.') != std::string::npos) { r.c = s0; throw r.error("argument.range.ints"); }
        char* end;
        double v = strtod(t.c_str(), &end);
        if (*end) { r.c = s0; throw r.error(ints ? "parsing.int.invalid" : "parsing.double.invalid", {t}); }
        return v;
    };
    Range g;
    g.min = number();
    if (r.can(2) && r.peek() == '.' && r.peek(1) == '.') { r.c += 2; g.max = number(); }
    else g.max = g.min;
    if (!g.min && !g.max) { r.c = st; throw r.error("argument.range.empty"); }
    if (g.min && g.max && *g.min > *g.max) { r.c = st; throw r.error("argument.range.swapped"); }
    return g;
}
inline std::string range_string(const Range& g) {
    auto f = [](double v) { return v == std::floor(v) ? std::to_string(int64_t(v)) : fmt_double(v); };
    if (g.min && g.max && *g.min == *g.max) return f(*g.min);
    return (g.min ? f(*g.min) : "") + ".." + (g.max ? f(*g.max) : "");
}

// ---------- entity selectors (EntitySelectorParser / EntitySelector) ----------
struct Selector {
    std::string text;
    bool by_name = false, by_uuid = false, self = false, includes_entities = false, players_only_type = false;
    std::string name;  // player name or dashed uuid
    int limit = INT_MAX;
    enum Sort { ARBITRARY, NEAREST, FURTHEST, RANDOM } sort = ARBITRARY;
    std::optional<double> x, y, z, dx, dy, dz;
    Range distance, level, xrot, yrot;
    std::vector<std::pair<std::string, bool>> names, teams, types, tags, predicates;  // value, negated
    std::vector<std::pair<int, bool>> gamemodes;
    std::vector<std::pair<std::string, Range>> score_ranges;
    bool unresolvable = false;  // a filter that no player can satisfy (e.g. type=cow)

    int max_results() const { return by_name || by_uuid || self ? 1 : limit; }
};

inline int gamemode_from(const std::string& s) {
    static const char* n[] = {"survival", "creative", "adventure", "spectator"};
    for (int i = 0; i < 4; i++) if (s == n[i]) return i;
    return -1;
}

inline Selector read_selector(StringReader& r, bool allow_selectors) {
    Selector sel;
    size_t st = r.c;
    if (r.can() && r.peek() == '@') {
        if (!allow_selectors) throw r.error("argument.entity.selector.not_allowed");
        r.skip();
        if (!r.can()) throw r.error("argument.entity.selector.missing");
        char k = r.next();
        switch (k) {
            case 'p': sel.limit = 1; sel.sort = Selector::NEAREST; sel.players_only_type = true; break;
            case 'a': sel.players_only_type = true; break;
            case 'r': sel.limit = 1; sel.sort = Selector::RANDOM; sel.players_only_type = true; break;
            case 's': sel.self = true; sel.includes_entities = true; break;
            case 'e': sel.includes_entities = true; break;
            case 'n': sel.limit = 1; sel.sort = Selector::NEAREST; sel.includes_entities = true; break;
            default: r.c = st + 1; throw r.error("argument.entity.selector.unknown", {"@" + std::string(1, k)});
        }
        if (r.can() && r.peek() == '[') {
            r.skip();
            r.skip_ws();
            while (r.can() && r.peek() != ']') {
                r.skip_ws();
                size_t os = r.c;
                std::string opt = r.read_string();
                r.skip_ws();
                if (!r.can() || r.peek() != '=') { r.c = os; throw r.error("argument.entity.options.valueless", {opt}); }
                r.skip();
                r.skip_ws();
                auto neg = [&] { r.skip_ws(); if (r.can() && r.peek() == '!') { r.skip(); r.skip_ws(); return true; } return false; };
                if (opt == "name") { bool n = neg(); sel.names.emplace_back(r.read_string(), n); }
                else if (opt == "distance") {
                    size_t vs = r.c;
                    sel.distance = read_range(r, false);
                    if ((sel.distance.min && *sel.distance.min < 0) || (sel.distance.max && *sel.distance.max < 0)) { r.c = vs; throw r.error("argument.entity.options.distance.negative"); }
                } else if (opt == "level") {
                    size_t vs = r.c;
                    sel.level = read_range(r, true);
                    if ((sel.level.min && *sel.level.min < 0) || (sel.level.max && *sel.level.max < 0)) { r.c = vs; throw r.error("argument.entity.options.level.negative"); }
                } else if (opt == "x") sel.x = r.read_double();
                else if (opt == "y") sel.y = r.read_double();
                else if (opt == "z") sel.z = r.read_double();
                else if (opt == "dx") sel.dx = r.read_double();
                else if (opt == "dy") sel.dy = r.read_double();
                else if (opt == "dz") sel.dz = r.read_double();
                else if (opt == "x_rotation") sel.xrot = read_range(r, false);
                else if (opt == "y_rotation") sel.yrot = read_range(r, false);
                else if (opt == "limit") {
                    size_t vs = r.c;
                    int l = r.read_int();
                    if (l < 1) { r.c = vs; throw r.error("argument.entity.options.limit.toosmall"); }
                    if (sel.self) { r.c = os; throw r.error("argument.entity.options.inapplicable", {opt}); }
                    sel.limit = l;
                } else if (opt == "sort") {
                    size_t vs = r.c;
                    std::string s = r.read_unquoted();
                    if (s == "nearest") sel.sort = Selector::NEAREST;
                    else if (s == "furthest") sel.sort = Selector::FURTHEST;
                    else if (s == "random") sel.sort = Selector::RANDOM;
                    else if (s == "arbitrary") sel.sort = Selector::ARBITRARY;
                    else { r.c = vs; throw r.error("argument.entity.options.sort.irreversible", {s}); }
                } else if (opt == "gamemode") {
                    bool n = neg();
                    size_t vs = r.c;
                    std::string s = r.read_unquoted();
                    int g = gamemode_from(s);
                    if (g < 0) { r.c = vs; throw r.error("argument.entity.options.mode.invalid", {s}); }
                    sel.gamemodes.emplace_back(g, n);
                } else if (opt == "team") { bool n = neg(); sel.teams.emplace_back(r.read_unquoted(), n); }
                else if (opt == "type") {
                    bool n = neg();
                    size_t vs = r.c;
                    bool tag = r.can() && r.peek() == '#';
                    if (tag) r.skip();
                    std::string id = read_id(r);
                    if (!tag && registry_index(*registry("minecraft:entity_type"), id) < 0) {
                        r.c = vs;
                        throw r.error("argument.entity.options.type.invalid", {id});
                    }
                    sel.types.emplace_back((tag ? "#" : "") + id, n);
                    if (!n && !tag && id == "minecraft:player") { sel.includes_entities = false; sel.players_only_type = true; }
                } else if (opt == "tag") { bool n = neg(); sel.tags.emplace_back(r.read_unquoted(), n); }
                else if (opt == "nbt") { neg(); read_snbt(r); }  // ponytail: entity NBT not matched
                else if (opt == "scores") {
                    r.expect('{');
                    r.skip_ws();
                    while (r.can() && r.peek() != '}') {
                        std::string obj = r.read_unquoted();
                        r.skip_ws();
                        r.expect('=');
                        r.skip_ws();
                        sel.score_ranges.emplace_back(obj, read_range(r, true));
                        r.skip_ws();
                        if (r.can() && r.peek() == ',') { r.skip(); r.skip_ws(); }
                    }
                    r.expect('}');
                } else if (opt == "advancements") { skip_bracketed(r, '{', '}'); }  // ponytail: no advancements yet
                else if (opt == "predicate") { bool n = neg(); sel.predicates.emplace_back(read_id(r), n); }
                else { r.c = os; throw r.error("argument.entity.options.unknown", {opt}); }
                r.skip_ws();
                if (r.can() && r.peek() == ',') { r.skip(); continue; }
                if (!r.can() || r.peek() != ']') throw r.error("argument.entity.options.unterminated");
            }
            if (!r.can()) throw r.error("argument.entity.options.unterminated");
            r.skip();
        }
    } else {
        while (r.can() && r.peek() != ' ') r.skip();
        sel.name = r.s.substr(st, r.c - st);
        bool uuid = sel.name.size() == 36 && std::count(sel.name.begin(), sel.name.end(), '-') == 4;
        if (uuid) sel.by_uuid = true;
        else {
            if (sel.name.empty() || sel.name.size() > 16) { r.c = st; throw r.error("argument.entity.invalid"); }
            sel.by_name = true;
        }
    }
    sel.text = r.s.substr(st, r.c - st);
    return sel;
}

inline std::vector<PlayerPtr> select_players_unsorted(const Selector& sel, const Source& src) {
    std::vector<PlayerPtr> out;
    if (sel.by_name || sel.by_uuid) {
        for (auto& p : snapshot())
            if (sel.by_name ? p->name == sel.name : strcasecmp(player_uuid(*p).c_str(), sel.name.c_str()) == 0) out.push_back(p);
        return out;
    }
    double ox = sel.x.value_or(src.x), oy = sel.y.value_or(src.y), oz = sel.z.value_or(src.z);
    std::vector<PlayerPtr> pool = sel.self ? (src.entity ? std::vector<PlayerPtr>{src.entity} : std::vector<PlayerPtr>{}) : snapshot();
    for (auto& p : pool) {
        auto is = [&](const auto& list, auto pred) {
            for (auto& [v, n] : list) if (pred(v) == n) return false;
            return true;
        };
        if (!is(sel.names, [&](const std::string& n) { return p->name == n; })) continue;
        if (!is(sel.gamemodes, [&](int g) { return p->gamemode == g; })) continue;
        if (!is(sel.teams, [&](const std::string& t) {
                Team* tm = team_of(p->name);
                return t.empty() ? tm != nullptr : tm && tm->name == t;
            })) continue;
        if (!is(sel.types, [&](const std::string& t) {
                if (t[0] == '#') return false;  // ponytail: entity type tags need entity_type tag data
                return t == "minecraft:player";
            })) continue;
        if (!is(sel.tags, [&](const std::string& t) {
                auto it = entity_tags.find(uuid_hex(*p));
                bool has_any = it != entity_tags.end() && !it->second.empty();
                return t.empty() ? has_any : has_any && it->second.count(t);
            })) continue;
        if (!sel.predicates.empty()) continue;  // ponytail: no loot predicates; nothing matches
        if (!sel.level.any() && !sel.level.matches(0)) continue;  // ponytail: no XP yet (level 0)
        if (!sel.xrot.any() && !sel.xrot.matches(p->pitch)) continue;
        if (!sel.yrot.any()) {
            double y = std::fmod(p->yaw, 360.0);
            if (y >= 180) y -= 360;
            if (y < -180) y += 360;
            if (!sel.yrot.matches(y)) continue;
        }
        if (sel.dx || sel.dy || sel.dz) {
            double dx = sel.dx.value_or(0), dy = sel.dy.value_or(0), dz = sel.dz.value_or(0);
            double x0 = std::min(ox, ox + dx), x1 = std::max(ox, ox + dx) + 1;
            double y0 = std::min(oy, oy + dy), y1 = std::max(oy, oy + dy) + 1;
            double z0 = std::min(oz, oz + dz), z1 = std::max(oz, oz + dz) + 1;
            // player bounding box 0.6 x 1.8 intersects the selection box
            if (!(p->x + 0.3 > x0 && p->x - 0.3 < x1 && p->y + 1.8 > y0 && p->y < y1 && p->z + 0.3 > z0 && p->z - 0.3 < z1)) continue;
        }
        if (!sel.distance.any()) {
            double d = std::sqrt((p->x - ox) * (p->x - ox) + (p->y - oy) * (p->y - oy) + (p->z - oz) * (p->z - oz));
            if (!sel.distance.matches(d)) continue;
        }
        bool ok = true;
        for (auto& [obj, range] : sel.score_ranges) {
            auto h = scores.find(p->name);
            if (h == scores.end() || !h->second.count(obj) || !range.matches(h->second[obj].value)) ok = false;
        }
        if (!ok) continue;
        out.push_back(p);
    }
    return out;
}
inline std::vector<PlayerPtr> select(const Selector& sel, const Source& src) {  // players only
    std::vector<PlayerPtr> out = select_players_unsorted(sel, src);
    if (sel.by_name || sel.by_uuid) return out;
    double ox = sel.x.value_or(src.x), oy = sel.y.value_or(src.y), oz = sel.z.value_or(src.z);
    auto dist = [&](const PlayerPtr& p) { return (p->x - ox) * (p->x - ox) + (p->y - oy) * (p->y - oy) + (p->z - oz) * (p->z - oz); };
    if (sel.sort == Selector::NEAREST) std::stable_sort(out.begin(), out.end(), [&](auto& a, auto& b) { return dist(a) < dist(b); });
    if (sel.sort == Selector::FURTHEST) std::stable_sort(out.begin(), out.end(), [&](auto& a, auto& b) { return dist(a) > dist(b); });
    if (sel.sort == Selector::RANDOM) std::shuffle(out.begin(), out.end(), cmd_rng);
    if (int(out.size()) > sel.limit) out.resize(size_t(sel.limit));
    return out;
}

// ---------- players + other entities (EntitySelector.findEntities) ----------
// Non-player entities matching the selector's filters (no sort/limit); defined in src/mobs/mobs.cpp.
void select_entity_candidates(const Selector& sel, const Source& src, double ox, double oy, double oz, std::vector<Entity*>& out);
Vec3 entity_position(const Entity* e);
struct Target {  // a selected entity: a player or another entity
    PlayerPtr p;
    Entity* e = nullptr;
    Vec3 pos() const { return p ? Vec3{p->x, p->y, p->z} : entity_position(e); }
    Text display() const { return p ? player_name(*p) : entity_display_name(e); }
    std::string name() const { return p ? p->name : entity_name_string(e); }
};
inline std::vector<Target> select_targets(const Selector& sel, const Source& src) {
    std::vector<Target> out;
    bool players_only = sel.players_only_type || sel.by_name;
    if (!(sel.self && src.mob))
        for (auto& p : select_players_unsorted(sel, src)) out.push_back({p, nullptr});
    if (!players_only) {
        std::vector<Entity*> es;
        double ox = sel.x.value_or(src.x), oy = sel.y.value_or(src.y), oz = sel.z.value_or(src.z);
        select_entity_candidates(sel, src, ox, oy, oz, es);
        for (Entity* e : es) out.push_back({nullptr, e});
    }
    if (sel.by_uuid) return out;
    double ox = sel.x.value_or(src.x), oy = sel.y.value_or(src.y), oz = sel.z.value_or(src.z);
    auto dist = [&](const Target& t) { Vec3 v = t.pos(); return (v.x - ox) * (v.x - ox) + (v.y - oy) * (v.y - oy) + (v.z - oz) * (v.z - oz); };
    if (sel.sort == Selector::NEAREST) std::stable_sort(out.begin(), out.end(), [&](auto& a, auto& b) { return dist(a) < dist(b); });
    if (sel.sort == Selector::FURTHEST) std::stable_sort(out.begin(), out.end(), [&](auto& a, auto& b) { return dist(a) > dist(b); });
    if (sel.sort == Selector::RANDOM) std::shuffle(out.begin(), out.end(), cmd_rng);
    if (int(out.size()) > sel.limit) out.resize(size_t(sel.limit));
    return out;
}

// ---------- parsed argument values ----------
struct ScoreHolders { bool all = false; Selector sel; std::string name; bool is_selector = false; };
struct GameProfiles { Selector sel; bool is_selector = false; std::string name; };
using Value = std::variant<std::monostate, int64_t, double, bool, std::string, Coords, BlockSpec, ItemSpec, Range, Selector, Nbt,
                           ScoreHolders, GameProfiles>;

// MessageArgument: resolves @-selectors in the text when the source may use them.
inline Text resolve_message(const std::string& msg, const Source& src) {
    if (src.level < 2 || msg.find('@') == std::string::npos) return lit(msg);
    Text out = lit("");
    std::string cur;
    StringReader r{msg};
    while (r.can()) {
        if (r.peek() == '@' && r.can(2) && strchr("pares n", r.peek(1)) && r.peek(1) != ' ') {
            size_t st = r.c;
            try {
                Selector sel = read_selector(r, true);
                append(out, lit(cur));
                cur.clear();
                std::vector<Text> names;
                for (auto& p : select(sel, src)) names.push_back(player_name(*p));
                append(out, join_list(names));
                continue;
            } catch (CmdError&) { r.c = st; }
        }
        cur += r.next();
    }
    append(out, lit(cur));
    return out;
}

inline std::string read_token(StringReader& r) {  // generic: up to the next top-level space
    size_t st = r.c;
    int depth = 0;
    while (r.can()) {
        char c = r.peek();
        if (c == '"' || c == '\'') { r.read_quoted(); continue; }
        if (c == ' ' && depth == 0) break;
        if (c == '[' || c == '{' || c == '(') depth++;
        if (c == ']' || c == '}' || c == ')') depth--;
        r.skip();
    }
    return r.s.substr(st, r.c - st);
}

inline Value parse_argument(const gen::CmdNode& n, StringReader& r, const Source& src) {
    size_t st = r.c;
    auto check = [&](double v, const char* low, const char* big, auto fmt) {
        if (n.has_min && v < n.min) { r.c = st; throw r.error(low, {fmt(n.min), fmt(v)}); }
        if (n.has_max && v > n.max) { r.c = st; throw r.error(big, {fmt(n.max), fmt(v)}); }
    };
    auto fi = [](double v) { return std::to_string(int64_t(v)); };
    auto fd = [](double v) { return fmt_double(v); };
    switch (n.parser) {
        case arg_types::bool_: return r.read_bool();
        case arg_types::integer: { int v = r.read_int(); check(v, "argument.integer.low", "argument.integer.big", fi); return int64_t(v); }
        case arg_types::long_: { int64_t v = r.read_long(); check(double(v), "argument.long.low", "argument.long.big", fi); return v; }
        case arg_types::float_: { double v = float(r.read_double(true)); check(v, "argument.float.low", "argument.float.big", fd); return v; }
        case arg_types::double_: { double v = r.read_double(); check(v, "argument.double.low", "argument.double.big", fd); return v; }
        case arg_types::string_:
            if (n.flags == 2) { std::string s = r.rest(); r.c = r.s.size(); return s; }
            return n.flags == 1 ? r.read_string() : r.read_unquoted();
        case arg_types::entity: {
            Selector sel = read_selector(r, src.level >= 2);
            bool single = n.flags & 1, players = n.flags & 2;
            if (single && sel.max_results() > 1) { r.c = st; throw r.error(players ? "argument.player.toomany" : "argument.entity.toomany"); }
            if (players && sel.includes_entities && !sel.self) { r.c = st; throw r.error("argument.player.entities"); }
            return sel;
        }
        case arg_types::game_profile: {
            GameProfiles g;
            if (r.can() && r.peek() == '@') { g.sel = read_selector(r, src.level >= 2); g.is_selector = true; }
            else { while (r.can() && r.peek() != ' ') r.skip(); g.name = r.s.substr(st, r.c - st); }
            return g;
        }
        case arg_types::score_holder: {
            ScoreHolders h;
            if (r.can() && r.peek() == '@') { h.sel = read_selector(r, src.level >= 2); h.is_selector = true; }
            else if (r.can() && r.peek() == '*') { r.skip(); h.all = true; }
            else { while (r.can() && r.peek() != ' ') r.skip(); h.name = r.s.substr(st, r.c - st); }
            if (!(n.flags & 1) && (h.all || (h.is_selector && h.sel.max_results() > 1))) { r.c = st; throw r.error("argument.entity.toomany"); }
            return h;
        }
        case arg_types::block_pos: return read_coords(r, 3, true, false, true, "argument.pos3d.incomplete");
        case arg_types::column_pos: return read_coords(r, 2, true, false, false, "argument.pos2d.incomplete");
        case arg_types::vec3: return read_coords(r, 3, false, true, true, "argument.pos3d.incomplete");
        case arg_types::vec2: return read_coords(r, 2, false, true, false, "argument.pos2d.incomplete");
        case arg_types::rotation: return read_coords(r, 2, false, false, false, "argument.rotation.incomplete");
        case arg_types::angle: {
            if (!r.can()) throw r.error("argument.angle.incomplete");
            Coords c; c.n = 1;
            if (r.peek() == '~') { c.c[0].rel = true; r.skip(); }
            if (r.can() && r.peek() != ' ') c.c[0].v = r.read_double(true);
            return c;
        }
        case arg_types::block_state: return read_block(r, false);
        case arg_types::block_predicate: return read_block(r, true);
        case arg_types::item_stack: return read_item(r, false);
        case arg_types::item_predicate: return read_item(r, true);
        case arg_types::int_range: return read_range(r, true);
        case arg_types::float_range: return read_range(r, false);
        case arg_types::nbt_compound_tag: {
            if (!r.can() || r.peek() != '{') throw r.error("argument.nbt.expected.compound");
            return read_snbt(r);
        }
        case arg_types::nbt_tag: return read_snbt(r);
        case arg_types::component: {
            Nbt v = read_snbt(r);
            try { return to_component(v); } catch (CmdError& e) { e.input = r.s; e.cursor = int(st); throw; }
        }
        case arg_types::style: {
            Nbt v = read_snbt(r);
            if (v.t != Nbt::Compound) { r.c = st; throw r.error("argument.style.invalid", {to_snbt(v)}); }
            return v;
        }
        case arg_types::message: { std::string s = r.rest(); r.c = r.s.size(); return s; }
        case arg_types::gamemode: {
            std::string s = r.read_unquoted();
            int g = gamemode_from(s);
            if (g < 0) { r.c = st; throw r.error("argument.gamemode.invalid", {s}); }
            return int64_t(g);
        }
        case arg_types::time_: {
            double v = r.read_double(true);
            std::string unit = r.read_unquoted();
            double mul = unit.empty() || unit == "t" ? 1 : unit == "s" ? 20 : unit == "d" ? 24000 : 0;
            if (mul == 0) throw r.error("argument.time.invalid_unit");
            int64_t ticks = std::llround(v * mul);
            if (ticks < int64_t(n.min)) { r.c = st; throw r.error("argument.time.tick_count_too_low", {fi(n.min), std::to_string(ticks)}); }
            return ticks;
        }
        case arg_types::team_color: {
            std::string s = r.read_unquoted();
            for (int i = 0; i < 16; i++) if (s == TEAM_COLORS[i]) return int64_t(i);
            if (s == "reset") return int64_t(-1);
            r.c = st;
            throw r.error("argument.color.invalid", {s});
        }
        case arg_types::hex_color: {
            std::string s = r.read_unquoted();
            if (s.size() == 6 && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) return int64_t(std::stoi(s, nullptr, 16));
            r.c = st;
            throw r.error("argument.hexcolor.invalid", {s});
        }
        case arg_types::entity_anchor: {
            std::string s = r.read_unquoted();
            if (s == "feet" || s == "eyes") return s;
            r.c = st;
            throw r.error("argument.anchor.invalid", {s});
        }
        case arg_types::swizzle: {
            std::string s = r.read_unquoted();
            std::set<char> seen;
            for (char c : s)
                if ((c != 'x' && c != 'y' && c != 'z') || !seen.insert(c).second) { r.c = st; throw r.error("arguments.swizzle.invalid"); }
            if (s.empty()) throw r.error("arguments.swizzle.invalid");
            return s;
        }
        case arg_types::operation: {
            std::string s;
            while (r.can() && r.peek() != ' ') s += r.next();
            static const char* ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "<", ">", "><"};
            for (auto o : ops) if (s == o) return s;
            r.c = st;
            throw r.error("arguments.operation.invalid");
        }
        case arg_types::scoreboard_slot: {
            std::string s = r.read_unquoted();
            static const char* base[] = {"list", "sidebar", "below_name"};
            for (int i = 0; i < 3; i++) if (s == base[i]) return int64_t(i);
            for (int i = 0; i < 16; i++) if (s == std::string("sidebar.team.") + TEAM_COLORS[i]) return int64_t(3 + i);
            r.c = st;
            throw r.error("argument.scoreboardDisplaySlot.invalid", {s});
        }
        case arg_types::objective: case arg_types::team: return r.read_unquoted();
        case arg_types::objective_criteria: {
            std::string s;
            while (r.can() && r.peek() != ' ') s += r.next();
            return s;
        }
        case arg_types::dimension: {
            std::string id = read_id(r);
            if (id != "minecraft:overworld" && id != "minecraft:the_nether" && id != "minecraft:the_end") {
                r.c = st;
                throw r.error("argument.dimension.invalid", {id});
            }
            return id;
        }
        case arg_types::uuid: {
            std::string s;
            while (r.can() && (isxdigit((unsigned char)r.peek()) || r.peek() == '-')) s += r.next();
            if (std::count(s.begin(), s.end(), '-') != 4) { r.c = st; throw r.error("argument.uuid.invalid"); }
            return s;
        }
        case arg_types::resource: case arg_types::resource_key: case arg_types::resource_or_tag:
        case arg_types::resource_or_tag_key: case arg_types::resource_selector: {
            bool tag = (n.parser == arg_types::resource_or_tag || n.parser == arg_types::resource_or_tag_key) && r.can() && r.peek() == '#';
            if (tag) r.skip();
            if (n.parser == arg_types::resource_selector) return read_token(r);
            std::string id = read_id(r);
            if (n.parser == arg_types::resource && !tag) {
                if (auto* reg = registry(n.registry); reg && registry_index(*reg, id) < 0) {
                    r.c = st;
                    if (std::string(n.registry) == "minecraft:entity_type") throw r.error("argument.resource.not_found", {id, n.registry});
                    throw r.error("argument.resource.not_found", {id, n.registry});
                }
            }
            return (tag ? "#" : "") + id;
        }
        case arg_types::resource_location: return read_id(r);
        case arg_types::function: {
            bool tag = r.can() && r.peek() == '#';
            if (tag) r.skip();
            return (tag ? "#" : "") + read_id(r);
        }
        case arg_types::particle: {
            std::string id = read_id(r);
            if (registry_index(*registry("minecraft:particle_type"), id) < 0) { r.c = st; throw r.error("particle.notFound", {id}); }
            if (r.can() && r.peek() == '{') read_snbt(r);
            return id;
        }
        case arg_types::heightmap: {
            std::string s = r.read_unquoted();
            static const char* hm[] = {"world_surface", "motion_blocking", "motion_blocking_no_leaves", "ocean_floor"};
            for (auto h : hm) if (s == h) return s;
            r.c = st;
            throw r.error("argument.enum.invalid", {s});
        }
        case arg_types::template_mirror: case arg_types::template_rotation: case arg_types::swing_animation:
            return r.read_unquoted();
        default:  // nbt_path, item_slot(s), loot_*, providers, slot_source, dialog, feature: syntax-only
            if (!r.can()) throw r.error("argument.id.invalid");
            return read_token(r);
    }
}
