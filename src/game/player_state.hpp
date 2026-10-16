#pragma once
// Per-player game state and network queues (embedded in Player, see server.hpp). Threading: see game/tick.hpp.
#include "../protocol.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// Item stack. Only the damage component is tracked so far (tools wear out); other components are dropped.
struct ItemStack {
    int item = -1, count = 0, damage = 0;
    bool empty() const { return item < 0 || count <= 0; }
    bool same(const ItemStack& o) const { return item == o.item && damage == o.damage; }  // isSameItemSameComponents
    bool operator==(const ItemStack& o) const {
        return (empty() && o.empty()) || (item == o.item && count == o.count && damage == o.damage);
    }
};

// Outgoing bytes: game code appends framed packets, the player's writer thread sends them in one batch per flush.
struct Outbox {
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> buf;
    bool flush_req = false, closed = false;
    std::thread writer;

    void push(const std::vector<uint8_t>& body) {
        uint8_t len[5];
        int n = 0;
        for (uint32_t u = uint32_t(body.size());; u >>= 7) {
            len[n++] = uint8_t(u > 0x7F ? (u & 0x7F) | 0x80 : u);
            if (u <= 0x7F) break;
        }
        std::lock_guard lock(m);
        if (closed) return;
        buf.insert(buf.end(), len, len + n);
        buf.insert(buf.end(), body.begin(), body.end());
    }
    void push_framed(const std::vector<uint8_t>& b) {
        std::lock_guard lock(m);
        if (!closed) buf.insert(buf.end(), b.begin(), b.end());
    }
    void flush() {
        {
            std::lock_guard lock(m);
            if (buf.empty()) return;
            flush_req = true;
        }
        cv.notify_one();
    }
};

// Serverbound play packets waiting for the tick thread.
struct Inbox {
    std::mutex m;
    std::vector<std::vector<uint8_t>> q;
};

inline constexpr int MENU_SLOTS = 46;  // InventoryMenu and CraftingMenu both have 46 slots

struct PlayerGame {
    // vitals (LivingEntity / FoodData)
    float health = 20, saturation = 5, exhaustion = 0, last_hurt = 0;
    int food = 20, food_timer = 0, invulnerable = 0;
    int xp_level = 0, xp_total = 0;
    float xp_progress = 0;
    double fall_distance = 0;
    bool dead = false, sprinting = false, sneaking = false, loaded = false;
    float sent_health = -1;
    int sent_food = -1;
    bool sent_sat_zero = false;
    // block breaking (ServerPlayerGameMode)
    bool digging = false, delayed = false;
    int dig_x = 0, dig_y = 0, dig_z = 0, dig_start = 0, dig_state = 0;
    int dly_x = 0, dly_y = 0, dly_z = 0, dly_start = 0, dly_state = 0;
    // using an item (eating)
    int use_ticks = 0, use_hand = 0;
    // menus: container 0 = inventory menu (always present), else the open crafting table
    ItemStack carried, result;
    std::array<ItemStack, 4> craft2;  // inventory 2x2 grid
    std::array<ItemStack, 9> craft3;  // crafting table 3x3 grid
    int container = 0, next_container = 0, state_id = 0;
    int table_x = 0, table_y = 0, table_z = 0;
    int qc_status = 0, qc_type = 0;  // quick craft (drag)
    uint64_t qc_slots = 0;
    std::array<ItemStack, MENU_SLOTS> remote{};  // what the client shows for the open menu
    ItemStack remote_carried;
    bool remote_valid = false;
    int keepalive_timer = 0;
};
