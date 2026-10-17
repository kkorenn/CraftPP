package prism.bridge;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import io.papermc.paper.threadedregions.scheduler.EntityScheduler;
import io.papermc.paper.threadedregions.scheduler.ScheduledTask;
import net.kyori.adventure.text.Component;
import org.bukkit.Location;
import org.bukkit.Material;
import org.bukkit.Server;
import org.bukkit.World;
import org.bukkit.entity.Entity;
import org.bukkit.entity.EntityType;
import org.bukkit.entity.LivingEntity;
import org.bukkit.entity.Player;
import org.bukkit.event.entity.CreatureSpawnEvent;
import org.bukkit.event.player.PlayerTeleportEvent;
import org.bukkit.inventory.EntityEquipment;
import org.bukkit.inventory.EquipmentSlot;
import org.bukkit.inventory.ItemStack;
import org.bukkit.plugin.Plugin;
import org.bukkit.util.Vector;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Consumer;

/**
 * Backing for non-player entities. The proxy implements the entity's own
 * Bukkit interface (Zombie, ArmorStand, Item...) so plugin casts work; state
 * lives in PrismMC's summoned-entity table and is read through on demand.
 * Mobs do not think yet (no AI), so this is spawn/position/name/health/remove.
 */
public final class EntityImpl implements ProxyBase.Backing, ProxyBase.HandleProvider {
    private static final Map<Integer, EntityImpl> BY_ID = new ConcurrentHashMap<>();

    private final int id;
    private final UUID uuid;
    private final EntityType type;
    private Entity proxy;
    private JsonObject snapshot;
    private final PdcImpl persistentData = new PdcImpl();
    private final Map<String, List<org.bukkit.metadata.MetadataValue>> metadata = new ConcurrentHashMap<>();
    private volatile boolean removed;

    private EntityImpl(int id, UUID uuid, EntityType type, JsonObject snapshot) {
        this.id = id;
        this.uuid = uuid;
        this.type = type;
        this.snapshot = snapshot;
        this.persistentData.onChange(() -> Main.rpc.api("entity.setPluginData", "id", id, "data", Persistence.serialize(persistentData)));
    }

    @Override
    public void attachProxy(Object proxy) {
        this.proxy = (Entity) proxy;
    }

    Entity proxy() {
        return proxy;
    }

    int id() {
        return id;
    }

    @Override
    public Object nmsHandle() {
        prism.bridge.nms.Nms nms = prism.bridge.nms.Nms.get();
        if (nms == null) {
            return null;
        }
        JsonObject state = snapshot;
        return nms.entityHandle(uuid, type.getKey().asString(), id,
                state.has("x") ? state.get("x").getAsDouble() : 0, state.has("y") ? state.get("y").getAsDouble() : 0, state.has("z") ? state.get("z").getAsDouble() : 0,
                state.has("yaw") ? state.get("yaw").getAsFloat() : 0f, state.has("pitch") ? state.get("pitch").getAsFloat() : 0f);
    }

    /** Entity proxy for a PrismMC snapshot (cached per entity id). */
    static EntityImpl fromSnapshot(JsonObject snapshot) {
        int id = snapshot.get("id").getAsInt();
        EntityImpl existing = BY_ID.get(id);
        if (existing != null) {
            existing.snapshot = snapshot;
            return existing;
        }
        EntityType type = EntityType.fromName(snapshot.get("type").getAsString().replace("minecraft:", ""));
        if (type == null) {
            type = EntityType.UNKNOWN;
        }
        EntityImpl backing = new EntityImpl(id, UUID.fromString(snapshot.get("uuid").getAsString()), type, snapshot);
        Class<?> entityClass = type.getEntityClass() != null && type.getEntityClass().isInterface() ? type.getEntityClass() : Entity.class;
        Class<?>[] extra = LivingEntity.class.isAssignableFrom(entityClass) ? new Class<?>[0] : new Class<?>[] {org.bukkit.entity.Damageable.class};
        ProxyBase.create(entityClass.asSubclass(Object.class), backing, extra);
        BY_ID.put(id, backing);
        if (snapshot.has("pluginData") && !snapshot.get("pluginData").isJsonNull()) {
            Persistence.restore(snapshot.get("pluginData").getAsString(), backing.persistentData);
        }
        return backing;
    }

