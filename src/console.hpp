#pragma once
// Server console like Paper's JLine terminal. On a TTY (stdin and stdout): a "> " prompt kept at the bottom
// while log lines print above it, line editing (arrows, home/end, backspace/delete, ctrl+A/E/U/W/K/L),
// history (up/down, saved to .console_history in JLine's "millis:line" format) and tab completion.
// Otherwise (pipes, systemd, scripts) plain line reading with no prompt or escape codes.
// Raw termios, no dependencies; blocks in poll()/read() so it costs no CPU while idle.
#include "log.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <vector>

namespace console {
// Tab completion: (text before the cursor) -> (byte offset where candidates start, candidates).
inline std::function<std::pair<size_t, std::vector<std::string>>(const std::string&)> complete;

namespace detail {
inline bool raw = false;
inline termios saved{};
inline std::string buf;  // UTF-8
inline size_t cur = 0;   // byte offset in buf
inline std::vector<std::string> history;
inline size_t hpos = 0;
inline std::string draft, history_path;
constexpr size_t HISTORY_SIZE = 500;  // JLine's default

inline bool cont(char c) { return (uint8_t(c) & 0xC0) == 0x80; }
// Terminal columns of a UTF-8 string (East Asian wide characters take two).
inline int width(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = uint8_t(s[i]);
        uint32_t cp = c;
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (n > 1) cp = c & (0x7F >> n);
        for (int k = 1; k < n && i + size_t(k) < s.size(); k++) cp = cp << 6 | (uint8_t(s[i + size_t(k)]) & 0x3F);
        i += size_t(n);
        bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
                    (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                    (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
                    (cp >= 0x20000 && cp <= 0x3FFFD);
        w += wide ? 2 : 1;
    }
    return w;
}

// Both run with logging::m held. ponytail: one terminal row; input longer than the width wraps and the
// redraw only clears the last row. Add horizontal scrolling if long commands get typed often.
inline void clear_line() { fputs("\r\x1b[K", stdout); }
inline void draw() {
    std::string s = "\r\x1b[K> " + buf;
    if (int back = width(buf.substr(cur))) s += "\x1b[" + std::to_string(back) + "D";
    fputs(s.c_str(), stdout);
}

inline void set_raw() {
    termios t = saved;
    t.c_lflag &= ~tcflag_t(ICANON | ECHO | IEXTEN);  // ISIG stays on: ctrl+C is SIGINT -> clean stop
    t.c_iflag &= ~tcflag_t(IXON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &t);
}
inline void on_fatal(int sig) {  // crash: give the shell its terminal back, then die as usual
    tcsetattr(0, TCSAFLUSH, &saved);
    signal(sig, SIG_DFL);
    raise(sig);
}
inline void on_tstp(int) {  // ctrl+Z: cooked mode while stopped, raw again on resume
    tcsetattr(0, TCSAFLUSH, &saved);
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
}
inline void on_cont(int) {
    signal(SIGTSTP, on_tstp);
    if (raw) set_raw();
}

inline void load_history() {
    std::ifstream f(history_path);
    for (std::string l; std::getline(f, l);) {
        size_t colon = l.find(':');  // JLine: "<epoch millis>:<line>"
        if (colon != std::string::npos && colon > 0 && l.find_first_not_of("0123456789") == colon) l = l.substr(colon + 1);
        std::string un;  // JLine escapes '\\' and newlines
        for (size_t i = 0; i < l.size(); i++) {
            if (l[i] == '\\' && i + 1 < l.size()) { i++; un += l[i] == 'n' ? '\n' : l[i]; }
            else un += l[i];
        }
        if (!un.empty()) history.push_back(un);
    }
    if (history.size() > HISTORY_SIZE) history.erase(history.begin(), history.end() - HISTORY_SIZE);
    // Keep the file bounded: rewrite it when it holds much more than we keep.
    if (history.size() == HISTORY_SIZE) {
        std::ofstream o(history_path + ".tmp");
        for (auto& h : history) o << "0:" << h << '\n';
        o.close();
        std::error_code ec;
        std::filesystem::rename(history_path + ".tmp", history_path, ec);
    }
}
inline void add_history(const std::string& line) {
    if (line.empty() || line[0] == ' ' || (!history.empty() && history.back() == line)) return;  // JLine ignore space/dups
    history.push_back(line);
    if (history.size() > HISTORY_SIZE) history.erase(history.begin());
    std::ofstream f(history_path, std::ios::app);
    std::string esc;
    for (char c : line) esc += c == '\\' ? std::string("\\\\") : std::string(1, c);
    f << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
      << ':' << esc << '\n';
}

inline void tab() {
    if (!complete) return;
    auto [start, cands] = complete(buf.substr(0, cur));
    if (cands.empty() || start > cur) return;
    std::string typed = buf.substr(start, cur - start);
    std::string common = cands[0];
    for (auto& c : cands) {
        size_t n = 0;
        while (n < common.size() && n < c.size() && common[n] == c[n]) n++;
        common.resize(n);
    }
    while (!common.empty() && cont(common.back())) common.pop_back();
    std::lock_guard lock(logging::m);
    if (cands.size() == 1 || (common.size() > typed.size() && common.compare(0, typed.size(), typed) == 0)) {
        std::string ins = cands.size() == 1 && cur == buf.size() ? cands[0] + " " : cands.size() == 1 ? cands[0] : common;
        buf.replace(start, cur - start, ins);
        cur = start + ins.size();
        draw();
    } else {  // ambiguous: list the candidates in columns above the prompt
        winsize ws{};
        int cols = ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col ? ws.ws_col : 80;
        int cw = 0;
        for (auto& c : cands) cw = std::max(cw, width(c) + 2);
        int per = std::max(1, cols / std::max(cw, 1));
        std::string out = "\r\x1b[K";
        for (size_t i = 0; i < cands.size(); i++) {
            out += cands[i];
            if ((int(i) + 1) % per == 0 || i + 1 == cands.size()) out += "\n";
            else out += std::string(size_t(cw - width(cands[i])), ' ');
        }
        fputs(out.c_str(), stdout);
        draw();
    }
    fflush(stdout);
}

// One key (or escape sequence). Returns true with `line` set when Enter completes a line.
inline bool key(const std::string& k, std::string& line) {
    auto left = [] { if (cur) { cur--; while (cur && cont(buf[cur])) cur--; } };
    auto right = [] { if (cur < buf.size()) { cur++; while (cur < buf.size() && cont(buf[cur])) cur++; } };
    auto hist = [](int d) {
        if (history.empty()) return;
        if (hpos == history.size() && d < 0) draft = buf;
        if (d < 0 && hpos == 0) return;
        if (d > 0 && hpos == history.size()) return;
        hpos = size_t(int(hpos) + d);
        buf = hpos == history.size() ? draft : history[hpos];
        cur = buf.size();
    };
    if (k == "\x09") { tab(); return false; }
    std::lock_guard lock(logging::m);
    if (k == "\n" || k == "\r") {
        cur = buf.size();
        draw();
        fputs("\n", stdout);
        fflush(stdout);
        line = buf;
        buf.clear();
        cur = 0;
        return true;
    }
    if (k == "\x01" || k == "\x1b[H" || k == "\x1bOH" || k == "\x1b[1~" || k == "\x1b[7~") cur = 0;
    else if (k == "\x05" || k == "\x1b[F" || k == "\x1bOF" || k == "\x1b[4~" || k == "\x1b[8~") cur = buf.size();
    else if (k == "\x02" || k == "\x1b[D" || k == "\x1bOD") left();
    else if (k == "\x06" || k == "\x1b[C" || k == "\x1bOC") right();
    else if (k == "\x1b[A" || k == "\x1bOA" || k == "\x10") hist(-1);
    else if (k == "\x1b[B" || k == "\x1bOB" || k == "\x0e") hist(1);
    else if (k == "\x7f" || k == "\x08") { size_t e = cur; left(); buf.erase(cur, e - cur); }
    else if (k == "\x1b[3~" || k == "\x04") {  // ctrl+D on an empty line is ignored, like Paper
        size_t s = cur; right(); buf.erase(s, cur - s); cur = s;
    }
    else if (k == "\x15") { buf.erase(0, cur); cur = 0; }
    else if (k == "\x0b") buf.erase(cur);
    else if (k == "\x17") {
        size_t s = cur;
        while (s && buf[s - 1] == ' ') s--;
        while (s && buf[s - 1] != ' ') s--;
        buf.erase(s, cur - s);
        cur = s;
    }
    else if (k == "\x0c") fputs("\x1b[H\x1b[2J", stdout);
    else if (uint8_t(k[0]) >= 0x20 && k[0] != 0x7f && k.size() < 8 && buf.size() < 32767) { buf.insert(cur, k); cur += k.size(); }
    draw();
    fflush(stdout);
    return false;
}
}  // namespace detail

// Restores the terminal (safe to call more than once, and from exit paths).
inline void restore() {
    if (!detail::raw) return;
    detail::raw = false;
    std::lock_guard lock(logging::m);
    logging::clear_prompt = logging::draw_prompt = nullptr;
    detail::clear_line();
    fflush(stdout);
    tcsetattr(0, TCSAFLUSH, &detail::saved);
}

// Reads console lines until stdin closes, calling on_line for each one (any thread may log meanwhile).
// on_eof runs when stdin ends; a TTY never ends (ctrl+D is ignored), a hangup sends SIGHUP instead.
inline void run(const std::string& history_path, const std::function<void(const std::string&)>& on_line,
                const std::function<void()>& on_eof) {
    using namespace detail;
    if (!isatty(0) || !isatty(1) || tcgetattr(0, &saved) != 0) {
        for (std::string line; std::getline(std::cin, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            on_line(line);
        }
        on_eof();
        return;
    }
    detail::history_path = history_path;
    load_history();
    hpos = history.size();
    for (int s : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL}) signal(s, on_fatal);
    signal(SIGTSTP, on_tstp);
    signal(SIGCONT, on_cont);
    atexit(restore);
    {
        std::lock_guard lock(logging::m);
        set_raw();
        raw = true;
        logging::clear_prompt = clear_line;
        logging::draw_prompt = draw;
        draw();
        fflush(stdout);
    }
    std::string pending;  // bytes of an incomplete escape sequence / UTF-8 character
    for (;;) {
        pollfd p{0, POLLIN, 0};
        if (poll(&p, 1, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char in[256];
        ssize_t n = read(0, in, sizeof in);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) break;
        pending.append(in, size_t(n));
        size_t i = 0;
        while (i < pending.size()) {
            size_t len = 1;
            uint8_t c = uint8_t(pending[i]);
            if (c == 0x1b) {  // ESC [ params final | ESC O x | ESC x
                if (i + 1 >= pending.size()) break;
                if (pending[i + 1] == '[') {
                    size_t j = i + 2;
                    while (j < pending.size() && !(pending[j] >= 0x40 && pending[j] <= 0x7e)) j++;
                    if (j >= pending.size()) break;
                    len = j - i + 1;
                } else len = pending[i + 1] == 'O' ? (i + 2 < pending.size() ? 3 : 0) : 2;
                if (!len) break;
            } else if (c >= 0xC0) {
                len = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                if (i + len > pending.size()) break;
            }
            std::string line;
            if (key(pending.substr(i, len), line)) {
                add_history(line);
                hpos = history.size();
                draft.clear();
                on_line(line);
            }
            i += len;
        }
        pending.erase(0, i);
    }
    restore();
}
}  // namespace console
