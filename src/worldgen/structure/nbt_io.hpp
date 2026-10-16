// Binary NBT reader (Java edition, big-endian, named root) for structure templates: gzip via zlib.
#pragma once
#include <string>
#include <vector>

#include "commands/nbt.hpp"

namespace structure {

Nbt readNbt(const std::vector<uint8_t>& raw);       // uncompressed payload with root tag header
Nbt readGzipNbtFile(const std::string& path);        // throws on I/O or format errors
std::vector<uint8_t> gunzip(const std::vector<uint8_t>& in);

}  // namespace structure
