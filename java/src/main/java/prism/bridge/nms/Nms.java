package prism.bridge.nms;

import com.google.gson.JsonObject;

import java.util.List;
import java.util.UUID;

/**
 * Entry point for server-internals (NMS) support. The implementation
 * (NmsCore) references net.minecraft classes and is only loaded when PrismMC
 * started the JVM with the Mojang server jar on the classpath
 * (-Dprismmc.nms=true); this facade has no such references so the bridge runs
 * without it.
 */
public interface Nms {
    Nms[] HOLDER = new Nms[1];

    static boolean enabled() {
        return Boolean.getBoolean("prismmc.nms");
    }

    static Nms get() {
        return HOLDER[0];
    }

    static void init() throws Exception {
        if (HOLDER[0] == null) {
            HOLDER[0] = (Nms) Class.forName("prism.bridge.nms.NmsCore").getConstructor().newInstance();
        }
    }

    /** The net.minecraft.server.level.ServerPlayer behind a Bukkit player (created on first use). */
    Object playerHandle(UUID uuid, String name, int entityId);

    /** Position, rotation and game mode from a fresh PrismMC snapshot onto an existing handle (no-op without one). */
    void updatePlayer(UUID uuid, JsonObject snapshot);

    void removePlayer(UUID uuid);

    /** The net.minecraft.world.entity.Entity behind a non-player entity (created on first use, null if the type has no NMS class). */
    Object entityHandle(UUID uuid, String type, int entityId, double x, double y, double z, float yaw, float pitch);

    void removeEntity(int entityId);

    /** The (never started) MinecraftServer instance CraftServer.getServer() hands out. */
    Object server();

    /** The overworld ServerLevel (block reads and entities answer from PrismMC). */
    Object level();

    /** Runs a clientbound packet PrismMC is about to send through the player's Netty pipeline; null means "send as is". */
    List<byte[]> filter(UUID uuid, byte[] packet);

    /** Runs a serverbound packet the client sent through the player's pipeline; null means "handle as is", empty means swallowed. */
    List<byte[]> inbound(UUID uuid, byte[] packet);

    /** Tells PrismMC which players have plugin handlers in their pipeline (only those pay the inbound round trip). */
    void publishInjection();

    /**
     * Paper sends the joining player's player-info ADD synchronously right after
     * PlayerJoinEvent; plugin packet trackers rely on seeing it before their async
     * join work runs. Primes injected pipelines with an equivalent packet (bytes discarded;
     * PrismMC sends the real one next).
     */
    void afterJoin(UUID uuid);
}