    static EntityImpl byId(int id) {
        EntityImpl cached = BY_ID.get(id);
        if (cached != null) {
            return cached;
        }
        JsonElement state = Main.rpc.api("entity.state", "id", id);
        return state == null || !state.isJsonObject() ? null : fromSnapshot(state.getAsJsonObject());
    }

    static List<Entity> all() {
        List<Entity> entities = new ArrayList<>();
        for (JsonElement element : Main.rpc.api("entity.list").getAsJsonArray()) {
            entities.add(fromSnapshot(element.getAsJsonObject()).proxy());
        }
        return entities;
    }

    static Entity spawn(Location location, EntityType type, CreatureSpawnEvent.SpawnReason reason, ItemStack item) {
        JsonObject result = Main.rpc.api("entity.spawn", "type", type.getKey().asString(), "x", location.getX(), "y", location.getY(), "z", location.getZ(),
                "yaw", location.getYaw(), "pitch", location.getPitch(), "item", item == null ? null : PrismItemStack.jsonOrNull(item)).getAsJsonObject();
        EntityImpl backing = fromSnapshot(result);
        if (backing.proxy instanceof LivingEntity living) {
            CreatureSpawnEvent event = new CreatureSpawnEvent(living, reason);
            org.bukkit.Bukkit.getPluginManager().callEvent(event);
            if (event.isCancelled()) {
                backing.remove();
                return backing.proxy;
            }
        }
        return backing.proxy;
    }

    static void forget(int id) {
        EntityImpl backing = BY_ID.remove(id);
        if (prism.bridge.nms.Nms.get() != null) {
            prism.bridge.nms.Nms.get().removeEntity(id);
        }
        if (backing != null) {
            backing.removed = true;
        }
    }

    private JsonObject state() {
        if (!removed) {
            JsonElement fresh = Main.rpc.api("entity.state", "id", id);
            if (fresh != null && fresh.isJsonObject()) {
                snapshot = fresh.getAsJsonObject();
            } else {
                removed = true;
            }
        }
        return snapshot;
    }

    private double num(String key, double fallback) {
        return snapshot.has(key) && !snapshot.get(key).isJsonNull() ? snapshot.get(key).getAsDouble() : fallback;
    }

    private boolean flag(String key) {
        return snapshot.has(key) && !snapshot.get(key).isJsonNull() && snapshot.get(key).getAsBoolean();
    }

    /** Fresh value of one snapshot field (one round trip). */
    private double live(String key, double fallback) {
        state();
        return num(key, fallback);
    }

    private boolean liveFlag(String key) {
        state();
        return flag(key);
    }

    private void set(String property, Object value) {
        if (removed) return;
        Main.rpc.api("entity.set", "id", id, "prop", property, "value", value);
        state();
    }

    private boolean has(String key) {
        return snapshot.has(key) && !snapshot.get(key).isJsonNull();
    }

    private void requireLiving(String method) {
        if (!has("health")) throw new UnsupportedOperationException(type + " has no " + method);
    }

    // ----- Entity -----

    public int getEntityId() {
        return id;
    }

    public UUID getUniqueId() {
        return uuid;
    }

    public EntityType getType() {
        return type;
    }

    public String getName() {
        String custom = getCustomName();
        return custom != null ? custom : type.getKey().getKey();
    }

    public Component name() {
        Component custom = customName();
        return custom != null ? custom : Component.translatable("entity.minecraft." + type.getKey().getKey());
    }

    public Server getServer() {
        return org.bukkit.Bukkit.getServer();
    }

    public World getWorld() {
        return Main.server.overworld();
    }

    public Location getLocation() {
        state();
        return new Location(getWorld(), num("x", 0), num("y", 64), num("z", 0), (float) num("yaw", 0), (float) num("pitch", 0));
    }

    public Location getLocation(Location target) {
        Location location = getLocation();
        if (target == null) {
            return location;
        }
        target.setWorld(location.getWorld());
        target.setX(location.getX());
        target.setY(location.getY());
        target.setZ(location.getZ());
        target.setYaw(location.getYaw());
        target.setPitch(location.getPitch());
        return target;
    }

    public double getX() {
        return live("x", 0);
    }

    public double getY() {
        return live("y", 0);
    }

    public double getZ() {
        return live("z", 0);
    }

    public float getYaw() {
        return (float) live("yaw", 0);
    }

    public float getPitch() {
        return (float) live("pitch", 0);
    }

    public boolean teleport(Location location) {
        return teleport(location, PlayerTeleportEvent.TeleportCause.PLUGIN);
    }

