#pragma once
// Thrown potions and the lingering cloud (net.minecraft.world.entity.projectile.ThrowableProjectile,
// throwableitemprojectile.{AbstractThrownPotion, ThrownSplashPotion, ThrownLingeringPotion, ThrownExperienceBottle},
// AreaEffectCloud) plus throwing them (SplashPotionItem, LingeringPotionItem, ExperienceBottleItem). effects.cpp only.

// ---------- ThrowableProjectile / ThrowableItemProjectile ----------
struct ThrownItem : Entity {
    ItemStack item;
    int owner = 0;
    bool left_owner = false;
    double default_gravity() const override { return 0.03; }
    int add_entity_data() const override { return owner; }
    void set_item(const ItemStack& s) {
        item = s;
        item.count = 1;
        data.set_raw(edata::ThrowableItemProjectile::ITEM_STACK, edser::ITEM_STACK, encode_item_stack(item));
    }
    void shoot_from_rotation(Entity& src, float xrot, float yrot, float yoff, float pow, float unc) {  // Projectile.shootFromRotation
        float xd = -mth::sin(yrot * mth::DEG) * mth::cos(xrot * mth::DEG);
        float yd = -mth::sin((xrot + yoff) * mth::DEG);
        float zd = mth::cos(yrot * mth::DEG) * mth::cos(xrot * mth::DEG);
        Vec3 m = Vec3{double(xd), double(yd), double(zd)}.normalize() +  // getMovementToShoot
                 Vec3{rand_triangle(0, 0.0172275 * unc), rand_triangle(0, 0.0172275 * unc), rand_triangle(0, 0.0172275 * unc)};
        vel = m * double(pow);
        yaw = yaw_o = float(std::atan2(vel.x, vel.z) * 180.0 / M_PI);
        pitch = pitch_o = float(std::atan2(vel.y, vel.horizontal()) * 180.0 / M_PI);
        Vec3 sm = src.pos - src.old;  // getKnownMovement
        vel = vel + Vec3{sm.x, src.on_ground ? 0.0 : sm.y, sm.z};
        needs_sync = true;
    }
    bool can_hit(Entity& e) const {  // Projectile.canHitEntity / canBeHitByProjectile
        if (!e.is_pickable() || e.is_spectator() || !e.is_alive() || e.type == entity_types::item || e.type == entity_types::experience_orb)
            return false;
        return !owner || left_owner || e.id != owner;
    }
    virtual void on_hit(const BlockHit* block, Entity* target, const Vec3& at) = 0;
    void tick() override {  // ThrowableProjectile.tick
        if (!left_owner) {  // Projectile.checkLeftOwner
            Entity* o = entity_by_id(owner);
            left_owner = !o || !o->bb.intersects(bb.expand_towards(vel).inflate(1.0));
        }
        apply_gravity();
        vel = vel * double(is_in_water() ? 0.8f : 0.99f);  // applyInertia
        Vec3 orig = pos;
        BlockHit bh = clip_blocks(orig, orig + vel);  // ProjectileUtil.getHitResultOnMoveVector
        Vec3 end = bh.hit ? bh.pos : orig + vel;
        float margin = std::max(0.0f, std::min(0.3f, float(tick_count - 2) / 20.0f));  // computeMargin
        Entity* hit = nullptr;
        double best = 1e300;
        Vec3 hit_at = end;
        for_each_entity_in(bb.expand_towards(end - orig).inflate(1.0), [&](Entity& e) {
            if (&e == this || !can_hit(e)) return;
            int f;
            AABB eb = e.bb.inflate(margin);
            double t = clip_box(eb, orig, end - orig, f);
            if (t < 0 && !eb.contains(orig)) return;
            Vec3 at = orig + (end - orig) * std::max(t, 0.0);
            double d = orig.dist_sqr(at);
            if (d < best) best = d, hit = &e, hit_at = at;
        });
        set_pos(hit ? hit_at : end);
        yaw = float(std::atan2(vel.x, vel.z) * 180.0 / M_PI);  // updateRotation (server: no lerp needed for packets)
        pitch = float(std::atan2(vel.y, vel.horizontal()) * 180.0 / M_PI);
        Entity::tick();
        if ((hit || bh.hit) && !removed) on_hit(hit ? nullptr : &bh, hit, hit ? hit_at : end);
    }
    void save_extra(Nbt& o) const override {
        o.set("Item", item_to_nbt(item));
        o.set("leftOwner", Nbt::num(Nbt::Byte, left_owner));
    }
    void load_extra(const Nbt& in) override {
        if (const Nbt* it = in.get("Item")) set_item(item_from_nbt(*it));
        left_owner = nbt_num(in, "leftOwner", 0) != 0;
    }
};

