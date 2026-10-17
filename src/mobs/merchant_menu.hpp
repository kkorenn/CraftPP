#pragma once
// MerchantMenu / MerchantContainer / MerchantResultSlot port on the shared Menu infrastructure (inventory.hpp),
// select_trade, and opening the trading screen. Traders register a lookup in `merchant_lookup`.
#include "../game/inventory.hpp"
#include "trading.hpp"

// Entity id -> its MerchantState and position (nullptr: gone/dead). Set by the villager/wandering trader code.
inline MerchantState* (*merchant_lookup)(int eid, double& x, double& y, double& z) = nullptr;

inline void send_merchant_offers(Player& p, MerchantState& m) {
    if (!m.offers.empty())
        p.send(merchant_offers_packet(p.g.container, m.offers, m.level, m.xp, m.profession >= 0 && m.levels, m.levels));
}

// Slots: 0-1 payment, 2 result, then the player inventory (Menu adds 27 main + 9 hotbar: 3-29, 30-38).
struct MerchantMenu : Menu {
    int trader;  // entity id
    std::array<ItemStack, 3> items;
    int active = -1, hint = 0;

    explicit MerchantMenu(int eid) : trader(eid) { type = menu_ids["merchant"]; }
    MerchantState* m() const {
        double x, y, z;
        return merchant_lookup ? merchant_lookup(trader, x, y, z) : nullptr;
    }
    int size() const override { return 3; }
    ItemStack& at(int i) override { return items[size_t(i)]; }
    bool may_place(int i, const ItemStack&) override { return i != 2; }
    bool pick_all_ok(int) override { return false; }  // MerchantMenu.canTakeItemForPickAll
    bool may_pickup(Player&, int i) override {         // MerchantResultSlot.mayPickup
        MerchantState* s = m();
        if (i != 2) return true;
        if (!s || active < 0 || size_t(active) >= s->offers.size()) return false;
        auto& o = s->offers[size_t(active)];
        return o.satisfied_by(items[0], items[1]) || o.satisfied_by(items[1], items[0]);
    }
    void changed(Player&, int i) override { if (i < 2) update_sell(); }
    // MerchantContainer.updateSellItem.
    void update_sell() {
        active = -1;
        MerchantState* s = m();
        const ItemStack& a = items[0].empty() ? items[1] : items[0];
        ItemStack b = items[0].empty() ? ItemStack{} : items[1];
        if (a.empty() || !s) { items[2] = {}; return; }
        if (s->offers.empty()) return;
        auto find = [&](const ItemStack& x, const ItemStack& y) -> int {  // MerchantOffers.getRecipeFor
            if (hint > 0 && hint < int(s->offers.size())) return s->offers[size_t(hint)].satisfied_by(x, y) ? hint : -1;
            for (size_t k = 0; k < s->offers.size(); k++)
                if (s->offers[k].satisfied_by(x, y)) return int(k);
            return -1;
        };
        int o = find(a, b);
        if (o < 0 || s->offers[size_t(o)].out_of_stock()) active = o, o = find(b, a);
        if (o >= 0 && !s->offers[size_t(o)].out_of_stock()) active = o, items[2] = s->offers[size_t(o)].result;
        else items[2] = {};
    }
    // MerchantResultSlot.onTake -> AbstractVillager.notifyTrade -> Villager.rewardTradeXp.
    // ponytail: no Regeneration effect on level-up (mob effects aren't ported).
    void on_take(Player& p, int i, const ItemStack&) override {
        if (i != 2) return;
        MerchantState* s = m();
        if (!s || active < 0 || size_t(active) >= s->offers.size()) return;
        MerchantOffer& o = s->offers[size_t(active)];
        if (o.take(items[0], items[1]) || o.take(items[1], items[0])) {
            o.uses++;
            bool levelled = false;
            int orb = s->reward_trade_xp(o, levelled);
            double x = p.x, y = p.y, z = p.z;
            if (merchant_lookup) merchant_lookup(trader, x, y, z);
            if (orb > 0) spawn_xp_orbs(x, y + 0.5, z, orb);
            s->last_traded_uh = p.uuid_hi, s->last_traded_ul = p.uuid_lo, s->traded = true;
            if (Entity* ve = entity_by_id(trader)) progress::villager_trade(p, *ve, o.result);
            if (levelled) {
                int hero_amp = -1;
                for (auto& eff : p.g.effects) if (eff.id == eff::hero_of_the_village) { hero_amp = eff.amp; break; }
                s->update_special_prices(s->gossips.reputation(p.uuid_hi, p.uuid_lo), hero_amp);
                send_merchant_offers(p, *s);
            }
        }
        update_sell();
    }
    ItemStack quick_move(Player& p, int i) override {  // MerchantMenu.quickMoveStack
        SlotRef r = menu_slot(p, i);
        if (r.s->empty()) return {};
        ItemStack stack = *r.s, clicked = stack;
        bool ok = i == 2 ? move_to(p, stack, 3, 39, true)
                  : i < 2  ? move_to(p, stack, 3, 39, false)
                  : i < 30 ? move_to(p, stack, 30, 39, false)
                           : move_to(p, stack, 3, 30, false);
        if (!ok || stack.count == clicked.count) return {};
        if (i == 2) {
            on_take(p, 2, clicked);  // vanilla refills the result slot from the offer; a remainder is discarded
        } else {
            *r.s = stack;
            if (i < 2) update_sell();
        }
        return clicked;
    }
    bool still_valid(Player& p) override {  // AbstractVillager.stillValid: alive, trading with p, within reach + 4
        double x = 0, y = 0, z = 0;
        MerchantState* s = merchant_lookup ? merchant_lookup(trader, x, y, z) : nullptr;
        double dx = p.x - x, dy = p.y + 0.9 - y, dz = p.z - z;  // ponytail: centre distance, not AABB distance
        return s && s->trading_player == p.eid && dx * dx + dy * dy + dz * dz <= 7.0 * 7.0;
    }
    void removed(Player& p) override {  // MerchantMenu.removed + setTradingPlayer(null); payment back to the player
        if (MerchantState* s = m(); s && s->trading_player == p.eid) s->trading_player = -1;
        for (int k = 0; k < 2; k++) {
            ItemStack& s = items[size_t(k)];
            if (s.empty()) continue;
            if (inventory_add(p, s), s.count > 0) player_drop(p, s, false);
            s = {};
        }
    }
    // MerchantMenu.tryMoveItems + moveFromInventoryToPaymentSlot.
    void select_trade(Player& p, int index) {
        MerchantState* s = m();
        if (!s) return;
        hint = index;
        update_sell();
        if (index < 0 || size_t(index) >= s->offers.size()) return;
        for (int k = 0; k < 2; k++) {
            if (items[size_t(k)].empty()) continue;
            if (!move_to(p, items[size_t(k)], 3, 39, true)) return;
            update_sell();
        }
        if (!items[0].empty() || !items[1].empty()) return;
        const MerchantOffer o = s->offers[size_t(index)];
        auto fill = [&](int k, const TradeCost& c) {
            ItemStack& pay = items[size_t(k)];
            for (int i = 3; i < 39; i++) {
                ItemStack& st = *menu_slot(p, i).s;
                if (st.empty() || st.item != c.item || !(pay.empty() || pay.same(st))) continue;
                int mx = max_stack(st), have = pay.empty() ? 0 : pay.count, n = std::min(mx - have, st.count);
                pay = st.copy(have + n);
                if ((st.count -= n) <= 0) st = {};
                update_sell();
                if (pay.count >= mx) break;
            }
        };
        fill(0, o.a);
        if (o.b.item >= 0) fill(1, o.b);
    }
};

