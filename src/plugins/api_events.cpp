// Bukkit events fired from packet handlers and game hooks: PlayerInteractEvent (block and air),
// PlayerInteractEntityEvent, EntityDamageByEntityEvent (player attacks), PlayerItemHeldEvent, PlayerDropItemEvent,
// EntityDeathEvent. Each wrapper costs one listener-flag check when no plugin listens.
#include "internal.hpp"
#include "../mobs/mobs.hpp"

namespace plugins::detail {
namespace {

std::vector<int> gone_entities;  // freed this tick; the bridge drops its cached proxies at the next tick

std::string target_json(int entity_id) {
    for (auto& p : snapshot())
        if (p->eid == entity_id) return ",\"targetId\":" + std::to_string(entity_id) + ",\"targetPlayer\":" + q(uuid_str(p->uuid_hi, p->uuid_lo));
    return ",\"targetId\":" + std::to_string(entity_id);
}
std::string who(const Player& p) { return "{\"uuid\":" + q(uuid_str(p.uuid_hi, p.uuid_lo)) + ",\"name\":" + q(p.name); }
const ItemStack& held(const Player& p, bool off) { return p.inv[off ? 40u : size_t(p.selected)]; }

bool interact(Player& p, const char* action, bool off, const std::string& block) {
    Json r = event("interact", who(p) + ",\"action\":" + q(action) + ",\"offHand\":" + (off ? "true" : "false") +
                                   ",\"item\":" + item_to_json(held(p, off)) + block + "}");
    return cancelled(r);
}

}  // namespace

void install_event_handlers() {
    // Installed last, so these run before the block place/break wrappers (Paper: interact precedes place).
    on(play_sb::use_item_on, [orig = original(play_sb::use_item_on)](Player& p, Reader& in, int id) {
        if (has("interact")) {
            Reader peek = in;
            bool off = peek.varint() == 1;
            int x, y, z;
            read_block_pos(peek, x, y, z);
            int face = peek.varint();
            peek.f32(); peek.f32(); peek.f32(); peek.u8(); peek.u8();
            int seq = peek.varint();
            std::string block = ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"z\":" + std::to_string(z) +
                                ",\"face\":" + std::to_string(face);
            if (interact(p, "RIGHT_CLICK_BLOCK", off, block)) {  // deny: undo the client's prediction
                p.send(block_update(x, y, z, world.get_block(x, y, z)));
                ack_block_changes(p, seq);
                p.g.remote_valid = false;
                return;
            }
        }
        if (orig) orig(p, in, id);
    });
    on(play_sb::use_item, [orig = original(play_sb::use_item)](Player& p, Reader& in, int id) {
        if (has("interact")) {
            Reader peek = in;
            bool off = peek.varint() == 1;
            int seq = peek.varint();
            if (!held(p, off).empty() && interact(p, "RIGHT_CLICK_AIR", off, "")) {
                ack_block_changes(p, seq);
                p.g.remote_valid = false;
                return;
            }
        }
        if (orig) orig(p, in, id);
    });
    on(play_sb::player_action, [orig = original(play_sb::player_action)](Player& p, Reader& in, int id) {
        Reader peek = in;
        int action = peek.varint();
        int x, y, z;
        read_block_pos(peek, x, y, z);
        int face = peek.u8();
        int seq = peek.varint();
        if (action == 0 && has("interact")) {  // START_DESTROY_BLOCK
            std::string block = ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"z\":" + std::to_string(z) +
                                ",\"face\":" + std::to_string(face);
            if (interact(p, "LEFT_CLICK_BLOCK", false, block)) {
                p.send(block_update(x, y, z, world.get_block(x, y, z)));
                ack_block_changes(p, seq);
                return;
            }
        }
        if ((action == 4 || action == 5) && has("drop")) {  // DROP_ALL_ITEMS / DROP_ITEM (26.3 numbering)
            int slot = p.selected;
            ItemStack before = p.inv[size_t(slot)];
            int first = next_entity_id.load();
            if (orig) orig(p, in, id);
            for (int e = first; e < next_entity_id.load(); e++) {
                Entity* item = entity_by_id(e);
                if (!item || item->type != entity_types::item || item->removed) continue;
                Json r = event("drop", who(p) + ",\"entityId\":" + std::to_string(e) + "}");
                if (cancelled(r)) {
                    item->discard();
                    set_inv(p, slot, before);
                    p.g.remote_valid = false;
                }
                break;
            }
            return;
        }
        if (orig) orig(p, in, id);
    });
    on(play_sb::set_carried_item, [orig = original(play_sb::set_carried_item)](Player& p, Reader& in, int id) {
        if (has("helditem")) {
            Reader peek = in;
            int slot = int16_t(peek.u16());
            if (slot >= 0 && slot <= 8 && slot != p.selected) {
                Json r = event("helditem", who(p) + ",\"previous\":" + std::to_string(p.selected) + ",\"slot\":" + std::to_string(slot) + "}");
                if (cancelled(r)) {
                    p.send(Writer().varint(play_cb::set_held_slot).varint(p.selected));
                    return;
                }
            }
        }
        if (orig) orig(p, in, id);
    });
    on(play_sb::interact, [orig = original(play_sb::interact)](Player& p, Reader& in, int id) {
        if (has("interactentity")) {
            Reader peek = in;
            int target = peek.varint();
            bool off = peek.varint() == 1;
            Json r = event("interactentity", who(p) + target_json(target) + ",\"offHand\":" + (off ? "true" : "false") + "}");
            if (cancelled(r)) {
                p.g.remote_valid = false;
                return;
            }
        }
        if (orig) orig(p, in, id);
    });
    on(play_sb::attack, [orig = original(play_sb::attack)](Player& p, Reader& in, int id) {
        if (has("attack")) {
            Reader peek = in;
            int target = peek.varint();
            PlayerEntity* self = player_entity(p);
            double damage = self ? self->attr(attr::attack_damage) : 1.0;
            Json r = event("attack", who(p) + target_json(target) + ",\"damage\":" + num(damage) + "}");
            if (cancelled(r)) return;
        }
        if (orig) orig(p, in, id);
    });

    living_death_hooks.push_back([](LivingEntity& e, const DamageSource&) {
        if (has("entitydeath")) event("entitydeath", entity_snapshot(e));
    });
    entity_removed_hooks.push_back([](Entity& e) { gone_entities.push_back(e.id); });
}

void flush_gone_entities() {
    if (gone_entities.empty()) return;
    std::string ids = "[";
    for (size_t i = 0; i < gone_entities.size(); i++) ids += (i ? "," : "") + std::to_string(gone_entities[i]);
    gone_entities.clear();
    try {
        request("entitygone", "{\"ids\":" + ids + "]}");
    } catch (const std::exception&) {
    }
}

}  // namespace plugins::detail
