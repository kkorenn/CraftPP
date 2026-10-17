#pragma once
// MerchantMenu / MerchantContainer / MerchantResultSlot port, select_trade, and opening the trading screen.
// The menu lives in PlayerGame (merchant_eid, merchant_active, merchant_hint, pay); inventory.hpp routes
// slot access here while p.g.merchant_eid >= 0. Traders register a lookup in `merchant_lookup`.
#include "../game/inventory.hpp"
#include "trading.hpp"

// Entity id -> its MerchantState and position (nullptr: gone/dead). Set by the villager/wandering trader code.
inline MerchantState* (*merchant_lookup)(int eid, double& x, double& y, double& z) = nullptr;

inline MerchantState* trader_of(Player& p) {
    double x = 0, y = 0, z = 0;
    return in_merchant(p) && merchant_lookup ? merchant_lookup(p.g.merchant_eid, x, y, z) : nullptr;
}

// MerchantContainer.updateSellItem.
inline void merchant_update_sell(Player& p) {
    auto& g = p.g;
    g.merchant_active = -1;
    MerchantState* m = trader_of(p);
    ItemStack a = g.pay[0].empty() ? g.pay[1] : g.pay[0], b = g.pay[0].empty() ? ItemStack{} : g.pay[1];
    if (a.empty() || !m) return void(g.result = {});
    auto find = [&](const ItemStack& x, const ItemStack& y) -> int {  // MerchantOffers.getRecipeFor
        int hint = g.merchant_hint;
        if (hint > 0 && hint < int(m->offers.size())) return m->offers[size_t(hint)].satisfied_by(x, y) ? hint : -1;
        for (size_t i = 0; i < m->offers.size(); i++)
            if (m->offers[i].satisfied_by(x, y)) return int(i);
        return -1;
    };
    if (m->offers.empty()) return;
    int o = find(a, b);
    if (o < 0 || m->offers[size_t(o)].out_of_stock()) g.merchant_active = o, o = find(b, a);
    if (o >= 0 && !m->offers[size_t(o)].out_of_stock()) {
        g.merchant_active = o;
        g.result = m->offers[size_t(o)].result;
    } else {
        g.result = {};
    }
}

inline void send_merchant_offers(Player& p, MerchantState& m) {
    if (!m.offers.empty())
        p.send(merchant_offers_packet(p.g.container, m.offers, m.level, m.xp, m.profession >= 0, true));
}

// MerchantResultSlot.onTake -> AbstractVillager.notifyTrade -> Villager.rewardTradeXp.
// ponytail: no XP orb or Regeneration effect on level-up yet (needs experience orbs / mob effects).
inline void merchant_take(Player& p) {
    MerchantState* m = trader_of(p);
    if (!m || p.g.merchant_active < 0 || size_t(p.g.merchant_active) >= m->offers.size()) return;
    MerchantOffer& o = m->offers[size_t(p.g.merchant_active)];
    if (o.take(p.g.pay[0], p.g.pay[1]) || o.take(p.g.pay[1], p.g.pay[0])) {
        o.uses++;
        bool levelled = false;
        m->reward_trade_xp(o, levelled);
        m->last_traded_uh = p.uuid_hi, m->last_traded_ul = p.uuid_lo, m->traded = true;
        if (levelled) {
            m->update_special_prices(m->gossips.reputation(p.uuid_hi, p.uuid_lo));
            send_merchant_offers(p, *m);
        }
    }
    merchant_update_sell(p);
}

// MerchantMenu.quickMoveStack.
inline ItemStack merchant_quick_move(Player& p, int i) {
    SlotRef r = menu_slot(p, i);
    if (r.s->empty()) return {};
    ItemStack stack = *r.s, clicked = stack;
    bool ok = i == 2 ? move_to(p, stack, 3, 39, true)
              : i < 2  ? move_to(p, stack, 3, 39, false)
              : i < 30 ? move_to(p, stack, 30, 39, false)
                       : move_to(p, stack, 3, 30, false);
    if (!ok || stack.count == clicked.count) return {};
    if (i == 2) {
        merchant_take(p);  // vanilla: the result slot is refilled by updateSellItem; a remainder is discarded
    } else {
        *r.s = stack;
        if (i < 2) merchant_update_sell(p);
    }
    return clicked;
}

