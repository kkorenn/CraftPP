#pragma once
// Anvil region files (vanilla RegionFile / RegionFileStorage): r.<rx>.<rz>.mca = 8 KiB header (1024 sector
// offsets + 1024 timestamps, big-endian) then 4 KiB sectors. A chunk = u32 length, u8 compression (2 = zlib),
// data; >= 256 sectors goes to c.<x>.<z>.mca(c) beside it (flag 0x80).
//
// Writes are queued (compressed at once, kept in memory) and written by flush(): per region file, all queued
// chunks go to free sectors (never over the old copy, like vanilla) and the header is written once. Reads see
// queued data first. Thread-safe; one lock around file I/O (an SD card is one queue anyway).
#include <cstdint>
#include <string>
#include <vector>

namespace storage {

// Uncompressed chunk NBT, or empty if the region has no such chunk (or it is unreadable: logged).
std::vector<uint8_t> read(const std::string& dir, int cx, int cz);
void write(const std::string& dir, int cx, int cz, const std::vector<uint8_t>& nbt);  // queued until flush()
void flush();      // writes everything queued; fsync per touched file when `sync` (server.properties sync-chunk-writes)
void close_all();  // flush, close files
size_t queued_bytes();
inline bool sync_writes = true;

}  // namespace storage