    public boolean teleport(Location location, PlayerTeleportEvent.TeleportCause cause) {
        if (removed || location == null) {
            return false;
        }
        Location from = getLocation();
        org.bukkit.event.entity.EntityTeleportEvent event = new org.bukkit.event.entity.EntityTeleportEvent(proxy, from, location.clone());
        Main.pluginManager.callEvent(event);
        if (event.isCancelled() || event.getTo() == null) {
            return false;
        }
        Location to = event.getTo();
        Main.rpc.api("entity.teleport", "id", id, "x", to.getX(), "y", to.getY(), "z", to.getZ(), "yaw", to.getYaw(), "pitch", to.getPitch());
        state();
        return true;
    }

    public boolean teleport(Location location, PlayerTeleportEvent.TeleportCause cause, io.papermc.paper.entity.TeleportFlag... flags) {
        return teleport(location, cause);
    }

    public boolean teleport(Location location, io.papermc.paper.entity.TeleportFlag... flags) {
        return teleport(location, PlayerTeleportEvent.TeleportCause.PLUGIN);
    }

    public boolean teleport(Entity destination) {
        return teleport(destination.getLocation());
    }

    public boolean teleport(Entity destination, PlayerTeleportEvent.TeleportCause cause) {
        return teleport(destination.getLocation(), cause);
    }

    public java.util.concurrent.CompletableFuture<Boolean> teleportAsync(Location location) {
        return java.util.concurrent.CompletableFuture.completedFuture(teleport(location));
    }

    public java.util.concurrent.CompletableFuture<Boolean> teleportAsync(Location location, PlayerTeleportEvent.TeleportCause cause) {
        return java.util.concurrent.CompletableFuture.completedFuture(teleport(location, cause));
    }

    public void setRotation(float yaw, float pitch) {
        Location location = getLocation();
        location.setYaw(yaw);
        location.setPitch(pitch);
        Main.rpc.api("entity.teleport", "id", id, "x", location.getX(), "y", location.getY(), "z", location.getZ(), "yaw", yaw, "pitch", pitch);
    }

    public void setVelocity(Vector velocity) {
        velocity.checkFinite();
        Main.rpc.api("entity.velocity", "id", id, "x", velocity.getX(), "y", velocity.getY(), "z", velocity.getZ());
    }

    public Vector getVelocity() {
        state();
        return new Vector(num("vx", 0), num("vy", 0), num("vz", 0));
    }

    public void remove() {
        if (!removed) {
            removed = true;
            Main.rpc.api("entity.remove", "id", id);
            BY_ID.remove(id);
            if (prism.bridge.nms.Nms.get() != null) { prism.bridge.nms.Nms.get().removeEntity(id); }
        }
    }

    public boolean isDead() {
        state();
        return removed || flag("dead");
    }

    public boolean isValid() {
        state();
        return !removed && !flag("dead");
    }

    public boolean isOnGround() {
        return liveFlag("onGround");
    }

    public boolean isInWater() {
        return liveFlag("inWater");
    }

    public boolean isInLava() {
        return liveFlag("inLava");
    }

    public boolean isInWaterOrRain() {
        return isInWater();
    }

    public double getHeight() {
        return live("height", 0);
    }

    public double getWidth() {
        return live("width", 0);
    }

    public org.bukkit.util.BoundingBox getBoundingBox() {
        state();
        double x = num("x", 0), y = num("y", 0), z = num("z", 0), half = num("width", 0) / 2;
        return new org.bukkit.util.BoundingBox(x - half, y, z - half, x + half, y + num("height", 0), z + half);
    }

    public List<Entity> getNearbyEntities(double x, double y, double z) {
        Location here = getLocation();
        List<Entity> result = new ArrayList<>();
        for (Entity entity : Main.server.overworld().getEntities()) {
            if (entity.getEntityId() != id && Math.abs(entity.getLocation().getX() - here.getX()) <= x
                    && Math.abs(entity.getLocation().getY() - here.getY()) <= y && Math.abs(entity.getLocation().getZ() - here.getZ()) <= z) {
                result.add(entity);
            }
        }
        return result;
    }

    public String getCustomName() {
        return has("customName") ? snapshot.get("customName").getAsString() : null;
    }

    public void setCustomName(String name) {
        Main.rpc.api("entity.setName", "id", id, "name", name, "visible", isCustomNameVisible());
        state();
    }

