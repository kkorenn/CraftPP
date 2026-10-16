// Roster data / trading / breeding self-checks against vanilla constants. Run from the repo root.
#include "play.hpp"
#include "mobs/breeding.hpp"
#include "mobs/trading.hpp"

#include <cassert>
#include <cstdio>

#define CHECK(c) ((c) ? (void)0 : (fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), std::exit(1)))

int main() {
    CHECK(load_game_data());
    CHECK(load_roster_data());
    const RosterType& cow = roster(entity_types::cow);
    CHECK(cow.name == "cow" && entity_type_id("minecraft:cow") == entity_types::cow);
    CHECK(cow.is_food(item_by_name["minecraft:wheat"]) && !cow.is_food(item_by_name["minecraft:bone"]));
    CHECK(cow.egg == item_by_name["minecraft:cow_spawn_egg"]);
    const RosterType& wolf = roster(entity_types::wolf);
    CHECK(wolf.tame == std::vector<uint16_t>{uint16_t(item_by_name["minecraft:bone"])});
    CHECK(roster(entity_types::horse).is_tempt(item_by_name["minecraft:golden_carrot"]));

    // Breeding: feed two cows, mate, cooldown, baby grows up.
    AgeState a, b;
    LoveState la, lb;
    int wheat = item_by_name["minecraft:wheat"];
    CHECK(animal_feed(cow, a, la, wheat, 1, 2) == FeedResult::in_love);
    CHECK(animal_feed(cow, a, la, wheat, 1, 2) == FeedResult::pass);  // already in love
    CHECK(animal_feed(cow, b, lb, wheat, 1, 2) == FeedResult::in_love);
    CHECK(can_mate(entity_types::cow, la, entity_types::cow, lb) && !can_mate(entity_types::cow, la, entity_types::pig, lb));
    int xp = finish_breeding(a, la, b, lb);
    CHECK(xp >= 1 && xp <= 7 && a.age == 6000 && !la.is_in_love());
    CHECK(animal_feed(cow, a, la, wheat, 1, 2) == FeedResult::pass);  // cooldown
    AgeState baby;
    CHECK(baby.set_age(BABY_START_AGE) && baby.baby());
    CHECK(animal_feed(cow, baby, la, wheat, 1, 2) == FeedResult::aged_up && baby.age == -24000 + 120 * 20);
    bool flipped = false;
    for (int i = 0; i < 30000; i++) flipped |= baby.tick();
    CHECK(flipped && !baby.baby() && baby.age == 0);

    // Taming: a bone eventually tames a wolf and it sits; 1/3 per bone.
    int bone = item_by_name["minecraft:bone"], tamed = 0;
    for (int i = 0; i < 3000; i++) {
        TameState s;
        tamed += try_tame(wolf, s, bone, 7, 8);
        if (s.tame) CHECK(s.ordered_to_sit && s.owned_by(7, 8) && s.flags() == 5);
    }
    CHECK(tamed > 850 && tamed < 1150);

    // Trading: a farmer gets 2 level-1 offers, levels up at 10 XP and gains 2 more.
    MerchantState m;
    m.profession = profession_id("farmer");
    CHECK(m.profession >= 0 && poi_types[size_t(professions[size_t(m.profession)].poi)].name == "farmer");
    CHECK(poi_for_block(block_by_name["minecraft:composter"]) == professions[size_t(m.profession)].poi);
    m.update_trades();
    CHECK(m.offers.size() == 2 && m.offers[0].a.item != m.offers[1].a.item);
    MerchantOffer& o = m.offers[0];
    ItemStack pa{o.a.item, 64, 0}, pb;
    CHECK(o.satisfied_by(pa, pb) && o.take(pa, pb) && pa.count == 64 - o.a.count);
    o.uses++;
    bool lv = false;
    for (int i = 0; i < 10 && !lv; i++) m.reward_trade_xp(o, lv);
    CHECK(lv && m.level == 2 && m.offers.size() == 4);
    MerchantOffer& o2 = m.offers[0];  // update_trades reallocated
    // Demand raises the price after a restock; reputation lowers it.
    o2.uses = o2.max_uses;
    m.restock(100);
    CHECK(o2.uses == 0 && o2.demand > 0);
    m.update_special_prices(50);
    CHECK(o2.special_price == -int(std::floor(50.0f * o2.price_multiplier)));
    Writer w = merchant_offers_packet(1, m.offers, m.level, m.xp, true, true);
    CHECK(w.b.size() > 20);

    // Gossip: trading reputation, max cap, decay.
    GossipContainer g;
    g.add(1, 1, GossipType::trading, 2);
    g.add(1, 1, GossipType::major_negative, 30);
    CHECK(g.reputation(1, 1) == 2 - 150);
    for (int i = 0; i < 20; i++) g.add(1, 1, GossipType::trading, 2);
    CHECK(g.find(1, 1, GossipType::trading)->value == 25);
    g.decay();
    CHECK(g.find(1, 1, GossipType::trading)->value == 23 && g.find(1, 1, GossipType::major_negative)->value == 20);
    GossipContainer g2;
    CHECK(g2.transfer_from(g, 10) >= 1 && !g2.entries.empty());
    puts("roster_test ok");
}
