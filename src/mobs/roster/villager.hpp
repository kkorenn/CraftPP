#pragma once
// Villager, WanderingTrader, ZombieVillager (npc.villager / npc.wanderingtrader / monster.zombie.ZombieVillager).
// Trading / gossip / restock state: ../trading.hpp (MerchantState), menu: ../merchant_menu.hpp.
// ponytail: vanilla villagers run a Brain (schedules, POI manager with 48-block searches, beds, meeting points,
// farming, breeding, golem summoning, raids). Here a goal set covers wandering, fleeing zombies, trading, job site
// claiming (bounded 8-block scan, staggered), working/restocking at the job site and gossip.
#include "common.hpp"

inline int villager_type_for_biome(int b) {  // VillagerType.byBiome
    using namespace biomes;
    static const std::pair<int, const char*> by_biome[] = {
        {badlands, "desert"}, {desert, "desert"}, {eroded_badlands, "desert"}, {wooded_badlands, "desert"},
        {bamboo_jungle, "jungle"}, {jungle, "jungle"}, {sparse_jungle, "jungle"},
        {savanna_plateau, "savanna"}, {savanna, "savanna"}, {windswept_savanna, "savanna"},
        {deep_frozen_ocean, "snow"}, {frozen_ocean, "snow"}, {frozen_river, "snow"}, {ice_spikes, "snow"},
        {snowy_beach, "snow"}, {snowy_taiga, "snow"}, {snowy_plains, "snow"}, {grove, "snow"},
        {snowy_slopes, "snow"}, {frozen_peaks, "snow"}, {jagged_peaks, "snow"},
        {swamp, "swamp"}, {mangrove_swamp, "swamp"},
        {old_growth_spruce_taiga, "taiga"}, {old_growth_pine_taiga, "taiga"}, {windswept_gravelly_hills, "taiga"},
        {windswept_hills, "taiga"}, {taiga, "taiga"}, {windswept_forest, "taiga"}};
    const char* name = "plains";
    for (auto& [id, n] : by_biome)
        if (id == b) name = n;
    for (size_t i = 0; i < villager_types.size(); i++)
        if (villager_types[i] == name) return int(i);
    return 0;
}
inline std::unordered_set<int64_t>& claimed_job_sites() {  // POI tickets (workstations have 1)
    static std::unordered_set<int64_t> s;
    return s;
}
inline int64_t overworld_time() { return clocks[0].total; }
inline bool zombie_type(int t) {
    return t == entity_types::zombie || t == entity_types::zombie_villager || t == entity_types::husk ||
           t == entity_types::drowned || t == entity_types::zoglin || t == entity_types::zombified_piglin;
}
inline bool villager_scary(LivingEntity& e) {  // VillagerHostilesSensor.ACCEPTABLE_DISTANCE_FROM_HOSTILES
    int t = e.type;
    return (zombie_type(t) && t != entity_types::zombified_piglin) || t == entity_types::evoker || t == entity_types::illusioner ||
           t == entity_types::pillager || t == entity_types::ravager || t == entity_types::vex || t == entity_types::vindicator;
}

