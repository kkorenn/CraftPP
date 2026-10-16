package prism.bridge;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import net.kyori.adventure.audience.Audience;
import org.bukkit.Difficulty;
import org.bukkit.Location;
import org.bukkit.Material;
import org.bukkit.NamespacedKey;
import org.bukkit.World;
import org.bukkit.block.Block;
import org.bukkit.entity.Entity;
import org.bukkit.entity.Player;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/** Backing for the single overworld {@link World} proxy. */
public final class WorldImpl implements ProxyBase.Backing, ProxyBase.HandleProvider {
    private final String name;
    private final long seed;
    private final UUID uid;
    private World proxy;

    @Override
    public Object nmsHandle() {
        prism.bridge.nms.Nms nms = prism.bridge.nms.Nms.get();
        return nms == null ? null : nms.level();
    }

    WorldImpl(String name, long seed) {
        this.name = name;
        this.seed = seed;
        this.uid = UUID.nameUUIDFromBytes(("CraftPP:world:" + name).getBytes());
    }

    @Override
    public void attachProxy(Object proxy) {
        this.proxy = (World) proxy;
    }

    public String getName() {
        return name;
    }

    public UUID getUID() {
        return uid;
    }

    public NamespacedKey getKey() {
        return NamespacedKey.minecraft("overworld");
    }

    public net.kyori.adventure.key.Key key() {
        return getKey();
    }

    public long getSeed() {
        return seed;
    }

    public World.Environment getEnvironment() {
        return World.Environment.NORMAL;
    }

    public int getMaxHeight() {
        return 320;
    }

    public int getMinHeight() {
        return -64;
    }

    public int getLogicalHeight() {
        return 384;
    }

    public int getSeaLevel() {
        return 63;
    }

    public File getWorldFolder() {
        return new File(Main.server.getWorldContainer(), name);
    }

    public Block getBlockAt(int x, int y, int z) {
        return ProxyBase.create(Block.class, new BlockImpl(this, x, y, z));
    }

    public Block getBlockAt(Location location) {
        return getBlockAt(location.getBlockX(), location.getBlockY(), location.getBlockZ());
    }

    public Material getType(int x, int y, int z) {
        return BlockImpl.materialAt(x, y, z);
    }

    public Material getType(Location location) {
        return getType(location.getBlockX(), location.getBlockY(), location.getBlockZ());
    }

    public void setType(int x, int y, int z, Material material) {
        BlockImpl.setMaterialAt(x, y, z, material);
    }

    public void setType(Location location, Material material) {
        setType(location.getBlockX(), location.getBlockY(), location.getBlockZ(), material);
    }

    public int getHighestBlockYAt(int x, int z) {
        return Main.rpc.api("world.highestY", "x", x, "z", z).getAsInt();
    }

    public int getHighestBlockYAt(Location location) {
        return getHighestBlockYAt(location.getBlockX(), location.getBlockZ());
    }

    public Block getHighestBlockAt(int x, int z) {
        return getBlockAt(x, getHighestBlockYAt(x, z), z);
    }

    public Block getHighestBlockAt(Location location) {
        return getHighestBlockAt(location.getBlockX(), location.getBlockZ());
    }

    public boolean isChunkLoaded(int x, int z) {
        return true;
    }

    public boolean isChunkGenerated(int x, int z) {
        return true;
    }

    public boolean loadChunk(int x, int z, boolean generate) {
        return true;
    }

    public void loadChunk(int x, int z) {
    }

    public boolean unloadChunk(int x, int z) {
        return true;
    }

    public boolean unloadChunk(int x, int z, boolean save) {
        return true;
    }

    public long getTime() {
        return getFullTime() % 24000;
    }

    public void setTime(long time) {
        long full = getFullTime();
        setFullTime(full - full % 24000 + time);
    }

    public long getFullTime() {
        return Main.rpc.api("world.time").getAsLong();
    }

    public void setFullTime(long time) {
        Main.rpc.api("world.setTime", "time", time);
    }

    public long getGameTime() {
        return getFullTime();
    }

    public boolean isDayTime() {
        long time = getTime();
        return time < 12300 || time > 23850;
    }

    public boolean hasStorm() {
        return false;
    }

