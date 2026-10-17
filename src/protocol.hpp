#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Minecraft wire format helpers: VarInt, strings, big-endian ints.
struct Reader {
    const uint8_t* p;
    size_t n, i = 0;
    Reader(const std::vector<uint8_t>& v) : p(v.data()), n(v.size()) {}
    Reader(const uint8_t* d, size_t len) : p(d), n(len) {}

    uint8_t u8() {
        if (i >= n) throw std::runtime_error("read past end");
        return p[i++];
    }
    int32_t varint() {
        uint32_t r = 0;
        for (int s = 0; s < 35; s += 7) {
            uint8_t b = u8();
            r |= uint32_t(b & 0x7F) << s;
            if (!(b & 0x80)) return int32_t(r);
        }
        throw std::runtime_error("varint too long");
    }
    uint16_t u16() { uint16_t a = u8(); return uint16_t(a << 8 | u8()); }
    int64_t i64() {
        uint64_t r = 0;
        for (int k = 0; k < 8; k++) r = r << 8 | u8();
        return int64_t(r);
    }
    int32_t i32() {
        uint32_t r = 0;
        for (int k = 0; k < 4; k++) r = r << 8 | u8();
        return int32_t(r);
    }
    float f32() { uint32_t u = uint32_t(i32()); float f; memcpy(&f, &u, 4); return f; }
    double f64() { uint64_t u = uint64_t(i64()); double d; memcpy(&d, &u, 8); return d; }
    std::string str(size_t max = 32767 * 4) {
        int32_t len = varint();
        if (len < 0 || size_t(len) > max || i + len > n) throw std::runtime_error("bad string");
        std::string s(reinterpret_cast<const char*>(p + i), len);
        i += len;
        return s;
    }
    std::vector<uint8_t> bytes(size_t len) {
        if (i + len > n) throw std::runtime_error("read past end");
        std::vector<uint8_t> out(p + i, p + i + len);
        i += len;
        return out;
    }
};

struct Writer {
    std::vector<uint8_t> b;
    Writer& u8(uint8_t v) { b.push_back(v); return *this; }
    Writer& varint(int32_t v) {
        uint32_t u = uint32_t(v);
        do {
            uint8_t x = u & 0x7F;
            u >>= 7;
            b.push_back(u ? x | 0x80 : x);
        } while (u);
        return *this;
    }
    Writer& boolean(bool v) { return u8(v ? 1 : 0); }
    Writer& i16(int16_t v) { u8(uint8_t(uint16_t(v) >> 8)); return u8(uint8_t(v)); }
    Writer& i32(int32_t v) {
        for (int k = 3; k >= 0; k--) b.push_back(uint8_t(uint32_t(v) >> (k * 8)));
        return *this;
    }
    Writer& i64(int64_t v) {
        for (int k = 7; k >= 0; k--) b.push_back(uint8_t(uint64_t(v) >> (k * 8)));
        return *this;
    }
    Writer& f32(float v) { uint32_t u; memcpy(&u, &v, 4); return i32(int32_t(u)); }
    Writer& f64(double v) { uint64_t u; memcpy(&u, &v, 8); return i64(int64_t(u)); }
    Writer& bytes(const void* p, size_t n) {
        auto c = static_cast<const uint8_t*>(p);
        b.insert(b.end(), c, c + n);
        return *this;
    }
    Writer& bytes(const std::vector<uint8_t>& v) { return bytes(v.data(), v.size()); }
    Writer& byte_array(const std::vector<uint8_t>& v) { varint(int32_t(v.size())); return bytes(v); }
    Writer& str(const std::string& s) {
        varint(int32_t(s.size()));
        b.insert(b.end(), s.begin(), s.end());
        return *this;
    }
};

// Text component as network NBT: {text:"...", color:"..."}.
// ponytail: plain UTF-8 used for NBT strings; real modified-UTF-8 differs only for NUL and emoji (4-byte chars).
inline Writer& text_component(Writer& w, const std::string& text, const std::string& color = "") {
    auto nbt_str = [&](const std::string& s) { w.u8(uint8_t(s.size() >> 8)).u8(uint8_t(s.size())); w.bytes(s.data(), s.size()); };
    w.u8(0x0A);  // compound, unnamed root
    w.u8(0x08); nbt_str("text"); nbt_str(text);
    if (!color.empty()) { w.u8(0x08); nbt_str("color"); nbt_str(color); }
    return w.u8(0x00);
}

// StringUtil.isAllowedChatCharacter over a whole message: no control chars, DEL or section sign.
inline bool chat_allowed(const std::string& s) {
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c == 127 || (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xA7)) return false;
    }
    return true;
}
// StringUtil.isValidPlayerName: at most 16 chars, all printable ASCII without spaces.
inline bool valid_player_name(const std::string& s) {
    if (s.size() > 16) return false;
    for (unsigned char c : s)
        if (c <= 32 || c >= 127) return false;
    return true;
}

// JSON string escaping for text components / status JSON.
inline std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            default:
                if (c < 0x20) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", c); o += buf; }
                else o += char(c);
        }
    }
    return o;
}