    public Component customName() {
        String name = getCustomName();
        return name == null ? null : ServerImpl.LEGACY.deserialize(name);
    }

    public void customName(Component name) {
        setCustomName(name == null ? null : ServerImpl.LEGACY.serialize(name));
    }

    public boolean isCustomNameVisible() {
        return flag("customNameVisible");
    }

    public void setCustomNameVisible(boolean visible) {
        Main.rpc.api("entity.setName", "id", id, "name", getCustomName(), "visible", visible);
        state();
    }

    public Set<String> getScoreboardTags() {
        state();
        Set<String> tags = new HashSet<>();
        if (snapshot.has("tags")) snapshot.getAsJsonArray("tags").forEach(t -> tags.add(t.getAsString()));
        return java.util.Collections.unmodifiableSet(tags);
    }

    public boolean addScoreboardTag(String tag) {
        if (getScoreboardTags().contains(tag) || getScoreboardTags().size() >= 1024) return false;
        set("tag.add", tag);
        return true;
    }

    public boolean removeScoreboardTag(String tag) {
        if (!getScoreboardTags().contains(tag)) return false;
        set("tag.remove", tag);
        return true;
    }

    public org.bukkit.persistence.PersistentDataContainer getPersistentDataContainer() {
        return persistentData;
    }

    public void setMetadata(String key, org.bukkit.metadata.MetadataValue value) {
        List<org.bukkit.metadata.MetadataValue> values = metadata.computeIfAbsent(key, k -> new ArrayList<>());
        values.removeIf(existing -> existing.getOwningPlugin() == value.getOwningPlugin());
        values.add(value);
    }

    public List<org.bukkit.metadata.MetadataValue> getMetadata(String key) {
        return metadata.getOrDefault(key, List.of());
    }

    public boolean hasMetadata(String key) {
        return metadata.containsKey(key) && !metadata.get(key).isEmpty();
    }

    public void removeMetadata(String key, Plugin plugin) {
        List<org.bukkit.metadata.MetadataValue> values = metadata.get(key);
        if (values != null) {
            values.removeIf(value -> value.getOwningPlugin() == plugin);
        }
    }

    public boolean isInsideVehicle() {
        return false;  // CraftPP has no vehicles yet: nothing can be ridden
    }

    public Entity getVehicle() {
        return null;
    }

    public List<Entity> getPassengers() {
        return List.of();
    }

    public boolean isEmpty() {
        return true;
    }

    public boolean isInvulnerable() {
        return liveFlag("invulnerable");
    }

    public void setInvulnerable(boolean value) {
        set("invulnerable", value);
    }

    public boolean isSilent() {
        return liveFlag("silent");
    }

    public void setSilent(boolean value) {
        set("silent", value);
    }

    public boolean hasGravity() {
        return liveFlag("gravity");
    }

    public void setGravity(boolean value) {
        set("gravity", value);
    }

    public boolean isGlowing() {
        return liveFlag("glowing");
    }

    public void setGlowing(boolean value) {
        set("glowing", value);
    }

    public boolean isInvisible() {
        return liveFlag("invisible");
    }

    public void setInvisible(boolean value) {
        set("invisible", value);
    }

    public boolean isPersistent() {
        state();
        return !has("persistent") || flag("persistent");
    }

    public void setPersistent(boolean value) {
        if (has("persistent")) set("persistent", value);
    }

    public int getTicksLived() {
        return (int) live("ticksLived", 0);
    }

    public void setTicksLived(int ticks) {
        if (ticks < 1) throw new IllegalArgumentException("Age value (" + ticks + ") must be positive");
        set("ticksLived", ticks);
    }

    public int getFireTicks() {
        return (int) live("fireTicks", 0);
    }

    public void setFireTicks(int ticks) {
        set("fireTicks", ticks);
    }

    public int getMaxFireTicks() {
        return 1;
    }

    public float getFallDistance() {
        return (float) live("fallDistance", 0);
    }

    public void setFallDistance(float distance) {
        set("fallDistance", distance);
    }

    public boolean isInWorld() {
        return !removed;
    }

    public boolean isTicking() {
        return !removed;
    }

    public org.bukkit.entity.Pose getPose() {
        int pose = (int) live("pose", 0);
        org.bukkit.entity.Pose[] poses = org.bukkit.entity.Pose.values();
        return pose >= 0 && pose < poses.length ? poses[pose] : org.bukkit.entity.Pose.STANDING;
    }

