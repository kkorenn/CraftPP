#pragma once
// CraftPP Blocks & World Mechanics:
// 1. Sapling growth into full trees (1x1, 2x2, bees, live feature buffering)
// 2. Bonemeal on grass blocks & huge mushrooms
// 3. Signs (front/back text, editor packet 61, update packet 62, waxing, dyeing, glowing/regular ink)
// 4. Lecterns (book insertion/removal, page turn menu, comparator output, 2-tick pulse)
// 5. Jukebox (disc insertion/ejection, record sound events, comparator 1..15)
// 6. Decorated Pots (sherd storage, item storage, cracked vs intact drops, comparator signal)
// 7. Primed TNT (entity type 136, 80-tick fuse, flashing sync, detonation, ignition sources)
// 8. Pistons pushing entities (swept AABB moving players and entities along push axis)

#include "live_level.hpp"
#include "../containers/block_entity.hpp"
#include "../game/inventory.hpp"
#include "../level/redstone.hpp"
#include "../mobs/types/misc.hpp"
#include "../mobs/manager.hpp"
#include "../worldgen/block_state.hpp"

namespace blocks_world {
namespace bet = block_entity_types;

// ==================================================================================================
// 1. Music disc helpers & Jukebox BE
// ==================================================================================================
inline int disc_comparator_output(int item_id) {
    if (item_id < 0 || size_t(item_id) >= item_info.size()) return 0;
    const std::string& name = item_info[size_t(item_id)].name;
    auto pos = name.find("music_disc_");
    if (pos == std::string::npos) return 0;
    std::string disc = name.substr(pos + 11);
    if (disc == "13") return 1;
    if (disc == "cat") return 2;
    if (disc == "blocks") return 3;
    if (disc == "chirp") return 4;
    if (disc == "far") return 5;
    if (disc == "mall") return 6;
    if (disc == "mellohi") return 7;
    if (disc == "stal") return 8;
    if (disc == "strad") return 9;
    if (disc == "ward") return 10;
    if (disc == "11") return 11;
    if (disc == "wait") return 12;
    if (disc == "pigstep") return 13;
    if (disc == "otherside") return 14;
    if (disc == "5") return 15;
    if (disc == "relic") return 14;
    if (disc == "precipice") return 13;
    if (disc == "creator") return 12;
    if (disc == "creator_music_box") return 11;
    if (disc == "tears") return 10;
    if (disc == "lava_chicken") return 9;
    if (disc == "bounce") return 8;
    return 1;
}

inline bool is_disc(int item_id) {
    return disc_comparator_output(item_id) > 0;
}

struct JukeboxBE : be::Live {
    ItemStack record;

    int get_comparator_signal() const override {
        if (record.empty()) return 0;
        return disc_comparator_output(record.item);
    }

    void load(const Nbt& in) override {
        if (const Nbt* it = in.get("RecordItem")) record = item_from(*it);
        else record = {};
    }

    void save(Nbt& out) const override {
        if (!record.empty()) out.set("RecordItem", item_nbt(record));
    }

    void removed_side_effects() override {
        if (!record.empty()) {
            be::drop_stack(x + 0.5, y + 0.5, z + 0.5, record);
            record = {};
            lvl::level_event(1011, lvl::Pos{x, y, z}, 0);
        }
    }
};

// ==================================================================================================
// 2. Books & Lectern BE + Menu
// ==================================================================================================
inline bool is_book_item(int item_id) {
    if (item_id < 0 || size_t(item_id) >= item_info.size()) return false;
    const std::string& name = item_info[size_t(item_id)].name;
    return name == "minecraft:written_book" || name == "minecraft:writable_book";
}

inline int get_book_page_count(const ItemStack& book) {
    if (book.empty()) return 0;
    if (const auto* bytes = comp::get(book, comp::written_book_content())) {
        try {
            Nbt n = comp::nbt_parse(*bytes);
            if (const Nbt* pages = n.get("pages")) {
                if (pages->t == Nbt::List) return std::max(1, int(pages->list.size()));
            }
        } catch (...) {}
    }
    if (const auto* bytes = comp::get(book, comp::writable_book_content())) {
        try {
            Nbt n = comp::nbt_parse(*bytes);
            if (const Nbt* pages = n.get("pages")) {
                if (pages->t == Nbt::List) return std::max(1, int(pages->list.size()));
            }
        } catch (...) {}
    }
    return 1;
}

struct LecternBE : be::Live {
    ItemStack book;
    int page = 0;
    int page_count = 0;

