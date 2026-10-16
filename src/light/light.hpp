#pragma once
// Sky + block light: a port of vanilla world/level/lighting (LightEngine, SkyLightEngine, BlockLightEngine,
// LayerLightSectionStorage, ChunkSkyLightSources, DataLayer). Implementation and design notes: light.cpp.
//
// Lifecycle (vanilla ChunkStatus INITIALIZE_LIGHT -> LIGHT, done lazily):
//  * ensure_lit(cx, cz) before a chunk is sent: generates the 5x5 around it (outside the light lock), then
//    initializes (section status + sky sources) and lights (propagateLightSources) the 3x3 around it. Light
//    reaches at most 15 blocks, so once the 3x3 is lit the chunk's light is final until a block changes.
//  * World::set_block calls block_changed() with mutex() held; the work is queued (checkBlock) and runs on
//    the next take_changes() (once per tick) or write().
// Light is recomputed when chunks load (not persisted); see light.cpp for the measurement behind that.
#include <cstdint>
#include <mutex>
#include <vector>

struct Chunk;
struct World;
struct Writer;

namespace light {
std::mutex& mutex();  // guards the engine and every block write to a chunk the engine may read
void ensure_lit(World& world, int cx, int cz);  // any thread; call without mutex() held
void write(Writer& w, int cx, int cz);          // light half of level_chunk_with_light (ensure_lit first)
void block_changed(int x, int y, int z, uint16_t old_state, uint16_t new_state);  // mutex() held

struct Change {
    int cx, cz;
    uint32_t sky, block;  // changed light sections, bit 0 = section y -5
};
std::vector<Change> take_changes();  // runs queued updates; columns changed since the last call
void write_update(Writer& w, const Change& c);  // body of light_update (without packet id)

}  // namespace light
