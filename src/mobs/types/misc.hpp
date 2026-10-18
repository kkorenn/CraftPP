#pragma once
// Non-mob entities the first mobs need: ExperienceOrb, Arrow (AbstractArrow physics) and ServerExplosion.
#include "animals.hpp"
#include "../../commands/vanilla.hpp"

// ---------- experience ----------
// xp_needed_for_next_level / give_experience_points: game/survival.hpp
inline std::unordered_map<const Player*, int> take_xp_delay;

struct ExperienceOrb : Entity {
    int age = 0, health = 5, count = 1, following = 0;
    int value() const { return int(data.get(edata::ExperienceOrb::VALUE, 0)); }
    void set_value(int v) { data.set_int(edata::ExperienceOrb::VALUE, v); }
    double default_gravity() const override { return 0.03; }
    bool is_attackable() const override { return false; }
    static int split_value(int v) {  // getExperienceValue
        for (int t : {2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3}) if (v >= t) return t;
        return 1;
    }
    bool can_merge(const ExperienceOrb& o, int id_mod, int v) const { return !o.removed && (o.id - id_mod) % 40 == 0 && o.value() == v; }
    void tick() override {
        Entity::tick();
        bool colliding = !no_block_collision(bb);
        if (was_eye_in_water) vel = {vel.x * 0.99f, std::min(vel.y + 5.0E-4f, 0.06000000238418579), vel.z * 0.99f};
        else if (!colliding) apply_gravity();
        if (fluid_type(block_at(block_x(), block_y(), block_z())) == FLUID_LAVA)
            vel = {(rand_float() - rand_float()) * 0.2f, 0.2f, (rand_float() - rand_float()) * 0.2f};
        if (tick_count % 20 == 1)  // scanForMerges
            for_each_entity_in(bb.inflate(0.5), [&](Entity& e) {
                if (&e == this || e.type != type) return;
                auto& o = static_cast<ExperienceOrb&>(e);
                if (can_merge(o, id, value())) { count += o.count; age = std::min(age, o.age); o.discard(); }
            }, false);
        Entity* f = entity_by_id(following);  // followNearbyPlayer
        if (!f || f->is_spectator() || f->dist_sqr(*this) > 64.0) {
            f = nullptr;
            double best = 64.0;
            for (Entity* p : player_entities()) {
                double d = p->dist_sqr(*this);
                if (d < best && !p->is_spectator() && p->is_alive()) best = d, f = p;
            }
            following = f ? f->id : 0;
        }
        if (f) {
            Vec3 d{f->pos.x - pos.x, f->pos.y + f->eye_height / 2.0 - pos.y, f->pos.z - pos.z};
            double pw = 1.0 - std::sqrt(d.length_sqr()) / 8.0;
            vel = vel + d.normalize() * (pw * pw * 0.1);
        }
        double fall = vel.y;
        move(SELF, vel);
        float fr = 0.98f;
        if (on_ground) fr *= phys_of(block_at(block_x(), on_pos_y(0.999999f), block_z())).friction;
        vel = vel * fr;
        if (vertical_collision_below && fall < -gravity()) vel.y = -fall * 0.4;
        if (++age >= 6000) discard();
    }
    bool hurt_server(const DamageSource& s, float d) override {
        if (is_invulnerable_to(s)) return false;
        sync_velocity = true;
        health = int(float(health) - d);
        if (health <= 0) discard();
        return true;
    }
    void player_touch(Player& p) override {
        int& delay = take_xp_delay[&p];
        if (delay != 0) return;
        delay = 2;
        Writer w;
        w.varint(play_cb::take_item_entity).varint(id).varint(p.eid).varint(1);
        broadcast(w);
        p.send(w);
        if (int left = repair_player_items(p, value()); left > 0) give_experience_points(p, left);
        if (--count == 0) discard();
    }
    void save_extra(Nbt& o) const override {
        o.set("Health", Nbt::num(Nbt::Short, health));
        o.set("Age", Nbt::num(Nbt::Short, age));
        o.set("Value", Nbt::num(Nbt::Short, value()));
        o.set("Count", Nbt::num(Nbt::Int, count));
    }
    void load_extra(const Nbt& in) override {
        health = int(nbt_num(in, "Health", 5));
        age = int(nbt_num(in, "Age", 0));
        set_value(int(nbt_num(in, "Value", 0)));
        count = std::max(1, int(nbt_num(in, "Count", 1)));
    }
};
inline void award_xp_orbs(double x, double y, double z, int amount) {  // ExperienceOrb.award (api: spawn_xp_orbs)
    while (amount > 0) {
        int v = ExperienceOrb::split_value(amount);
        amount -= v;
        int idm = rand_int(40);
        bool merged = false;
        for_each_entity_in(AABB{x - 0.5, y - 0.5, z - 0.5, x + 0.5, y + 0.5, z + 0.5}, [&](Entity& e) {
            if (merged || e.type != entity_types::experience_orb) return;
            auto& o = static_cast<ExperienceOrb&>(e);
            if (o.can_merge(o, idm, v)) { o.count++; o.age = 0; merged = true; }
        }, false);
        if (merged) continue;
        auto o = create_entity(entity_types::experience_orb);
        auto* orb = static_cast<ExperienceOrb*>(o.get());
        orb->snap_to(x, y, z, rand_float() * 360.0f, 0);
        orb->vel = {(rand_double() * 0.2 - 0.1) * 2.0, rand_double() * 0.2 * 2.0, (rand_double() * 0.2 - 0.1) * 2.0};
        orb->set_value(v);
        add_entity(std::move(o));
    }
}

