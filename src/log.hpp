#pragma once
// Paper-style logging. Console: "[HH:MM:SS LEVEL]: msg" (ANSI colours only on a TTY, WARN yellow / ERROR red,
// § codes rendered like TerminalConsoleAppender). logs/latest.log: "[HH:MM:SS] [Server thread/LEVEL]: msg",
// plain text. On startup the previous latest.log is rolled to logs/YYYY-MM-DD-N.log.gz like log4j
// (plain .log when built without zlib). Thread-safe; console.hpp hooks in to keep its prompt at the bottom.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#ifdef CRAFTPP_ZLIB
#include <zlib.h>
#endif

enum class LogLevel { INFO, WARN, ERROR };

namespace logging {
inline std::mutex m;
inline FILE* file = nullptr;
inline bool ansi = false;
inline void (*clear_prompt)() = nullptr;  // console.hpp; both called with `m` held
inline void (*draw_prompt)() = nullptr;

// § code -> ANSI escape (TerminalConsoleAppender's MinecraftFormattingConverter).
inline const char* ansi_code(char c) {
    static const char* const colors[16] = {"0;30;22", "0;34;22", "0;32;22", "0;36;22", "0;31;22", "0;35;22",
                                           "0;33;22", "0;37;22", "0;30;1",  "0;34;1",  "0;32;1",  "0;36;1",
                                           "0;31;1",  "0;35;1",  "0;33;1",  "0;37;1"};
    c = char(tolower((unsigned char)c));
    if (c >= '0' && c <= '9') return colors[c - '0'];
    if (c >= 'a' && c <= 'f') return colors[c - 'a' + 10];
    switch (c) {
        case 'k': return "5";
        case 'l': return "1";
        case 'm': return "9";
        case 'n': return "4";
        case 'o': return "3";
        case 'r': return "";
    }
    return nullptr;
}

// Strips (or, with `to_ansi`, renders) § formatting, including the §x§R§R§G§G§B§B hex form.
inline std::string render(const std::string& s, bool to_ansi) {
    std::string o;
    bool used = false;
    char last = 'r';  // skip codes that change nothing (component rendering repeats them)
    for (size_t i = 0; i < s.size(); i++) {
        bool sect = (unsigned char)s[i] == 0xC2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0xA7;
        if (!sect) { o += s[i]; continue; }
        char c = s[i + 2];
        if ((c == 'x' || c == 'X') && i + 2 + 18 < s.size() + 1) {  // §x then six "§h" pairs (3 bytes each)
            std::string hex;
            for (size_t k = 0; k < 6; k++) {
                size_t at = i + 3 + k * 3;
                if (at + 2 < s.size() && (unsigned char)s[at] == 0xC2 && (unsigned char)s[at + 1] == 0xA7) hex += s[at + 2];
            }
            if (hex.size() == 6 && hex.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
                if (to_ansi) {
                    long v = strtol(hex.c_str(), nullptr, 16);
                    o += "\x1b[38;2;" + std::to_string(v >> 16) + ";" + std::to_string((v >> 8) & 255) + ";" +
                         std::to_string(v & 255) + "m";
                    used = true;
                }
                last = 'x';
                i += 2 + 18;
                continue;
            }
        }
        if (const char* a = ansi_code(c)) {
            c = char(tolower((unsigned char)c));
            if (to_ansi && c != last) { o += "\x1b["; o += a; o += 'm'; used = true; }
            last = c;
            i += 2;
            continue;
        }
        o += s[i];
    }
    if (used) o += "\x1b[m";
    return o;
}

// Minecraft colour name -> § code letter ('\0' if unknown).
inline char color_code(const std::string& name) {
    static const char* const names[16] = {"black", "dark_blue", "dark_green", "dark_aqua", "dark_red", "dark_purple",
                                          "gold", "gray", "dark_gray", "blue", "green", "aqua", "red", "light_purple",
                                          "yellow", "white"};
    for (int i = 0; i < 16; i++)
        if (name == names[i]) return "0123456789abcdef"[i];
    return '\0';
}

inline void write(LogLevel level, const std::string& msg, const char* thread) {
    static const char* const names[] = {"INFO", "WARN", "ERROR"};
    const char* name = names[int(level)];
    char ts[16];
    time_t t = time(nullptr);
    tm lt{};
    localtime_r(&t, &lt);
    strftime(ts, sizeof ts, "%H:%M:%S", &lt);
    std::string text = render(msg, false);
    std::string line;
    if (ansi) {
        const char* hl = level == LogLevel::WARN ? "\x1b[33;1m" : level == LogLevel::ERROR ? "\x1b[31;1m" : "";
        line = std::string(hl) + "[" + ts + " " + name + "]: " + render(msg, true) + (*hl ? "\x1b[m" : "") + "\n";
    } else {
        line = std::string("[") + ts + " " + name + "]: " + text + "\n";
    }
    std::lock_guard lock(m);
    if (clear_prompt) clear_prompt();
    fwrite(line.data(), 1, line.size(), stdout);
    if (draw_prompt) draw_prompt();
    fflush(stdout);
    if (file) {
        fprintf(file, "[%s] [%s/%s]: %s\n", ts, thread, name, text.c_str());
        fflush(file);
    }
}

inline std::string vformat(const char* fmt, va_list ap) {
    char small[512];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(small, sizeof small, fmt, copy);
    va_end(copy);
    if (n < int(sizeof small)) return std::string(small, size_t(std::max(n, 0)));
    std::string big(size_t(n) + 1, '\0');
    vsnprintf(big.data(), big.size(), fmt, ap);
    big.resize(size_t(n));
    return big;
}

// logs/latest.log -> logs/<date of its last write>-N.log.gz, then a fresh latest.log.
inline void open(const std::string& dir) {
    ansi = isatty(1) && !getenv("NO_COLOR") && !(getenv("TERM") && !strcmp(getenv("TERM"), "dumb"));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string latest = dir + "/latest.log";
    struct stat st{};
    if (stat(latest.c_str(), &st) == 0 && st.st_size > 0) {
        char date[16];
        tm lt{};
        localtime_r(&st.st_mtime, &lt);
        strftime(date, sizeof date, "%Y-%m-%d", &lt);
        std::string base;
        for (int n = 1;; n++) {
            base = dir + "/" + date + "-" + std::to_string(n) + ".log";
            if (!std::filesystem::exists(base, ec) && !std::filesystem::exists(base + ".gz", ec)) break;
        }
#ifdef CRAFTPP_ZLIB
        bool ok = false;
        if (FILE* in = fopen(latest.c_str(), "rb")) {
            if (gzFile gz = gzopen((base + ".gz").c_str(), "wb")) {
                char buf[16384];
                ok = true;
                for (size_t r; (r = fread(buf, 1, sizeof buf, in)) > 0;)
                    if (gzwrite(gz, buf, unsigned(r)) != int(r)) ok = false;
                if (gzclose(gz) != Z_OK) ok = false;
            }
            fclose(in);
        }
        if (ok) std::filesystem::remove(latest, ec);
        else std::filesystem::remove(base + ".gz", ec), std::filesystem::rename(latest, base, ec);
#else
        std::filesystem::rename(latest, base, ec);  // built without zlib: keep the rolled log uncompressed
#endif
    }
    file = fopen(latest.c_str(), "w");
}

inline void close() {
    std::lock_guard lock(m);
    if (file) fclose(file);
    file = nullptr;
}
}  // namespace logging

inline void log_msg(LogLevel level, const std::string& msg, const char* thread = "Server thread") {
    logging::write(level, msg, thread);
}
[[gnu::format(printf, 1, 2)]] inline void log_info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = logging::vformat(fmt, ap);
    va_end(ap);
    logging::write(LogLevel::INFO, s, "Server thread");
}
[[gnu::format(printf, 1, 2)]] inline void log_warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = logging::vformat(fmt, ap);
    va_end(ap);
    logging::write(LogLevel::WARN, s, "Server thread");
}
[[gnu::format(printf, 1, 2)]] inline void log_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = logging::vformat(fmt, ap);
    va_end(ap);
    logging::write(LogLevel::ERROR, s, "Server thread");
}
