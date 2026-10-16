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
    private final Set<String> tags = new HashSet<>();
    private final Map<String, List<org.bukkit.metadata.MetadataValue>> metadata = new ConcurrentHashMap<>();
    private ItemStack itemStack;
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
        backing.itemStack = item;
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
        return Component.text(getName());
    }

    public Server getServer() {
        return org.bukkit.Bukkit.getServer();
    }

    public World getWorld() {
        return Main.server.overworld();
    }

    public Location getLocation() {
        JsonObject state = state();
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
        return getLocation().getX();
    }

    public double getY() {
        return getLocation().getY();
    }

    public double getZ() {
        return getLocation().getZ();
    }

    public float getYaw() {
        return getLocation().getYaw();
    }

    public float getPitch() {
        return getLocation().getPitch();
    }

    public boolean teleport(Location location) {
        return teleport(location, PlayerTeleportEvent.TeleportCause.PLUGIN);
    }

    public boolean teleport(Location location, PlayerTeleportEvent.TeleportCause cause) {
        if (removed) {
            return false;
        }
        Main.rpc.api("entity.teleport", "id", id, "x", location.getX(), "y", location.getY(), "z", location.getZ(), "yaw", location.getYaw(), "pitch", location.getPitch());
        return true;
    }

    public boolean teleport(Entity destination) {
        return teleport(destination.getLocation());
    }

    public java.util.concurrent.CompletableFuture<Boolean> teleportAsync(Location location) {
        return java.util.concurrent.CompletableFuture.completedFuture(teleport(location));
    }

    public void setRotation(float yaw, float pitch) {
        Location location = getLocation();
        location.setYaw(yaw);
        location.setPitch(pitch);
        teleport(location);
    }

    public void setVelocity(Vector velocity) {
        Main.rpc.api("entity.velocity", "id", id, "x", velocity.getX(), "y", velocity.getY(), "z", velocity.getZ());
    }

    public Vector getVelocity() {
        return new Vector();
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
        return removed || getHealth() <= 0;
    }

    public boolean isValid() {
        return !removed && state() != null;
    }

    public boolean isOnGround() {
        return true;
    }

    public boolean isInWater() {
        return false;
    }

    public double getHeight() {
        return 1.8;
    }

    public double getWidth() {
        return 0.6;
    }

    public org.bukkit.util.BoundingBox getBoundingBox() {
        Location location = getLocation();
        return org.bukkit.util.BoundingBox.of(location, 0.3, 0.9, 0.3).shift(0, 0.9, 0);
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
        JsonObject state = snapshot;
        return state.has("customName") && !state.get("customName").isJsonNull() ? state.get("customName").getAsString() : null;
    }

    public void setCustomName(String name) {
        Main.rpc.api("entity.setName", "id", id, "name", name, "visible", isCustomNameVisible());
        snapshot.addProperty("customName", name);
    }

    public Component customName() {
        String name = getCustomName();
        return name == null ? null : ServerImpl.LEGACY.deserialize(name);
    }

    public void customName(Component name) {
        setCustomName(name == null ? null : ServerImpl.LEGACY.serialize(name));
    }

    public boolean isCustomNameVisible() {
        return snapshot.has("customNameVisible") && snapshot.get("customNameVisible").getAsBoolean();
    }

    public void setCustomNameVisible(boolean visible) {
        snapshot.addProperty("customNameVisible", visible);
        Main.rpc.api("entity.setName", "id", id, "name", getCustomName(), "visible", visible);
    }

    public Set<String> getScoreboardTags() {
        return tags;
    }

    public boolean addScoreboardTag(String tag) {
        return tags.add(tag);
    }

    public boolean removeScoreboardTag(String tag) {
        return tags.remove(tag);
    }

    public org.bukkit.persistence.PersistentDataContainer getPersistentDataContainer() {
        return persistentData;
    }

    public void setMetadata(String key, org.bukkit.metadata.MetadataValue value) {
        metadata.computeIfAbsent(key, k -> new ArrayList<>()).add(value);
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
        return false;
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
        return false;
    }

    public void setInvulnerable(boolean value) {
    }

    public boolean isSilent() {
        return false;
    }

    public void setSilent(boolean value) {
    }

    public boolean hasGravity() {
        return true;
    }

    public void setGravity(boolean value) {
    }

    public boolean isGlowing() {
        return false;
    }

    public void setGlowing(boolean value) {
    }

    public boolean isInvisible() {
        return false;
    }

    public void setInvisible(boolean value) {
    }

    public boolean isPersistent() {
        return true;
    }

    public void setPersistent(boolean value) {
    }

    public int getTicksLived() {
        return (int) Main.scheduler.currentTick();
    }

    public void setTicksLived(int ticks) {
    }

    public int getFireTicks() {
        return 0;
    }

    public void setFireTicks(int ticks) {
    }

    public float getFallDistance() {
        return 0;
    }

    public void setFallDistance(float distance) {
    }

    public boolean isInWorld() {
        return !removed;
    }

    public boolean isTicking() {
        return !removed;
    }

    public org.bukkit.entity.Pose getPose() {
        return org.bukkit.entity.Pose.STANDING;
    }

    public boolean isSneaking() {
        return false;
    }

    public void setSneaking(boolean value) {
    }

    public org.bukkit.entity.SpawnCategory getSpawnCategory() {
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
        return num("health", 20);
    }

    public void setHealth(double health) {
        snapshot.addProperty("health", health);
        Main.rpc.api("entity.setHealth", "id", id, "value", health);
        if (health <= 0) {
            removed = true;
            BY_ID.remove(id);
        if (prism.bridge.nms.Nms.get() != null) { prism.bridge.nms.Nms.get().removeEntity(id); }
        }
    }

    public double getMaxHealth() {
        return 20;
    }

    public void setMaxHealth(double health) {
    }

    public void damage(double amount) {
        Main.rpc.api("entity.damage", "id", id, "amount", amount);
        state();
    }

    public void damage(double amount, Entity source) {
        damage(amount);
    }

    public double getAbsorptionAmount() {
        return 0;
    }

    public void setAbsorptionAmount(double amount) {
    }

    public org.bukkit.attribute.AttributeInstance getAttribute(org.bukkit.attribute.Attribute attribute) {
        return null;
    }

    public void registerAttribute(org.bukkit.attribute.Attribute attribute) {
    }

    public Location getEyeLocation() {
        return getLocation().add(0, 1.5, 0);
    }

    public double getEyeHeight() {
        return 1.5;
    }

    public double getEyeHeight(boolean ignorePose) {
        return 1.5;
    }

    public boolean hasAI() {
        return false;
    }

    public void setAI(boolean ai) {
    }

    public boolean hasLineOfSight(Entity other) {
        return true;
    }

    public boolean isLeashed() {
        return false;
    }

    public boolean addPotionEffect(org.bukkit.potion.PotionEffect effect) {
        return false;
    }

    public boolean hasPotionEffect(org.bukkit.potion.PotionEffectType type) {
        return false;
    }

    public java.util.Collection<org.bukkit.potion.PotionEffect> getActivePotionEffects() {
        return List.of();
    }

    public void removePotionEffect(org.bukkit.potion.PotionEffectType type) {
    }

    public boolean getRemoveWhenFarAway() {
        return false;
    }

    public void setRemoveWhenFarAway(boolean value) {
    }

    public boolean getCanPickupItems() {
        return false;
    }

    public void setCanPickupItems(boolean value) {
    }

    public boolean isCollidable() {
        return true;
    }

    public void setCollidable(boolean value) {
    }

    public Player getKiller() {
        return null;
    }

    public int getNoDamageTicks() {
        return 0;
    }

    public void setNoDamageTicks(int ticks) {
    }

    public double getLastDamage() {
        return 0;
    }

    public void setLastDamage(double damage) {
    }

    public int getRemainingAir() {
        return 300;
    }

    public void setRemainingAir(int ticks) {
    }

    public int getMaximumAir() {
        return 300;
    }

    public boolean isGliding() {
        return false;
    }

    public void setGliding(boolean value) {
    }

    public boolean isSwimming() {
        return false;
    }

    public boolean isSleeping() {
        return false;
    }

    public boolean isClimbing() {
        return false;
    }

    public EntityEquipment getEquipment() {
        return new Equipment();
    }

    // ----- Item entity -----

    public ItemStack getItemStack() {
        return itemStack == null ? new PrismItemStack(Material.AIR, 0) : itemStack;
    }

    public void setItemStack(ItemStack stack) {
        itemStack = stack;
    }

    public int getPickupDelay() {
        return 10;
    }

    public void setPickupDelay(int delay) {
    }

    public boolean canPlayerPickup() {
        return true;
    }

    public void setCanPlayerPickup(boolean value) {
    }

    public boolean canMobPickup() {
        return false;
    }

    public void setCanMobPickup(boolean value) {
    }

    public boolean isUnlimitedLifetime() {
        return false;
    }

    public void setUnlimitedLifetime(boolean value) {
    }

    public UUID getThrower() {
        return null;
    }

    public void setThrower(UUID thrower) {
    }

    public UUID getOwner() {
        return null;
    }

    public void setOwner(UUID owner) {
    }

    // ----- ArmorStand / misc common calls -----

    public boolean isVisible() {
        return true;
    }

    public void setVisible(boolean value) {
    }

    public boolean isMarker() {
        return false;
    }

    public void setMarker(boolean value) {
    }

    public boolean hasBasePlate() {
        return true;
    }

    public void setBasePlate(boolean value) {
    }

    public boolean hasArms() {
        return false;
    }

    public void setArms(boolean value) {
    }

    public boolean isSmall() {
        return false;
    }

    public void setSmall(boolean value) {
    }

    public boolean isBaby() {
        return false;
    }

    public void setBaby(boolean value) {
    }

    public boolean isAdult() {
        return true;
    }

    public void setAdult() {
    }

    public int getAge() {
        return 0;
    }

    public void setAge(int age) {
    }

    public LivingEntity getTarget() {
        return null;
    }

    public void setTarget(LivingEntity target) {
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
        private final Map<EquipmentSlot, ItemStack> items = new ConcurrentHashMap<>();

        private ItemStack get(EquipmentSlot slot) {
            ItemStack item = items.get(slot);
            return item == null ? new PrismItemStack(Material.AIR, 0) : item;
        }

        private void set(EquipmentSlot slot, ItemStack item) {
            if (item == null || item.getType().isAir()) {
                items.remove(slot);
            } else {
                items.put(slot, item.clone());
            }
            int protocolSlot = switch (slot) {
                case HAND -> 0;
                case OFF_HAND -> 1;
                case FEET -> 2;
                case LEGS -> 3;
                case CHEST -> 4;
                case HEAD -> 5;
                case BODY -> 6;
                default -> 7;
            };
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
                if (items.containsKey(slot)) {
                    set(slot, null);
                }
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