inline Nbt save_offers(const MerchantOffers& offers) {
    Nbt list = Nbt::make_list();
    auto cost = [](int item, int count) {
        Nbt c = Nbt::compound();
        c.set("id", Nbt::str(item_info[size_t(item)].name));
        c.set("count", Nbt::num(Nbt::Int, count));
        return c;
    };
    for (auto& o : offers) {
        Nbt r = Nbt::compound();
        r.set("buy", cost(o.a.item, o.a.count));
        if (o.b.item >= 0) r.set("buyB", cost(o.b.item, o.b.count));
        r.set("sell", cost(o.result.item, o.result.count));
        r.set("uses", Nbt::num(Nbt::Int, o.uses));
        r.set("maxUses", Nbt::num(Nbt::Int, o.max_uses));
        r.set("rewardExp", Nbt::num(Nbt::Byte, o.reward_exp));
        r.set("specialPrice", Nbt::num(Nbt::Int, o.special_price));
        r.set("demand", Nbt::num(Nbt::Int, o.demand));
        r.set("priceMultiplier", Nbt::real(Nbt::Float, o.price_multiplier));
        r.set("xp", Nbt::num(Nbt::Int, o.xp));
        list.list.push_back(std::move(r));
    }
    Nbt out = Nbt::compound();
    out.set("Recipes", std::move(list));
    return out;
}
inline void load_offers(const Nbt* in, MerchantOffers& offers) {
    offers.clear();
    const Nbt* rs = in ? in->get("Recipes") : nullptr;
    if (!rs) return;
    auto cost = [](const Nbt* c) -> TradeCost {
        if (!c) return {};
        const Nbt* id = c->get("id");
        auto it = id ? item_by_name.find(id->s) : item_by_name.end();
        return it == item_by_name.end() ? TradeCost{} : TradeCost{it->second, int(nbt_num(*c, "count", 1))};
    };
    for (auto& r : rs->list) {
        MerchantOffer o;
        o.a = cost(r.get("buy"));
        o.b = r.get("buyB") ? cost(r.get("buyB")) : TradeCost{};
        TradeCost s = cost(r.get("sell"));
        if (o.a.item < 0 || s.item < 0) continue;
        o.result = {s.item, s.count, 0};
        o.uses = int(nbt_num(r, "uses", 0)), o.max_uses = int(nbt_num(r, "maxUses", 4));
        o.reward_exp = nbt_num(r, "rewardExp", 1) != 0;
        o.special_price = int(nbt_num(r, "specialPrice", 0)), o.demand = int(nbt_num(r, "demand", 0));
        o.price_multiplier = float(nbt_num(r, "priceMultiplier", 0)), o.xp = int(nbt_num(r, "xp", 1));
        offers.push_back(o);
    }
}
inline Nbt save_gossips(const GossipContainer& g) {
    Nbt list = Nbt::make_list();
    for (auto& e : g.entries) {
        Nbt c = Nbt::compound();
        c.set("Target", uuid_nbt(e.uh, e.ul));
        c.set("Type", Nbt::str(gossip_info[int(e.type)].id));
        c.set("Value", Nbt::num(Nbt::Int, e.value));
        list.list.push_back(std::move(c));
    }
    return list;
}
inline void load_gossips(const Nbt* in, GossipContainer& g) {
    g.entries.clear();
    if (!in) return;
    for (auto& c : in->list) {
        int64_t hi, lo;
        const Nbt* t = c.get("Type");
        if (!uuid_from_nbt(c.get("Target"), hi, lo) || !t) continue;
        for (int k = 0; k < 5; k++)
            if (t->s == gossip_info[k].id) g.entries.push_back({hi, lo, GossipType(k), int(nbt_num(c, "Value", 0))});
    }
}

// ---------- AbstractVillager ----------
struct AbstractVillager : AgeableMob {
    MerchantState m;
    bool offers_ready = false;

    virtual void update_trades() = 0;
    MerchantOffers& offers() {  // AbstractVillager.getOffers: generated on first use
        if (!offers_ready) offers_ready = true, update_trades();
        return m.offers;
    }
    bool is_trading() const { return m.trading_player >= 0; }
    Player* trading_player() const {
        for (auto& p : players)
            if (p->eid == m.trading_player) return p.get();
        return nullptr;
    }
    void stop_trading() {
        if (Player* p = trading_player(); p && merchant_menu_of(*p) && merchant_menu_of(*p)->trader == id) close_container(*p, true);
        m.trading_player = -1;
    }
    void set_unhappy() {  // setUnhappy
        data.set_int(edata::AbstractVillager::UNHAPPY_COUNTER, 40);
        make_sound("minecraft:entity.villager.no");
    }
    void custom_server_ai_step() override {
        AgeableMob::custom_server_ai_step();
        int u = int(data.get(edata::AbstractVillager::UNHAPPY_COUNTER, 0));
        if (u > 0) data.set_int(edata::AbstractVillager::UNHAPPY_COUNTER, u - 1);
        if (is_trading()) {
            Player* p = trading_player();
            if (!p || !merchant_menu_of(*p) || merchant_menu_of(*p)->trader != id || p->g.dead) m.trading_player = -1;
        }
    }
    void die(const DamageSource& s) override { stop_trading(); AgeableMob::die(s); }
    void remove(Removal r) override { stop_trading(); AgeableMob::remove(r); }
    bool remove_when_far_away(double) override { return false; }
    std::string title_key() const { return "entity.minecraft." + info().id; }
};

