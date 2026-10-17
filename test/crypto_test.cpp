// Known-answer tests for src/net/crypto (SHA-1/256, AES-128, CFB8, Minecraft server hash) + RSA round trips.
#include "net/crypto.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace crypto;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; i++) snprintf(b, 3, "%02x", p[i]), s += b;
    return s;
}
static Bytes unhex(const char* s) {
    Bytes b;
    for (; s[0] && s[1]; s += 2) b.push_back(uint8_t(strtol(std::string(s, 2).c_str(), nullptr, 16)));
    return b;
}
static std::string mc_hash(const char* name) {
    auto d = Sha1().update(std::string(name)).final();
    return minecraft_digest_hex(d.data(), d.size());
}

int main() {
    const char* abc56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    auto s1 = Sha1().update(std::string("abc")).final();
    CHECK(hex(s1.data(), 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    s1 = Sha1().update(std::string(abc56)).final();
    CHECK(hex(s1.data(), 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    auto s2 = Sha256().update(std::string("abc")).final();
    CHECK(hex(s2.data(), 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Sha256 split;  // streaming across block boundaries
    for (const char* p = abc56; *p; p++) split.update(p, 1);
    s2 = split.final();
    CHECK(hex(s2.data(), 32) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // wiki.vg "Protocol Encryption" server hash examples (Java BigInteger.toString(16) of the SHA-1).
    CHECK(mc_hash("Notch") == "4ed1f46bbe04bc756bcb17c0c7ce3e4632f06a48");
    CHECK(mc_hash("jeb_") == "-7c9d5b0044c130109a5d7b5fb5c317c02b4e28c1");
    CHECK(mc_hash("simon") == "88e16a1019277b15d58faf0541e11910eb756f6");

    // FIPS-197 C.1
    Bytes key = unhex("000102030405060708090a0b0c0d0e0f"), pt = unhex("00112233445566778899aabbccddeeff");
    Cfb8 aes(key.data());
    uint8_t out[16];
    aes128_encrypt_block(aes.rk, pt.data(), out);
    CHECK(hex(out, 16) == "69c4e0d86a7b0430d8cdb78070b4c55a");

    // SP 800-38A F.3.7 / F.3.8 CFB8-AES128
    Bytes k2 = unhex("2b7e151628aed2a6abf7158809cf4f3c"), iv = unhex("000102030405060708090a0b0c0d0e0f");
    Bytes msg = unhex("6bc1bee22e409f96e93d7e117393172aae2d");
    Cfb8 enc(k2.data()), dec(k2.data());
    memcpy(enc.iv, iv.data(), 16), memcpy(dec.iv, iv.data(), 16);
    Bytes c = msg;
    enc.encrypt(c.data(), 10), enc.encrypt(c.data() + 10, c.size() - 10);  // streaming
    CHECK(hex(c.data(), c.size()) == "3b79424c9c0dd436bace9e0ed4586a4f32b9");
    dec.decrypt(c.data(), c.size());
    CHECK(c == msg);

    CHECK(base64(unhex("666f6f626172")) == "Zm9vYmFy");
    CHECK(base64(unhex("666f6f62")) == "Zm9vYg==");
    CHECK(base64_decode("Zm9v\nYmE=") == unhex("666f6f6261"));

    // RSA: 1024-bit login key (vanilla Crypt.generateKeyPair), PKCS#1 encrypt/decrypt, signatures, SPKI.
    auto t0 = std::chrono::steady_clock::now();
    RsaKey rsa = rsa_generate(1024);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(rsa.pub.bytes() == 128);
    CHECK(rsa.der.size() == 162);  // what vanilla's 1024-bit getEncoded() is
    auto back = spki_decode(rsa.der);
    CHECK(back && back->n == rsa.pub.n && back->e == rsa.pub.e);
    Bytes secret(16);
    random_bytes(secret.data(), 16);
    t0 = std::chrono::steady_clock::now();
    auto plain = rsa_decrypt_pkcs1(rsa, rsa_encrypt_pkcs1(rsa.pub, secret));
    double dms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(plain && *plain == secret);
    Bytes junk(128, 0x42);
    CHECK(!rsa_decrypt_pkcs1(rsa, junk));
    auto h = Sha256().update(std::string("hello")).final();
    Bytes sig = rsa_sign(rsa, Hash::Sha256, h.data());
    CHECK(rsa_verify(rsa.pub, Hash::Sha256, h.data(), sig));
    sig[5] ^= 1;
    CHECK(!rsa_verify(rsa.pub, Hash::Sha256, h.data(), sig));
    auto h1 = Sha1().update(std::string("hello")).final();
    CHECK(rsa_verify(rsa.pub, Hash::Sha1, h1.data(), rsa_sign(rsa, Hash::Sha1, h1.data())));
    CHECK(!rsa_verify(rsa.pub, Hash::Sha256, h1.data(), rsa_sign(rsa, Hash::Sha1, h1.data())));

    // CFB8 throughput (one AES block per byte, so this is what login/play encryption costs per MiB).
    Bytes big(1 << 20);
    t0 = std::chrono::steady_clock::now();
    enc.encrypt(big.data(), big.size());
    double cms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("crypto_test: RSA-1024 keygen %.1f ms, decrypt %.2f ms, CFB8 %.1f ms/MiB\n", ms, dms, cms);
    if (failures) return 1;
    printf("crypto_test: all passed\n");
}
