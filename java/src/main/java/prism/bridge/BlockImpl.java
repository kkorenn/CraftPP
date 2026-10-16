package prism.bridge;

import com.google.gson.JsonObject;
import org.bukkit.Chunk;
import org.bukkit.Location;
import org.bukkit.Material;
import org.bukkit.World;
import org.bukkit.block.Biome;
import org.bukkit.block.Block;
import org.bukkit.block.BlockFace;
import org.bukkit.block.BlockState;

import java.util.Locale;

/** Backing for {@link Block} proxies: coordinates plus read-through type lookups. */
public final class BlockImpl implements ProxyBase.Backing {
    private final WorldImpl world;
    private final int x;
    private final int y;
    private final int z;
    private Block proxy;

    BlockImpl(WorldImpl world, int x, int y, int z) {
        this.world = world;
        this.x = x;
        this.y = y;
        this.z = z;
    }

    @Override
    public void attachProxy(Object proxy) {
        this.proxy = (Block) proxy;
    }

    static Material materialAt(int x, int y, int z) {
        JsonObject block = Main.rpc.api("world.block", "x", x, "y", y, "z", z).getAsJsonObject();
        return materialFromKey(block.get("block").getAsString());
    }

    static Material materialFromKey(String key) {
        Material material = Material.matchMaterial(key);
        return material == null ? Material.AIR : material;
    }

    static void setMaterialAt(int x, int y, int z, Material material) {
        Main.rpc.api("world.setBlock", "x", x, "y", y, "z", z, "block", material.getKey().asString());
    }

    public int getX() {
        return x;
    }

    public int getY() {
        return y;
    }

    public int getZ() {
        return z;
    }

    public World getWorld() {
        return Main.server.overworld();
    }

    public Location getLocation() {
        return new Location(getWorld(), x, y, z);
    }

    public Location getLocation(Location target) {
        if (target == null) {
            return getLocation();
        }
        target.setWorld(getWorld());
        target.setX(x);
        target.setY(y);
        target.setZ(z);
        target.setYaw(0);
        target.setPitch(0);
        return target;
    }

    public Material getType() {
        return materialAt(x, y, z);
    }

    public void setType(Material material) {
        setMaterialAt(x, y, z, material);
    }

    public void setType(Material material, boolean applyPhysics) {
        setType(material);
    }

    public org.bukkit.block.data.BlockData getBlockData() {
        return BlockDataImpl.fromJson(Main.rpc.api("world.blockData", "x", x, "y", y, "z", z).getAsJsonObject());
    }

    public void setBlockData(org.bukkit.block.data.BlockData data) {
        Main.server.overworld().setBlockData(x, y, z, data);
    }

    public void setBlockData(org.bukkit.block.data.BlockData data, boolean applyPhysics) {
        setBlockData(data);
    }

    public org.bukkit.Chunk getChunk() {
        return Main.server.overworld().getChunkAt(x >> 4, z >> 4);
    }

    public org.bukkit.block.Biome getBiome() {
        return Main.server.overworld().getBiome(x, y, z);
    }

    public void setBiome(org.bukkit.block.Biome biome) {
        Main.server.overworld().setBiome(x, y, z, biome);
    }

    public byte getLightLevel() {
        return (byte) Math.max(getLightFromSky(), getLightFromBlocks());
    }

    public byte getLightFromSky() {
        return (byte) Main.rpc.api("world.lightAt", "x", x, "y", y, "z", z).getAsJsonObject().get("sky").getAsInt();
    }

    public byte getLightFromBlocks() {
        return (byte) Main.rpc.api("world.lightAt", "x", x, "y", y, "z", z).getAsJsonObject().get("block").getAsInt();
    }

    public boolean isEmpty() {
        return getType().isAir();
    }

    public boolean isLiquid() {
        Material type = getType();
        return type == Material.WATER || type == Material.LAVA;
    }

    public boolean isSolid() {
        return getType().isSolid();
    }

    public boolean isPassable() {
        return !getType().isSolid();
    }

    public boolean isCollidable() {
        return getType().isCollidable();
    }

    public boolean isBurnable() {
        return getType().isBurnable();
    }

    public boolean isReplaceable() {
        return getType().isAir();
    }

    public Block getRelative(int dx, int dy, int dz) {
        return ProxyBase.create(Block.class, new BlockImpl(world, x + dx, y + dy, z + dz));
    }

    public Block getRelative(BlockFace face) {
        return getRelative(face.getModX(), face.getModY(), face.getModZ());
    }

