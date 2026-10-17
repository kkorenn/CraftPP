// Plugin API routes for non-player entities (src/mobs). The bridge's EntityImpl reads entity.state snapshots
// and writes through the routes below. Tick thread only, like the rest of api().
#include "internal.hpp"
#include "../mobs/mobs.hpp"

namespace plugins::detail {
namespace {

Entity* entity_arg(const Json& a) {
    Entity* e = entity_by_id(int(num_arg(a, "id", -1)));
    return e && !e->removed && !e->as_player_const() ? e : nullptr;
}
Entity& require_entity(const Json& a) {
    Entity* e = entity_arg(a);
    if (!e) throw std::runtime_error("Entity is not in the world");
    return *e;
}

std::string snapshot(Entity& e) {
    auto b = [](bool v) { return v ? "true" : "false"; };
    World* level = e.level();
    std::string s = "{\"id\":" + std::to_string(e.id) + ",\"uuid\":" + q(uuid_str(e.uuid_hi, e.uuid_lo)) +
                    ",\"type\":" + q("minecraft:" + e.info().id) + ",\"world\":" + q(level ? level->id : "minecraft:overworld") +
                    ",\"x\":" + num(e.pos.x) + ",\"y\":" + num(e.pos.y) + ",\"z\":" + num(e.pos.z) + ",\"yaw\":" + num(e.yaw) +
                    ",\"pitch\":" + num(e.pitch) + ",\"vx\":" + num(e.vel.x) + ",\"vy\":" + num(e.vel.y) + ",\"vz\":" + num(e.vel.z) +
                    ",\"width\":" + num(e.width) + ",\"height\":" + num(e.height) + ",\"eyeHeight\":" + num(e.eye_height) +
                    ",\"onGround\":" + b(e.on_ground) + ",\"inWater\":" + b(e.is_in_water()) + ",\"inLava\":" + b(e.is_in_lava()) +
                    ",\"fireTicks\":" + std::to_string(e.fire_ticks) + ",\"fallDistance\":" + num(e.fall_distance) +
                    ",\"ticksLived\":" + std::to_string(e.tick_count) + ",\"silent\":" + b(e.silent()) +
                    ",\"gravity\":" + b(!e.no_gravity()) + ",\"glowing\":" + b(e.glowing_tag) +
                    ",\"invisible\":" + b(e.shared_flag(5)) + ",\"invulnerable\":" + b(e.invulnerable) +
                    ",\"air\":" + std::to_string(e.air_supply()) + ",\"pose\":" + std::to_string(int(e.pose())) +
                    ",\"customNameVisible\":" + b(e.data.get(edata::Entity::CUSTOM_NAME_VISIBLE, 0)) +
                    ",\"customName\":" + (e.has_custom_name() ? q(e.custom_name) : std::string("null"));
    std::string tags;
    for (auto& t : e.tags) tags += (tags.empty() ? "" : ",") + q(t);
    s += ",\"tags\":[" + tags + "]";
    if (LivingEntity* l = e.as_living()) {
        s += ",\"health\":" + num(l->health()) + ",\"maxHealth\":" + num(l->max_health()) + ",\"absorption\":" + num(l->absorption) +
             ",\"dead\":" + b(l->is_dead_or_dying()) + ",\"baby\":" + b(l->is_baby()) + ",\"headYaw\":" + num(l->y_head_rot) +
             ",\"noDamageTicks\":" + std::to_string(e.invulnerable_time) + ",\"equipment\":[";
        for (int slot = 0; slot < 8; slot++) s += (slot ? "," : "") + item_to_json(l->equipment[size_t(slot)]);
        s += "]";
    }
    if (Mob* m = e.as_mob()) {
        LivingEntity* t = m->target_unchecked();
        s += ",\"ai\":" + std::string(b(!m->no_ai())) + ",\"persistent\":" + b(m->persistence_required) +
             ",\"target\":" + (t ? std::to_string(t->id) : std::string("null"));
    }
    if (e.type == entity_types::item) s += ",\"item\":" + item_to_json(static_cast<ItemEntity&>(e).stack);
    return s + "}";
}

void teleport(Entity& e, double x, double y, double z, float yaw, float pitch) {
    e.yaw = yaw, e.pitch = pitch;
    if (LivingEntity* l = e.as_living()) l->y_head_rot = l->y_body_rot = yaw;
    e.teleport_to(x, y, z);
    if (Mob* m = e.as_mob()) m->navigation->stop();
    mark_entity_sync(e);
}

// Entity.set*(value) for the simple properties of Entity / LivingEntity / Mob.
void set_property(Entity& e, const std::string& prop, const Json& v) {
    auto boolean = [&] { return v.isBool() && v.asBool(); };
    auto number = [&] { return v.isNumber() ? v.asDouble() : 0.0; };
    LivingEntity* l = e.as_living();
    Mob* m = e.as_mob();
    if (prop == "silent") e.set_silent(boolean());
    else if (prop == "gravity") e.set_no_gravity(!boolean());
    else if (prop == "glowing") { e.glowing_tag = boolean(); e.set_shared_flag(6, e.glowing_tag); }
    else if (prop == "invisible") e.set_shared_flag(5, boolean());
    else if (prop == "invulnerable") e.invulnerable = boolean();
    else if (prop == "fireTicks") e.fire_ticks = int(number());
    else if (prop == "fallDistance") e.fall_distance = number();
    else if (prop == "ticksLived") e.tick_count = std::max(1, int(number()));
    else if (prop == "air") e.set_air_supply(int(number()));
    else if (prop == "noDamageTicks") e.invulnerable_time = std::max(0, int(number()));
    else if (prop == "tag.add") { if (std::find(e.tags.begin(), e.tags.end(), v.asString()) == e.tags.end() && e.tags.size() < 1024) e.tags.push_back(v.asString()); }
    else if (prop == "tag.remove") std::erase(e.tags, v.asString());
    else if (prop == "absorption" && l) l->absorption = float(std::max(0.0, number()));
    else if (prop == "maxHealth" && l) {
        if (number() <= 0) throw std::runtime_error("Max health must be greater than 0");
        l->set_attr_base(attr::max_health, number());
        if (l->health() > l->max_health()) l->set_health(l->max_health());
    } else if (prop == "headYaw" && l) l->y_head_rot = float(number());
    else if (prop == "ai" && m) m->set_no_ai(!boolean());
    else if (prop == "persistent" && m) m->persistence_required = boolean();
    else if (prop == "target" && m) m->set_target(v.isNumber() ? living_of(entity_by_id(v.asInt())) : nullptr);
    else if (prop == "item" && e.type == entity_types::item) static_cast<ItemEntity&>(e).set_item(item_from_json(v));
    else if (prop == "pickupDelay" && e.type == entity_types::item) static_cast<ItemEntity&>(e).pickup_delay = std::clamp(int(number()), 0, 32767);
    else throw std::runtime_error("CraftPP does not implement entity property " + prop + " for " + e.info().id + " yet");
    mark_entity_sync(e);
}

}  // namespace

std::optional<std::string> api_entity(const std::string& m, const Json& a) {
    if (m.rfind("entity.", 0) != 0) return std::nullopt;
    if (m == "entity.list") {  // every loaded non-player entity, all levels
        std::string s = "[";
        for_each_entity([&](Entity& e) {
            if (!e.as_player_const()) s += (s.size() > 1 ? "," : "") + snapshot(e);
        });
        return s + "]";
    }
    if (m == "entity.state") {
        Entity* e = entity_arg(a);
        return e ? snapshot(*e) : "null";
    }
    if (m == "entity.spawn") {
        int type = entity_type_id(text_arg(a, "type"));
        if (type < 0) throw std::runtime_error("Unknown entity type " + text_arg(a, "type"));
        double x = num_arg(a, "x"), y = num_arg(a, "y"), z = num_arg(a, "z");
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::abs(x) > 3.0E7 || std::abs(z) > 3.0E7)
            throw std::runtime_error("Invalid spawn position");
        World* level = level_by_id(text_arg(a, "world", "minecraft:overworld"));
        InLevel in(level ? level : &worlds[DIM_OVERWORLD]);
        auto up = create_entity(type);
        if (!up) throw std::runtime_error("CraftPP cannot spawn " + entity_type_info[size_t(type)].id + " yet");
        up->snap_to(x, y, z, float(num_arg(a, "yaw")), float(num_arg(a, "pitch")));
        if (LivingEntity* l = up->as_living()) l->y_head_rot = l->y_body_rot = up->yaw;
        if (type == entity_types::item) {
            auto& item = static_cast<ItemEntity&>(*up);
            const Json* stack = a.find("item");
            item.set_item(stack ? item_from_json(*stack) : ItemStack{});
            if (item.stack.empty()) throw std::runtime_error("An item entity needs an item");
            item.pickup_delay = 10;  // World.dropItem
        }
        // CraftRegionAccessor.addEntity: mobs get finalizeSpawn with EntitySpawnReason.COMMAND (randomized data)
        if (Mob* mob = up->as_mob(); mob && bool_arg(a, "randomize", true)) mob->finalize_spawn(SpawnReason::COMMAND, nullptr);
        Entity* e = add_entity(std::move(up));
        mark_entity_sync(*e);
        return snapshot(*e);
    }
    Entity& e = require_entity(a);
    InLevel in(e.level());
    if (m == "entity.teleport") {
        teleport(e, num_arg(a, "x"), num_arg(a, "y"), num_arg(a, "z"), float(num_arg(a, "yaw", e.yaw)), float(num_arg(a, "pitch", e.pitch)));
        return "true";
    }
    if (m == "entity.velocity") {
        double x = num_arg(a, "x"), y = num_arg(a, "y"), z = num_arg(a, "z");
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) throw std::runtime_error("Velocity must be finite");
        e.vel = {x, y, z};
        e.sync_velocity = true;  // hurtMarked: the tracker sends set_entity_motion
        mark_entity_sync(e);
        return "null";
    }
    if (m == "entity.remove") {
        e.discard();
        return "null";
    }
    if (m == "entity.setName") {
        auto* n = a.find("name");
        std::string name = n && n->isString() ? n->asString() : "";
        e.custom_name = name;
        Writer w;
        w.boolean(!name.empty());
        if (!name.empty()) legacy_component(w, name);
        e.data.set_raw(edata::Entity::CUSTOM_NAME, edser::OPTIONAL_COMPONENT, std::move(w.b));
        e.set_custom_name_visible(bool_arg(a, "visible"));
        mark_entity_sync(e);
        return "null";
    }
    if (m == "entity.setHealth") {
        LivingEntity* l = e.as_living();
        if (!l) throw std::runtime_error("Entity has no health");
        double v = num_arg(a, "value");
        if (!std::isfinite(v) || v < 0 || v > l->max_health())
            throw std::runtime_error("Health value (" + num(v) + ") must be between 0 and " + num(l->max_health()));
        l->set_health(float(v));
        if (v == 0) l->die(DamageSource::of("generic"));  // CraftLivingEntity.setHealth
        mark_entity_sync(e);
        return "null";
    }
    if (m == "entity.damage") {
        Entity* source = entity_by_id(int(num_arg(a, "source", 0)));
        const char* type = !source ? "generic" : source->as_player() ? "player_attack" : "mob_attack";
        bool hurt = e.hurt_server(DamageSource::of(text_arg(a, "type", type), source), float(num_arg(a, "amount")));
        mark_entity_sync(e);
        return hurt ? "true" : "false";
    }
    if (m == "entity.equipment") {
        LivingEntity* l = e.as_living();
        int slot = int(num_arg(a, "slot", -1));
        if (!l || slot < 0 || slot > 7) throw std::runtime_error("Entity has no equipment slot " + std::to_string(slot));
        const Json* item = a.find("item");
        l->set_item_slot(slot, item ? item_from_json(*item) : ItemStack{});
        mark_entity_sync(e);
        return "null";
    }
    if (m == "entity.set") {
        set_property(e, text_arg(a, "prop"), a["value"]);
        return "null";
    }
    return std::nullopt;
}

}  // namespace plugins::detail
