#pragma once
// Live block entities (BlockEntity + BaseContainerBlockEntity ports). Tick thread only.
//
// Storage: the chunk's BlockEntity list (world.hpp) holds each block entity's saved NBT; that is what chunk
// files persist. A Live object holds the game state while something uses it (open menu, ticking furnace,
// hopper, comparator read): created from the chunk entry on demand, written back (flush) every 30 s, before
// world saves and when released. A Live keeps its chunk loaded (ChunkPtr); it is released once no player views
// the chunk and no menu uses it. Ticking types are revived when their chunk loads (chunk_loaded_hook).
//
// API for other modules:
//   be::get(x, y, z)            -> std::shared_ptr<be::Live> (null: no block entity there)
//   be::comparator_signal(x,y,z) -> AbstractContainerMenu.getRedstoneSignalFromContainer for the block there
//   be::container_items(live)   -> Container view (hoppers, droppers): items + size
// Each level (worlds[]) has its own registry; game code reaches the acting level's through live() (cur_level).
#include "components.hpp"
#include "../worldgen/block_state.hpp"

#include <unordered_map>

namespace be {

struct Live {
    int x = 0, y = 0, z = 0, type = -1, state = 0;
    World* level = nullptr;  // the level it lives in
    ChunkPtr chunk;
    bool removed = false, dirty = false;
    std::vector<uint8_t> custom_name;  // CustomName text component (network NBT), empty = none
    std::vector<ItemStack> items;      // Container contents (empty vector: not a container)
    std::string loot_table;            // RandomizableContainerBlockEntity: unpacked on first open
    int64_t loot_seed = 0;
    int openers = 0;                   // ContainerOpenersCounter