// ---------- arrows (AbstractArrow / Arrow) ----------
struct Arrow : Entity {
    int owner = 0, life = 0, shake = 0, in_ground_time = 0;
    int last_state = -1;
    double base_damage = 2.0;
    bool left_owner = false;
    enum Pickup { DISALLOWED, ALLOWED, CREATIVE_ONLY } pickup = DISALLOWED;
    double default_gravity() const override { return 0.05; }
    bool in_ground() const { return data.get(edata::AbstractArrow::IN_GROUND, 0); }
    void set_in_ground(bool v) { data.set_bool(edata::AbstractArrow::IN_GROUND, v); }
    bool crit() const { return data.get(edata::AbstractArrow::ID_FLAGS, 0) & 1; }
    void set_crit(bool v) { int f = int(data.get(edata::AbstractArrow::ID_FLAGS, 0)); data.set_byte(edata::AbstractArrow::ID_FLAGS, v ? f | 1 : f & ~1); }
    bool is_pushed_by_fluid() const override { return !in_ground(); }
    void push(double x, double y, double z) override { if (!in_ground()) Entity::push(x, y, z); }
    int add_entity_data() const override { return owner; }
    static float lerp_rotation(float o, float r) {
        while (r - o < -180.0f) o -= 360.0f;
        while (r - o >= 180.0f) o += 360.0f;
        return mth::lerp(0.2f, o, r);
    }
    void shoot(double xd, double yd, double zd, float pow, float unc) {  // Projectile.shoot
        Vec3 m = Vec3{xd, yd, zd}.normalize() + Vec3{rand_triangle(0, 0.0172275 * unc), rand_triangle(0, 0.0172275 * unc), rand_triangle(0, 0.0172275 * unc)};
        vel = m * pow;
        needs_sync = true;
        yaw = yaw_o = float(std::atan2(vel.x, vel.z) * 180.0 / M_PI);
        pitch = pitch_o = float(std::atan2(vel.y, vel.horizontal()) * 180.0 / M_PI);
    }
    bool can_hit(Entity& e) {  // Projectile.canHitEntity
        if (!e.is_pickable() || e.is_spectator() || !e.is_alive() || e.type == entity_types::item ||
            e.type == entity_types::experience_orb || e.type == type)
            return false;
        return !owner || left_owner || e.id != owner;
    }
    void tick() override {  // AbstractArrow.tick
        Vec3 m = vel;
        int bx = block_x(), by = block_y(), bz = block_z();
        int bs = block_at(bx, by, bz);
        if (!sflag(bs, SF_AIR))
            for (const AABB* b = shape_begin(bs); b != shape_end(bs); b++)
                if (b->move(bx, by, bz).contains(pos)) { vel = {0, 0, 0}; set_in_ground(true); break; }
        if (shake > 0) shake--;
        if (is_in_water_or_rain()) clear_fire();
        if (in_ground()) {
            if (last_state != bs && no_block_collision(AABB{pos.x, pos.y, pos.z, pos.x, pos.y, pos.z}.inflate(0.06))) {
                set_in_ground(false);  // startFalling
                vel = vel.multiply(rand_float() * 0.2f, rand_float() * 0.2f, rand_float() * 0.2f);
                life = 0;
            } else if (++life >= 1200) discard();
            in_ground_time++;
            set_shared_flag(0, fire_ticks > 0);
            return;
        }
        in_ground_time = 0;
        Vec3 orig = pos;
        if (is_in_water()) vel = vel * 0.6f;  // getWaterInertia
        yaw = lerp_rotation(yaw, float(std::atan2(m.x, m.z) * 180.0 / M_PI));
        pitch = lerp_rotation(pitch, float(std::atan2(m.y, m.horizontal()) * 180.0 / M_PI));
        if (!left_owner) {  // checkLeftOwner
            Entity* o = entity_by_id(owner);
            AABB a = bb.expand_towards(vel).inflate(1.0);
            left_owner = !o || !o->bb.intersects(a);
        }
        BlockHit bh = clip_blocks(orig, orig + vel);
        Vec3 end = bh.hit ? bh.pos : orig + vel;
        // ProjectileUtil.getManyEntityHitResult: nearest entity whose inflated box the segment crosses
        Entity* hit = nullptr;
        double best = 1e300, hit_t = 0;
        for_each_entity_in(bb.expand_towards(vel).inflate(1.0), [&](Entity& e) {
            if (!can_hit(e)) return;
            int f;
            AABB eb = e.bb.inflate(0.3);  // ProjectileUtil hit margin
            double t = clip_box(eb, orig, end - orig, f);
            if (t < 0 && !eb.contains(orig)) return;
            double d = orig.dist_sqr(e.pos);
            if (d < best) best = d, hit = &e, hit_t = std::max(t, 0.0);
        });
        if (hit) {
            set_pos(orig + (end - orig) * hit_t);
            on_hit_entity(*hit);
            needs_sync = true;
        } else {
            set_pos(end);
            if (bh.hit) {
                on_hit_block(bh);
                needs_sync = true;
            }
        }
        if (removed) return;
        if (!is_in_water()) vel = vel * 0.99f;
        if (!in_ground()) apply_gravity();
        Entity::tick();
    }
    void on_hit_entity(Entity& e) {
        float pow = float(vel.length());
        Entity* o = entity_by_id(owner);
        DamageSource s = DamageSource::of("arrow", o ? o : this, this);
        int dmg = int(std::ceil(std::clamp(double(pow) * base_damage, 0.0, 2.147483647E9)));
        if (crit()) dmg = int(std::min<int64_t>(int64_t(rand_int(dmg / 2 + 2)) + dmg, INT32_MAX));
        if (LivingEntity* lo = living_of(o)) lo->set_last_hurt_mob(e);
        if (is_on_fire()) e.ignite_for_ticks(100);
        if (e.hurt_server(s, float(dmg))) {
            if (LivingEntity* l = e.as_living())
                l->data.set_int(edata::LivingEntity::ARROW_COUNT, int(l->data.get(edata::LivingEntity::ARROW_COUNT, 0)) + 1);
            if (e.as_player() && o && o->as_player() && &e != o)
                o->as_player()->send(std::move(Writer().varint(play_cb::game_event).u8(6).f32(0)));  // PLAY_ARROW_HIT_SOUND
            play_sound("minecraft:entity.arrow.hit", 1.0f, 1.2f / (rand_float() * 0.2f + 0.9f));
            discard();
        } else {  // deflect (REVERSE): bounce back
            vel = vel * -0.1;
            yaw += 180.0f, yaw_o += 180.0f;
            if (vel.length_sqr() < 1.0E-7) {
                if (pickup == ALLOWED) spawn_item({item_by_name["minecraft:arrow"], 1, 0}, pos.x, pos.y + 0.1, pos.z);
                discard();
            }
        }
    }
    void on_hit_block(const BlockHit& h) {
        last_state = block_at(h.bx, h.by, h.bz);
        Vec3 off{double((vel.x > 0) - (vel.x < 0)), double((vel.y > 0) - (vel.y < 0)), double((vel.z > 0) - (vel.z < 0))};
        set_pos(pos - off * 0.05f);
        vel = {0, 0, 0};
        play_sound("minecraft:entity.arrow.hit", 1.0f, 1.2f / (rand_float() * 0.2f + 0.9f));
        set_in_ground(true);
        shake = 7;
        set_crit(false);
    }
    void player_touch(Player& p) override {  // AbstractArrow.playerTouch -> tryPickup
        if (!in_ground() || shake > 0) return;
        bool ok = pickup == ALLOWED ? true : pickup == CREATIVE_ONLY && p.gamemode == 1;
        if (pickup == ALLOWED) ok = give_item(p, item_by_name["minecraft:arrow"], 1);
        if (!ok) return;
        Writer w;
        w.varint(play_cb::take_item_entity).varint(id).varint(p.eid).varint(1);
        broadcast(w);
        p.send(w);
        discard();
    }
    void save_extra(Nbt& o) const override {
        o.set("life", Nbt::num(Nbt::Short, life));
        o.set("shake", Nbt::num(Nbt::Byte, shake));
        o.set("inGround", Nbt::num(Nbt::Byte, in_ground()));
        o.set("pickup", Nbt::num(Nbt::Byte, int(pickup)));
        o.set("damage", Nbt::real(Nbt::Double, base_damage));
        o.set("crit", Nbt::num(Nbt::Byte, crit()));
    }
    void load_extra(const Nbt& in) override {
        life = int(nbt_num(in, "life", 0));
        shake = int(nbt_num(in, "shake", 0));
        set_in_ground(nbt_num(in, "inGround", 0) != 0);
        pickup = Pickup(int(nbt_num(in, "pickup", 0)));
        base_damage = nbt_num(in, "damage", 2.0);
        set_crit(nbt_num(in, "crit", 0) != 0);
    }
};

