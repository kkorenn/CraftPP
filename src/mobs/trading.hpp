#pragma once
// Villager trading (world/item/trading, npc/villager, ai/gossip ports): trade data from data/roster.json,
// MerchantOffer, offer generation from trade sets, demand/restock/special prices, gossip and the
// merchant_offers packet. Entity-agnostic: a villager/wandering trader embeds a MerchantState.
#include "../game/entity.hpp"
#include "../worldgen/json.hpp"

#include <array>
#include <optional>

struct TradeCost {
    int item = -1, count = 0;
};
struct VillagerTrade {
    TradeCost wants, b;  // b.item < 0: no additional_wants
    int gives = -1, gives_count = 1, max_uses = 4, xp = 1;
    float reputation_discount = 0;
    std::vector<uint8_t> variants;  // merchant_predicate villager/variant (villager_type ids); empty = any
    bool unsupported = false;       // needs item components (enchantments, dyes, potions, maps) we don't model yet
};
struct TradeSet {
    std::vector<uint16_t> trades;
    int amount = 0;
    bool allow_duplicates = false;
};
struct PoiType {
    std::string name;
    std::vector<uint16_t> blocks;  // block indices (data.hpp block_info)
    int max_tickets = 0, valid_range = 1;
};
struct Profession {
    std::string name;
    int id = 0;                         // villager_profession protocol id
    int poi = -1;                       // index into poi_types; -1 none/nitwit
    std::array<int, 6> trade_set{-1, -1, -1, -1, -1, -1};  // by level 1..5
    std::vector<uint16_t> requested;    // extra items it picks up (farmer seeds/wheat/bone meal)
    std::vector<uint16_t> secondary_poi;  // block indices (farmer: farmland)
};

inline std::vector<VillagerTrade> villager_trades;
inline std::vector<TradeSet> trade_sets;
inline std::vector<std::string> trade_set_names;
inline std::vector<PoiType> poi_types;
inline std::vector<Profession> professions;      // in villager_profession registry order is not needed: by name
inline std::vector<std::string> villager_types;  // registry order = protocol id

inline int trade_set_id(const std::string& name) {
    for (size_t i = 0; i < trade_set_names.size(); i++)
        if (trade_set_names[i] == name) return int(i);
    return -1;
}
inline int profession_id(const std::string& name) {
    for (size_t i = 0; i < professions.size(); i++)
        if (professions[i].name == name) return int(i);
    return -1;
}
inline int poi_for_block(int block) {  // PoiTypes.forState
    for (size_t i = 0; i < poi_types.size(); i++)
        if (std::find(poi_types[i].blocks.begin(), poi_types[i].blocks.end(), uint16_t(block)) != poi_types[i].blocks.end())
            return int(i);
    return -1;
}

inline bool load_trading(const std::string& path = "data/roster.json") {
    Json j;
    try {
        j = Json::parseFile(path);
    } catch (const std::exception& e) {
        fprintf(stderr, "%s: %s (run: python3 tools/gen_roster_data.py)\n", path.c_str(), e.what());
        return false;
    }
    for (auto& v : j["villager_types"].arr) villager_types.push_back(v.asString());
    for (auto& t : j["trades"].arr) {
        VillagerTrade vt;
        auto& w = t["wants"];
        vt.wants = {w[0].asInt(), w[1].asInt()};
        vt.unsupported = w.size() > 2 || t.has("mod") || t.has("gives_components");
        if (auto* b = t.find("b")) vt.b = {(*b)[0].asInt(), (*b)[1].asInt()};
        vt.gives = t["gives"][0].asInt(), vt.gives_count = t["gives"][1].asInt();
        vt.max_uses = t["max_uses"].asInt(), vt.xp = t["xp"].asInt();
        vt.reputation_discount = t["disc"].asFloat();
        if (auto* vs = t.find("variants"))
            for (auto& v : vs->arr)
                for (size_t k = 0; k < villager_types.size(); k++)
                    if (villager_types[k] == v.asString()) vt.variants.push_back(uint8_t(k));
        villager_trades.push_back(std::move(vt));
    }
    for (auto& [name, s] : j["trade_sets"].obj) {
        TradeSet ts;
        for (auto& x : s["trades"].arr) ts.trades.push_back(uint16_t(x.asInt()));
        ts.amount = s["amount"].asInt();
        ts.allow_duplicates = s["dup"].asBool();
        trade_set_names.push_back(name);
        trade_sets.push_back(std::move(ts));
    }
    auto blocks = [](const Json& names) {
        std::vector<uint16_t> v;
        for (auto& b : names.arr)
            if (auto it = block_by_name.find("minecraft:" + b.asString()); it != block_by_name.end()) v.push_back(uint16_t(it->second));
        return v;
    };
    std::vector<std::string> poi_names;
    for (auto& [name, p] : j["poi"].obj) {
        poi_types.push_back({name, blocks(p["blocks"]), p["tickets"].asInt(), p["range"].asInt()});
        poi_names.push_back(name);
    }
    for (auto& [name, p] : j["professions"].obj) {
        Profession pr;
        pr.name = name;
        pr.id = p["id"].asInt();
        if (!p["poi"].isNull())
            for (size_t i = 0; i < poi_names.size(); i++)
                if (poi_names[i] == p["poi"].asString()) pr.poi = int(i);
        for (auto& [lvl, ts] : p["trades"].obj) pr.trade_set[size_t(std::stoi(lvl))] = trade_set_id(ts.asString());
        if (auto* r = p.find("requested"))
            for (auto& x : r->arr) pr.requested.push_back(uint16_t(x.asInt()));
        if (auto* s = p.find("secondary_poi")) pr.secondary_poi = blocks(*s);
        professions.push_back(std::move(pr));
    }
    return true;
}