    public void setStorm(boolean value) {
    }

    public boolean isThundering() {
        return false;
    }

    public void setThundering(boolean value) {
    }

    public boolean isClearWeather() {
        return true;
    }

    public Difficulty getDifficulty() {
        return Difficulty.values()[Main.rpc.api("world.difficulty").getAsInt()];
    }

    public void setDifficulty(Difficulty difficulty) {
        Main.rpc.api("world.setDifficulty", "value", difficulty.ordinal());
    }

    public Location getSpawnLocation() {
        JsonElement result = Main.rpc.api("world.spawn");
        if (result == null || !result.isJsonObject()) {
            return new Location(proxy, 0.5, 64, 0.5);
        }
        JsonObject spawn = result.getAsJsonObject();
        return new Location(proxy, spawn.get("x").getAsInt() + 0.5, spawn.get("y").getAsInt(), spawn.get("z").getAsInt() + 0.5);
    }

    public boolean setSpawnLocation(Location location) {
        return setSpawnLocation(location.getBlockX(), location.getBlockY(), location.getBlockZ());
    }

    public boolean setSpawnLocation(int x, int y, int z) {
        Main.rpc.api("world.setSpawn", "x", x, "y", y, "z", z);
        return true;
    }

    public boolean setSpawnLocation(int x, int y, int z, float angle) {
        return setSpawnLocation(x, y, z);
    }

    public List<Player> getPlayers() {
        return new ArrayList<>(Main.server.getOnlinePlayers());
    }

    public List<Entity> getEntities() {
        List<Entity> entities = new ArrayList<>(Main.server.getOnlinePlayers());
        entities.addAll(EntityImpl.all());
        return entities;
    }

    public List<org.bukkit.entity.LivingEntity> getLivingEntities() {
        List<org.bukkit.entity.LivingEntity> living = new ArrayList<>(Main.server.getOnlinePlayers());
        for (Entity entity : EntityImpl.all()) {
            if (entity instanceof org.bukkit.entity.LivingEntity livingEntity) {
                living.add(livingEntity);
            }
        }
        return living;
    }

    public <T extends Entity> java.util.Collection<T> getEntitiesByClass(Class<T> type) {
        List<T> result = new ArrayList<>();
        for (Entity entity : getEntities()) {
            if (type.isInstance(entity)) {
                result.add(type.cast(entity));
            }
        }
        return result;
    }

    public java.util.Collection<Entity> getEntitiesByClasses(Class<?>... types) {
        List<Entity> result = new ArrayList<>();
        for (Entity entity : getEntities()) {
            for (Class<?> type : types) {
                if (type.isInstance(entity)) {
                    result.add(entity);
                    break;
                }
            }
        }
        return result;
    }

    public java.util.Collection<Entity> getNearbyEntities(Location location, double x, double y, double z) {
        List<Entity> result = new ArrayList<>();
        for (Entity entity : getEntities()) {
            Location at = entity.getLocation();
            if (Math.abs(at.getX() - location.getX()) <= x && Math.abs(at.getY() - location.getY()) <= y && Math.abs(at.getZ() - location.getZ()) <= z) {
                result.add(entity);
            }
        }
        return result;
    }

    public java.util.Collection<Entity> getNearbyEntities(Location location, double x, double y, double z, java.util.function.Predicate<? super Entity> filter) {
        List<Entity> result = new ArrayList<>();
        for (Entity entity : getNearbyEntities(location, x, y, z)) {
            if (filter == null || filter.test(entity)) {
                result.add(entity);
            }
        }
        return result;
    }

    public Entity spawnEntity(Location location, org.bukkit.entity.EntityType type) {
        return EntityImpl.spawn(location, type, org.bukkit.event.entity.CreatureSpawnEvent.SpawnReason.CUSTOM, null);
    }

    public Entity spawnEntity(Location location, org.bukkit.entity.EntityType type, boolean randomizeData) {
        return spawnEntity(location, type);
    }

    public Entity spawnEntity(Location location, org.bukkit.entity.EntityType type, org.bukkit.event.entity.CreatureSpawnEvent.SpawnReason reason) {
        return EntityImpl.spawn(location, type, reason, null);
    }

