#pragma once
// Socket I/O: length-prefixed packets, optional zlib compression (vanilla network-compression-threshold),
// no encryption (offline mode).
#include "protocol.hpp"

#include <sys/socket.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>

inline int compression_threshold = -1;            // network-compression-threshold; -1 = off (set at startup)
inline thread_local bool conn_compressed = false;  // network thread: our connection has sent set_compression

inline void put_varint(std::vector<uint8_t>& o, uint32_t v) {
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        o.push_back(v ? b | 0x80 : b);
    } while (v);
}

// Appends one frame: [len][packet], or compressed: [len][0][packet] below the threshold,
// [len][uncompressed len][zlib(packet)] at or above it. zlib level 1: docs/performance.md.
inline void frame_packet(std::vector<uint8_t>& out, const uint8_t* p, size_t n, bool compressed) {
    if (!compressed) {
        put_varint(out, uint32_t(n));
    } else if (int(n) < compression_threshold) {
        put_varint(out, uint32_t(n + 1));
        out.push_back(0);
    } else {
        thread_local std::vector<uint8_t> z;
        z.resize(compressBound(uLong(n)));
        uLongf zn = uLongf(z.size());
        compress2(z.data(), &zn, p, uLong(n), 1);
        std::vector<uint8_t> dl;
        put_varint(dl, uint32_t(n));
        put_varint(out, uint32_t(dl.size() + zn));
        out.insert(out.end(), dl.begin(), dl.end());
        out.insert(out.end(), z.begin(), z.begin() + std::ptrdiff_t(zn));
        return;
    }
    out.insert(out.end(), p, p + n);
}

// Re-frames a blob of uncompressed frames (data/config.bin, the commands packet) for a compressed connection.
inline std::vector<uint8_t> reframe(const std::vector<uint8_t>& blob, bool compressed) {
    if (!compressed) return blob;
    std::vector<uint8_t> out;
    for (size_t i = 0; i < blob.size();) {
        uint32_t len = 0;
        for (int s = 0; i < blob.size(); s += 7) {
            uint8_t b = blob[i++];
            len |= uint32_t(b & 0x7F) << s;
            if (!(b & 0x80)) break;
        }
        if (i + len > blob.size()) throw std::runtime_error("reframe: truncated blob");
        frame_packet(out, blob.data() + i, len, true);
        i += len;
    }
    return out;
}

// Reads go through a per-thread buffer (one network thread per connection): a recv() takes every packet the
// client has sent so far instead of 2+ syscalls per packet (length bytes, then body).
struct RecvBuf { uint8_t b[8192]; size_t i = 0, n = 0; };
inline thread_local RecvBuf conn_rbuf;

inline bool read_exact(int fd, uint8_t* buf, size_t n) {
    RecvBuf& rb = conn_rbuf;
    while (n) {
        if (rb.i == rb.n) {
            if (n >= sizeof rb.b) {  // big body: straight into the destination
                ssize_t r = recv(fd, buf, n, 0);
                if (r <= 0) return false;
                buf += r, n -= size_t(r);
                continue;
            }
            ssize_t r = recv(fd, rb.b, sizeof rb.b, 0);
            if (r <= 0) return false;
            rb.i = 0, rb.n = size_t(r);
        }
        size_t k = std::min(n, rb.n - rb.i);
        memcpy(buf, rb.b + rb.i, k);
        rb.i += k, buf += k, n -= k;
    }
    return true;
}

// Reads one packet (decompressed when this connection uses compression).
inline bool read_packet(int fd, std::vector<uint8_t>& out) {
    uint32_t len = 0;
    for (int s = 0;; s += 7) {
        uint8_t b;
        if (s >= 21 || !read_exact(fd, &b, 1)) return false;  // 3 bytes max = 2 MiB
        len |= uint32_t(b & 0x7F) << s;
        if (!(b & 0x80)) break;
    }
    out.resize(len);
    if (!read_exact(fd, out.data(), len)) return false;
    if (!conn_compressed) return true;
    Reader r(out);
    uint32_t data_len = uint32_t(r.varint());
    if (data_len == 0) {
        out.erase(out.begin(), out.begin() + std::ptrdiff_t(r.i));
        return true;
    }
    if (data_len > (8u << 20)) throw std::runtime_error("packet too big");  // vanilla MAXIMUM_UNCOMPRESSED_LENGTH
    std::vector<uint8_t> raw(data_len);
    uLongf n = data_len;
    if (uncompress(raw.data(), &n, out.data() + r.i, uLong(out.size() - r.i)) != Z_OK || n != data_len)
        throw std::runtime_error("bad compressed packet");
    out.swap(raw);
    return true;
}

inline void send_raw(int fd, const uint8_t* p, size_t n) {
    while (n) {
        ssize_t r = send(fd, p, n, 0);
        if (r <= 0) throw std::runtime_error("send failed");
        p += r;
        n -= size_t(r);
    }
}

inline void send_packet(int fd, const Writer& body) {
    std::vector<uint8_t> frame;
    frame_packet(frame, body.b.data(), body.b.size(), conn_compressed);
    send_raw(fd, frame.data(), frame.size());
}

// Login: enables compression on this connection (before login_finished, like vanilla) when configured.
inline void start_compression(int fd, int set_compression_id) {
    if (compression_threshold < 0) return;
    send_packet(fd, Writer().varint(set_compression_id).varint(compression_threshold));
    conn_compressed = true;
}