// MerchantMenu.removed (+ AbstractVillager.setTradingPlayer(null)). Payment goes back to the inventory.
inline void merchant_closed(Player& p) {
    if (MerchantState* m = trader_of(p)) m->trading_player = -1;
    for (auto& s : p.g.pay) {
        if (s.empty()) continue;
        if (inventory_add(p, s), s.count > 0) player_drop(p, s, false);
        s = {};
    }
    p.g.merchant_eid = -1, p.g.merchant_active = -1, p.g.merchant_hint = 0;
    p.g.result = {};
}

// Villager.startTrading / Merchant.openTradingScreen. `title_key` is a translation key
// (entity.minecraft.villager.<profession>, entity.minecraft.wandering_trader).
inline void open_merchant(Player& p, int eid, MerchantState& m, const std::string& title_key) {
    if (p.g.container) close_container(p, true);
    else return_grid(p);
    m.update_special_prices(m.gossips.reputation(p.uuid_hi, p.uuid_lo));
    m.trading_player = p.eid;
    p.g.next_container = p.g.next_container % 100 + 1;
    p.g.container = p.g.next_container;
    p.g.merchant_eid = eid;
    Writer w;
    w.varint(play_cb::open_screen).varint(p.g.container).varint(menu_ids["merchant"]);
    w.u8(0x0A).u8(0x08).u8(0).u8(9).bytes("translate", 9);  // {translate: title_key}
    w.u8(uint8_t(title_key.size() >> 8)).u8(uint8_t(title_key.size())).bytes(title_key.data(), title_key.size()).u8(0);
    p.send(w);
    send_merchant_offers(p, m);
    p.g.remote_valid = false;
}

// MerchantMenu.tryMoveItems + moveFromInventoryToPaymentSlot.
inline void merchant_select_trade(Player& p, int index) {
    MerchantState* m = trader_of(p);
    if (!m) return;
    p.g.merchant_hint = index;
    merchant_update_sell(p);
    if (index < 0 || size_t(index) >= m->offers.size()) return;
    for (int k = 0; k < 2; k++) {
        ItemStack& s = p.g.pay[size_t(k)];
        if (s.empty()) continue;
        if (!move_to(p, s, 3, 39, true)) return;
        merchant_update_sell(p);
    }
    if (!p.g.pay[0].empty() || !p.g.pay[1].empty()) return;
    const MerchantOffer& o = m->offers[size_t(index)];
    auto fill = [&](int k, const TradeCost& c) {
        ItemStack& pay = p.g.pay[size_t(k)];
        for (int i = 3; i < 39; i++) {
            ItemStack& s = *menu_slot(p, i).s;
            if (s.empty() || s.item != c.item || !(pay.empty() || pay.same(s))) continue;
            int mx = max_stack(s), n = std::min(mx - (pay.empty() ? 0 : pay.count), s.count);
            pay = {s.item, (pay.empty() ? 0 : pay.count) + n, s.damage};
            if ((s.count -= n) <= 0) s = {};
            merchant_update_sell(p);
            if (pay.count >= mx) break;
        }
    };
    fill(0, o.a);
    if (o.b.item >= 0) fill(1, o.b);
}

// AbstractVillager.stillValid: trader alive, still trading with p, within interaction range + 4.
inline void merchant_player_tick(Player& p) {
    if (!in_merchant(p)) return;
    double x = 0, y = 0, z = 0;
    MerchantState* m = merchant_lookup ? merchant_lookup(p.g.merchant_eid, x, y, z) : nullptr;
    double dx = p.x - x, dy = p.y + 0.9 - y, dz = p.z - z;  // ponytail: centre distance, not AABB distance
    if (!m || m->trading_player != p.eid || dx * dx + dy * dy + dz * dz > 7.0 * 7.0) close_container(p, true);
}

inline void merchant_menu_init() {
    on(play_sb::select_trade, [](Player& p, Reader& in, int) { merchant_select_trade(p, in.varint()); });
    on_player_tick(merchant_player_tick);
}