    public Entity spawnEntity(Location location, org.bukkit.entity.EntityType type, org.bukkit.event.entity.CreatureSpawnEvent.SpawnReason reason, java.util.function.Consumer<? super Entity> function) {
        Entity entity = EntityImpl.spawn(location, type, reason, null);
        if (function != null) {
            function.accept(entity);
        }
        return entity;
    }

    @SuppressWarnings("unchecked")
    public <T extends Entity> T spawn(Location location, Class<T> type) {
        return (T) spawnEntity(location, entityTypeFor(type));
    }

    @SuppressWarnings("unchecked")
    public <T extends Entity> T spawn(Location location, Class<T> type, java.util.function.Consumer<? super T> function) {
        T entity = (T) spawnEntity(location, entityTypeFor(type));
        if (function != null) {
            function.accept(entity);
        }
        return entity;
    }

    private static org.bukkit.entity.EntityType entityTypeFor(Class<?> type) {
        for (org.bukkit.entity.EntityType candidate : org.bukkit.entity.EntityType.values()) {
            if (candidate.getEntityClass() == type) {
                return candidate;
            }
        }
        for (org.bukkit.entity.EntityType candidate : org.bukkit.entity.EntityType.values()) {
            if (candidate.getEntityClass() != null && type.isAssignableFrom(candidate.getEntityClass()) && candidate.isSpawnable()) {
                return candidate;
            }
        }
        throw new IllegalArgumentException("Cannot spawn an entity for " + type.getName());
    }

    public org.bukkit.entity.Item dropItem(Location location, org.bukkit.inventory.ItemStack item) {
        return (org.bukkit.entity.Item) EntityImpl.spawn(location, org.bukkit.entity.EntityType.ITEM, org.bukkit.event.entity.CreatureSpawnEvent.SpawnReason.CUSTOM, item);
    }

    public org.bukkit.entity.Item dropItem(Location location, org.bukkit.inventory.ItemStack item, java.util.function.Consumer<? super org.bukkit.entity.Item> function) {
        org.bukkit.entity.Item dropped = dropItem(location, item);
        if (function != null) {
            function.accept(dropped);
        }
        return dropped;
    }

    public org.bukkit.entity.Item dropItemNaturally(Location location, org.bukkit.inventory.ItemStack item) {
        org.bukkit.entity.Item dropped = dropItem(location.clone().add(0.5 - Math.random() * 0.5, 0.2, 0.5 - Math.random() * 0.5), item);
        dropped.setVelocity(new org.bukkit.util.Vector((Math.random() - 0.5) * 0.1, 0.2, (Math.random() - 0.5) * 0.1));
        return dropped;
    }

    public org.bukkit.entity.LightningStrike strikeLightning(Location location) {
        return (org.bukkit.entity.LightningStrike) spawnEntity(location, org.bukkit.entity.EntityType.LIGHTNING_BOLT);
    }

    public org.bukkit.entity.LightningStrike strikeLightningEffect(Location location) {
        return strikeLightning(location);
    }

    public int getEntityCount() {
        return getEntities().size();
    }

    // ----- chunks -----

    public org.bukkit.Chunk getChunkAt(int x, int z) {
        return ChunkImpl.at(this, x, z);
    }

    public org.bukkit.Chunk getChunkAt(Location location) {
        return getChunkAt(location.getBlockX() >> 4, location.getBlockZ() >> 4);
    }

    public org.bukkit.Chunk getChunkAt(Block block) {
        return getChunkAt(block.getX() >> 4, block.getZ() >> 4);
    }

    public org.bukkit.Chunk getChunkAt(long chunkKey) {
        return getChunkAt((int) chunkKey, (int) (chunkKey >> 32));
    }

    public org.bukkit.Chunk getChunkAt(int x, int z, boolean generate) {
        return getChunkAt(x, z);
    }

    public org.bukkit.Chunk[] getLoadedChunks() {
        java.util.Set<Long> keys = new java.util.LinkedHashSet<>();
        List<org.bukkit.Chunk> chunks = new ArrayList<>();
        for (Player player : getPlayers()) {
            int cx = player.getLocation().getBlockX() >> 4;
            int cz = player.getLocation().getBlockZ() >> 4;
            for (int dx = -3; dx <= 3; dx++) {
                for (int dz = -3; dz <= 3; dz++) {
                    if (keys.add((((long) (cx + dx)) << 32) | ((cz + dz) & 0xFFFFFFFFL))) {
                        chunks.add(getChunkAt(cx + dx, cz + dz));
                    }
                }
            }
        }
        return chunks.toArray(new org.bukkit.Chunk[0]);
    }

