#include "crypto.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace crypto {

// ---------- SHA-1 / SHA-256 ----------

static inline uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
static inline uint32_t ror(uint32_t x, int n) { return x >> n | x << (32 - n); }
static inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
static inline void put_be32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

template <class F>
static void md_update(uint8_t* buf, uint64_t& len, const uint8_t* p, size_t n, F block) {
    size_t used = size_t(len % 64);
    len += n;
    if (used) {
        size_t k = std::min(64 - used, n);
        memcpy(buf + used, p, k);
        p += k, n -= k;
        if (used + k < 64) return;
        block(buf);
    }
    for (; n >= 64; p += 64, n -= 64) block(p);
    memcpy(buf, p, n);
}

template <class H>
static void md_pad(H& h) {
    uint64_t bits = h.len * 8;
    uint8_t pad[72] = {0x80};
    size_t used = size_t(h.len % 64);
    h.update(pad, (used < 56 ? 56 : 120) - used);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
    h.update(l, 8);
}

static void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

Sha1::Sha1() : h{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0} {}
Sha1& Sha1::update(const void* p, size_t n) {
    md_update(buf, len, static_cast<const uint8_t*>(p), n, [this](const uint8_t* b) { sha1_block(h, b); });
    return *this;
}
std::array<uint8_t, 20> Sha1::final() {
    md_pad(*this);
    std::array<uint8_t, 20> out;
    for (int i = 0; i < 5; i++) put_be32(&out[size_t(4 * i)], h[i]);
    return out;
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

Sha256::Sha256() : h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}
Sha256& Sha256::update(const void* p, size_t n) {
    md_update(buf, len, static_cast<const uint8_t*>(p), n, [this](const uint8_t* b) { sha256_block(h, b); });
    return *this;
}
std::array<uint8_t, 32> Sha256::final() {
    md_pad(*this);
    std::array<uint8_t, 32> out;
    for (int i = 0; i < 8; i++) put_be32(&out[size_t(4 * i)], h[i]);
    return out;
}

std::string minecraft_digest_hex(const uint8_t* digest, size_t n) {
    Bytes d(digest, digest + n);
    bool neg = n && (d[0] & 0x80);
    if (neg) {  // two's complement magnitude
        for (auto& b : d) b = uint8_t(~b);
        for (size_t i = n; i-- > 0;)
            if (++d[i]) break;
    }
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) s += hex[b >> 4], s += hex[b & 15];
    s.erase(0, std::min(s.find_first_not_of('0'), s.size() - 1));
    if (s.empty()) s = "0";
    return neg ? "-" + s : s;
}

// ---------- AES-128 (encryption only: CFB decrypts with the forward cipher) ----------

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59,
    0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1,
    0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83,
    0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
    0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c,
    0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee,
    0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6,
    0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
    0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1,
    0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static const struct TeTable {  // Te0[x] = (2s, s, s, 3s); the other three are rotations
    uint32_t t[256];
    TeTable() {
        for (int x = 0; x < 256; x++) {
            uint32_t s = SBOX[x], s2 = (s << 1) ^ (s & 0x80 ? 0x11B : 0), s3 = s2 ^ s;
            t[x] = s2 << 24 | s << 16 | s << 8 | s3;
        }
    }
} TE;