// ---------- AreaEffectCloud ----------
struct AreaEffectCloud : Entity {
    PotionContents contents;
    float duration_scale = 1.0f;
    int duration = -1, wait_time = 20, reapplication_delay = 20, duration_on_use = 0, owner = 0;
    float radius_on_use = 0.0f, radius_per_tick = 0.0f;
    std::vector<std::pair<int, int>> victims;  // entity id -> tick it may be affected again
    float radius() const { return data.get_float(edata::AreaEffectCloud::RADIUS, 3.0f); }
    void set_radius(float r) {
        r = std::clamp(r, 0.0f, 32.0f);
        data.set_float(edata::AreaEffectCloud::RADIUS, r);
        Vec3 p = pos;
        set_size(r * 2.0f, 0.5f, 0.425f);  // EntityDimensions.scalable(2r, 0.5)
        set_pos(p.x, p.y, p.z);
    }
    void init() override { Entity::init(); set_radius(3.0f); update_particle(); }
    void update_particle() {  // ColorParticleOption(entity_effect, opaque potion colour)
        static const int entity_effect = reg_id("particle_type", "minecraft:entity_effect");
        Writer w;
        w.varint(entity_effect).i32(int32_t(uint32_t(contents.color_value()) | 0xFF000000u));
        data.set_raw(edata::AreaEffectCloud::PARTICLE, edser::PARTICLE, w.b);
    }
    bool is_pushable() const override { return false; }
    bool is_pickable() const override { return false; }
    bool is_attackable() const override { return false; }
    void tick() override {  // AreaEffectCloud.tick -> serverTick (noPhysics: no movement)
        Entity::tick();
        if (duration != -1 && tick_count - wait_time >= duration) { discard(); return; }
        bool waiting = tick_count < wait_time;
        if (bool(data.get(edata::AreaEffectCloud::WAITING, 0)) != waiting) data.set_bool(edata::AreaEffectCloud::WAITING, waiting);
        if (waiting) return;
        float r = radius();
        if (radius_per_tick != 0.0f) {
            r += radius_per_tick;
            if (r < 0.5f) { discard(); return; }
            set_radius(r);
        }
        if (tick_count % 5 != 0) return;
        std::erase_if(victims, [&](auto& v) { return tick_count >= v.second; });
        if (!contents.has_effects()) { victims.clear(); return; }
        std::vector<EffectInstance> all = contents.scaled_effects(duration_scale);
        std::vector<LivingEntity*> in;
        for_each_entity_in(bb, [&](Entity& e) { if (LivingEntity* l = e.as_living(); l && l->is_alive()) in.push_back(l); });
        for (LivingEntity* l : in) {
            if (std::any_of(victims.begin(), victims.end(), [&](auto& v) { return v.first == l->id; })) continue;
            if (std::none_of(all.begin(), all.end(), [&](auto& x) { return can_be_affected(*l, x.id); })) continue;
            // ponytail: isAffectedByPotions (armor stands, creative players) not checked
            double xd = l->pos.x - pos.x, zd = l->pos.z - pos.z;
            if (xd * xd + zd * zd > double(r) * r) continue;
            victims.push_back({l->id, tick_count + reapplication_delay});
            for (auto& x : all) {
                if (mob_effects[size_t(x.id)].instant) apply_instant(*l, x.id, x.amp, 0.5, this, entity_by_id(owner));
                else add_effect(*l, x, this);
            }
            if (radius_on_use != 0.0f) {
                r += radius_on_use;
                if (r < 0.5f) { discard(); return; }
                set_radius(r);
            }
            if (duration_on_use != 0 && duration != -1 && (duration += duration_on_use) <= 0) { discard(); return; }
        }
    }
    void save_extra(Nbt& o) const override {
        o.set("Age", Nbt::num(Nbt::Int, tick_count));
        o.set("Duration", Nbt::num(Nbt::Int, duration));
        o.set("WaitTime", Nbt::num(Nbt::Int, wait_time));
        o.set("ReapplicationDelay", Nbt::num(Nbt::Int, reapplication_delay));
        o.set("DurationOnUse", Nbt::num(Nbt::Int, duration_on_use));
        o.set("RadiusOnUse", Nbt::real(Nbt::Float, radius_on_use));
        o.set("RadiusPerTick", Nbt::real(Nbt::Float, radius_per_tick));
        o.set("Radius", Nbt::real(Nbt::Float, radius()));
        if (contents.potion >= 0 || !contents.custom.empty() || contents.color) {
            Nbt pc = Nbt::compound();  // PotionContents.CODEC
            if (contents.potion >= 0) pc.set("potion", Nbt::str("minecraft:" + potions[size_t(contents.potion)].name));
            if (contents.color) pc.set("custom_color", Nbt::num(Nbt::Int, *contents.color));
            if (!contents.custom.empty()) {
                Nbt l = Nbt::make_list();
                for (auto& x : contents.custom) l.list.push_back(effect_to_nbt(x));
                pc.set("custom_effects", l);
            }
            o.set("potion_contents", pc);
        }
        if (duration_scale != 1.0f) o.set("potion_duration_scale", Nbt::real(Nbt::Float, duration_scale));
    }
    void load_extra(const Nbt& in) override {
        tick_count = int(nbt_num(in, "Age", 0));
        duration = int(nbt_num(in, "Duration", -1));
        wait_time = int(nbt_num(in, "WaitTime", 20));
        reapplication_delay = int(nbt_num(in, "ReapplicationDelay", 20));
        duration_on_use = int(nbt_num(in, "DurationOnUse", 0));
        radius_on_use = float(nbt_num(in, "RadiusOnUse", 0));
        radius_per_tick = float(nbt_num(in, "RadiusPerTick", 0));
        set_radius(float(nbt_num(in, "Radius", 3.0)));
        contents = {};
        if (const Nbt* pc = in.get("potion_contents"); pc && pc->t == Nbt::Compound) {
            if (const Nbt* p = pc->get("potion"); p && p->t == Nbt::String) contents.potion = potion_id(p->s);
            if (pc->get("custom_color")) contents.color = int(nbt_num(*pc, "custom_color", 0));
            if (const Nbt* l = pc->get("custom_effects"); l && l->t == Nbt::List)
                for (auto& c : l->list) {
                    EffectInstance x;
                    if (effect_from_nbt(c, x)) contents.custom.push_back(std::move(x));
                }
        } else if (const Nbt* p = in.get("potion_contents"); p && p->t == Nbt::String) {
            contents.potion = potion_id(p->s);  // PotionContents.CODEC also accepts a bare potion id
        }
        duration_scale = float(nbt_num(in, "potion_duration_scale", 1.0));
        update_particle();
    }
};