    public boolean isSneaking() {
        return getPose() == org.bukkit.entity.Pose.SNEAKING;
    }

    public org.bukkit.entity.SpawnCategory getSpawnCategory() {
        if (proxy instanceof org.bukkit.entity.Monster) return org.bukkit.entity.SpawnCategory.MONSTER;
        if (proxy instanceof org.bukkit.entity.WaterMob) return org.bukkit.entity.SpawnCategory.WATER_ANIMAL;
        if (proxy instanceof org.bukkit.entity.Ambient) return org.bukkit.entity.SpawnCategory.AMBIENT;
        if (proxy instanceof org.bukkit.entity.Animals) return org.bukkit.entity.SpawnCategory.ANIMAL;
        return org.bukkit.entity.SpawnCategory.MISC;
    }

    public EntityScheduler getScheduler() {
        return new Scheduler();
    }

    public String getScoreboardEntryName() {
        return uuid.toString();
    }

    // ----- LivingEntity / Damageable -----

    public double getHealth() {
        requireLiving("health");
        return live("health", 0);
    }

    public void setHealth(double health) {
        requireLiving("health");
        double max = getMaxHealth();
        if (!(health >= 0 && health <= max)) {  // CraftLivingEntity.setHealth
            throw new IllegalArgumentException("Health value (" + health + ") must be between 0 and " + max + ". (" + health + ")");
        }
        Main.rpc.api("entity.setHealth", "id", id, "value", health);
        state();
    }

    public double getMaxHealth() {
        requireLiving("max health");
        return live("maxHealth", 20);
    }

    public void setMaxHealth(double health) {
        requireLiving("max health");
        if (!(health > 0)) throw new IllegalArgumentException("Max health amount (" + health + ") must be greater than 0");
        set("maxHealth", health);
    }

    public void resetMaxHealth() {
        throw new UnsupportedOperationException("CraftPP does not implement LivingEntity.resetMaxHealth yet");
    }

    public void damage(double amount) {
        Main.rpc.api("entity.damage", "id", id, "amount", amount);
        state();
    }

    public void damage(double amount, Entity source) {
        Main.rpc.api("entity.damage", "id", id, "amount", amount, "source", source == null ? 0 : source.getEntityId());
        state();
    }

    public double getAbsorptionAmount() {
        return live("absorption", 0);
    }

    public void setAbsorptionAmount(double amount) {
        if (amount < 0 || !Double.isFinite(amount)) throw new IllegalArgumentException("amount < 0 or non-finite");
        set("absorption", amount);
    }

    public Location getEyeLocation() {
        Location location = getLocation();
        return location.add(0, num("eyeHeight", 0), 0);
    }

    public double getEyeHeight() {
        return live("eyeHeight", 0);
    }

    public double getEyeHeight(boolean ignorePose) {
        return getEyeHeight();
    }

    public boolean hasAI() {
        state();
        return flag("ai");
    }

    public void setAI(boolean ai) {
        if (has("ai")) set("ai", ai);
    }

    public boolean isAware() {
        return hasAI();
    }

    public int getNoDamageTicks() {
        return (int) live("noDamageTicks", 0);
    }

    public void setNoDamageTicks(int ticks) {
        set("noDamageTicks", ticks);
    }

    public int getMaximumNoDamageTicks() {
        return 20;
    }

    public int getRemainingAir() {
        return (int) live("air", 300);
    }

    public void setRemainingAir(int ticks) {
        set("air", ticks);
    }

    public int getMaximumAir() {
        return 300;
    }

    public boolean isSwimming() {
        return getPose() == org.bukkit.entity.Pose.SWIMMING;
    }

    public boolean isSleeping() {
        return getPose() == org.bukkit.entity.Pose.SLEEPING;
    }

    public boolean isGliding() {
        return getPose() == org.bukkit.entity.Pose.FALL_FLYING;
    }

    public EntityEquipment getEquipment() {
        return has("equipment") ? new Equipment() : null;
    }

    public float getBodyYaw() {
        return (float) live("yaw", 0);
    }

    public float getHeadYaw() {
        return (float) live("headYaw", 0);
    }

    public void setHeadYaw(float yaw) {
        set("headYaw", yaw);
    }

    // ----- Item entity -----

    public ItemStack getItemStack() {
        state();
        ItemStack stack = has("item") ? PrismItemStack.fromJson(snapshot.get("item")) : null;
        return stack == null ? new PrismItemStack(Material.AIR, 0) : stack;
    }