void aes128_encrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]) {
    const uint32_t* T = TE.t;
    uint32_t s0 = be32(in) ^ rk[0], s1 = be32(in + 4) ^ rk[1], s2 = be32(in + 8) ^ rk[2], s3 = be32(in + 12) ^ rk[3];
    for (int r = 1; r < 10; r++) {
        const uint32_t* k = rk + 4 * r;
        uint32_t t0 = T[s0 >> 24] ^ ror(T[s1 >> 16 & 255], 8) ^ ror(T[s2 >> 8 & 255], 16) ^ ror(T[s3 & 255], 24) ^ k[0];
        uint32_t t1 = T[s1 >> 24] ^ ror(T[s2 >> 16 & 255], 8) ^ ror(T[s3 >> 8 & 255], 16) ^ ror(T[s0 & 255], 24) ^ k[1];
        uint32_t t2 = T[s2 >> 24] ^ ror(T[s3 >> 16 & 255], 8) ^ ror(T[s0 >> 8 & 255], 16) ^ ror(T[s1 & 255], 24) ^ k[2];
        uint32_t t3 = T[s3 >> 24] ^ ror(T[s0 >> 16 & 255], 8) ^ ror(T[s1 >> 8 & 255], 16) ^ ror(T[s2 & 255], 24) ^ k[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    auto last = [](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
        return (uint32_t(SBOX[a >> 24]) << 24 | uint32_t(SBOX[b >> 16 & 255]) << 16 | uint32_t(SBOX[c >> 8 & 255]) << 8 |
                SBOX[d & 255]) ^ k;
    };
    put_be32(out, last(s0, s1, s2, s3, rk[40]));
    put_be32(out + 4, last(s1, s2, s3, s0, rk[41]));
    put_be32(out + 8, last(s2, s3, s0, s1, rk[42]));
    put_be32(out + 12, last(s3, s0, s1, s2, rk[43]));
}

Cfb8::Cfb8(const uint8_t key[16]) {
    static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    for (int i = 0; i < 4; i++) rk[i] = be32(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if (i % 4 == 0) {
            t = t << 8 | t >> 24;
            t = uint32_t(SBOX[t >> 24]) << 24 | uint32_t(SBOX[t >> 16 & 255]) << 16 | uint32_t(SBOX[t >> 8 & 255]) << 8 | SBOX[t & 255];
            t ^= uint32_t(rcon[i / 4 - 1]) << 24;
        }
        rk[i] = rk[i - 4] ^ t;
    }
    memcpy(iv, key, 16);
}

void Cfb8::encrypt(uint8_t* p, size_t n) {
    uint8_t o[16];
    for (size_t i = 0; i < n; i++) {
        aes128_encrypt_block(rk, iv, o);
        p[i] ^= o[0];
        memmove(iv, iv + 1, 15);
        iv[15] = p[i];
    }
}

void Cfb8::decrypt(uint8_t* p, size_t n) {
    uint8_t o[16];
    for (size_t i = 0; i < n; i++) {
        aes128_encrypt_block(rk, iv, o);
        memmove(iv, iv + 1, 15);
        iv[15] = p[i];
        p[i] ^= o[0];
    }
}

// ---------- random, base64 ----------

void random_bytes(void* p, size_t n) {
    static FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(p, 1, n, f) != n) throw std::runtime_error("no /dev/urandom");
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const uint8_t* p, size_t n) {
    std::string s;
    s.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        s += B64[v >> 18], s += B64[v >> 12 & 63];
        s += i + 1 < n ? B64[v >> 6 & 63] : '=';
        s += i + 2 < n ? B64[v & 63] : '=';
    }
    return s;
}

std::optional<Bytes> base64_decode(const std::string& s) {
    Bytes out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const char* q = strchr(B64, c);
        if (!q || !c) return std::nullopt;
        acc = acc << 6 | uint32_t(q - B64);
        if ((bits += 6) >= 8) out.push_back(uint8_t(acc >> (bits -= 8)));
    }
    return out;
}

// ---------- big numbers ----------

static Limbs from_be(const uint8_t* p, size_t n, size_t k) {
    Limbs a(k, 0);
    for (size_t i = 0; i < n; i++) {
        size_t bit = (n - 1 - i) * 8;
        if (bit / 32 < k) a[bit / 32] |= uint32_t(p[i]) << (bit % 32);
        else if (p[i]) return {};  // doesn't fit
    }
    return a;
}

static Bytes to_be(const Limbs& a, size_t n) {
    Bytes b(n, 0);
    for (size_t i = 0; i < n; i++) {
        size_t bit = (n - 1 - i) * 8;
        if (bit / 32 < a.size()) b[i] = uint8_t(a[bit / 32] >> (bit % 32));
    }
    return b;
}

static size_t bit_length(const Limbs& a) {
    for (size_t i = a.size(); i-- > 0;)
        if (a[i]) return i * 32 + 32 - size_t(__builtin_clz(a[i]));
    return 0;
}