// Look at / stand still for the trading player (TradeWithPlayerGoal; villagers do it from the brain).
struct TradeWithPlayerGoal : Goal {
    AbstractVillager& v;
    explicit TradeWithPlayerGoal(AbstractVillager& vv) : v(vv) { flags = GF_MOVE | GF_LOOK | GF_JUMP; }
    bool can_use() override { return v.is_trading() && v.on_ground && !v.is_in_water(); }
    void start() override { v.navigation->stop(); }
    void tick() override {
        if (Player* p = v.trading_player())
            if (PlayerEntity* pe = player_entity(*p)) v.look_control->set_look_at(*pe, 10.0f, float(v.max_head_x_rot()));
    }
    bool requires_update_every_tick() override { return true; }
};

// ---------- Villager ----------
struct Villager : AbstractVillager {
    int64_t job = INT64_MIN;  // claimed job site (packed position)
    int job_x = 0, job_y = 0, job_z = 0;

    int profession() const { return m.profession; }
    bool is_nitwit() const { return m.profession >= 0 && professions[size_t(m.profession)].name == "nitwit"; }
    bool unemployed() const { return m.profession < 0 || professions[size_t(m.profession)].name == "none"; }
    void sync_villager_data() {
        Writer w;
        write_villager_data(w, m.villager_type, m.profession < 0 ? profession_id("none") : m.profession, m.level);
        data.set_raw(edata::Villager::VILLAGER_DATA, edser::VILLAGER_DATA, w.b);
    }
    void set_profession(int prof) {
        m.profession = prof;
        sync_villager_data();
    }
    void init() override {
        AbstractVillager::init();
        m.profession = profession_id("none");
        sync_villager_data();
        navigation->set_can_open_doors(true);
    }
    void update_trades() override { m.update_trades(); }
    std::string ambient_sound() const override {
        if (is_trading()) return "minecraft:entity.villager.trade";
        return "minecraft:entity.villager.ambient";
    }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.villager.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.villager.death"; }
    void register_goals() override {
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        goal_selector.add(1, std::make_unique<TradeWithPlayerGoal>(*this));
        goal_selector.add(1, std::make_unique<AvoidEntityGoal>(*this, villager_scary, 8.0f, 0.6, 0.6));
        goal_selector.add(1, std::make_unique<PanicGoal>(*this, 0.6));
        goal_selector.add(3, std::make_unique<GoToJobSiteGoal>(*this));
        goal_selector.add(8, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 0.6));
        goal_selector.add(9, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(10, std::make_unique<RandomLookAroundGoal>(*this));
    }
    bool work_hours() const { int64_t t = overworld_time() % 24000; return t >= 2000 && t < 9000; }  // Schedule.VILLAGER_DEFAULT WORK
    struct GoToJobSiteGoal : Goal {
        Villager& v;
        explicit GoToJobSiteGoal(Villager& vv) : v(vv) { flags = GF_MOVE; }
        double d2() const { double dx = v.job_x + 0.5 - v.pos.x, dz = v.job_z + 0.5 - v.pos.z; return dx * dx + dz * dz; }
        bool can_use() override { return v.job != INT64_MIN && v.work_hours() && !v.is_trading() && d2() > 4.0 && rand_int(20) == 0; }
        bool can_continue_to_use() override { return !v.navigation->is_done() && d2() > 2.0 && !v.is_trading(); }
        void start() override { v.navigation->move_to(v.job_x + 0.5, v.job_y, v.job_z + 0.5, 1, 0.5); }
        void stop() override { v.navigation->stop(); }
    };
    void release_job() {
        if (job != INT64_MIN) claimed_job_sites().erase(job);
        job = INT64_MIN;
    }
    // Workstation claiming (AcquirePoi(job sites) + AssignProfessionFromJobSite) over a bounded box.
    void look_for_job() {
        if (is_baby() || is_nitwit()) return;
        int bx = block_x(), by = block_y(), bz = block_z();
        int want = unemployed() ? -1 : professions[size_t(m.profession)].poi;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -8; dx <= 8; dx++)
                for (int dz = -8; dz <= 8; dz++) {
                    int x = bx + dx, y = by + dy, z = bz + dz;
                    if (!chunk_loaded_at(x, z)) continue;
                    int poi = poi_for_block(block_index(block_at(x, y, z)));
                    if (poi < 0 || (want >= 0 && poi != want) || claimed_job_sites().count(int64_t(pack_pos(x, y, z)))) continue;
                    int prof = -1;
                    for (size_t i = 0; i < professions.size(); i++)
                        if (professions[i].poi == poi) prof = int(i);
                    if (prof < 0) continue;
                    job = int64_t(pack_pos(x, y, z)), job_x = x, job_y = y, job_z = z;
                    claimed_job_sites().insert(job);
                    if (unemployed()) {
                        set_profession(prof);
                        m.level = std::max(m.level, 1);
                        sync_villager_data();
                        offers_ready = false;  // trades are rolled on first trade (getOffers)
                        m.offers.clear();
                        play_sound("minecraft:entity.villager.work_" + professions[size_t(prof)].name, 1.0f, 1.0f);
                    }
                    return;
                }
    }
    void check_job_site() {  // the workstation is gone: lose it; untraded villagers lose their profession
        if (job == INT64_MIN || !chunk_loaded_at(job_x, job_z)) return;
        int poi = poi_for_block(block_index(block_at(job_x, job_y, job_z)));
        if (poi >= 0 && !unemployed() && poi == professions[size_t(m.profession)].poi) return;
        release_job();
        if (m.xp == 0 && m.level <= 1 && !unemployed()) {
            set_profession(profession_id("none"));
            m.offers.clear();
            offers_ready = false;
        }
    }
    void custom_server_ai_step() override {
        AbstractVillager::custom_server_ai_step();
        if (m.ai_step()) broadcast_event(14);
        m.maybe_decay_gossip(game_time);
        if (((tick_count + id) % 40) == 0) {
            check_job_site();
            if (job == INT64_MIN) look_for_job();
        }
        // WorkAtPoi: at the job site in work hours, restock (Villager.shouldRestock) and play the work sound.
        if (job != INT64_MIN && work_hours() && (tick_count + id) % 20 == 0) {
            double dx = job_x + 0.5 - pos.x, dz = job_z + 0.5 - pos.z;
            if (dx * dx + dz * dz < 4.0 && offers_ready && m.should_restock(game_time, overworld_time() / 24000)) {
                m.restock(game_time);
                if (Player* p = trading_player()) {
                    m.update_special_prices(m.gossips.reputation(p->uuid_hi, p->uuid_lo));
                    send_merchant_offers(*p, m);
                }
                play_sound("minecraft:entity.villager.work_" + professions[size_t(m.profession)].name, 1.0f, 1.0f);
            }
        }
        // GossipGoal stand-in: every 60 s talk to one villager within 4 blocks (Villager.gossip).
        if ((tick_count + id) % 1200 == 0) {
            Villager* other = nullptr;
            for_each_entity_in(bb.inflate(4.0), [&](Entity& e) {
                if (!other && &e != this && e.type == entity_types::villager) other = static_cast<Villager*>(&e);
            }, false);
            if (other) gossip_with(*other);
        }
        if ((m.offers.empty() && offers_ready) && is_trading()) stop_trading();
    }
    void gossip_with(Villager& t) {
        int64_t now = game_time;
        if ((now < m.last_gossip_time || now >= m.last_gossip_time + 1200) &&
            (now < t.m.last_gossip_time || now >= t.m.last_gossip_time + 1200)) {
            m.gossips.transfer_from(t.m.gossips, 10);
            m.last_gossip_time = t.m.last_gossip_time = now;
        }
    }
    int mob_interact(Player& p, int hand) override {  // Villager.mobInteract
        int egg = item_by_name["minecraft:villager_spawn_egg"];
        if (held_item(p, hand) == egg || !is_alive() || is_trading()) return AbstractVillager::mob_interact(p, hand);
        if (is_baby()) {
            set_unhappy();
            return IR_SUCCESS;
        }
        if (unemployed() || is_nitwit()) {  // no trades
            if (hand == 0) set_unhappy();
            return IR_CONSUME;
        }
        bool none = offers().empty();
        if (hand == 0 && none) set_unhappy();
        if (none) return IR_CONSUME;
        int hero_amp = -1;
        for (auto& eff : p.g.effects) if (eff.id == eff::hero_of_the_village) { hero_amp = eff.amp; break; }
        m.update_special_prices(m.gossips.reputation(p.uuid_hi, p.uuid_lo), hero_amp);
        open_merchant(p, id, m, "entity.minecraft.villager." + professions[size_t(m.profession)].name);
        return IR_SUCCESS;
    }
    bool hurt_server(const DamageSource& s, float dmg) override {
        if (!AbstractVillager::hurt_server(s, dmg)) return false;
        if (s.causing && s.causing->as_player()) {  // ReputationEventType.VILLAGER_HURT
            m.gossips.add(s.causing->uuid_hi, s.causing->uuid_lo, GossipType::minor_negative, 25);
            if (Player* tp = trading_player(); tp && tp->uuid_hi == s.causing->uuid_hi && tp->uuid_lo == s.causing->uuid_lo) {
                int hero_amp = -1;
                for (auto& eff : tp->g.effects) if (eff.id == eff::hero_of_the_village) { hero_amp = eff.amp; break; }
                m.update_special_prices(m.gossips.reputation(tp->uuid_hi, tp->uuid_lo), hero_amp), send_merchant_offers(*tp, m);
            }
        }
        return true;
    }
    void thunder_hit(Entity* lightning) override {
        if (difficulty() == 0) return;
        release_job();
        stop_trading();
        auto e = create_entity(entity_types::witch);
        if (e) {
            e->snap_to(pos.x, pos.y, pos.z, yaw, pitch);
            if (auto* m = dynamic_cast<Mob*>(e.get())) {
                m->finalize_spawn(SpawnReason::CONVERSION, nullptr);
                m->set_persistence_required();
            }
            remove(Removal::DISCARDED);
            add_entity(std::move(e));
        } else {
            Entity::thunder_hit(lightning);
        }
    }
    void die(const DamageSource& s) override;  // zombie conversion (roster.cpp)
    void remove(Removal r) override {
        if (r != Removal::UNLOADED_TO_CHUNK) release_job();
        else if (job != INT64_MIN) claimed_job_sites().erase(job);
        AbstractVillager::remove(r);
    }
    SpawnGroupData* finalize_spawn(SpawnReason r, SpawnGroupData* g) override {
        m.villager_type = villager_type_for_biome(biome_at_pos(block_x(), block_y(), block_z()));
        sync_villager_data();
        return AbstractVillager::finalize_spawn(r, g);
    }
    std::unique_ptr<Entity> breed_offspring(AgeableMob&) override { return create_entity(type); }
    void save_extra(Nbt& o) const override {
        AbstractVillager::save_extra(o);
        Nbt vd = Nbt::compound();
        vd.set("type", Nbt::str("minecraft:" + villager_types[size_t(m.villager_type)]));
        vd.set("profession", Nbt::str("minecraft:" + (m.profession < 0 ? std::string("none") : professions[size_t(m.profession)].name)));
        vd.set("level", Nbt::num(Nbt::Int, m.level));
        o.set("VillagerData", std::move(vd));
        o.set("Xp", Nbt::num(Nbt::Int, m.xp));
        if (offers_ready) o.set("Offers", save_offers(m.offers));
        o.set("Gossips", save_gossips(m.gossips));
        o.set("LastRestock", Nbt::num(Nbt::Long, m.last_restock_time));
        o.set("RestocksToday", Nbt::num(Nbt::Int, m.restocks_today));
        o.set("LastGossipDecay", Nbt::num(Nbt::Long, m.last_gossip_decay));
    }
    void load_extra(const Nbt& in) override {
        AbstractVillager::load_extra(in);
        if (const Nbt* vd = in.get("VillagerData")) {
            auto strip = [](const Nbt* s) { return s && s->t == Nbt::String ? (s->s.rfind("minecraft:", 0) == 0 ? s->s.substr(10) : s->s) : ""; };
            std::string t = strip(vd->get("type")), p = strip(vd->get("profession"));
            for (size_t i = 0; i < villager_types.size(); i++)
                if (villager_types[i] == t) m.villager_type = int(i);
            if (int pid = profession_id(p); pid >= 0) m.profession = pid;
            m.level = std::clamp(int(nbt_num(*vd, "level", 1)), 1, 5);
        }
        m.xp = int(nbt_num(in, "Xp", 0));
        if (const Nbt* o = in.get("Offers")) load_offers(o, m.offers), offers_ready = true;
        load_gossips(in.get("Gossips"), m.gossips);
        m.last_restock_time = int64_t(nbt_num(in, "LastRestock", 0));
        m.restocks_today = int(nbt_num(in, "RestocksToday", 0));
        m.last_gossip_decay = int64_t(nbt_num(in, "LastGossipDecay", 0));
        sync_villager_data();
    }
};