// ---------- explosions (ServerExplosion) ----------
enum class ExplosionInteraction { NONE, BLOCK, MOB, TNT };
inline float seen_percent(const Vec3& c, Entity& e) {  // ServerExplosion.getSeenPercent
    const AABB& b = e.bb;
    double xs = 1.0 / ((b.x1 - b.x0) * 2.0 + 1.0), ys = 1.0 / ((b.y1 - b.y0) * 2.0 + 1.0), zs = 1.0 / ((b.z1 - b.z0) * 2.0 + 1.0);
    double xo = (1.0 - std::floor(1.0 / xs) * xs) / 2.0, zo = (1.0 - std::floor(1.0 / zs) * zs) / 2.0;
    if (xs < 0 || ys < 0 || zs < 0) return 0.0f;
    int hits = 0, count = 0;
    for (double xx = 0.0; xx <= 1.0; xx += xs)
        for (double yy = 0.0; yy <= 1.0; yy += ys)
            for (double zz = 0.0; zz <= 1.0; zz += zs) {
                Vec3 from{mth::lerp(xx, b.x0, b.x1) + xo, mth::lerp(yy, b.y0, b.y1), mth::lerp(zz, b.z0, b.z1) + zo};
                if (!clip_blocks(from, c).hit) hits++;
                count++;
            }
    return float(hits) / float(count);
}
// Level.explode(source, x, y, z, radius, interaction). Returns the number of blocks blown.
inline int explode(Entity* source, double x, double y, double z, float r, bool fire, ExplosionInteraction inter) {
    Vec3 c{x, y, z};
    bool destroy = inter == ExplosionInteraction::BLOCK || inter == ExplosionInteraction::TNT ||
                   (inter == ExplosionInteraction::MOB && rule("mob_griefing"));
    const char* decay_rule = inter == ExplosionInteraction::TNT ? "tnt_explosion_drop_decay"
                             : inter == ExplosionInteraction::MOB ? "mob_explosion_drop_decay" : "block_explosion_drop_decay";
    bool decay = destroy && rule(decay_rule);
    Entity* indirect = source && source->as_living() ? source : nullptr;  // Explosion.getIndirectSourceEntity
    if (auto* a = dynamic_cast<Arrow*>(source)) indirect = living_of(entity_by_id(a->owner));
    DamageSource ds = DamageSource::of(indirect && source ? "player_explosion" : "explosion", indirect, source);
    // calculateExplodedPositions
    std::vector<int64_t> blow;
    std::vector<std::array<int, 3>> blow_pos;
    for (int xx = 0; xx < 16; xx++)
        for (int yy = 0; yy < 16; yy++)
            for (int zz = 0; zz < 16; zz++) {
                if (!(xx == 0 || xx == 15 || yy == 0 || yy == 15 || zz == 0 || zz == 15)) continue;
                double xd = xx / 15.0f * 2.0f - 1.0f, yd = yy / 15.0f * 2.0f - 1.0f, zd = zz / 15.0f * 2.0f - 1.0f;
                double d = std::sqrt(xd * xd + yd * yd + zd * zd);
                xd /= d, yd /= d, zd /= d;
                float power = r * (0.7f + rand_float() * 0.6f);
                double xp = x, yp = y, zp = z;
                for (; power > 0.0f; power -= 0.22500001f) {
                    int bx = mth::floor(xp), by = mth::floor(yp), bz = mth::floor(zp);
                    if (by < MIN_Y || by >= MIN_Y + HEIGHT) break;
                    int s = block_at(bx, by, bz);
                    if (!sflag(s, SF_AIR) || fluid_type(s)) {
                        float res = phys_of(s).blast;
                        if (fluid_type(s)) res = std::max(res, 100.0f);  // FluidState.getExplosionResistance (water/lava 100)
                        power -= (res + 0.3f) * 0.3f;
                    }
                    if (power > 0.0f) {
                        int64_t k = mth::block_key(bx, by, bz);
                        if (std::find(blow.begin(), blow.end(), k) == blow.end()) blow.push_back(k), blow_pos.push_back({bx, by, bz});
                    }
                    xp += xd * 0.3f, yp += yd * 0.3f, zp += zd * 0.3f;
                }
            }
    // hurtEntities
    std::vector<std::pair<Player*, Vec3>> hit_players;
    if (r >= 1.0E-5f) {
        float dr = r * 2.0f;
        AABB box{double(mth::floor(x - dr - 1.0)), double(mth::floor(y - dr - 1.0)), double(mth::floor(z - dr - 1.0)),
                 double(mth::floor(x + dr + 1.0)), double(mth::floor(y + dr + 1.0)), double(mth::floor(z + dr + 1.0))};
        std::vector<Entity*> victims;
        for_each_entity_in(box, [&](Entity& e) { if (&e != source) victims.push_back(&e); });
        for (Entity* e : victims) {
            if (e->removed || e->is_spectator()) continue;
            double dist = std::sqrt(e->dist_sqr(x, y, z)) / dr;
            if (dist > 1.0) continue;
            Vec3 dir = (e->eye_pos() - c).normalize();
            float exposure = seen_percent(c, *e);
            double pw = (1.0 - dist) * exposure;
            e->hurt_server(ds, float((pw * pw + pw) / 2.0 * 7.0 * dr + 1.0));
            double kr = e->as_living() ? e->as_living()->attr(attr::explosion_knockback_resistance) : 0.0;
            Vec3 kb = dir * ((1.0 - dist) * exposure * (1.0 - kr));
            if (Player* p = e->as_player()) {
                if (!(p->gamemode == 1)) hit_players.emplace_back(p, kb);
            } else {
                e->push(kb.x, kb.y, kb.z);
            }
        }
    }
    // interactWithBlocks
    if (destroy && !blow_pos.empty()) {
        std::shuffle(blow_pos.begin(), blow_pos.end(), game_rng);
        struct Stack { int x, y, z; ItemStack s; };
        std::vector<Stack> stacks;
        BlockBatch batch;
        static std::vector<ItemStack> drops;
        for (auto& b : blow_pos) {
            int s = block_at(b[0], b[1], b[2]);
            if (sflag(s, SF_AIR)) continue;
            if (block_of(s).name == "minecraft:tnt") {
                if (rule("tnt_explodes")) {
                    std::string err;
                    if (Entity* e = summon_entity(entity_types::tnt, b[0] + 0.5, b[1], b[2] + 0.5, nullptr, err)) {
                        e->data.set_int(edata::PrimedTnt::FUSE, rand_between(10, 30));
                        mark_entity_sync(*e);
                    }
                }
                batch.set(b[0], b[1], b[2], blocks::air);
                continue;
            }
            const BlockInfo& bi = block_of(s);
            if (bi.loot >= 0) {
                drops.clear();
                LootContext ctx{s, b[0], b[1], b[2], {}, decay ? r : 0.0f};
                ctx.this_entity = source;
                roll_loot(loot_tables[size_t(bi.loot)], ctx, drops);
                for (auto& d : drops) {  // addOrAppendStack (max 16 per combined stack)
                    ItemStack in = d;
                    for (auto& st : stacks) {
                        if (in.empty()) break;
                        if (st.s.same(in) && st.s.count < 16) {
                            int n = std::min(in.count, std::min(16, max_stack(st.s)) - st.s.count);
                            if (n > 0) st.s.count += n, in.count -= n;
                        }
                    }
                    if (!in.empty()) stacks.push_back({b[0], b[1], b[2], in});
                }
            }
            batch.set(b[0], b[1], b[2], blocks::air);
        }
        batch.flush();
        for (auto& st : stacks) pop_resource(st.x, st.y, st.z, st.s);
    }
    if (fire)
        for (auto& b : blow_pos)
            if (rand_int(3) == 0 && sflag(block_at(b[0], b[1], b[2]), SF_AIR) && sflag(block_at(b[0], b[1] - 1, b[2]), SF_SOLID_RENDER))
                change_block(b[0], b[1], b[2], blocks::fire);
    // ClientboundExplodePacket to players within 64 blocks
    bool small = r < 2.0f || !destroy;
    for (auto& pp : players) {
        Player& p = *pp;
        if ((p.x - x) * (p.x - x) + (p.y - y) * (p.y - y) + (p.z - z) * (p.z - z) >= 4096.0) continue;
        Writer w;
        w.varint(play_cb::explode).f64(x).f64(y).f64(z).f32(r).i32(int32_t(blow_pos.size()));
        const Vec3* kb = nullptr;
        for (auto& [hp, v] : hit_players) if (hp == &p) kb = &v;
        if (kb) w.boolean(true).f64(kb->x).f64(kb->y).f64(kb->z);
        else w.boolean(false);
        w.varint(reg_id("particle_type", small ? "explosion" : "explosion_emitter"));  // no particle options
        w.varint(reg_id("sound_event", "entity.generic.explode") + 1);              // holder: registry id + 1
        w.varint(2).varint(reg_id("particle_type", "poof")).f32(0.5f).f32(1.0f).varint(1)
            .varint(reg_id("particle_type", "smoke")).f32(1.0f).f32(1.0f).varint(1);  // DEFAULT_EXPLOSION_BLOCK_PARTICLES
        w.boolean(!source || !source->silent());
        p.send(w);
    }
    return int(blow_pos.size());
}
