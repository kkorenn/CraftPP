#pragma once
// Minimal blocking HTTP(S) GET for Mojang's session/services APIs (online mode). https:// needs the build to
// have found OpenSSL (CRAFTPP_TLS); http:// always works (tests point the session host at a local server).
#include <string>

namespace http {
struct Response {
    int status = 0;     // 0 = transport error (see error)
    std::string body, error;
};
Response get(const std::string& url, int timeout_seconds = 10);
bool tls_available();
std::string url_encode(const std::string& s);
}  // namespace http
