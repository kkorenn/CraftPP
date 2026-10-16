#pragma once
// Socket I/O: length-prefixed packets (no compression/encryption yet).
#include "protocol.hpp"

#include <sys/socket.h>

#include <algorithm>

inline bool read_exact(int fd, uint8_t* buf, size_t n) {
    while (n) {
        ssize_t r = recv(fd, buf, n, 0);
        if (r <= 0) return false;
        buf += r;
        n -= size_t(r);
    }
    return true;
}

// Reads one length-prefixed packet (no compression/encryption yet).
inline bool read_packet(int fd, std::vector<uint8_t>& out) {
    uint32_t len = 0;
    for (int s = 0;; s += 7) {
        uint8_t b;
        if (s >= 21 || !read_exact(fd, &b, 1)) return false;  // 3 bytes max = 2 MiB
        len |= uint32_t(b & 0x7F) << s;
        if (!(b & 0x80)) break;
    }
    // Grow with the data actually received: a bare 2 MiB length header must not cost 2 MiB of memory.
    out.clear();
    for (size_t got = 0; got < len;) {
        size_t step = std::min<size_t>(len - got, 64u << 10);
        out.resize(got + step);
        if (!read_exact(fd, out.data() + got, step)) return false;
        got += step;
    }
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
    Writer frame;
    frame.varint(int32_t(body.b.size()));
    frame.bytes(body.b);
    send_raw(fd, frame.b.data(), frame.b.size());
}