    public boolean isChunkLoaded(org.bukkit.Chunk chunk) {
        return true;
    }

    public java.util.concurrent.CompletableFuture<org.bukkit.Chunk> getChunkAtAsync(int x, int z) {
        return java.util.concurrent.CompletableFuture.completedFuture(getChunkAt(x, z));
    }

    public java.util.concurrent.CompletableFuture<org.bukkit.Chunk> getChunkAtAsync(int x, int z, boolean generate) {
        return getChunkAtAsync(x, z);
    }

    public java.util.concurrent.CompletableFuture<org.bukkit.Chunk> getChunkAtAsync(Location location) {
        return getChunkAtAsync(location.getBlockX() >> 4, location.getBlockZ() >> 4);
    }

    public java.util.concurrent.CompletableFuture<org.bukkit.Chunk> getChunkAtAsync(Block block) {
        return getChunkAtAsync(block.getX() >> 4, block.getZ() >> 4);
    }

    // ----- biomes / border / block data -----

    public org.bukkit.block.Biome getBiome(int x, int y, int z) {
        String key = Main.rpc.api("world.biome", "x", x, "y", y, "z", z).getAsString();
        Object biome = Registries.byKey(io.papermc.paper.registry.RegistryKey.BIOME).get(NamespacedKey.fromString(key));
        return biome instanceof org.bukkit.block.Biome value ? value : null;
    }

    public org.bukkit.block.Biome getBiome(int x, int z) {
        return getBiome(x, 64, z);
    }

    public org.bukkit.block.Biome getBiome(Location location) {
        return getBiome(location.getBlockX(), location.getBlockY(), location.getBlockZ());
    }

    public void setBiome(int x, int y, int z, org.bukkit.block.Biome biome) {
        Main.rpc.api("world.setBiome", "x", x, "y", y, "z", z, "biome", biome.getKey().asString());
    }

    public void setBiome(int x, int z, org.bukkit.block.Biome biome) {
        for (int y = -64; y < 320; y += 4) {
            setBiome(x, y, z, biome);
        }
    }

    public void setBiome(Location location, org.bukkit.block.Biome biome) {
        setBiome(location.getBlockX(), location.getBlockY(), location.getBlockZ(), biome);
    }

    public org.bukkit.WorldBorder getWorldBorder() {
        return new WorldBorderImpl(proxy);
    }

    public org.bukkit.block.data.BlockData getBlockData(int x, int y, int z) {
        return BlockDataImpl.fromJson(Main.rpc.api("world.blockData", "x", x, "y", y, "z", z).getAsJsonObject());
    }

    public org.bukkit.block.data.BlockData getBlockData(Location location) {
        return getBlockData(location.getBlockX(), location.getBlockY(), location.getBlockZ());
    }

    public void setBlockData(int x, int y, int z, org.bukkit.block.data.BlockData data) {
        BlockDataImpl impl = BlockDataImpl.unwrap(data);
        Main.rpc.api("world.setBlockData", "x", x, "y", y, "z", z, "block", data.getMaterial().getKey().asString(),
                "properties", impl == null ? new com.google.gson.JsonObject() : impl.propertiesJson());
    }

    public void setBlockData(Location location, org.bukkit.block.data.BlockData data) {
        setBlockData(location.getBlockX(), location.getBlockY(), location.getBlockZ(), data);
    }

    public org.bukkit.persistence.PersistentDataContainer getPersistentDataContainer() {
        return persistentData;
    }

    private final PdcImpl persistentData = createWorldData();

    private PdcImpl createWorldData() {
        PdcImpl container = new PdcImpl();
        JsonElement stored = Main.rpc.api("world.pluginData");
        if (stored != null && stored.isJsonPrimitive()) {
            Persistence.restore(stored.getAsString(), container);
        }
        container.onChange(() -> Main.rpc.api("world.setPluginData", "data", Persistence.serialize(container)));
        return container;
    }