// ---------- MerchantOffer ----------
struct MerchantOffer {
    TradeCost a, b;  // b.item < 0: none
    ItemStack result;
    int uses = 0, max_uses = 4, xp = 1, special_price = 0, demand = 0;
    float price_multiplier = 0;
    bool reward_exp = true;

    int cost_a() const {  // getModifiedCostCount(baseCostA)
        int demand_diff = std::max(0, int(std::floor(float(a.count * demand) * price_multiplier)));
        return std::clamp(a.count + demand_diff + special_price, 1, max_stack(a.item));
    }
    bool out_of_stock() const { return uses >= max_uses; }
    void update_demand() { demand = demand + uses - (max_uses - uses); }
    bool satisfied_by(const ItemStack& pa, const ItemStack& pb) const {  // ponytail: components are not compared
        if (pa.empty() || pa.item != a.item || pa.count < cost_a()) return false;
        return b.item < 0 ? pb.empty() : !pb.empty() && pb.item == b.item && pb.count >= b.count;
    }
    bool take(ItemStack& pa, ItemStack& pb) const {
        if (!satisfied_by(pa, pb)) return false;
        pa.count -= cost_a();
        if (b.item >= 0) pb.count -= b.count;
        if (pa.count <= 0) pa = {};
        if (pb.count <= 0) pb = {};
        return true;
    }
};
using MerchantOffers = std::vector<MerchantOffer>;

// VillagerTrade.getOffer; nullopt = this trade can't be offered (predicate fails, or unsupported).
inline std::optional<MerchantOffer> make_offer(const VillagerTrade& t, int villager_type) {
    if (t.unsupported) return std::nullopt;
    if (!t.variants.empty() && std::find(t.variants.begin(), t.variants.end(), uint8_t(villager_type)) == t.variants.end())
        return std::nullopt;
    MerchantOffer o;
    o.a = {t.wants.item, std::clamp(t.wants.count, 0, max_stack(t.wants.item))};
    if (o.a.count < 1) return std::nullopt;
    if (t.b.item >= 0) {
        o.b = {t.b.item, std::clamp(t.b.count, 0, max_stack(t.b.item))};
        if (o.b.count < 1) return std::nullopt;
    }
    o.result = {t.gives, t.gives_count, 0};
    o.max_uses = std::max(t.max_uses, 1), o.xp = std::max(t.xp, 0);
    o.price_multiplier = std::max(t.reputation_discount, 0.0f);
    return o;
}

// AbstractVillager.addOffersFromTradeSet. ponytail: rolls game_rng, not vanilla's per-world random sequence.
inline void add_offers_from_trade_set(MerchantOffers& offers, int set, int villager_type) {
    if (set < 0) return;
    const TradeSet& ts = trade_sets[size_t(set)];
    std::vector<uint16_t> pool = ts.trades;
    for (int found = 0; found < ts.amount && !pool.empty();) {
        size_t roll = size_t(rand_int(int(pool.size())));
        auto o = make_offer(villager_trades[pool[roll]], villager_type);
        if (!ts.allow_duplicates || !o) pool.erase(pool.begin() + long(roll));
        if (o) offers.push_back(*o), found++;
    }
}