static int cmp(const Limbs& a, const Limbs& b) {
    for (size_t i = std::max(a.size(), b.size()); i-- > 0;) {
        uint32_t x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

static void sub_in(Limbs& a, const Limbs& b) {  // a -= b (a >= b)
    int64_t borrow = 0;
    for (size_t i = 0; i < a.size(); i++) {
        int64_t d = int64_t(a[i]) - (i < b.size() ? b[i] : 0) - borrow;
        borrow = d < 0;
        a[i] = uint32_t(d);
    }
}

static Limbs mul(const Limbs& a, const Limbs& b) {
    Limbs r(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); i++) {
        uint64_t c = 0;
        for (size_t j = 0; j < b.size(); j++) {
            c += uint64_t(a[i]) * b[j] + r[i + j];
            r[i + j] = uint32_t(c);
            c >>= 32;
        }
        r[i + b.size()] = uint32_t(c);
    }
    return r;
}

static uint32_t mod_small(const Limbs& a, uint32_t m) {
    uint64_t r = 0;
    for (size_t i = a.size(); i-- > 0;) r = (r << 32 | a[i]) % m;
    return uint32_t(r);
}

// Montgomery arithmetic modulo an odd n (CIOS).
struct Mont {
    Limbs n, rr, one;  // rr = R^2 mod n, one = R mod n
    size_t k;
    uint32_t n0;  // -n^-1 mod 2^32
    explicit Mont(const Limbs& mod) : n(mod), k(mod.size()) {
        while (k > 1 && !n[k - 1]) k--;
        n.resize(k);
        uint32_t inv = 1;
        for (int i = 0; i < 5; i++) inv *= 2 - n[0] * inv;
        n0 = uint32_t(0) - inv;
        Limbs x(k, 0);  // 2^(64k) mod n by doubling 1
        x[0] = 1;
        for (size_t i = 0; i < 64 * k; i++) {
            uint32_t carry = 0;
            for (size_t j = 0; j < k; j++) {
                uint32_t v = x[j];
                x[j] = v << 1 | carry;
                carry = v >> 31;
            }
            if (carry || cmp(x, n) >= 0) sub_in(x, n);
            if (i + 1 == 32 * k) one = x;
        }
        rr = x;
    }
    Limbs mul(const Limbs& a, const Limbs& b) const {
        std::vector<uint32_t> t(k + 2, 0);
        for (size_t i = 0; i < k; i++) {
            uint64_t c = 0;
            for (size_t j = 0; j < k; j++) {
                c += uint64_t(a[j]) * b[i] + t[j];
                t[j] = uint32_t(c);
                c >>= 32;
            }
            c += t[k];
            t[k] = uint32_t(c);
            t[k + 1] = uint32_t(c >> 32);
            uint32_t m = t[0] * n0;
            c = (uint64_t(m) * n[0] + t[0]) >> 32;
            for (size_t j = 1; j < k; j++) {
                c += uint64_t(m) * n[j] + t[j];
                t[j - 1] = uint32_t(c);
                c >>= 32;
            }
            c += t[k];
            t[k - 1] = uint32_t(c);
            t[k] = t[k + 1] + uint32_t(c >> 32);
        }
        Limbs r(t.begin(), t.begin() + std::ptrdiff_t(k));
        if (t[k] || cmp(r, n) >= 0) sub_in(r, n);
        return r;
    }
    Limbs to(Limbs a) const { a.resize(k, 0); return mul(a, rr); }
    Limbs from(const Limbs& a) const { Limbs u(k, 0); u[0] = 1; return mul(a, u); }
    Limbs pow_m(const Limbs& base_m, const Limbs& e) const {  // Montgomery domain in and out
        Limbs x = one;
        for (size_t i = bit_length(e); i-- > 0;) {
            x = mul(x, x);
            if (e[i / 32] >> (i % 32) & 1) x = mul(x, base_m);
        }
        return x;
    }
    Limbs pow(const Limbs& base, const Limbs& e) const { return from(pow_m(to(base), e)); }
};

static Limbs random_limbs(size_t k) {
    Limbs a(k);
    random_bytes(a.data(), k * 4);
    return a;
}

static bool probably_prime(const Limbs& p, int rounds) {
    Mont M(p);
    size_t k = M.k;
    Limbs pm1 = M.n;
    pm1[0] -= 1;  // p is odd
    size_t s = 0;
    while (!(pm1[s / 32] >> (s % 32) & 1)) s++;
    Limbs d(k, 0);
    for (size_t i = 0; i < k * 32 - s; i++)
        if (pm1[(i + s) / 32] >> ((i + s) % 32) & 1) d[i / 32] |= 1u << (i % 32);
    Limbs mone = M.to(pm1);
    for (int r = 0; r < rounds; r++) {
        Limbs a = random_limbs(k);
        a[k - 1] %= M.n[k - 1];  // a < p
        a[0] |= 2;
        Limbs x = M.pow_m(M.to(a), d);
        if (x == M.one || x == mone) continue;
        bool witness = true;
        for (size_t i = 1; i < s && witness; i++) {
            x = M.mul(x, x);
            if (x == mone) witness = false;
            else if (x == M.one) return false;
        }
        if (witness) return false;
    }
    return true;
}

static Limbs random_prime(size_t bits, uint32_t e) {
    static const std::vector<uint32_t> small = [] {
        std::vector<uint32_t> v;
        for (uint32_t i = 3; i < 2000; i += 2) {
            bool prime = true;
            for (uint32_t j = 3; j * j <= i && prime; j += 2) prime = i % j;
            if (prime) v.push_back(i);
        }
        return v;
    }();
    size_t k = bits / 32;
    for (;;) {
        Limbs p = random_limbs(k);
        p[k - 1] |= 0xC0000000u;  // two top bits: p*q has exactly 2*bits bits
        p[0] |= 1;
        bool ok = mod_small(p, e) != 1;  // gcd(e, p - 1) = 1
        for (size_t i = 0; i < small.size() && ok; i++) ok = mod_small(p, small[i]) != 0;
        if (ok && probably_prime(p, 8)) return p;
    }
}

size_t RsaPublic::bytes() const { return (bit_length(n) + 7) / 8; }

RsaKey rsa_generate(int bits, uint32_t e) {
    size_t half = size_t(bits) / 2;
    Limbs p = random_prime(half, e), q;
    do q = random_prime(half, e);
    while (q == p);
    RsaKey k;
    k.pub.n = mul(p, q);
    k.pub.e = {e};
    p[0] -= 1, q[0] -= 1;
    Limbs phi = mul(p, q);
    // d = (1 + t * phi) / e with t * phi = -1 (mod e); e is prime, so t = -(phi mod e)^(e-2) mod e.
    uint64_t pm = mod_small(phi, e), inv = 1, b = pm;
    for (uint32_t x = e - 2; x; x >>= 1, b = b * b % e)
        if (x & 1) inv = inv * b % e;
    uint64_t t = (e - inv) % e, c = 1;  // c: the +1
    Limbs num(phi.size() + 1, 0);
    for (size_t i = 0; i < phi.size(); i++) {
        c += phi[i] * t;
        num[i] = uint32_t(c);
        c >>= 32;
    }
    num[phi.size()] = uint32_t(c);
    uint64_t r = 0;
    for (size_t i = num.size(); i-- > 0;) {
        r = r << 32 | num[i];
        num[i] = uint32_t(r / e);
        r %= e;
    }
    num.resize(k.pub.n.size());
    k.d = num;
    k.der = spki_encode(k.pub);
    return k;
}

// ---------- DER ----------

static void der_len(Bytes& o, size_t n) {
    if (n < 128) o.push_back(uint8_t(n));
    else if (n < 256) o.push_back(0x81), o.push_back(uint8_t(n));
    else o.push_back(0x82), o.push_back(uint8_t(n >> 8)), o.push_back(uint8_t(n));
}
static Bytes der(uint8_t tag, const Bytes& content) {
    Bytes o{tag};
    der_len(o, content.size());
    o.insert(o.end(), content.begin(), content.end());
    return o;
}
static Bytes der_int(const Limbs& a) {
    Bytes b = to_be(a, (bit_length(a) + 7) / 8 + 1);  // leading 0 keeps it positive
    size_t i = 0;
    while (i + 1 < b.size() && b[i] == 0 && !(b[i + 1] & 0x80)) i++;
    return der(0x02, Bytes(b.begin() + std::ptrdiff_t(i), b.end()));
}
static const uint8_t RSA_ALG[] = {0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00};

Bytes spki_encode(const RsaPublic& k) {
    Bytes rsa = der_int(k.n), e = der_int(k.e);
    rsa.insert(rsa.end(), e.begin(), e.end());
    Bytes bits{0};
    Bytes seq = der(0x30, rsa);
    bits.insert(bits.end(), seq.begin(), seq.end());
    Bytes body(RSA_ALG, RSA_ALG + sizeof RSA_ALG), bs = der(0x03, bits);
    body.insert(body.end(), bs.begin(), bs.end());
    return der(0x30, body);
}

namespace {
struct Der {
    const uint8_t* p;
    size_t n, i = 0;
    bool next(uint8_t tag, const uint8_t*& c, size_t& len) {
        if (i + 2 > n || p[i] != tag) return false;
        size_t l = p[i + 1];
        i += 2;
        if (l & 0x80) {
            int m = l & 0x7F;
            if (m < 1 || m > 3 || i + size_t(m) > n) return false;
            l = 0;
            while (m--) l = l << 8 | p[i++];
        }
        if (i + l > n) return false;
        c = p + i, len = l, i += l;
        return true;
    }
};
}  // namespace

std::optional<RsaPublic> spki_decode(const uint8_t* p, size_t n) {
    const uint8_t *c, *alg, *bits, *rsa, *np, *ep;
    size_t cl, al, bl, rl, nl, el;
    Der top{p, n};
    if (!top.next(0x30, c, cl) || top.i != n) return std::nullopt;
    Der in{c, cl};
    if (!in.next(0x30, alg, al) || al < 11 || memcmp(alg, RSA_ALG + 2, 11) != 0) return std::nullopt;
    if (!in.next(0x03, bits, bl) || bl < 1 || bits[0] != 0) return std::nullopt;
    Der b{bits + 1, bl - 1};
    if (!b.next(0x30, rsa, rl)) return std::nullopt;
    Der r{rsa, rl};
    if (!r.next(0x02, np, nl) || !r.next(0x02, ep, el) || nl > 1025 || el > 1025) return std::nullopt;
    RsaPublic k;
    k.n = from_be(np, nl, (nl + 3) / 4);
    k.e = from_be(ep, el, (el + 3) / 4);
    for (Limbs* l : {&k.n, &k.e})
        while (l->size() > 1 && !l->back()) l->pop_back();
    if (k.n.empty() || !(k.n[0] & 1) || bit_length(k.n) < 512) return std::nullopt;
    return k;
}

// ---------- RSA PKCS#1 v1.5 ----------

static Limbs modexp(const Limbs& n, const Bytes& in, const Limbs& e) {
    Limbs x = from_be(in.data(), in.size(), n.size());
    if (x.empty() || cmp(x, n) >= 0) return {};
    return Mont(n).pow(x, e);
}

std::optional<Bytes> rsa_decrypt_pkcs1(const RsaKey& k, const Bytes& c) {
    size_t kb = k.pub.bytes();
    if (c.size() > kb) return std::nullopt;
    Limbs m = modexp(k.pub.n, c, k.d);
    if (m.empty()) return std::nullopt;
    Bytes em = to_be(m, kb);
    if (em[0] != 0 || em[1] != 2) return std::nullopt;
    size_t i = 2;
    while (i < em.size() && em[i]) i++;
    if (i >= em.size() || i < 10) return std::nullopt;
    return Bytes(em.begin() + std::ptrdiff_t(i + 1), em.end());
}

Bytes rsa_encrypt_pkcs1(const RsaPublic& k, const Bytes& m) {
    size_t kb = k.bytes();
    if (m.size() + 11 > kb) throw std::runtime_error("RSA message too long");
    Bytes em(kb, 0);
    em[1] = 2;
    for (size_t i = 2; i < kb - m.size() - 1; i++)
        do random_bytes(&em[i], 1);
        while (!em[i]);
    std::copy(m.begin(), m.end(), em.end() - std::ptrdiff_t(m.size()));
    return to_be(modexp(k.n, em, k.e), kb);
}

static Bytes pkcs1_sig_block(size_t kb, Hash h, const uint8_t* digest) {
    static const uint8_t SHA1_INFO[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
    static const uint8_t SHA256_INFO[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                          0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
    Bytes t = h == Hash::Sha1 ? Bytes(SHA1_INFO, SHA1_INFO + sizeof SHA1_INFO) : Bytes(SHA256_INFO, SHA256_INFO + sizeof SHA256_INFO);
    t.insert(t.end(), digest, digest + (h == Hash::Sha1 ? 20 : 32));
    if (t.size() + 11 > kb) return {};
    Bytes em(kb, 0xFF);
    em[0] = 0, em[1] = 1, em[kb - t.size() - 1] = 0;
    std::copy(t.begin(), t.end(), em.end() - std::ptrdiff_t(t.size()));
    return em;
}

bool rsa_verify(const RsaPublic& k, Hash h, const uint8_t* digest, const Bytes& sig) {
    size_t kb = k.bytes();
    if (sig.size() != kb) return false;
    Bytes want = pkcs1_sig_block(kb, h, digest);
    Limbs m = modexp(k.n, sig, k.e);
    return !want.empty() && !m.empty() && to_be(m, kb) == want;
}

Bytes rsa_sign(const RsaKey& k, Hash h, const uint8_t* digest) {
    size_t kb = k.pub.bytes();
    return to_be(modexp(k.pub.n, pkcs1_sig_block(kb, h, digest), k.d), kb);
}

}  // namespace crypto