    public boolean createExplosion(Location location, float power) {
        return true;
    }

    public boolean createExplosion(Location location, float power, boolean setFire) {
        return true;
    }

    public boolean createExplosion(Location location, float power, boolean setFire, boolean breakBlocks) {
        return true;
    }

    public void playSound(Location location, org.bukkit.Sound sound, float volume, float pitch) {
        for (Player player : getPlayers()) {
            player.playSound(location, sound, volume, pitch);
        }
    }

    public void playSound(Location location, String sound, float volume, float pitch) {
        for (Player player : getPlayers()) {
            player.playSound(location, sound, volume, pitch);
        }
    }

    public void playSound(Location location, org.bukkit.Sound sound, org.bukkit.SoundCategory category, float volume, float pitch) {
        playSound(location, sound, volume, pitch);
    }

    public void playSound(Location location, String sound, org.bukkit.SoundCategory category, float volume, float pitch) {
        playSound(location, sound, volume, pitch);
    }

    public Entity getEntity(UUID uuid) {
        for (Entity entity : getEntities()) {
            if (entity.getUniqueId().equals(uuid)) {
                return entity;
            }
        }
        return null;
    }

    public int getPlayerCount() {
        return getPlayers().size();
    }

    public int getTickableTileEntityCount() {
        return 0;
    }

    public int getTileEntityCount() {
        return 0;
    }

    public int getChunkCount() {
        return 0;
    }

    public Iterable<? extends Audience> audiences() {
        return getPlayers();
    }

    public boolean getPVP() {
        return true;
    }

    public void setPVP(boolean value) {
    }

    public boolean getKeepSpawnInMemory() {
        return true;
    }

    public void setKeepSpawnInMemory(boolean value) {
    }

    public boolean isAutoSave() {
        return true;
    }

    public void setAutoSave(boolean value) {
    }

    public void save() {
        Main.rpc.api("world.save");
    }

    public boolean canGenerateStructures() {
        return true;
    }

    public boolean isHardcore() {
        return false;
    }

    public org.bukkit.WorldType getWorldType() {
        return org.bukkit.WorldType.NORMAL;
    }

    public org.bukkit.generator.ChunkGenerator getGenerator() {
        return null;
    }

    public org.bukkit.generator.BiomeProvider getBiomeProvider() {
        return null;
    }

    public boolean setGameRuleValue(String rule, String value) {
        Main.rpc.api("world.setGameRule", "rule", rule, "value", value);
        return true;
    }

    public String getGameRuleValue(String rule) {
        JsonElement value = Main.rpc.api("world.gameRule", "rule", rule);
        return value == null || value.isJsonNull() ? null : value.getAsString();
    }

    public String[] getGameRules() {
        List<String> names = new ArrayList<>();
        for (JsonElement element : Main.rpc.api("world.gameRules").getAsJsonArray()) {
            names.add(element.getAsString());
        }
        return names.toArray(new String[0]);
    }

    public boolean isGameRule(String rule) {
        return getGameRuleValue(rule) != null;
    }

    @SuppressWarnings("unchecked")
    public <T> T getGameRuleValue(org.bukkit.GameRule<T> rule) {
        String value = getGameRuleValue(rule.getName());
        if (value == null) {
            return null;
        }
        if (rule.getType() == Boolean.class) {
            return (T) Boolean.valueOf(value);
        }
        return (T) Integer.valueOf(value);
    }

    public <T> T getGameRuleDefault(org.bukkit.GameRule<T> rule) {
        return getGameRuleValue(rule);
    }

    public <T> boolean setGameRule(org.bukkit.GameRule<T> rule, T value) {
        return setGameRuleValue(rule.getName(), String.valueOf(value));
    }

    public double getHumidity(int x, int y, int z) {
        return 0.5;
    }

    public double getTemperature(int x, int y, int z) {
        return 0.5;
    }

    @Override
    public boolean equals(Object other) {
        return ProxyBase.unwrap(other) instanceof WorldImpl world && world.uid.equals(uid);
    }

    @Override
    public int hashCode() {
        return uid.hashCode();
    }

    @Override
    public String toString() {
        return "CraftWorld{name=" + name + "}";
    }
}