// ---------- gossip (GossipContainer) ----------
enum class GossipType : uint8_t { major_negative, minor_negative, minor_positive, major_positive, trading };
struct GossipTypeInfo {
    const char* id;
    int weight, max, decay_per_day, decay_per_transfer;
};
inline constexpr GossipTypeInfo gossip_info[5] = {
    {"major_negative", -5, 100, 10, 10}, {"minor_negative", -1, 200, 20, 20}, {"minor_positive", 1, 25, 1, 5},
    {"major_positive", 5, 20, 0, 20},    {"trading", 1, 25, 2, 20}};

struct Gossip {
    int64_t uh, ul;
    GossipType type;
    int value;
};
// Flat vector: villagers know few players; linear scans beat a map at this size and memory.
struct GossipContainer {
    std::vector<Gossip> entries;

    Gossip* find(int64_t uh, int64_t ul, GossipType t) {
        for (auto& g : entries)
            if (g.uh == uh && g.ul == ul && g.type == t) return &g;
        return nullptr;
    }
    int reputation(int64_t uh, int64_t ul) const {
        int r = 0;
        for (auto& g : entries)
            if (g.uh == uh && g.ul == ul) r += g.value * gossip_info[int(g.type)].weight;
        return r;
    }
    void add(int64_t uh, int64_t ul, GossipType t, int amount) {
        const auto& info = gossip_info[int(t)];
        Gossip* g = find(uh, ul, t);
        if (!g) {
            entries.push_back({uh, ul, t, amount});
            g = &entries.back();
        } else {
            int sum = g->value + amount;
            g->value = sum > info.max ? std::max(info.max, g->value) : sum;
        }
        g->value = std::min(g->value, info.max);
        if (g->value < 2) entries.erase(entries.begin() + (g - entries.data()));
    }
    void decay() {
        for (auto& g : entries) g.value -= gossip_info[int(g.type)].decay_per_day;
        std::erase_if(entries, [](const Gossip& g) { return g.value < 2; });
    }
    // selectGossipsForTransfer + transferFrom. Returns the number of distinct gossips picked.
    int transfer_from(const GossipContainer& src, int max_count) {
        if (src.entries.empty()) return 0;
        std::vector<int> ranges(src.entries.size());
        int end = 0;
        for (size_t i = 0; i < src.entries.size(); i++)
            end += std::abs(src.entries[i].value * gossip_info[int(src.entries[i].type)].weight), ranges[i] = end - 1;
        std::vector<size_t> picked;
        for (int i = 0; i < max_count; i++) {
            size_t idx = size_t(std::lower_bound(ranges.begin(), ranges.end(), rand_int(end)) - ranges.begin());
            if (std::find(picked.begin(), picked.end(), idx) == picked.end()) picked.push_back(idx);
        }
        for (size_t idx : picked) {
            const Gossip& s = src.entries[idx];
            int v = s.value - gossip_info[int(s.type)].decay_per_transfer;
            if (v < 2) continue;
            if (Gossip* g = find(s.uh, s.ul, s.type)) g->value = std::max(g->value, v);
            else entries.push_back({s.uh, s.ul, s.type, v});
        }
        return int(picked.size());
    }
};

// ---------- villager trade progression (Villager.java) ----------
inline constexpr int VILLAGER_XP_THRESHOLDS[5] = {0, 10, 70, 150, 250};
inline bool villager_can_level_up(int level) { return level >= 1 && level < 5; }
inline int villager_min_xp(int level) { return villager_can_level_up(level) ? VILLAGER_XP_THRESHOLDS[level - 1] : 0; }
inline int villager_max_xp(int level) { return villager_can_level_up(level) ? VILLAGER_XP_THRESHOLDS[level] : 0; }

struct MerchantState {
    MerchantOffers offers;
    GossipContainer gossips;
    int profession = -1, villager_type = 0, level = 1, xp = 0;
    int restocks_today = 0, trading_player = -1;  // trading player's entity id
    bool traded = false;                          // lastTradedPlayer set (consumed by ai_step)
    bool levels = true;                           // false: wandering trader (no career)
    int64_t last_traded_uh = 0, last_traded_ul = 0;
    int64_t last_restock_time = 0, last_restock_check_day = 0, last_gossip_time = 0, last_gossip_decay = 0;