// ---------- AbstractThrownPotion: splash / lingering ----------
struct ThrownPotion : ThrownItem {
    double default_gravity() const override { return 0.05; }
    void on_hit(const BlockHit* block, Entity* target, const Vec3& at) override {
        // ponytail: water bottles dousing fire/candles/campfires, hurting water-sensitive mobs and rehydrating
        // axolotls (affectEntitiesAround, douseFire) are not ported.
        (void)block;
        PotionContents pc = potion_contents(item);
        if (pc.has_effects()) {
            if (type == entity_types::lingering_potion) lingering(target);
            else splash(at);
        }
        bool instant = false;
        if (pc.potion >= 0)
            for (auto& e : potions[size_t(pc.potion)].effects) instant = instant || mob_effects[size_t(e.id)].instant;
        level_event(instant ? 2007 : 2002, block_x(), block_y(), block_z(), pc.color_value(), nullptr);
        level_event(instant ? 1054 : 1053, block_x(), block_y(), block_z(), 0, nullptr);
        discard();
    }
    void splash(const Vec3& at) {  // ThrownSplashPotion.onHitAsPotion
        PotionContents pc = potion_contents(item);
        float scale_d = potion_duration_scale(item);
        std::vector<EffectInstance> all = pc.all_effects();
        AABB pbox = bb.move(at.x - pos.x, at.y - pos.y, at.z - pos.z);
        float margin = std::max(0.0f, std::min(0.3f, float(tick_count - 2) / 20.0f));
        std::vector<LivingEntity*> hit;
        for_each_entity_in(pbox.inflate(4.0, 2.0, 4.0), [&](Entity& e) { if (LivingEntity* l = e.as_living()) hit.push_back(l); });
        Entity* src = entity_by_id(owner);
        for (LivingEntity* l : hit) {
            AABB o = l->bb.inflate(margin);  // AABB.distanceToSqr(AABB)
            double dx = std::max({pbox.x0 - o.x1, o.x0 - pbox.x1, 0.0}), dy = std::max({pbox.y0 - o.y1, o.y0 - pbox.y1, 0.0}),
                   dz = std::max({pbox.z0 - o.z1, o.z0 - pbox.z1, 0.0});
            double dist = dx * dx + dy * dy + dz * dz;
            if (dist >= 16.0) continue;
            double scale = 1.0 - std::sqrt(dist) / 4.0;
            for (auto& x : all) {
                if (mob_effects[size_t(x.id)].instant) { apply_instant(*l, x.id, x.amp, scale, this, src); continue; }
                int d = x.infinite() || x.duration == 0 ? x.duration : int(scale * double(x.duration) * double(scale_d) + 0.5);
                EffectInstance n(x.id, d, x.amp, x.ambient, x.visible);
                if (!n.ends_within(20)) add_effect(*l, n, src ? src : this);
            }
        }
    }
    void lingering(Entity* target) {  // ThrownLingeringPotion.onHitAsPotion
        auto c = create_entity(entity_types::area_effect_cloud);
        auto* cloud = static_cast<AreaEffectCloud*>(c.get());
        Vec3 p = target ? target->pos : pos;
        cloud->snap_to(p.x, p.y, p.z, 0, 0);
        cloud->owner = owner;
        cloud->set_radius(3.0f);
        cloud->radius_on_use = -0.5f;
        cloud->duration = 600;
        cloud->wait_time = 10;
        cloud->radius_per_tick = -cloud->radius() / float(cloud->duration);
        cloud->contents = potion_contents(item);  // applyComponentsFromItemStack
        cloud->duration_scale = potion_duration_scale(item);
        cloud->update_particle();
        add_entity(std::move(c));
    }
};