    void update_page_count() {
        page_count = get_book_page_count(book);
        if (page_count > 0) page = std::clamp(page, 0, page_count - 1);
        else page = 0;
    }

    void set_book(ItemStack b) {
        book = std::move(b);
        page = 0;
        update_page_count();
        set_changed();
        be::flush(*this);
        lvl::update_neighbour_for_output_signal(lvl::Pos{x, y, z}, lvl::block(state));
    }

    ItemStack take_book() {
        ItemStack b = book;
        book = {};
        page = 0;
        page_count = 0;
        set_changed();
        be::flush(*this);
        int cur_s = world.get_block(x, y, z);
        int new_s = bs::R().with(bs::R().with(cur_s, "has_book", "false"), "powered", "false");
        world.set_block(x, y, z, uint16_t(new_s));
        broadcast_level(block_update(x, y, z, new_s), cur_level);
        lvl::update_neighbors(lvl::Pos{x, y, z}.below(), lvl::block(cur_s));
        lvl::update_neighbour_for_output_signal(lvl::Pos{x, y, z}, lvl::block(cur_s));
        return b;
    }

    void set_page(int p) {
        if (page_count <= 0) { page = 0; return; }
        int np = std::clamp(p, 0, page_count - 1);
        if (np != page) {
            page = np;
            set_changed();
            be::flush(*this);
            int s = world.get_block(x, y, z);
            int powered_state = bs::R().with(s, "powered", "true");
            world.set_block(x, y, z, uint16_t(powered_state));
            broadcast_level(block_update(x, y, z, powered_state), cur_level);
            lvl::level_event(1043, lvl::Pos{x, y, z}, 0);
            lvl::update_neighbors(lvl::Pos{x, y, z}.below(), lvl::block(s));
            lvl::schedule_block(lvl::Pos{x, y, z}, lvl::block(s), 2, 0);
            lvl::update_neighbour_for_output_signal(lvl::Pos{x, y, z}, lvl::block(s));
        }
    }

    int get_comparator_signal() const override {
        if (book.empty()) return 0;
        if (page_count <= 1) return 15;
        return 1 + int(std::floor(14.0 * double(page) / double(page_count - 1)));
    }

    void load(const Nbt& in) override {
        if (const Nbt* b = in.get("Book")) book = item_from(*b);
        else book = {};
        if (const Nbt* p = in.get("Page")) page = int(p->i);
        else page = 0;
        update_page_count();
    }

    void save(Nbt& out) const override {
        if (!book.empty()) {
            out.set("Book", item_nbt(book));
            out.set("Page", Nbt::num(Nbt::Int, page));
        }
    }

    void removed_side_effects() override {
        if (!book.empty()) {
            be::drop_stack(x + 0.5, y + 1.0, z + 0.5, book);
            book = {};
        }
    }
};

struct LecternMenu : Menu {
    std::shared_ptr<LecternBE> be;
    ItemStack dummy;

    LecternMenu(std::shared_ptr<LecternBE> b) : be(std::move(b)) {
        type = menu_ids["lectern"];
    }

    bool has_inventory() const override { return false; }
    int size() const override { return 1; }
    ItemStack& at(int) override { return be ? be->book : dummy; }
    int data_count() const override { return 1; }
    int data(int i) override { return (i == 0 && be) ? be->page : 0; }
    ItemStack quick_move(Player&, int) override { return ItemStack{}; }

    bool still_valid(Player& p) override {
        if (!be || be->removed || be->book.empty()) return false;
        double dx = p.x - (be->x + 0.5), dy = p.y - (be->y + 0.5), dz = p.z - (be->z + 0.5);
        return dx * dx + dy * dy + dz * dz <= 64.0;
    }

