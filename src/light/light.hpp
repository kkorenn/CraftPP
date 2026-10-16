#pragma once
// Sky + block light: a port of vanilla world/level/lighting (LightEngine, SkyLightEngine, BlockLightEngine,
// LayerLightSectionStorage, ChunkSkyLightSources, DataLayer). Implementation and design notes: light.cpp.
//
// Lifecycle (vanilla ChunkStatus INITIALIZE_LIGHT -> LIGHT):
//  * Generated worlds: attach(world, pipeline) sets the pipeline's INITIALIZE_LIGHT / LIGHT steps and its
//    on_block_changed / on_take hooks, so a chunk is lit when it reaches LIGHT, from the proto chunks, as in
//    vanilla. Light of a chunk whose neighbours light later changes; take_changes() reports it.
//  * Without a pipeline (flat worlds) ensure_lit lights lazily: it generates the 5x5 around the chunk, then
//    initializes and lights the 3x3 (light reaches < 16 blocks, so the chunk's light is then final).
//  * produced() (World::produce, worker) lights a generated chunk's 3x3 through the pipeline. A chunk loaded
//    from disk comes with its saved light (vanilla "lighted" chunk: no propagation); if the pipeline already
//    lit its generated version as a neighbour, the saved edits are applied as block changes instead.
//    ensure_lit() before a send is then a check (flat worlds: the lazy lighting above).
//  * Light changes reset the chunk's cached frame (Chunk::body) and bump Chunk::light_version.
//  * World::set_block calls block_changed() with mutex() held; the work is queued (checkBlock) and runs on
//    the next take_changes() (once per tick) or write().
#include <cstdint>
#include <mutex>
#include <vector>

struct Chunk;
struct World;
struct Writer;
namespace worldgen { class Pipeline; }

namespace light {
void attach(World& world, worldgen::Pipeline& pipeline);  // before the first chunk is generated
std::mutex& mutex();  // guards the engine and every block write to a chunk the engine may read
void produced(World& world, Chunk& c, bool loaded);      // World::produce, before the chunk is published (worker thread)
void ensure_lit(World& world, Chunk& c);    // before sending; any thread, without mutex() or c.m held
uint32_t write(Writer& w, Chunk& c);
std::vector<uint8_t> save(Chunk& c, uint32_t& version);  // light to store with the chunk (-> Chunk::saved_light)        // light tail of level_chunk_with_light; returns c.light_version
void block_changed(int x, int y, int z, uint16_t old_state, uint16_t new_state);  // mutex() held
// Light level at a block (LevelLightEngine.getLayerListener(...).getLightValue); -1 when the column isn't lit yet.
// Takes mutex(); call without it held.
int get(int x, int y, int z, bool sky);
int level(bool sky, int x, int y, int z);  // get() with unlit columns read as sky 15 / block 0

struct Change {
    int cx, cz;
    uint32_t sky, block;  // changed light sections, bit 0 = section y -5
    bool all;             // send to every viewer, not only view-border ones (a loaded chunk's edits relit)
};
std::vector<Change> take_changes();  // runs queued updates; columns changed since the last call
void write_update(Writer& w, const Change& c);  // body of light_update (without packet id)

}  // namespace light