// ---------- ThrownExperienceBottle ----------
struct ThrownExperienceBottle : ThrownItem {
    double default_gravity() const override { return 0.07; }
    void on_hit(const BlockHit*, Entity*, const Vec3& at) override {
        level_event(2002, block_x(), block_y(), block_z(), -13083194, nullptr);
        level_event(1053, block_x(), block_y(), block_z(), 0, nullptr);
        // ponytail: awardWithDirection's orb direction is not ported (orbs get the default random push)
        award_xp_orbs(at.x, at.y, at.z, 3 + rand_int(5) + rand_int(5));
        discard();
    }
};

// SplashPotionItem / LingeringPotionItem / ExperienceBottleItem.use. Returns true when the item was thrown.
inline bool throw_item(Player& p, int hand) {
    static const int splash = item_by_name["minecraft:splash_potion"], lingering = item_by_name["minecraft:lingering_potion"],
                     bottle = item_by_name["minecraft:experience_bottle"];
    ItemStack& s = p.inv[hand ? 40u : size_t(p.selected)];
    if (s.empty() || (s.item != splash && s.item != lingering && s.item != bottle)) return false;
    PlayerEntity* pe = player_entity(p);
    if (!pe || p.g.dead) return false;
    const char* sound = s.item == splash ? "minecraft:entity.splash_potion.throw"
                      : s.item == lingering ? "minecraft:entity.lingering_potion.throw" : "minecraft:entity.experience_bottle.throw";
    play_sound(sound, s.item == bottle ? SRC_NEUTRAL : SRC_PLAYERS, p.x, p.y, p.z, 0.5f, 0.4f / (rand_float() * 0.4f + 0.8f));
    int type = s.item == splash ? entity_types::splash_potion : s.item == lingering ? entity_types::lingering_potion : entity_types::experience_bottle;
    auto e = create_entity(type);
    if (!e) return false;
    auto* t = static_cast<ThrownItem*>(e.get());
    t->snap_to(p.x, p.y + 1.62 - 0.1f, p.z, 0, 0);  // owner.getEyeY() - 0.1F
    t->owner = p.eid;
    t->set_item(s);
    t->shoot_from_rotation(*pe, p.pitch, p.yaw, -20.0f, s.item == bottle ? 0.7f : 0.5f, 1.0f);
    add_entity(std::move(e));
    if (survival_like(p) || p.gamemode == 3) s.count--;  // ItemStack.consume: not in creative
    if (s.count <= 0) s = {};
    return true;
}

static MobRegistration potion_entity_registration([] {
    register_entity_factory(entity_types::splash_potion, make_entity<ThrownPotion>);
    register_entity_factory(entity_types::lingering_potion, make_entity<ThrownPotion>);
    register_entity_factory(entity_types::experience_bottle, make_entity<ThrownExperienceBottle>);
    register_entity_factory(entity_types::area_effect_cloud, make_entity<AreaEffectCloud>);
});