    public void setItemStack(ItemStack stack) {
        if (stack == null || stack.getType().isAir()) throw new IllegalArgumentException("Cannot set an empty item stack");
        set("item", PrismItemStack.jsonOrNull(stack));
    }

    public void setPickupDelay(int delay) {
        set("pickupDelay", delay);
    }

    // ----- Ageable / Mob -----

    public boolean isBaby() {
        return liveFlag("baby");
    }

    public boolean isAdult() {
        return !isBaby();
    }

    public LivingEntity getTarget() {
        state();
        if (!has("target")) return null;
        EntityImpl target = byId(snapshot.get("target").getAsInt());
        if (target != null && target.proxy instanceof LivingEntity living) return living;
        int targetId = snapshot.get("target").getAsInt();
        for (Player player : org.bukkit.Bukkit.getOnlinePlayers()) {
            if (player.getEntityId() == targetId) return player;
        }
        return null;
    }

    public void setTarget(LivingEntity target) {
        if (!has("ai")) throw new UnsupportedOperationException(type + " cannot have a target");
        set("target", target == null ? null : target.getEntityId());
    }

    @Override
    public boolean equals(Object other) {
        return ProxyBase.unwrap(other) instanceof EntityImpl entity && entity.id == id;
    }

    @Override
    public int hashCode() {
        return id;
    }

    @Override
    public String toString() {
        return "CraftEntity{id=" + id + ", type=" + type + "}";
    }

    /** Entity equipment shown to clients through set_equipment; stored here. */
    private final class Equipment implements EntityEquipment {
        private static int protocolSlot(EquipmentSlot slot) {
            return switch (slot) {
                case HAND -> 0;
                case OFF_HAND -> 1;
                case FEET -> 2;
                case LEGS -> 3;
                case CHEST -> 4;
                case HEAD -> 5;
                case BODY -> 6;
                default -> 7;
            };
        }

        private ItemStack get(EquipmentSlot slot) {
            state();
            ItemStack item = snapshot.has("equipment") ? PrismItemStack.fromJson(snapshot.getAsJsonArray("equipment").get(protocolSlot(slot))) : null;
            return item == null ? new PrismItemStack(Material.AIR, 0) : item;
        }

        private void set(EquipmentSlot slot, ItemStack item) {
            int protocolSlot = protocolSlot(slot);
            Main.rpc.api("entity.equipment", "id", id, "slot", protocolSlot, "item", PrismItemStack.jsonOrNull(item));
        }

        @Override
        public void setItem(EquipmentSlot slot, ItemStack item) {
            set(slot, item);
        }

        @Override
        public void setItem(EquipmentSlot slot, ItemStack item, boolean silent) {
            set(slot, item);
        }

        @Override
        public ItemStack getItem(EquipmentSlot slot) {
            return get(slot);
        }

        @Override
        public ItemStack getItemInMainHand() {
            return get(EquipmentSlot.HAND);
        }

        @Override
        public void setItemInMainHand(ItemStack item) {
            set(EquipmentSlot.HAND, item);
        }

        @Override
        public void setItemInMainHand(ItemStack item, boolean silent) {
            set(EquipmentSlot.HAND, item);
        }

        @Override
        public ItemStack getItemInOffHand() {
            return get(EquipmentSlot.OFF_HAND);
        }

        @Override
        public void setItemInOffHand(ItemStack item) {
            set(EquipmentSlot.OFF_HAND, item);
        }

        @Override
        public void setItemInOffHand(ItemStack item, boolean silent) {
            set(EquipmentSlot.OFF_HAND, item);
        }

        @Override
        public ItemStack getItemInHand() {
            return getItemInMainHand();
        }

        @Override
        public void setItemInHand(ItemStack item) {
            setItemInMainHand(item);
        }

        @Override
        public ItemStack getHelmet() {
            return get(EquipmentSlot.HEAD);
        }

        @Override
        public void setHelmet(ItemStack item) {
            set(EquipmentSlot.HEAD, item);
        }

        @Override
        public void setHelmet(ItemStack item, boolean silent) {
            set(EquipmentSlot.HEAD, item);
        }

        @Override
        public ItemStack getChestplate() {
            return get(EquipmentSlot.CHEST);
        }

        @Override
        public void setChestplate(ItemStack item) {
            set(EquipmentSlot.CHEST, item);
        }

