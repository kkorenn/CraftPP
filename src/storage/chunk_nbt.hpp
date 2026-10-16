#pragma once
// Chunk <-> vanilla chunk NBT (SerializableChunkData, DataVersion 26.3): sections (block_states / biomes
// paletted by name, BlockLight / SkyLight), block_entities, Heightmaps, Status, PostProcessing, ticks.
// Shared by World chunks (status full) and evicted proto chunks (worldgen/pipeline).
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../commands/nbt.hpp"

struct Chunk;

namespace storage {

using Heightmaps = std::array<std::array<int16_t, 256>, 6>;  // worldgen::Heightmap order, first free y

// <world>/dimensions/minecraft/overworld (26.x layout); region/ and entities/ live below it.
std::string dimension_dir();
inline std::string region_dir() { return dimension_dir() + "/region"; }
inline std::string entities_dir() { return dimension_dir() + "/entities"; }

// Root compound with DataVersion, position, status, sections (+ light from light::save's format if non-empty:
// isLightOn), block entities, heightmaps (bit h of hm_mask = worldgen::Heightmap h; null = computed FINAL four),
// empty tick lists, PostProcessing and structures. Caller holds c.m.
Nbt encode(const Chunk& c, const std::string& status, const std::vector<uint8_t>& light, const Heightmaps* hm = nullptr,
           unsigned hm_mask = 0);
// Sections and block entities into c; light (light::save format) only when isLightOn. Throws on bad data.
void decode(const Nbt& root, Chunk& c, std::vector<uint8_t>* light);
std::string status_of(const Nbt& root);  // "minecraft:full", ... ("" if absent: vanilla treats it as no chunk)
// Heightmaps present in the tag (mask of decoded types).
unsigned decode_heightmaps(const Nbt& root, Heightmaps& hm);

// Subsystem tags of World chunks (block ticks, structures, ...): called after encode / decode, c.m held on save.
struct ChunkHook {
    std::function<void(const Chunk&, Nbt& root)> save;
    std::function<void(Chunk&, const Nbt& root)> load;
};
std::vector<ChunkHook>& chunk_hooks();

// Entities (entities/r.x.z.mca, vanilla EntityStorage): for the entity subsystem, which owns the entity NBT
// (vanilla Entity.save compounds with "id"). Saving an empty list removes nothing on disk but writes an empty
// entry, like vanilla. Queued with the chunk writes (flush on autosave).
void save_entities(int cx, int cz, const std::vector<Nbt>& entities);
std::vector<Nbt> load_entities(int cx, int cz);

}  // namespace storage