    virtual ~Live() = default;
    virtual void load(const Nbt&) {}
    virtual void save(Nbt&) const {}
    virtual bool ticks() const { return false; }
    virtual void tick() {}
    virtual void removed_side_effects() {}  // preRemoveSideEffects (default: drop container contents)
    virtual bool full_update_tag() const { return false; }  // getUpdateTag = saveCustomOnly
    virtual int max_stack() const { return 99; }              // Container.getMaxStackSize
    virtual bool can_place(int, const ItemStack&) const { return true; }  // Container.canPlaceItem
    virtual bool can_take(int, const ItemStack&) const { return true; }   // Container.canTakeItem
    virtual bool worldly() const { return false; }                        // WorldlyContainer: faces below
    virtual std::vector<int> face_slots(int /*bs::Dir*/) const { return {}; }
    virtual bool can_place_face(int slot, const ItemStack& s, int) const { return can_place(slot, s); }
    virtual bool can_take_face(int, const ItemStack&, int) const { return true; }
    virtual void item_set(int) {}  // setItem side effects (furnace input restarts cooking)
    virtual void opened(Player&) {}
    virtual void closed(Player&) {}
    void set_changed() { dirty = true; }
};

using LiveMap = std::unordered_map<uint64_t, std::shared_ptr<Live>>;
inline LiveMap live_maps[DIM_COUNT];
inline LiveMap& live() { return live_maps[dim_of(cur_level)]; }  // registry of the acting level
inline std::shared_ptr<Live> graveyard;  // block entity removed this tick (loot copy_components reads it)
inline int64_t graveyard_tick = -1;
using Factory = std::shared_ptr<Live> (*)(int type);
inline std::map<int, Factory> factories;  // block_entity_types id -> constructor (registered by modules)
inline std::vector<int> empty_update_tag;  // types whose getUpdateTag is {} (containers)
inline std::vector<int> ticking_types;     // revived when their chunk loads

inline uint64_t key(int x, int y, int z) {
    return (uint64_t(uint32_t(x) & 0x3FFFFFF) << 38) | (uint64_t(uint32_t(z) & 0x3FFFFFF) << 12) | uint64_t(uint32_t(y) & 0xFFF);
}
inline int block_entity_type(int state) { return bs::R().block(bs::R().block_of(state)).block_entity; }

inline void write_name_nbt(Nbt& n, const Live& b) {
    if (b.custom_name.empty()) return;
    Reader r(b.custom_name);
    n.set("CustomName", comp::nbt_read(r));
}
inline Nbt save_nbt(const Live& b) {
    Nbt n = Nbt::compound();
    write_name_nbt(n, b);
    if (!b.loot_table.empty()) {
        n.set("LootTable", Nbt::str(b.loot_table));
        if (b.loot_seed) n.set("LootTableSeed", Nbt::num(Nbt::Long, b.loot_seed));
    } else if (!b.items.empty()) {
        n.set("Items", comp::items_nbt(b.items.data(), int(b.items.size())));
    }
    b.save(n);
    return n;
}
inline void load_nbt(Live& b, const Nbt& n) {
    if (const Nbt* c = n.get("CustomName")) b.custom_name = comp::nbt_bytes(*c);
    b.loot_table = comp::nbt_str(n, "LootTable");
    b.loot_seed = comp::nbt_int(n, "LootTableSeed");
    if (!b.items.empty()) comp::items_from_nbt(n, "Items", b.items.data(), int(b.items.size()));
    b.load(n);
}

inline Writer block_entity_data_packet(int x, int y, int z, int type, const std::vector<uint8_t>& nbt) {
    Writer w;
    w.varint(play_cb::block_entity_data).i64(int64_t(key(x, y, z))).varint(type).bytes(nbt);
    return w;
}

// Writes the live state into the chunk entry (creating it if needed).
inline void flush(Live& b) {
    b.dirty = false;
    std::vector<uint8_t> bytes = comp::nbt_bytes(save_nbt(b));
    bool changed = true;
    {
        std::lock_guard lock(b.chunk->m);
        BlockEntity* e = nullptr;
        for (auto& c : b.chunk->block_entities)
            if (c.x == (b.x & 15) && c.z == (b.z & 15) && c.y == b.y) e = &c;
        if (!e) {
            b.chunk->block_entities.push_back({uint8_t(b.x & 15), uint8_t(b.z & 15), int16_t(b.y), b.type, {}});
            e = &b.chunk->block_entities.back();
        }
        changed = e->nbt != bytes;
        e->type = b.type;
        e->nbt = std::move(bytes);
        if (changed) {
            b.chunk->dirty = true;
            if (b.full_update_tag()) b.chunk->body.reset();
        }
    }
    if (changed && b.full_update_tag())  // sendBlockUpdated -> ClientboundBlockEntityDataPacket
        broadcast_near(b.x, b.z, block_entity_data_packet(b.x, b.y, b.z, b.type, comp::nbt_bytes(save_nbt(b))));
}

inline std::shared_ptr<Live> create(const ChunkPtr& c, int x, int y, int z, int state, int type, const std::vector<uint8_t>* nbt) {
    auto f = factories.find(type);
    if (f == factories.end()) return nullptr;
    auto b = f->second(type);
    b->x = x, b->y = y, b->z = z, b->type = type, b->state = state, b->chunk = c, b->level = cur_level;
    if (nbt) {
        try {
            load_nbt(*b, comp::nbt_parse(*nbt));
        } catch (const std::exception& e) {
            log_warn("Block entity at %d %d %d: bad NBT (%s)", x, y, z, e.what());
        }
    }
    live()[key(x, y, z)] = b;
    return b;
}

// The live block entity at a position (loads the chunk if needed), or null.
inline std::shared_ptr<Live> get(int x, int y, int z) {
    if (auto it = live().find(key(x, y, z)); it != live().end()) return it->second;
    if (y < world->min_y || y >= world->max_y()) return nullptr;
    ChunkPtr c = world.get_ptr(x >> 4, z >> 4);
    int state, type;
    std::vector<uint8_t> nbt;
    bool have = false;
    {
        std::lock_guard lock(c->m);
        state = c->get(x & 15, y, z & 15);
        for (auto& e : c->block_entities)
            if (e.x == (x & 15) && e.z == (z & 15) && e.y == y) nbt = e.nbt, type = e.type, have = true;
    }
    if (!have) type = block_entity_type(state);
    if (type < 0 || type != block_entity_type(state)) return nullptr;
    auto b = create(c, x, y, z, state, type, have ? &nbt : nullptr);
    if (b && !have) flush(*b);
    return b;
}

// Containers.dropItemStack
inline void drop_stack(double x, double y, double z, ItemStack s) {
    const double size = 0.25, range = 1.0 - size, half = size / 2.0;
    double xo = std::floor(x) + rand_double() * range + half, yo = std::floor(y) + rand_double() * range,
           zo = std::floor(z) + rand_double() * range + half;
    auto tri = [](double mean, double spread) { return mean + spread * (rand_double() - rand_double()); };
    while (!s.empty()) {
        int n = std::min(s.count, rand_int(21) + 10);
        ItemStack part = s.copy(n);
        s.count -= n;
        spawn_item(part, xo, yo, zo, tri(0, 0.11485000171139836), tri(0.2, 0.11485000171139836), tri(0, 0.11485000171139836), 0);
    }
}
inline void drop_contents(Live& b) {
    for (auto& s : b.items)
        if (!s.empty()) drop_stack(b.x, b.y, b.z, s), s = {};
}

// World::set_block hook: keep the live object for same-block state changes, else remove (dropping contents)
// and create the new block's entry.
inline void block_changed(int x, int y, int z, int old_state, int new_state) {
    bool same = state_block_index(old_state) == state_block_index(new_state);
    LiveMap& map = live();
    auto it = map.find(key(x, y, z));
    if (it != map.end()) {
        if (same) {
            it->second->state = new_state;
            return;
        }
        auto b = it->second;
        map.erase(it);
        b->removed = true;
        graveyard = b, graveyard_tick = server_tick;
        b->removed_side_effects();
    }
    if (same) return;
    int type = block_entity_type(new_state);
    if (type < 0) return;
    ChunkPtr c = world.get_ptr(x >> 4, z >> 4);
    if (factories.count(type)) {
        auto b = create(c, x, y, z, new_state, type, nullptr);
        flush(*b);
    } else {
        std::lock_guard lock(c->m);
        c->block_entities.push_back({uint8_t(x & 15), uint8_t(z & 15), int16_t(y), type, {0x0a, 0x00}});
    }
}
// The block entity just removed at a position (for drops computed after removal), or null.
inline std::shared_ptr<Live> removed_at(int x, int y, int z) {
    if (graveyard && graveyard_tick == server_tick && graveyard->level == cur_level && graveyard->x == x && graveyard->y == y &&
        graveyard->z == z)
        return graveyard;
    return nullptr;
}

inline bool viewed(int cx, int cz) {
    for (auto& p : players)
        if (sees_chunk(*p, cx, cz)) return true;
    return false;
}
inline void flush_all() {
    for (auto& w : worlds) {
        InLevel in(&w);  // flush broadcasts block entity data to the level's viewers
        for (auto& [k, b] : live())
            if (b->dirty) flush(*b);
    }
}

inline void tick() {
    static std::vector<std::shared_ptr<Live>> ticking;
    for (auto& w : worlds) {
        LiveMap& map = live_maps[dim_of(&w)];
        if (map.empty()) continue;
        InLevel in(&w);
        ticking.clear();
        for (auto& [k, b] : map)
            if (b->ticks()) ticking.push_back(b);
        for (auto& b : ticking)
            if (!b->removed) b->tick();
        ticking.clear();
        if (server_tick % 100 == 0) {  // release what nobody uses any more
            for (auto it = map.begin(); it != map.end();) {
                auto& b = it->second;
                if (it->second.use_count() == 1 && !viewed(b->x >> 4, b->z >> 4)) {
                    if (b->dirty) flush(*b);
                    it = map.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    if (server_tick % 600 == 0) flush_all();
    if (graveyard && graveyard_tick != server_tick) graveyard.reset();
}

// Chunk loaded (worker thread): revive ticking block entities.
inline void chunk_loaded(const ChunkPtr& c) {
    std::vector<std::tuple<int, int, int>> todo;
    {
        std::lock_guard lock(c->m);
        for (auto& e : c->block_entities) {
            if (std::find(ticking_types.begin(), ticking_types.end(), e.type) != ticking_types.end()) todo.emplace_back(c->cx * 16 + e.x, int(e.y), c->cz * 16 + e.z);
        }
    }
    if (todo.empty()) return;
    run_on_tick([c, todo] {
        InLevel in(&worlds[c->dimension]);
        for (auto [x, y, z] : todo) {
            if (live().count(key(x, y, z))) continue;
            std::vector<uint8_t> nbt;
            int state = 0, type = -1;
            {
                std::lock_guard lock(c->m);
                state = c->get(x & 15, y, z & 15);
                for (auto& e : c->block_entities)
                    if (e.x == (x & 15) && e.z == (z & 15) && e.y == y) nbt = e.nbt, type = e.type;
            }
            if (type >= 0 && type == block_entity_type(state)) create(c, x, y, z, state, type, &nbt);
        }
    });
}

inline void update_tag(const BlockEntity& e, Writer& w) {
    if (std::find(empty_update_tag.begin(), empty_update_tag.end(), e.type) != empty_update_tag.end()) w.u8(0x0a).u8(0);
    else w.bytes(e.nbt.data(), e.nbt.size());
}

// ---------- Container helpers ----------
inline int container_max(const Live& b, const ItemStack& s) { return std::min(b.max_stack(), max_stack(s)); }
inline int signal_from_items(const ItemStack* items, int n, int container_max_stack) {
    if (n <= 0) return 0;
    float total = 0;
    for (int i = 0; i < n; i++)
        if (!items[i].empty()) total += float(items[i].count) / float(std::min(container_max_stack, max_stack(items[i])));
    total /= float(n);
    return int(std::floor(total * 14.0f)) + (total > 0 ? 1 : 0);  // Mth.lerpDiscrete(total, 0, 15)
}
// Comparator output for container blocks (double chests: combined). Set by chest.hpp for chests.
inline int (*container_signal_hook)(Live& b) = nullptr;
inline int comparator_signal(int x, int y, int z) {
    auto b = get(x, y, z);
    if (!b || b->items.empty()) return 0;
    if (container_signal_hook) return container_signal_hook(*b);
    return signal_from_items(b->items.data(), int(b->items.size()), b->max_stack());
}

}  // namespace be