// ---------- WanderingTrader ----------
struct WanderingTrader : AbstractVillager {
    int despawn_delay = 0;
    void init() override { AbstractVillager::init(); m.levels = false; }
    void update_trades() override {  // WanderingTrader.updateTrades: buying, uncommon, common
        for (const char* s : {"wandering_trader/buying", "wandering_trader/uncommon", "wandering_trader/common"})
            add_offers_from_trade_set(m.offers, trade_set_id(s), 0);
    }
    std::string ambient_sound() const override { return is_trading() ? "minecraft:entity.wandering_trader.trade" : "minecraft:entity.wandering_trader.ambient"; }
    std::string hurt_sound(const DamageSource&) const override { return "minecraft:entity.wandering_trader.hurt"; }
    std::string death_sound() const override { return "minecraft:entity.wandering_trader.death"; }
    void register_goals() override {  // WanderingTrader.registerGoals (minus drinking potions/milk)
        goal_selector.add(0, std::make_unique<FloatGoal>(*this));
        goal_selector.add(1, std::make_unique<TradeWithPlayerGoal>(*this));
        goal_selector.add(1, std::make_unique<AvoidEntityGoal>(*this, villager_scary, 8.0f, 0.5, 0.5));
        goal_selector.add(1, std::make_unique<PanicGoal>(*this, 0.5));
        goal_selector.add(8, std::make_unique<WaterAvoidingRandomStrollGoal>(*this, 0.35));
        goal_selector.add(9, std::make_unique<LookAtPlayerGoal>(*this, 8.0f));
        goal_selector.add(10, std::make_unique<RandomLookAroundGoal>(*this));
    }
    bool is_baby() const override { return false; }
    int mob_interact(Player& p, int hand) override {  // WanderingTrader.mobInteract
        if (held_item(p, hand) == item_by_name["minecraft:wandering_trader_spawn_egg"] || !is_alive() || is_trading())
            return AbstractVillager::mob_interact(p, hand);
        if (hand == 0) make_sound("minecraft:entity.wandering_trader.yes");
        if (offers().empty()) return IR_CONSUME;
        open_merchant(p, id, m, "entity.minecraft.wandering_trader");
        return IR_SUCCESS;
    }
    void custom_server_ai_step() override {
        AbstractVillager::custom_server_ai_step();
        m.traded = false;  // no reputation for wandering traders
        if (despawn_delay > 0 && !is_trading() && --despawn_delay == 0) remove(Removal::DISCARDED);  // maybeDespawn
    }
    void save_extra(Nbt& o) const override {
        AbstractVillager::save_extra(o);
        if (offers_ready) o.set("Offers", save_offers(m.offers));
        o.set("DespawnDelay", Nbt::num(Nbt::Int, despawn_delay));
    }
    void load_extra(const Nbt& in) override {
        AbstractVillager::load_extra(in);
        if (const Nbt* o = in.get("Offers")) load_offers(o, m.offers), offers_ready = true;
        despawn_delay = int(nbt_num(in, "DespawnDelay", 0));
    }
};