        @Override
        public void setChestplate(ItemStack item, boolean silent) {
            set(EquipmentSlot.CHEST, item);
        }

        @Override
        public ItemStack getLeggings() {
            return get(EquipmentSlot.LEGS);
        }

        @Override
        public void setLeggings(ItemStack item) {
            set(EquipmentSlot.LEGS, item);
        }

        @Override
        public void setLeggings(ItemStack item, boolean silent) {
            set(EquipmentSlot.LEGS, item);
        }

        @Override
        public ItemStack getBoots() {
            return get(EquipmentSlot.FEET);
        }

        @Override
        public void setBoots(ItemStack item) {
            set(EquipmentSlot.FEET, item);
        }

        @Override
        public void setBoots(ItemStack item, boolean silent) {
            set(EquipmentSlot.FEET, item);
        }

        @Override
        public ItemStack[] getArmorContents() {
            return new ItemStack[] {getBoots(), getLeggings(), getChestplate(), getHelmet()};
        }

        @Override
        public void setArmorContents(ItemStack[] items) {
            EquipmentSlot[] slots = {EquipmentSlot.FEET, EquipmentSlot.LEGS, EquipmentSlot.CHEST, EquipmentSlot.HEAD};
            for (int i = 0; i < slots.length; i++) {
                set(slots[i], i < items.length ? items[i] : null);
            }
        }

        @Override
        public void clear() {
            for (EquipmentSlot slot : EquipmentSlot.values()) {
                set(slot, null);
            }
        }

        @Override
        public float getItemInHandDropChance() {
            return 0.085f;
        }

        @Override
        public void setItemInHandDropChance(float chance) {
        }

        @Override
        public float getItemInMainHandDropChance() {
            return 0.085f;
        }

        @Override
        public void setItemInMainHandDropChance(float chance) {
        }

        @Override
        public float getItemInOffHandDropChance() {
            return 0.085f;
        }

        @Override
        public void setItemInOffHandDropChance(float chance) {
        }

        @Override
        public float getHelmetDropChance() {
            return 0.085f;
        }

        @Override
        public void setHelmetDropChance(float chance) {
        }

        @Override
        public float getChestplateDropChance() {
            return 0.085f;
        }

        @Override
        public void setChestplateDropChance(float chance) {
        }

        @Override
        public float getLeggingsDropChance() {
            return 0.085f;
        }

        @Override
        public void setLeggingsDropChance(float chance) {
        }

        @Override
        public float getBootsDropChance() {
            return 0.085f;
        }

        @Override
        public void setBootsDropChance(float chance) {
        }

        @Override
        public Entity getHolder() {
            return proxy;
        }

        @Override
        public float getDropChance(EquipmentSlot slot) {
            return 0.085f;
        }

        @Override
        public void setDropChance(EquipmentSlot slot, float chance) {
        }
    }

    /** Per-entity scheduler mapped onto the global one; tasks stop once the entity is gone. */
    private final class Scheduler implements EntityScheduler {
        @Override
        public boolean execute(Plugin plugin, Runnable run, Runnable retired, long delay) {
            if (removed) {
                return false;
            }
            Main.scheduler.runTaskLater(plugin, () -> {
                if (removed) {
                    if (retired != null) retired.run();
                } else {
                    run.run();
                }
            }, delay);
            return true;
        }

        @Override
        public ScheduledTask run(Plugin plugin, Consumer<ScheduledTask> task, Runnable retired) {
            return Main.server.getGlobalRegionScheduler().run(plugin, t -> {
                if (removed) {
                    if (retired != null) retired.run();
                } else {
                    task.accept(t);
                }
            });
        }

        @Override
        public ScheduledTask runDelayed(Plugin plugin, Consumer<ScheduledTask> task, Runnable retired, long delayTicks) {
            return Main.server.getGlobalRegionScheduler().runDelayed(plugin, t -> {
                if (removed) {
                    if (retired != null) retired.run();
                } else {
                    task.accept(t);
                }
            }, delayTicks);
        }

        @Override
        public ScheduledTask runAtFixedRate(Plugin plugin, Consumer<ScheduledTask> task, Runnable retired, long initialDelayTicks, long periodTicks) {
            return Main.server.getGlobalRegionScheduler().runAtFixedRate(plugin, t -> {
                if (removed) {
                    t.cancel();
                    if (retired != null) retired.run();
                } else {
                    task.accept(t);
                }
            }, initialDelayTicks, periodTicks);
        }
    }
}