    public Block getRelative(BlockFace face, int distance) {
        return getRelative(face.getModX() * distance, face.getModY() * distance, face.getModZ() * distance);
    }

    public BlockFace getFace(Block block) {
        for (BlockFace face : BlockFace.values()) {
            if (x + face.getModX() == block.getX() && y + face.getModY() == block.getY() && z + face.getModZ() == block.getZ()) {
                return face;
            }
        }
        return null;
    }

    public BlockState getState() {
        return ProxyBase.create(BlockState.class, new StateImpl(this, getType()));
    }

    public BlockState getState(boolean useSnapshot) {
        return getState();
    }

    public boolean breakNaturally() {
        setType(Material.AIR);
        return true;
    }

    public boolean breakNaturally(org.bukkit.inventory.ItemStack tool) {
        return breakNaturally();
    }

    public java.util.Collection<org.bukkit.inventory.ItemStack> getDrops() {
        return java.util.List.of();
    }

    public java.util.Collection<org.bukkit.inventory.ItemStack> getDrops(org.bukkit.inventory.ItemStack tool) {
        return java.util.List.of();
    }

    public boolean isBlockPowered() {
        return false;
    }

    public boolean isBlockIndirectlyPowered() {
        return false;
    }

    public int getBlockPower() {
        return 0;
    }

    public org.bukkit.block.PistonMoveReaction getPistonMoveReaction() {
        return org.bukkit.block.PistonMoveReaction.MOVE;
    }

    public String getTranslationKey() {
        return "block." + getType().getKey().getNamespace() + "." + getType().getKey().getKey();
    }

    public boolean isPreferredTool(org.bukkit.inventory.ItemStack tool) {
        return true;
    }

    public float getBreakSpeed(org.bukkit.entity.Player player) {
        return 1.0f;
    }

    public boolean canPlace(org.bukkit.block.data.BlockData data) {
        return true;
    }

    public org.bukkit.util.BoundingBox getBoundingBox() {
        return new org.bukkit.util.BoundingBox(x, y, z, x + 1, y + 1, z + 1);
    }

    public java.util.List<org.bukkit.metadata.MetadataValue> getMetadata(String key) {
        return java.util.List.of();
    }

    public boolean hasMetadata(String key) {
        return false;
    }

    @Override
    public boolean equals(Object other) {
        return ProxyBase.unwrap(other) instanceof BlockImpl block && block.x == x && block.y == y && block.z == z;
    }

    @Override
    public int hashCode() {
        return (x * 31 + y) * 31 + z;
    }

    @Override
    public String toString() {
        return "CraftBlock{pos=" + x + "," + y + "," + z + ",type=" + getType().name().toLowerCase(Locale.ROOT) + "}";
    }

    /** Snapshot BlockState: the type at capture time. */
    static final class StateImpl {
        private final BlockImpl block;
        private Material type;

        StateImpl(BlockImpl block, Material type) {
            this.block = block;
            this.type = type;
        }

        public Block getBlock() {
            return block.proxy;
        }

        public Material getType() {
            return type;
        }

        public void setType(Material material) {
            type = material;
        }

        public org.bukkit.block.data.BlockData getBlockData() {
            return org.bukkit.Bukkit.createBlockData(type);
        }

        public void setBlockData(org.bukkit.block.data.BlockData data) {
            type = data.getMaterial();
        }

        public World getWorld() {
            return block.getWorld();
        }

        public int getX() {
            return block.x;
        }

        public int getY() {
            return block.y;
        }

        public int getZ() {
            return block.z;
        }

        public Location getLocation() {
            return block.getLocation();
        }

        public Location getLocation(Location target) {
            return block.getLocation(target);
        }

        public Chunk getChunk() {
            return block.getChunk();
        }

        public boolean update() {
            return update(false, true);
        }

        public boolean update(boolean force) {
            return update(force, true);
        }

        public boolean update(boolean force, boolean applyPhysics) {
            block.setType(type);
            return true;
        }

        public boolean isPlaced() {
            return true;
        }

        public byte getLightLevel() {
            return 15;
        }

        public byte getRawData() {
            return 0;
        }

        public void setRawData(byte data) {
        }

        public boolean isCollidable() {
            return type.isCollidable();
        }

        public boolean isSnapshot() {
            return true;
        }

        public java.util.List<org.bukkit.metadata.MetadataValue> getMetadata(String key) {
            return java.util.List.of();
        }

        public boolean hasMetadata(String key) {
            return false;
        }

        public BlockState copy() {
            return ProxyBase.create(BlockState.class, new StateImpl(block, type));
        }

        public BlockState copy(Location location) {
            return copy();
        }
    }
}
