#include "http.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <mutex>

#ifdef CRAFTPP_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace http {

bool tls_available() {
#ifdef CRAFTPP_TLS
    return true;
#else
    return false;
#endif
}

std::string url_encode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            o += b;
        }
    }
    return o;
}

namespace {
struct Fd {
    int fd = -1;
    ~Fd() { if (fd >= 0) close(fd); }
};

int connect_to(const std::string& host, const std::string& port, int timeout, std::string& err) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) {
        err = std::string("resolve ") + host + ": " + gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{timeout, 0};  // also bounds connect() on Linux
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
            err = std::string("connect ") + host + ": " + strerror(errno);
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    return fd;
}

bool parse_response(const std::string& raw, Response& r) {
    size_t eol = raw.find("\r\n"), end = raw.find("\r\n\r\n");
    if (eol == std::string::npos || end == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) return false;
    size_t sp = raw.find(' ');
    r.status = atoi(raw.c_str() + sp + 1);
    std::string headers = raw.substr(0, end);
    for (auto& c : headers) c = char(tolower(c));
    std::string body = raw.substr(end + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string out;
        for (size_t i = 0;;) {
            size_t le = body.find("\r\n", i);
            if (le == std::string::npos) return false;
            size_t n = strtoul(body.c_str() + i, nullptr, 16);
            if (n == 0) break;
            if (le + 2 + n > body.size()) return false;
            out.append(body, le + 2, n);
            i = le + 2 + n + 2;
        }
        body = std::move(out);
    }
    r.body = std::move(body);
    return true;
}

#ifdef CRAFTPP_TLS
SSL_CTX* tls_ctx() {
    static std::once_flag once;
    static SSL_CTX* ctx = nullptr;
    std::call_once(once, [] {
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) return;
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    });
    return ctx;
}
#endif
}  // namespace

Response get(const std::string& url, int timeout) {
    Response r;
    bool tls = url.compare(0, 8, "https://") == 0;
    if (!tls && url.compare(0, 7, "http://") != 0) return r.error = "unsupported URL " + url, r;
    size_t hs = tls ? 8 : 7, ps = url.find('/', hs);
    std::string hostport = url.substr(hs, ps == std::string::npos ? std::string::npos : ps - hs);
    std::string path = ps == std::string::npos ? "/" : url.substr(ps);
    std::string host = hostport, port = tls ? "443" : "80";
    if (size_t c = hostport.rfind(':'); c != std::string::npos) host = hostport.substr(0, c), port = hostport.substr(c + 1);
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + hostport +
                      "\r\nUser-Agent: CraftPP\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    Fd s;
    s.fd = connect_to(host, port, timeout, r.error);
    if (s.fd < 0) return r;
    std::string raw;
    char buf[4096];
    constexpr size_t MAX = 1 << 20;
    if (!tls) {
        for (size_t off = 0; off < req.size();) {
            ssize_t n = send(s.fd, req.data() + off, req.size() - off, 0);
            if (n <= 0) return r.error = "send failed", r;
            off += size_t(n);
        }
        for (ssize_t n; raw.size() < MAX && (n = recv(s.fd, buf, sizeof buf, 0)) > 0;) raw.append(buf, size_t(n));
    } else {
#ifdef CRAFTPP_TLS
        SSL_CTX* ctx = tls_ctx();
        if (!ctx) return r.error = "TLS init failed", r;
        std::unique_ptr<SSL, void (*)(SSL*)> ssl(SSL_new(ctx), SSL_free);
        SSL_set_fd(ssl.get(), s.fd);
        SSL_set_tlsext_host_name(ssl.get(), host.c_str());
        SSL_set1_host(ssl.get(), host.c_str());
        if (SSL_connect(ssl.get()) != 1) {
            char e[256];
            ERR_error_string_n(ERR_get_error(), e, sizeof e);
            return r.error = std::string("TLS handshake with ") + host + ": " + e, r;
        }
        if (SSL_write(ssl.get(), req.data(), int(req.size())) != int(req.size())) return r.error = "TLS write failed", r;
        for (int n; raw.size() < MAX && (n = SSL_read(ssl.get(), buf, sizeof buf)) > 0;) raw.append(buf, size_t(n));
#else
        return r.error = "https needs a build with OpenSSL (cmake -DCRAFTPP_TLS=ON)", r;
#endif
    }
    if (!parse_response(raw, r)) r.status = 0, r.error = "bad HTTP response from " + host;
    return r;
}

}  // namespace http