// Villager.startTrading / Merchant.openTradingScreen. `title_key`: entity.minecraft.villager.<profession> etc.
inline void open_merchant(Player& p, int eid, MerchantState& m, const std::string& title_key) {
    int hero_amp = -1;
    for (auto& eff : p.g.effects) if (eff.id == eff::hero_of_the_village) { hero_amp = eff.amp; break; }
    m.update_special_prices(m.gossips.reputation(p.uuid_hi, p.uuid_lo), hero_amp);
    Writer t;
    t.u8(0x0A).u8(0x08).u8(0).u8(9).bytes("translate", 9);  // {translate: title_key}
    t.u8(uint8_t(title_key.size() >> 8)).u8(uint8_t(title_key.size())).bytes(title_key.data(), title_key.size()).u8(0);
    open_menu(p, std::make_shared<MerchantMenu>(eid), t.b);
    m.trading_player = p.eid;
    send_merchant_offers(p, m);
}
inline MerchantMenu* merchant_menu_of(Player& p) { return dynamic_cast<MerchantMenu*>(p.g.menu.get()); }

inline void merchant_menu_init() {
    on(play_sb::select_trade, [](Player& p, Reader& in, int) {
        int i = in.varint();
        if (MerchantMenu* mm = merchant_menu_of(p); mm && mm->still_valid(p)) mm->select_trade(p, i);
    });
}