    bool button(Player& p, int btn) override {
        if (!be) return false;
        if (btn >= 100) {
            be->set_page(btn - 100);
            return true;
        }
        if (btn == 1) {
            be->set_page(be->page - 1);
            return true;
        }
        if (btn == 2) {
            be->set_page(be->page + 1);
            return true;
        }
        if (btn == 3) {
            ItemStack b = be->take_book();
            if (!b.empty()) {
                give_item(p, b.item, b.count);
                close_container(p, true);
            }
            return true;
        }
        return false;
    }
};

// ==================================================================================================
// 3. Decorated Pot BE
// ==================================================================================================
struct DecoratedPotBE : be::Live {
    std::vector<std::string> sherds;
    ItemStack item;

    bool full_update_tag() const override { return true; }

    int get_comparator_signal() const override {
        if (item.empty()) return 0;
        int max_st = ::max_stack(item);
        if (max_st <= 0) max_st = 64;
        return 1 + int(std::floor(14.0 * double(item.count) / double(max_st)));
    }

    void load(const Nbt& in) override {
        sherds.clear();
        if (const Nbt* s = in.get("sherds")) {
            if (s->t == Nbt::List) {
                for (auto& elem : s->list) {
                    if (elem.t == Nbt::String) sherds.push_back(elem.s);
                }
            }
        }
        if (const Nbt* it = in.get("item")) {
            item = item_from(*it);
        } else {
            item = {};
        }
    }

    void save(Nbt& out) const override {
        if (!sherds.empty()) {
            Nbt list = Nbt::make_list();
            for (const auto& sh : sherds) list.list.push_back(Nbt::str(sh));
            out.set("sherds", std::move(list));
        }
        if (!item.empty()) {
            out.set("item", item_nbt(item));
        }
    }

    void removed_side_effects() override {
        bool cracked = bs::R().value(state, "cracked") == "true";
        if (cracked) {
            // Drop 4 sherds (or bricks for missing)
            int brick_id = item_by_name["minecraft:brick"];
            for (int i = 0; i < 4; i++) {
                int drop_item = brick_id;
                if (i < int(sherds.size()) && !sherds[size_t(i)].empty()) {
                    auto it = item_by_name.find(sherds[size_t(i)]);
                    if (it != item_by_name.end()) drop_item = it->second;
                }
                if (drop_item >= 0) be::drop_stack(x + 0.5, y + 0.5, z + 0.5, ItemStack{drop_item, 1, 0});
            }
        } else {
            // Drop intact pot
            int pot_id = item_by_name["minecraft:decorated_pot"];
            if (pot_id >= 0) {
                ItemStack pot{pot_id, 1, 0};
                be::drop_stack(x + 0.5, y + 0.5, z + 0.5, pot);
            }
        }
        if (!item.empty()) {
            be::drop_stack(x + 0.5, y + 0.5, z + 0.5, item);
            item = {};
        }
    }
};

// ==================================================================================================
// 4. Signs BE
// ==================================================================================================
inline std::string dye_color(int item_id) {
    if (item_id < 0 || size_t(item_id) >= item_info.size()) return "";
    const std::string& name = item_info[size_t(item_id)].name;
    auto pos = name.rfind("_dye");
    if (pos == std::string::npos || pos + 4 != name.size()) return "";
    auto colon = name.find(':');
    size_t start = colon == std::string::npos ? 0 : colon + 1;
    return name.substr(start, pos - start);
}

struct SignBE : be::Live {
    std::vector<std::string> front_messages{"", "", "", ""};
    std::vector<std::string> back_messages{"", "", "", ""};
    std::string front_color = "black";
    std::string back_color = "black";
    bool front_glowing = false;
    bool back_glowing = false;
    bool is_waxed = false;

    bool full_update_tag() const override { return true; }

    bool is_facing_front(double px, double pz) const {
        double sx = x + 0.5, sz = z + 0.5;
        double xd = px - sx, zd = pz - sz;
        float player_rot = float(std::atan2(zd, xd) * 180.0 / M_PI) - 90.0f;
        float sign_rot = 0.0f;
        if (bs::R().has(state, "rotation")) {
            int r = bs::R().int_value(state, "rotation");
            sign_rot = float(r) * 360.0f / 16.0f;
        } else if (bs::R().has(state, "facing")) {
            std::string f(bs::R().value(state, "facing"));
            if (f == "north") sign_rot = 180.0f;
            else if (f == "south") sign_rot = 0.0f;
            else if (f == "west") sign_rot = 90.0f;
            else if (f == "east") sign_rot = 270.0f;
        }
        float diff = std::fmod(std::abs(sign_rot - player_rot), 360.0f);
        if (diff > 180.0f) diff = 360.0f - diff;
        return diff <= 90.0f;
    }

