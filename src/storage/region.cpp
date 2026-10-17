#include "region.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#include "../log.hpp"
#include "nbt_io.hpp"

namespace storage {
namespace {

constexpr int SECTOR = 4096;

int floor_div32(int v) { return v >> 5; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void put_be32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
void sync_fd(int fd) {
#ifdef __APPLE__
    fsync(fd);
#else
    fdatasync(fd);
#endif
}

struct RegionFile {
    std::string dir;
    int rx, rz, fd = -1;
    uint32_t offsets[1024] = {}, stamps[1024] = {};
    std::vector<bool> used;  // RegionBitmap

    ~RegionFile() { if (fd >= 0) close(fd); }
    std::string external(int cx, int cz) const { return dir + "/c." + std::to_string(cx) + "." + std::to_string(cz) + ".mcc"; }

    bool open_file(const std::string& path, bool create) {
        fd = ::open(path.c_str(), create ? O_RDWR | O_CREAT : O_RDWR, 0644);
        if (fd < 0) return false;
        struct stat st{};
        fstat(fd, &st);
        uint8_t h[2 * SECTOR] = {};
        if (st.st_size < 2 * SECTOR) {
            if (pwrite(fd, h, sizeof h, 0) != ssize_t(sizeof h)) return false;
            st.st_size = 2 * SECTOR;
        } else if (pread(fd, h, sizeof h, 0) != ssize_t(sizeof h)) {
            return false;
        }
        size_t sectors = size_t((st.st_size + SECTOR - 1) / SECTOR);
        used.assign(std::max<size_t>(sectors, 2), false);
        used[0] = used[1] = true;
        for (int i = 0; i < 1024; i++) {
            offsets[i] = be32(h + 4 * i), stamps[i] = be32(h + SECTOR + 4 * i);
            uint32_t at = offsets[i] >> 8, n = offsets[i] & 255;
            if (!offsets[i]) continue;
            if (at < 2 || n == 0 || at + n > sectors) {  // RegionFile: invalid sector entries are dropped
                log_warn("Region %s: chunk %d has invalid sector at index %u; skipping", path.c_str(), i, at);
                offsets[i] = 0;
                continue;
            }
            for (uint32_t s = at; s < at + n; s++) used[s] = true;
        }
        return true;
    }
    uint32_t allocate(uint32_t n) {
        for (size_t s = 2;; s++) {
            if (s + n > used.size()) used.resize(s + n, false);
            size_t k = 0;
            while (k < n && !used[s + k]) k++;
            if (k == n) {
                for (k = 0; k < n; k++) used[s + k] = true;
                return uint32_t(s);
            }
            s += k;
        }
    }
};

std::mutex m;
std::map<std::string, std::unique_ptr<RegionFile>> files;  // by path
std::map<std::tuple<std::string, int, int>, std::vector<uint8_t>> queue;  // (dir, cx, cz) -> zlib data
size_t queue_size = 0;

std::string region_path(const std::string& dir, int rx, int rz) {
    return dir + "/r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca";
}

RegionFile* get(const std::string& dir, int rx, int rz, bool create) {  // m held
    std::string path = region_path(dir, rx, rz);
    if (auto it = files.find(path); it != files.end()) return it->second.get();
    if (!create && access(path.c_str(), F_OK) != 0) return nullptr;
    if (create) std::filesystem::create_directories(dir);
    if (files.size() >= 64) files.clear();  // ponytail: drop all handles past 64 (vanilla keeps 256, LRU)
    auto f = std::make_unique<RegionFile>();
    f->dir = dir, f->rx = rx, f->rz = rz;
    if (!f->open_file(path, create)) {
        log_error("Failed to open region file %s", path.c_str());
        return nullptr;
    }
    return (files[path] = std::move(f)).get();
}

// Compressed payload (after the 5-byte chunk header) -> NBT bytes.
std::vector<uint8_t> decompress(uint8_t type, const uint8_t* p, size_t n, const char* where) {
    std::vector<uint8_t> out;
    if (type == 3) return std::vector<uint8_t>(p, p + n);
    if ((type == 1 || type == 2) && nbtio::inflate_bytes(p, n, out)) return out;
    log_error("Chunk %s: unsupported or corrupt compression (type %d)", where, type);  // 4 = LZ4: not supported
    return {};
}

}  // namespace

std::vector<uint8_t> read(const std::string& dir, int cx, int cz) {
    std::vector<uint8_t> raw;
    uint8_t type = 2;
    std::string ext;
    {
        std::lock_guard lock(m);
        if (auto it = queue.find({dir, cx, cz}); it != queue.end()) {
            raw = it->second;
        } else {
            RegionFile* f = get(dir, floor_div32(cx), floor_div32(cz), false);
            if (!f) return {};
            uint32_t off = f->offsets[(cx & 31) + (cz & 31) * 32];
            if (!off) return {};
            std::vector<uint8_t> buf(size_t(off & 255) * SECTOR);
            if (pread(f->fd, buf.data(), buf.size(), off_t(off >> 8) * SECTOR) < 5) return {};
            uint32_t len = be32(buf.data());
            type = buf[4];
            if (type & 0x80) {
                ext = f->external(cx, cz), type &= 0x7F;
            } else {
                if (len < 1 || len - 1 > buf.size() - 5) {
                    log_error("Chunk [%d, %d] in %s has a bad length", cx, cz, dir.c_str());
                    return {};
                }
                raw.assign(buf.begin() + 5, buf.begin() + 5 + (len - 1));
            }
        }
    }
    if (!ext.empty()) {
        FILE* e = fopen(ext.c_str(), "rb");
        if (!e) return {};
        uint8_t b[16384];
        for (size_t k; (k = fread(b, 1, sizeof b, e)) > 0;) raw.insert(raw.end(), b, b + k);
        fclose(e);
    }
    std::string where = "[" + std::to_string(cx) + ", " + std::to_string(cz) + "] in " + dir;
    return decompress(type, raw.data(), raw.size(), where.c_str());
}

void write(const std::string& dir, int cx, int cz, const std::vector<uint8_t>& nbt) {
    auto z = nbtio::deflate_bytes(nbt.data(), nbt.size(), false, 4);  // vanilla zlib default is 6; 4: ~2x faster
    std::lock_guard lock(m);
    auto& slot = queue[{dir, cx, cz}];
    queue_size += z.size() - slot.size();
    slot = std::move(z);
}

size_t queued_bytes() {
    std::lock_guard lock(m);
    return queue_size;
}

void flush() {
    std::lock_guard lock(m);
    if (queue.empty()) return;
    uint32_t now = uint32_t(time(nullptr));
    // Group by region file (queue is ordered by dir, then cx: collect per (dir, rx, rz)).
    std::map<std::tuple<std::string, int, int>, std::vector<decltype(queue)::iterator>> by_file;
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        auto& [dir, cx, cz] = it->first;
        by_file[{dir, floor_div32(cx), floor_div32(cz)}].push_back(it);
    }
    for (auto& [key, chunks] : by_file) {
        auto& [dir, rx, rz] = key;
        RegionFile* f = get(dir, rx, rz, true);
        if (!f) continue;  // logged; stays queued
        std::vector<std::pair<uint32_t, uint32_t>> freed;
        for (auto it : chunks) {
            auto& [d, cx, cz] = it->first;
            const std::vector<uint8_t>& z = it->second;
            int idx = (cx & 31) + (cz & 31) * 32;
            bool big = z.size() + 5 > size_t(255) * SECTOR;
            std::vector<uint8_t> buf;
            if (big) {  // RegionFile.write: oversized chunk to c.x.z.mcc, 1-sector stub
                std::string ext = f->external(cx, cz), tmp = ext + ".tmp";
                if (FILE* e = fopen(tmp.c_str(), "wb")) {
                    fwrite(z.data(), 1, z.size(), e);
                    fclose(e);
                    rename(tmp.c_str(), ext.c_str());
                }
                buf.assign(SECTOR, 0);
                put_be32(buf.data(), 1), buf[4] = 2 | 0x80;
            } else {
                buf.assign((z.size() + 5 + SECTOR - 1) / SECTOR * SECTOR, 0);
                put_be32(buf.data(), uint32_t(z.size() + 1)), buf[4] = 2;
                std::copy(z.begin(), z.end(), buf.begin() + 5);
                if (f->offsets[idx]) unlink(f->external(cx, cz).c_str());
            }
            uint32_t n = uint32_t(buf.size() / SECTOR), at = f->allocate(n);
            if (pwrite(f->fd, buf.data(), buf.size(), off_t(at) * SECTOR) != ssize_t(buf.size())) {
                log_error("Failed to write chunk [%d, %d] to %s", cx, cz, dir.c_str());
                continue;
            }
            if (f->offsets[idx]) freed.push_back({f->offsets[idx] >> 8, f->offsets[idx] & 255});
            f->offsets[idx] = at << 8 | n, f->stamps[idx] = now;
        }
        if (sync_writes) sync_fd(f->fd);  // data before the header that points at it
        uint8_t h[2 * SECTOR];
        for (int i = 0; i < 1024; i++) put_be32(h + 4 * i, f->offsets[i]), put_be32(h + SECTOR + 4 * i, f->stamps[i]);
        if (pwrite(f->fd, h, sizeof h, 0) != ssize_t(sizeof h)) log_error("Failed to write region header %s", region_path(dir, rx, rz).c_str());
        if (sync_writes) sync_fd(f->fd);
        for (auto [s, n] : freed)  // only now: the old copies stayed valid until the new header was written
            for (uint32_t k = s; k < s + n; k++) f->used[k] = false;
        for (auto it : chunks) queue_size -= it->second.size(), queue.erase(it);
    }
}

void close_all() {
    flush();
    std::lock_guard lock(m);
    files.clear();
}

}  // namespace storage