    // Villager.customServerAiStep trade bookkeeping: ReputationEventType.TRADE gossip. Returns true when the
    // villager should broadcast entity_event 14 (happy particles).
    bool ai_step() {
        if (!traded) return false;
        gossips.add(last_traded_uh, last_traded_ul, GossipType::trading, 2);
        traded = false;
        return true;
    }
    // Villager.maybeDecayGossip.
    void maybe_decay_gossip(int64_t game_time) {
        if (last_gossip_decay == 0) last_gossip_decay = game_time;
        else if (game_time >= last_gossip_decay + 24000) gossips.decay(), last_gossip_decay = game_time;
    }
    // Villager.updateTrades: adds the trade set for the current level.
    void update_trades() {
        if (profession >= 0 && level >= 1 && level <= 5)
            add_offers_from_trade_set(offers, professions[size_t(profession)].trade_set[size_t(level)], villager_type);
    }
    // Villager.updateSpecialPrices (hero_amplifier < 0: no Hero of the Village).
    void update_special_prices(int reputation, int hero_amplifier = -1) {
        double hero = hero_amplifier < 0 ? 0.0 : double(0.3f + 0.0625f * float(hero_amplifier));
        for (auto& o : offers) {
            o.special_price = 0;
            if (reputation != 0) o.special_price += -int(std::floor(float(reputation) * o.price_multiplier));
            if (hero > 0) o.special_price += -std::max(int(std::floor(hero * o.a.count)), 1);
        }
    }
    bool needs_restock() const {
        return std::any_of(offers.begin(), offers.end(), [](const MerchantOffer& o) { return o.uses > 0; });
    }
    bool allowed_to_restock(int64_t game_time) const {
        return restocks_today == 0 || (restocks_today < 2 && game_time > last_restock_time + 2400);
    }
    void update_demand() {
        for (auto& o : offers) o.update_demand();
    }
    void restock(int64_t game_time) {
        update_demand();
        for (auto& o : offers) o.uses = 0;
        last_restock_time = game_time;
        restocks_today++;
    }
    void catch_up_demand() {
        int missed = 2 - restocks_today;
        if (missed > 0)
            for (auto& o : offers) o.uses = 0;
        for (int i = 0; i < missed; i++) update_demand();
    }
    // Villager.shouldRestock; `day` = overworld day count.
    bool should_restock(int64_t game_time, int64_t day) {
        bool new_day = game_time > last_restock_time + 12000;
        new_day |= last_restock_check_day > 0 && day > last_restock_check_day;
        last_restock_check_day = day;
        if (new_day) {
            last_restock_time = game_time;
            catch_up_demand();
            restocks_today = 0;
        }
        return allowed_to_restock(game_time) && needs_restock();
    }
    // Villager.rewardTradeXp minus the orb/effect side effects. Returns the XP orb value; sets `levelled`.
    int reward_trade_xp(const MerchantOffer& o, bool& levelled) {
        int pop = 3 + rand_int(4);
        xp += o.xp;
        levelled = levels && villager_can_level_up(level) && xp >= villager_max_xp(level);
        if (levelled) {
            level++;
            update_trades();
            pop += 5;
        }
        return o.reward_exp ? pop : 0;
    }
};

// VILLAGER_DATA entity data value (serializer villager_data): type, profession, level.
inline Writer& write_villager_data(Writer& w, int villager_type, int profession, int level) {
    return w.varint(villager_type).varint(profession >= 0 ? professions[size_t(profession)].id : 0).varint(level);
}

// ClientboundMerchantOffersPacket. ItemCost = item, count, empty component predicate.
inline Writer merchant_offers_packet(int container, const MerchantOffers& offers, int level, int xp, bool show_progress,
                                     bool can_restock) {
    Writer w;
    w.varint(play_cb::merchant_offers).varint(container).varint(int32_t(offers.size()));
    for (auto& o : offers) {
        w.varint(o.a.item).varint(o.a.count).varint(0);
        write_slot(w, o.result);
        w.boolean(o.b.item >= 0);
        if (o.b.item >= 0) w.varint(o.b.item).varint(o.b.count).varint(0);
        w.boolean(o.out_of_stock()).i32(o.uses).i32(o.max_uses).i32(o.xp).i32(o.special_price).f32(o.price_multiplier)
            .i32(o.demand);
    }
    return std::move(w.varint(level).varint(xp).boolean(show_progress).boolean(can_restock));
}