    void load(const Nbt& in) override {
        auto load_side = [](const Nbt* tag, std::vector<std::string>& msgs, std::string& color, bool& glowing) {
            if (!tag || tag->t != Nbt::Compound) return;
            if (const Nbt* col = tag->get("color")) if (col->t == Nbt::String) color = col->s;
            if (const Nbt* glow = tag->get("has_glowing_text")) glowing = glow->i != 0;
            if (const Nbt* mlist = tag->get("messages")) {
                if (mlist->t == Nbt::List) {
                    msgs.clear();
                    for (size_t i = 0; i < std::min(size_t(4), mlist->list.size()); i++) {
                        const Nbt& item = mlist->list[i];
                        std::string s = item.s;
                        if (s.rfind("{\"text\":\"", 0) == 0 && s.size() >= 11 && s.back() == '}') {
                            s = s.substr(9, s.size() - 11);
                            if (!s.empty() && s.back() == '"') s.pop_back();
                        }
                        msgs.push_back(s);
                    }
                    while (msgs.size() < 4) msgs.push_back("");
                }
            }
        };
        load_side(in.get("front_text"), front_messages, front_color, front_glowing);
        load_side(in.get("back_text"), back_messages, back_color, back_glowing);
        if (const Nbt* wax = in.get("is_waxed")) is_waxed = wax->i != 0;
    }

    void save(Nbt& out) const override {
        auto save_side = [](const std::vector<std::string>& msgs, const std::string& color, bool glowing) -> Nbt {
            Nbt tag = Nbt::compound();
            Nbt list = Nbt::make_list();
            for (const auto& m : msgs) {
                list.list.push_back(Nbt::str("{\"text\":\"" + m + "\"}"));
            }
            tag.set("messages", std::move(list));
            tag.set("color", Nbt::str(color));
            tag.set("has_glowing_text", Nbt::num(Nbt::Byte, glowing ? 1 : 0));
            return tag;
        };
        out.set("front_text", save_side(front_messages, front_color, front_glowing));
        out.set("back_text", save_side(back_messages, back_color, back_glowing));
        out.set("is_waxed", Nbt::num(Nbt::Byte, is_waxed ? 1 : 0));
    }
};

// ==================================================================================================
// 5. Primed TNT Entity
// ==================================================================================================
struct PrimedTnt : Entity {
    int fuse = 80;

    PrimedTnt() = default;

    void init() override {
        Entity::init();
        set_size(0.98f, 0.98f, 0.98f);
        data.define(edata::PrimedTnt::FUSE, fuse);
        data.define(edata::PrimedTnt::BLOCK_STATE, blocks::tnt);
    }

    double default_gravity() const override { return 0.04; }
    bool is_pickable() const override { return !removed; }

    void tick() override {
        base_tick();
        vel.y -= default_gravity();
        move(MoverType::SELF, vel);
        vel = vel * 0.98;
        if (on_ground) {
            vel = vel.multiply(0.7, -0.5, 0.7);
        }
        int f = data.get(edata::PrimedTnt::FUSE, fuse) - 1;
        fuse = f;
        data.set_int(edata::PrimedTnt::FUSE, f);
        if (f <= 0) {
            discard();
            if (rule("tnt_explodes")) {
                explode(this, pos.x, pos.y + 0.0625, pos.z, 4.0f, false, ExplosionInteraction::TNT);
            }
        }
    }

