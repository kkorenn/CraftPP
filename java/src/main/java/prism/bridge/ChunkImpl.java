package prism.bridge;

import com.google.gson.JsonElement;
import org.bukkit.Chunk;
import org.bukkit.World;
import org.bukkit.block.Block;
import org.bukkit.block.BlockState;
import org.bukkit.entity.Entity;
import org.bukkit.entity.Player;
import org.bukkit.plugin.Plugin;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/** Backing for Chunk proxies: coordinates, block access, entities inside, per-chunk plugin data. */
public final class ChunkImpl implements ProxyBase.Backing {
    private static final Map<Long, ChunkImpl> CACHE = new ConcurrentHashMap<>();

    private final WorldImpl world;
    private final int x;
    private final int z;
    private Chunk proxy;
    private final PdcImpl persistentData = new PdcImpl();
    private boolean forceLoaded;

    private ChunkImpl(WorldImpl world, int x, int z) {
        this.world = world;
        this.x = x;
        this.z = z;
        persistentData.onChange(() -> Main.rpc.api("world.setChunkPluginData", "x", x, "z", z, "data", Persistence.serialize(persistentData)));
    }

    @Override
    public void attachProxy(Object proxy) {
        this.proxy = (Chunk) proxy;
    }

    static Chunk at(WorldImpl world, int x, int z) {
        long key = ((long) x << 32) | (z & 0xFFFFFFFFL);
        ChunkImpl chunk = CACHE.computeIfAbsent(key, k -> {
            ChunkImpl backing = new ChunkImpl(world, x, z);
            ProxyBase.create(Chunk.class, backing);
            JsonElement data = Main.rpc.api("world.chunkPluginData", "x", x, "z", z);
            if (data != null && data.isJsonPrimitive()) {
                Persistence.restore(data.getAsString(), backing.persistentData);
            }
            return backing;
        });
        return chunk.proxy;
    }

    public int getX() {
        return x;
    }

    public int getZ() {
        return z;
    }

    public World getWorld() {
        return Main.server.overworld();
    }

    public Block getBlock(int localX, int y, int localZ) {
        return Main.server.overworld().getBlockAt((x << 4) + (localX & 15), y, (z << 4) + (localZ & 15));
    }

    public boolean isEntitiesLoaded() {
        return true;
    }

    public Entity[] getEntities() {
        List<Entity> inside = new ArrayList<>();
        for (Entity entity : Main.server.overworld().getEntities()) {
            if ((entity.getLocation().getBlockX() >> 4) == x && (entity.getLocation().getBlockZ() >> 4) == z) {
                inside.add(entity);
            }
        }
        return inside.toArray(new Entity[0]);
    }

    public BlockState[] getTileEntities() {
        return new BlockState[0];
    }

    public BlockState[] getTileEntities(boolean useSnapshot) {
        return new BlockState[0];
    }

    public Collection<BlockState> getTileEntities(java.util.function.Predicate<? super Block> filter, boolean useSnapshot) {
        return List.of();
    }

    public boolean isGenerated() {
        return true;
    }

    public boolean isLoaded() {
        return true;
    }

    public boolean load(boolean generate) {
        return true;
    }

    public boolean load() {
        return true;
    }

    public boolean unload(boolean save) {
        return false;
    }

    public boolean unload() {
        return false;
    }

    public boolean isSlimeChunk() {
        long seed = world.getSeed();
        java.util.Random random = new java.util.Random(seed + (long) (x * x * 0x4c1906) + (long) (x * 0x5ac0db) + (long) (z * z) * 0x4307a7L + (long) (z * 0x5f24f) ^ 0x3ad8025fL);
        return random.nextInt(10) == 0;
    }

    public boolean isForceLoaded() {
        return forceLoaded;
    }

    public void setForceLoaded(boolean value) {
        forceLoaded = value;
    }

    public boolean addPluginChunkTicket(Plugin plugin) {
        return true;
    }

    public boolean removePluginChunkTicket(Plugin plugin) {
        return true;
    }

    public Collection<Plugin> getPluginChunkTickets() {
        return List.of();
    }

    public long getInhabitedTime() {
        return 0;
    }

    public void setInhabitedTime(long ticks) {
    }

    public boolean contains(org.bukkit.block.data.BlockData block) {
        return false;
    }

    public boolean contains(org.bukkit.block.Biome biome) {
        return false;
    }

    public Chunk.LoadLevel getLoadLevel() {
        return Chunk.LoadLevel.ENTITY_TICKING;
    }

    public Collection<Player> getPlayersSeeingChunk() {
        List<Player> players = new ArrayList<>();
        for (Player player : Main.server.getOnlinePlayers()) {
            if (Math.abs((player.getLocation().getBlockX() >> 4) - x) <= 10 && Math.abs((player.getLocation().getBlockZ() >> 4) - z) <= 10) {
                players.add(player);
            }
        }
        return players;
    }

    public org.bukkit.persistence.PersistentDataContainer getPersistentDataContainer() {
        return persistentData;
    }

    public org.bukkit.ChunkSnapshot getChunkSnapshot() {
        throw new UnsupportedOperationException("CraftPP does not provide chunk snapshots yet");
    }

    @Override
    public boolean equals(Object other) {
        return ProxyBase.unwrap(other) instanceof ChunkImpl chunk && chunk.x == x && chunk.z == z;
    }

    @Override
    public int hashCode() {
        return x * 31 + z;
    }

    @Override
    public String toString() {
        return "CraftChunk{x=" + x + ",z=" + z + "}";
    }
}
