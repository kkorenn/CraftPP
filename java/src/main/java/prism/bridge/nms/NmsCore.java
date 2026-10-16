package prism.bridge.nms;

import com.google.gson.JsonObject;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.ByteBufUtil;
import io.netty.buffer.Unpooled;
import net.minecraft.SharedConstants;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.network.ProtocolInfo;
import net.minecraft.network.RegistryFriendlyByteBuf;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.Packet;
import net.minecraft.network.protocol.game.ClientGamePacketListener;
import net.minecraft.network.protocol.game.GameProtocols;
import net.minecraft.network.protocol.game.ServerGamePacketListener;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.dedicated.DedicatedServer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.phys.Vec3;
import prism.bridge.Main;

import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * Boots the vanilla registries (the same jar-truth path
 * tools/jar-truth/MaterialTruth.java uses) and owns the play-protocol codecs
 * used to turn NMS packet objects into bytes and back.
 */
public final class NmsCore implements Nms {
    static final sun.misc.Unsafe UNSAFE;
    private static final Logger LOGGER = Logger.getLogger("NMS");

    static {
        try {
            Field field = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
            field.setAccessible(true);
            UNSAFE = (sun.misc.Unsafe) field.get(null);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    static RegistryAccess.Frozen registries;
    static HolderLookup.Provider vanillaLookup;
    private static ProtocolInfo<ClientGamePacketListener> clientbound;
    private static ProtocolInfo<ServerGamePacketListener> serverbound;
    private static Object fakeServer;
    private static PrismServerLevel overworld;
    final Map<UUID, NmsPlayer> players = new ConcurrentHashMap<>();
    final Map<Integer, Entity> entities = new ConcurrentHashMap<>();

    public NmsCore() throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        // Item data components are data-driven in 26.3; bind them like the server does.
        vanillaLookup = VanillaRegistries.createWorldLookup();
        for (var pending : BuiltInRegistries.DATA_COMPONENT_INITIALIZERS.build(vanillaLookup)) {
            pending.apply();
        }
        // ponytail: built-in registries only; datapack registries (biomes, dimension types)
        // are read from the vanilla lookup where needed (PrismServerLevel).
        registries = RegistryAccess.fromRegistryOfRegistries(BuiltInRegistries.REGISTRY);
        clientbound = GameProtocols.CLIENTBOUND_TEMPLATE.bind(RegistryFriendlyByteBuf.decorator(registries));
        serverbound = GameProtocols.SERVERBOUND_TEMPLATE.bind(RegistryFriendlyByteBuf.decorator(registries), new GameProtocols.Context() {
            @Override public boolean hasInfiniteMaterials() { return false; }
            @Override public boolean canUseCommandBlocks() { return false; }
        });
        fakeServer = UNSAFE.allocateInstance(DedicatedServer.class);
        // Entity constructors and level code read server.getWorldData() for feature flags, difficulty and game rules.
        Object worldData = java.lang.reflect.Proxy.newProxyInstance(NmsCore.class.getClassLoader(),
                new Class<?>[] {net.minecraft.world.level.storage.WorldData.class}, (proxy, method, args) -> switch (method.getName()) {
                    case "enabledFeatures" -> net.minecraft.world.flag.FeatureFlags.DEFAULT_FLAGS;
                    case "getDifficulty" -> net.minecraft.world.Difficulty.NORMAL;
                    case "getGameType" -> net.minecraft.world.level.GameType.SURVIVAL;
                    case "getGameRules" -> new net.minecraft.world.level.gamerules.GameRules(net.minecraft.world.flag.FeatureFlags.DEFAULT_FLAGS);
                    case "getLevelName" -> "world";
                    case "isInitialized" -> true;
                    case "toString" -> "PrismWorldData";
                    case "hashCode" -> 0;
                    case "equals" -> proxy == args[0];
                    default -> {
                        Class<?> returned = method.getReturnType();
                        yield returned == boolean.class ? false : returned.isPrimitive() && returned != void.class ? 0 : null;
                    }
                });
        setField(net.minecraft.server.MinecraftServer.class, "worldData", fakeServer, worldData);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    static byte[] encode(Packet<?> packet) {
        ByteBuf buf = Unpooled.buffer();
        try {
            ((StreamCodec<ByteBuf, Packet<?>>) (StreamCodec) clientbound.codec()).encode(buf, packet);
            return ByteBufUtil.getBytes(buf);
        } finally {
            buf.release();
        }
    }

    static Packet<?> decode(byte[] bytes) {
        ByteBuf buf = Unpooled.wrappedBuffer(bytes);
        try {
            return clientbound.codec().decode(buf);
        } finally {
            buf.release();
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    static byte[] encodeServerbound(Packet<?> packet) {
        ByteBuf buf = Unpooled.buffer();
        try {
            ((StreamCodec<ByteBuf, Packet<?>>) (StreamCodec) serverbound.codec()).encode(buf, packet);
            return ByteBufUtil.getBytes(buf);
        } finally {
            buf.release();
        }
    }

    static Packet<?> decodeServerbound(byte[] bytes) {
        ByteBuf buf = Unpooled.wrappedBuffer(bytes);
        try {
            return serverbound.codec().decode(buf);
        } finally {
            buf.release();
        }
    }

    static void setField(Class<?> owner, String name, Object target, Object value) {
        try {
            Field field = owner.getDeclaredField(name);
            field.setAccessible(true);
            field.set(target, value);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Could not set " + owner.getSimpleName() + "." + name, e);
        }
    }

    /** Position and rotation without running Entity.setPos (which needs the entity's dimensions). */
    static void place(Entity entity, double x, double y, double z, float yaw, float pitch) {
        setField(Entity.class, "position", entity, new Vec3(x, y, z));
        setField(Entity.class, "blockPosition", entity, net.minecraft.core.BlockPos.containing(x, y, z));
        setField(Entity.class, "yRot", entity, yaw);
        setField(Entity.class, "xRot", entity, pitch);
    }

    static <T> T allocate(Class<T> type) {
        try {
            return type.cast(UNSAFE.allocateInstance(type));
        } catch (InstantiationException e) {
            throw new IllegalStateException(e);
        }
    }

    @Override
    public Object playerHandle(UUID uuid, String name, int entityId) {
        return players.computeIfAbsent(uuid, u -> new NmsPlayer(this, u, name, entityId)).handle;
    }

    @Override
    public void updatePlayer(UUID uuid, JsonObject snapshot) {
        NmsPlayer player = players.get(uuid);
        if (player != null) {
            player.update(snapshot);
        }
    }

    @Override
    public void removePlayer(UUID uuid) {
        NmsPlayer player = players.remove(uuid);
        if (player != null) {
            player.close();
        }
    }

    @Override
    public Object entityHandle(UUID uuid, String type, int entityId, double x, double y, double z, float yaw, float pitch) {
        Entity existing = entities.get(entityId);
        if (existing != null) {
            place(existing, x, y, z, yaw, pitch);
            return existing;
        }
        EntityType<?> entityType = BuiltInRegistries.ENTITY_TYPE.getOptional(net.minecraft.resources.Identifier.parse(type)).orElse(null);
        if (entityType == null) {
            return null;
        }
        Entity entity;
        try {
            // Real constructor against the shim level: entity data, attributes and dimensions come out populated.
            entity = entityType.create((PrismServerLevel) level(), EntitySpawnReason.COMMAND);
        } catch (Throwable constructorFailed) {
            LOGGER.log(Level.INFO, "NMS constructor for " + type + " failed, allocating without it: " + constructorFailed);
            entity = null;
        }
        if (entity == null) {
            try {
                entity = allocate(entityType.getBaseClass());
                setField(Entity.class, "type", entity, entityType);
            } catch (RuntimeException e) {
                LOGGER.log(Level.WARNING, "No NMS handle for " + type, e);
                return null;
            }
        }
        entity.setId(entityId);
        entity.setUUID(uuid);
        place(entity, x, y, z, yaw, pitch);
        entities.put(entityId, entity);
        return entity;
    }

    @Override
    public void removeEntity(int entityId) {
        entities.remove(entityId);
    }

    Entity entityById(int id) {
        Entity entity = entities.get(id);
        if (entity != null) {
            return entity;
        }
        for (NmsPlayer player : players.values()) {
            if (player.handle.getId() == id) {
                return player.handle;
            }
        }
        return null;
    }

    @Override
    public Object server() {
        return fakeServer;
    }

    @Override
    public synchronized Object level() {
        if (overworld == null) {
            overworld = PrismServerLevel.create(this, fakeServer);
        }
        return overworld;
    }

    @Override
    public List<byte[]> filter(UUID uuid, byte[] packet) {
        NmsPlayer player = players.get(uuid);
        if (player == null || !player.injected()) {
            return null;
        }
        Packet<?> decoded;
        try {
            decoded = decode(packet);
        } catch (RuntimeException e) {
            return null;
        }
        return player.filter(decoded);
    }

    @Override
    public List<byte[]> inbound(UUID uuid, byte[] packet) {
        NmsPlayer player = players.get(uuid);
        if (player == null || !player.injected()) {
            return null;
        }
        Packet<?> decoded;
        try {
            decoded = decodeServerbound(packet);
        } catch (RuntimeException e) {
            return null;
        }
        List<byte[]> out = new ArrayList<>();
        for (Packet<?> passed : player.inbound(decoded)) {
            try {
                out.add(encodeServerbound(passed));
            } catch (RuntimeException e) {
                LOGGER.log(Level.WARNING, "Could not re-encode " + passed.getClass().getSimpleName(), e);
            }
        }
        return out;
    }

    @Override
    public void publishInjection() {
        for (NmsPlayer player : players.values()) {
            boolean injected = player.injected();
            if (player.publishedInjected != injected) {
                player.publishedInjected = injected;
                JsonObject payload = new JsonObject();
                payload.addProperty("uuid", player.uuid.toString());
                payload.addProperty("injected", injected);
                Main.rpc.notify("pipeline", payload);
            }
        }
    }

    @Override
    public void afterJoin(UUID uuid) {
        NmsPlayer player = players.get(uuid);
        if (player == null || !player.injected()) {
            return;
        }
        try {
            var actions = java.util.EnumSet.of(
                    net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket.Action.ADD_PLAYER,
                    net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket.Action.UPDATE_LISTED);
            player.filter(new net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket(actions, List.of(player.handle)));
        } catch (RuntimeException e) {
            LOGGER.log(Level.WARNING, "Could not prime the pipeline for " + uuid, e);
        }
    }

    static String base64(byte[] bytes) {
        return Base64.getEncoder().encodeToString(bytes);
    }
}
