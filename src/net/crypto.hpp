#pragma once
// Vendored crypto for the Minecraft protocol (no library dependency): SHA-1/SHA-256, AES-128 in CFB8 (the
// connection cipher, vanilla Crypt.getCipher "AES/CFB8/NoPadding" with IV = key), RSA (1024-bit login key pair,
// PKCS#1 v1.5 decrypt, SHA1/SHA256withRSA verify for profile keys and chat), X.509 SubjectPublicKeyInfo DER and
// base64. Constant-time is not a goal beyond what vanilla's threat model needs (keys live for one server run).
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace crypto {

using Bytes = std::vector<uint8_t>;

struct Sha1 {
    uint32_t h[5];
    uint8_t buf[64];
    uint64_t len = 0;
    Sha1();
    Sha1& update(const void* p, size_t n);
    Sha1& update(const std::string& s) { return update(s.data(), s.size()); }
    Sha1& update(const Bytes& b) { return update(b.data(), b.size()); }
    std::array<uint8_t, 20> final();
};

struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len = 0;
    Sha256();
    Sha256& update(const void* p, size_t n);
    Sha256& update(const std::string& s) { return update(s.data(), s.size()); }
    Sha256& update(const Bytes& b) { return update(b.data(), b.size()); }
    std::array<uint8_t, 32> final();
};

// Java's new BigInteger(digest).toString(16): signed two's complement, no leading zeros (the server hash).
std::string minecraft_digest_hex(const uint8_t* digest, size_t n);

// AES-128/CFB8 stream (one direction). Vanilla uses the shared secret as both key and IV.
struct Cfb8 {
    uint32_t rk[44];
    uint8_t iv[16];
    explicit Cfb8(const uint8_t key[16]);
    void encrypt(uint8_t* p, size_t n);
    void decrypt(uint8_t* p, size_t n);
};
void aes128_encrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]);

void random_bytes(void* p, size_t n);  // /dev/urandom
std::string base64(const uint8_t* p, size_t n);
inline std::string base64(const Bytes& b) { return base64(b.data(), b.size()); }
std::optional<Bytes> base64_decode(const std::string& s);  // skips whitespace (MIME)

// RSA. Numbers are little-endian 32-bit limbs.
using Limbs = std::vector<uint32_t>;
struct RsaPublic {
    Limbs n, e;
    size_t bytes() const;  // modulus size in bytes
};
struct RsaKey {
    RsaPublic pub;
    Limbs d;
    Bytes der;  // X.509 SubjectPublicKeyInfo of pub (what vanilla sends in ClientboundHelloPacket)
};
RsaKey rsa_generate(int bits = 1024, uint32_t e = 65537);
Bytes spki_encode(const RsaPublic& k);
std::optional<RsaPublic> spki_decode(const uint8_t* p, size_t n);
inline std::optional<RsaPublic> spki_decode(const Bytes& b) { return spki_decode(b.data(), b.size()); }
std::optional<Bytes> rsa_decrypt_pkcs1(const RsaKey& k, const Bytes& c);  // Cipher "RSA" (PKCS#1 v1.5)
Bytes rsa_encrypt_pkcs1(const RsaPublic& k, const Bytes& m);              // tests / client side
enum class Hash { Sha1, Sha256 };
bool rsa_verify(const RsaPublic& k, Hash h, const uint8_t* digest, const Bytes& sig);  // PKCS#1 v1.5
Bytes rsa_sign(const RsaKey& k, Hash h, const uint8_t* digest);                       // tests / client side

}  // namespace crypto