    void save_extra(Nbt& out) const override {
        out.set("fuse", Nbt::num(Nbt::Short, fuse));
    }
    void load_extra(const Nbt& in) override {
        fuse = int(nbt_num(in, "fuse", 80));
        data.set_int(edata::PrimedTnt::FUSE, fuse);
    }
};

// ==================================================================================================
// 6. Interaction dispatch for signs, lecterns, jukebox, decorated pot
// ==================================================================================================
inline bool handle_block_interact(Player& p, int hand, int x, int y, int z, int face) {
    (void)face;
    int state = world.get_block(x, y, z);
    int type = be::block_entity_type(state);
    ItemStack& slot = p.inv[hand == 1 ? 40u : size_t(p.selected)];
    bool have_items = !p.inv[size_t(p.selected)].empty() || !p.inv[40].empty();
    if (p.g.sneaking && have_items) return false;

    // --- Sign ---
    if (type == bet::sign || type == bet::hanging_sign) {
        auto b = std::dynamic_pointer_cast<SignBE>(be::get(x, y, z));
        if (!b) return false;
        bool is_front = b->is_facing_front(p.x, p.z);
        if (!slot.empty()) {
            const std::string& iname = item_info[size_t(slot.item)].name;
            if (iname == "minecraft:honeycomb") {
                if (!b->is_waxed) {
                    b->is_waxed = true;
                    b->set_changed();
                    be::flush(*b);
                    lvl::level_event(3003, lvl::Pos{x, y, z}, 0);
                    if (p.gamemode != 1) slot.count--;
                    if (slot.count <= 0) slot = {};
                    return true;
                }
                return false;
            }
            std::string dye = dye_color(slot.item);
            if (!dye.empty()) {
                if (b->is_waxed) {
                    play_sound("block.sign.waxed_interact_fail", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
                    return true;
                }
                if (is_front) b->front_color = dye;
                else b->back_color = dye;
                b->set_changed();
                be::flush(*b);
                play_sound("item.dye.use", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
                if (p.gamemode != 1) slot.count--;
                if (slot.count <= 0) slot = {};
                return true;
            }
            if (iname == "minecraft:glow_ink_sac") {
                if (b->is_waxed) {
                    play_sound("block.sign.waxed_interact_fail", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
                    return true;
                }
                if (is_front) b->front_glowing = true;
                else b->back_glowing = true;
                b->set_changed();
                be::flush(*b);
                play_sound("item.glow_ink_sac.use", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
                if (p.gamemode != 1) slot.count--;
                if (slot.count <= 0) slot = {};
                return true;
            }
            if (iname == "minecraft:ink_sac") {
                if (b->is_waxed) {
                    play_sound("block.sign.waxed_interact_fail", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
                    return true;
                }
                if (is_front) b->front_glowing = false;
                else b->back_glowing = false;
                b->set_changed();
                be::flush(*b);
                play_sound("item.ink_sac.use", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
                if (p.gamemode != 1) slot.count--;
                if (slot.count <= 0) slot = {};
                return true;
            }
        }
        if (b->is_waxed) {
            play_sound("block.sign.waxed_interact_fail", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
            return true;
        }
        // Open sign editor packet 61
        Writer w;
        w.varint(play_cb::open_sign_editor).i64(int64_t(be::key(x, y, z))).varint(is_front ? 1 : 0);
        p.send(w);
        return true;
    }

    // --- Lectern ---
    if (type == bet::lectern) {
        auto b = std::dynamic_pointer_cast<LecternBE>(be::get(x, y, z));
        if (!b) return false;
        bool has_book = bs::R().value(state, "has_book") == "true";
        if (!has_book && !slot.empty() && is_book_item(slot.item)) {
            b->set_book(slot.copy(1));
            if (p.gamemode != 1) slot.count--;
            if (slot.count <= 0) slot = {};
            int cur_s = world.get_block(x, y, z);
            int new_s = bs::R().with(bs::R().with(cur_s, "has_book", "true"), "powered", "false");
            world.set_block(x, y, z, uint16_t(new_s));
            broadcast_level(block_update(x, y, z, new_s), cur_level);
            play_sound("item.book.put", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
            lvl::update_neighbors(lvl::Pos{x, y, z}.below(), lvl::block(cur_s));
            return true;
        }
        if (has_book) {
            auto m = std::make_shared<LecternMenu>(b);
            // {translate:"container.lectern"}
            std::vector<uint8_t> title = {0x0A, 0x08, 0, 9, 't','r','a','n','s','l','a','t','e', 0, 17, 'c','o','n','t','a','i','n','e','r','.','l','e','c','t','e','r','n', 0};
            open_menu(p, m, title);
            return true;
        }
        return false;
    }

    // --- Jukebox ---
    if (type == bet::jukebox) {
        auto b = std::dynamic_pointer_cast<JukeboxBE>(be::get(x, y, z));
        if (!b) return false;
        bool has_record = bs::R().value(state, "has_record") == "true";
        if (has_record) {
            // Eject record
            if (!b->record.empty()) {
                be::drop_stack(x + 0.5, y + 1.0, z + 0.5, b->record);
                b->record = {};
            }
            int cur_s = world.get_block(x, y, z);
            int new_s = bs::R().with(cur_s, "has_record", "false");
            world.set_block(x, y, z, uint16_t(new_s));
            broadcast_level(block_update(x, y, z, new_s), cur_level);
            lvl::level_event(1011, lvl::Pos{x, y, z}, 0);
            b->set_changed();
            be::flush(*b);
            lvl::update_neighbour_for_output_signal(lvl::Pos{x, y, z}, lvl::block(cur_s));
            return true;
        } else if (!slot.empty() && is_disc(slot.item)) {
            // Insert record
            b->record = slot.copy(1);
            if (p.gamemode != 1) slot.count--;
            if (slot.count <= 0) slot = {};
            int cur_s = world.get_block(x, y, z);
            int new_s = bs::R().with(cur_s, "has_record", "true");
            world.set_block(x, y, z, uint16_t(new_s));
            broadcast_level(block_update(x, y, z, new_s), cur_level);
            lvl::level_event(1010, lvl::Pos{x, y, z}, b->record.item);
            b->set_changed();
            be::flush(*b);
            lvl::update_neighbour_for_output_signal(lvl::Pos{x, y, z}, lvl::block(cur_s));
            return true;
        }
        return false;
    }

    // --- Decorated Pot ---
    if (type == bet::decorated_pot) {
        auto b = std::dynamic_pointer_cast<DecoratedPotBE>(be::get(x, y, z));
        if (!b) return false;
        if (!slot.empty()) {
            int max_st = max_stack(slot);
            if (b->item.empty() || (b->item.item == slot.item && b->item.count < max_st)) {
                if (b->item.empty()) {
                    b->item = slot.copy(1);
                } else {
                    b->item.count++;
                }
                if (p.gamemode != 1) slot.count--;
                if (slot.count <= 0) slot = {};
                b->set_changed();
                be::flush(*b);
                cont::block_event(x, y, z, 1, 0, state); // wobble positive
                float pitch = 0.7f + 0.5f * (float(b->item.count) / float(max_st));
                play_sound("block.decorated_pot.insert", x + 0.5, y + 0.5, z + 0.5, 1.0f, pitch);
                lvl::update_neighbour_for_output_signal(lvl::Pos{x, y, z}, lvl::block(state));
                return true;
            }
        }
        // wobble negative / fail
        cont::block_event(x, y, z, 1, 1, state);
        play_sound("block.decorated_pot.insert_fail", x + 0.5, y + 0.5, z + 0.5, 1.0f, 1.0f);
        return true;
    }

    return false;
}

// ==================================================================================================
// 7. Sign update packet handler
// ==================================================================================================
inline void handle_sign_update(Player& p, int x, int y, int z, const std::vector<std::string>& lines, bool is_front) {
    auto b = std::dynamic_pointer_cast<SignBE>(be::get(x, y, z));
    if (!b || b->is_waxed) return;
    auto& target = is_front ? b->front_messages : b->back_messages;
    target = lines;
    while (target.size() < 4) target.push_back("");
    b->set_changed();
    be::flush(*b);
}

// ==================================================================================================
// 8. Piston moving entities & Dispenser TNT
// ==================================================================================================
inline void piston_push_entities(int x, int y, int z, int dir, bool extending, float delta_progress) {
    if (delta_progress <= 0.0f) return;
    int move_dir = extending ? dir : lvl::OPP[dir];
    double dx = lvl::DX[move_dir] * delta_progress;
    double dy = lvl::DY[move_dir] * delta_progress;
    double dz = lvl::DZ[move_dir] * delta_progress;

    double min_x = std::min(double(x), double(x) + dx);
    double max_x = std::max(double(x + 1), double(x + 1) + dx);
    double min_y = std::min(double(y), double(y) + dy);
    double max_y = std::max(double(y + 1), double(y + 1) + dy);
    double min_z = std::min(double(z), double(z) + dz);
    double max_z = std::max(double(z + 1), double(z + 1) + dz);

    AABB swept{min_x, min_y, min_z, max_x, max_y, max_z};
    for_each_entity_in(swept, [&](Entity& e) {
        if (e.removed) return;
        if (Player* pl = e.as_player()) {
            pl->x += dx;
            pl->y += dy;
            pl->z += dz;
            pl->send(Writer().varint(play_cb::player_position).varint(next_teleport_id(*pl))
                         .f64(pl->x).f64(pl->y).f64(pl->z).f64(0).f64(0).f64(0).f32(pl->yaw).f32(pl->pitch).i32(0));
            broadcast_position(*pl);
        } else {
            e.pos = e.pos + Vec3{dx, dy, dz};
            e.set_pos(e.pos);
            e.sync_position = true;
            e.needs_sync = true;
            mark_entity_sync(e);
        }
    }, true);
}

inline bool dispenser_trigger(const lvl::Pos& p, int state) {
    auto b = be::get(p.x, p.y, p.z);
    if (!b) return false;
    for (auto& it : b->items) {
        if (!it.empty() && size_t(it.item) < item_info.size() && item_info[size_t(it.item)].name == "minecraft:tnt") {
            it.count--;
            if (it.count == 0) it = {};
            b->set_changed();
            be::flush(*b);

            int dir = lvl::rfacing(state);
            double sx = p.x + 0.5 + lvl::DX[dir] * 0.7;
            double sy = p.y + 0.5 + lvl::DY[dir] * 0.7;
            double sz = p.z + 0.5 + lvl::DZ[dir] * 0.7;
            std::string err;
            if (Entity* ent = summon_entity(entity_types::tnt, sx, sy, sz, nullptr, err)) {
                double rot = rand_double() * (M_PI * 2);
                ent->vel = Vec3{-std::sin(rot) * 0.02, 0.2, -std::cos(rot) * 0.02};
                ent->needs_sync = true;
                mark_entity_sync(*ent);
            }
            lvl::level_event(1000, p, 0); // SOUND_DISPENSER_DISPENSE
            return true;
        }
    }
    lvl::level_event(1001, p, 0); // SOUND_DISPENSER_FAIL
    return false;
}

// ==================================================================================================
// 9. Blocks World Subsystem Init
// ==================================================================================================
inline void init() {
    // Block entity factories
    be::factories[bet::sign] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<SignBE>(); };
    be::factories[bet::hanging_sign] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<SignBE>(); };
    be::factories[bet::lectern] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<LecternBE>(); };
    be::factories[bet::jukebox] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<JukeboxBE>(); };
    be::factories[bet::decorated_pot] = [](int) -> std::shared_ptr<be::Live> { return std::make_shared<DecoratedPotBE>(); };

    // Primed TNT entity factory
    register_entity_factory(entity_types::tnt, make_entity<PrimedTnt>);

    // Redstone & worldgen hooks
    lvl::piston_push_entities_hook = piston_push_entities;
    lvl::dispenser_trigger_hook = dispenser_trigger;
    lvl::bonemeal_tree_hook = [](const lvl::Pos& p, int, RandomSource& r) { return grow_tree(p, r); };
    lvl::bonemeal_mushroom_hook = [](const lvl::Pos& p, int, RandomSource& r) { return grow_mushroom(p, r); };
    lvl::bonemeal_grass_hook = [](const lvl::Pos& p, RandomSource& r) { return bonemeal_grass(p, r); };
    lvl::sapling_grow_hook = [](const lvl::Pos& p, RandomSource& r) { return grow_tree(p, r); };

    // Chain use_block_hook
    static bool (*prev_use_block_hook)(Player&, int, int, int, int, int) = nullptr;
    prev_use_block_hook = use_block_hook;
    use_block_hook = [](Player& p, int hand, int x, int y, int z, int face) -> bool {
        if (handle_block_interact(p, hand, x, y, z, face)) return true;
        return prev_use_block_hook ? prev_use_block_hook(p, hand, x, y, z, face) : false;
    };

    on(play_sb::sign_update, [](Player& p, Reader& in, int) {
        int x, y, z;
        read_block_pos(in, x, y, z);
        std::vector<std::string> lines(4);
        for (int i = 0; i < 4; i++) lines[i] = in.str(384);
        int slot = in.varint();
        handle_sign_update(p, x, y, z, lines, slot == 1);
    });
}

}  // namespace blocks_world
