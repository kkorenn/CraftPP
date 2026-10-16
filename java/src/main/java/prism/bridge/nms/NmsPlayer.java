package prism.bridge.nms;

import com.google.gson.JsonObject;
import com.mojang.authlib.GameProfile;
import io.netty.channel.Channel;
import io.netty.channel.ChannelDuplexHandler;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.ChannelOutboundHandlerAdapter;
import io.netty.channel.ChannelPromise;
import io.netty.channel.embedded.EmbeddedChannel;
import net.minecraft.network.Connection;
import net.minecraft.network.ConnectionProtocol;
import net.minecraft.network.PacketListener;
import net.minecraft.network.TickablePacketListener;
import net.minecraft.network.protocol.Packet;
import net.minecraft.network.protocol.PacketFlow;
import net.minecraft.network.protocol.game.ServerGamePacketListener;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.level.ServerPlayerGameMode;
import net.minecraft.server.network.ServerCommonPacketListenerImpl;
import net.minecraft.server.network.ServerGamePacketListenerImpl;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.GameType;
import prism.bridge.Main;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * The NMS view of one online player: a ServerPlayer whose connection is a real
 * vanilla Connection sitting in an in-memory Netty pipeline with vanilla's
 * handler names ("splitter", "decoder", "prepender", "encoder",
 * "packet_handler"). Plugins send packets or inject handlers exactly as on
 * Paper; the "encoder" turns whatever reaches it into bytes for PrismMC, and
 * a recording packet listener behind "packet_handler" catches serverbound
 * packets plugins let through.
 */
final class NmsPlayer {
    private static final Logger LOGGER = Logger.getLogger("NMS");
    private static final int VANILLA_HANDLERS = 5;

    final UUID uuid;
    final ServerPlayer handle;
    final Connection connection;
    final EmbeddedChannel channel;
    boolean publishedInjected;
    private final NmsCore core;
    private final ThreadLocal<List<byte[]>> capture = new ThreadLocal<>();
    private final ThreadLocal<List<Packet<?>>> inboundCapture = new ThreadLocal<>();

    NmsPlayer(NmsCore core, UUID uuid, String name, int entityId) {
        this.core = core;
        this.uuid = uuid;
        handle = NmsCore.allocate(PrismServerPlayer.class);
        ((PrismServerPlayer) handle).prismName = name;
        // The vanilla constructor never ran: fill the state its methods read (type, dimensions, synched data).
        NmsCore.setField(Entity.class, "type", handle, net.minecraft.world.entity.EntityTypes.PLAYER);
        NmsCore.setField(Entity.class, "dimensions", handle, net.minecraft.world.entity.EntityTypes.PLAYER.getDimensions());
        try {
            var builder = new net.minecraft.network.syncher.SynchedEntityData.Builder(handle);
            // Entity's constructor defines ids 0..7 itself before the per-class defineSynchedData chain.
            Object[][] base = {
                    {"DATA_SHARED_FLAGS_ID", (byte) 0}, {"DATA_AIR_SUPPLY_ID", 300}, {"DATA_CUSTOM_NAME", java.util.Optional.empty()},
                    {"DATA_CUSTOM_NAME_VISIBLE", false}, {"DATA_SILENT", false}, {"DATA_NO_GRAVITY", false},
                    {"DATA_POSE", net.minecraft.world.entity.Pose.STANDING}, {"DATA_TICKS_FROZEN", 0},
            };
            for (Object[] entry : base) {
                java.lang.reflect.Field field = Entity.class.getDeclaredField((String) entry[0]);
                field.setAccessible(true);
                @SuppressWarnings("unchecked")
                var accessor = (net.minecraft.network.syncher.EntityDataAccessor<Object>) field.get(null);
                builder.define(accessor, entry[1]);
            }
            java.lang.reflect.Method define = Entity.class.getDeclaredMethod("defineSynchedData", net.minecraft.network.syncher.SynchedEntityData.Builder.class);
            define.setAccessible(true);
            define.invoke(handle, builder);
            NmsCore.setField(Entity.class, "entityData", handle, builder.build());
        } catch (ReflectiveOperationException | RuntimeException e) {
            LOGGER.log(Level.WARNING, "Could not build synched entity data for " + name, e);
        }
        handle.setId(entityId);
        handle.setUUID(uuid);
        NmsCore.setField(net.minecraft.world.entity.player.Player.class, "gameProfile", handle, new GameProfile(uuid, name));
        NmsCore.setField(ServerPlayer.class, "gameMode", handle, new ServerPlayerGameMode(handle));
        NmsCore.place(handle, 0, 64, 0, 0, 0);

        connection = new PrismConnection();
        channel = new EmbeddedChannel(new ChannelInitializer<Channel>() {
            @Override
            protected void initChannel(Channel ch) {
                ch.pipeline()
                        .addLast("splitter", new ChannelDuplexHandler())
                        .addLast("decoder", new ChannelDuplexHandler())
                        .addLast("prepender", new ChannelDuplexHandler())
                        .addLast("encoder", new Encoder())
                        .addLast("packet_handler", connection);
            }
        });
        // Serverbound packets that plugins let through reach this recording listener instead of vanilla's handlers.
        NmsCore.setField(Connection.class, "packetListener", connection, recordingListener());

        ServerGamePacketListenerImpl listener = NmsCore.allocate(ServerGamePacketListenerImpl.class);
        NmsCore.setField(ServerCommonPacketListenerImpl.class, "connection", listener, connection);
        NmsCore.setField(ServerCommonPacketListenerImpl.class, "server", listener, Nms.get() != null ? Nms.get().server() : null);
        listener.player = handle;
        handle.connection = listener;
    }

    private PacketListener recordingListener() {
        InvocationHandler handler = (proxy, method, args) -> {
            if (args != null && args.length == 1 && args[0] instanceof Packet<?> packet && method.getReturnType() == void.class) {
                List<Packet<?>> list = inboundCapture.get();
                if (list != null) {
                    list.add(packet);
                }
                return null;
            }
            return switch (method.getName()) {
                case "flow" -> PacketFlow.SERVERBOUND;
                case "protocol" -> ConnectionProtocol.PLAY;
                case "isAcceptingMessages", "shouldHandleMessage" -> true;
                case "shouldPropagateHandlingExceptions" -> false;
                case "toString" -> "PrismRecordingListener";
                case "hashCode" -> System.identityHashCode(proxy);
                case "equals" -> proxy == args[0];
                default -> {
                    Class<?> returned = method.getReturnType();
                    yield returned == boolean.class ? false : returned.isPrimitive() && returned != void.class ? 0 : null;
                }
            };
        };
        return (PacketListener) Proxy.newProxyInstance(NmsPlayer.class.getClassLoader(),
                new Class<?>[] {ServerGamePacketListener.class, TickablePacketListener.class}, handler);
    }

    void update(JsonObject snapshot) {
        if (snapshot.has("x")) {
            NmsCore.place(handle, snapshot.get("x").getAsDouble(), snapshot.get("y").getAsDouble(), snapshot.get("z").getAsDouble(),
                    snapshot.has("yaw") ? snapshot.get("yaw").getAsFloat() : 0f, snapshot.has("pitch") ? snapshot.get("pitch").getAsFloat() : 0f);
        }
        if (snapshot.has("gamemode")) {
            GameType type = GameType.byId(snapshot.get("gamemode").getAsInt());
            NmsCore.setField(ServerPlayerGameMode.class, "gameModeForPlayer", handle.gameMode, type);
        }
    }

    /** True once a plugin added its own handler next to vanilla's five. */
    boolean injected() {
        return channel.pipeline().names().size() > VANILLA_HANDLERS + 1; // +1: Netty's tail context.
    }

    /** Pushes a PrismMC clientbound packet through the pipeline; returns what came out (empty = dropped). */
    List<byte[]> filter(Packet<?> packet) {
        List<byte[]> out = new ArrayList<>();
        capture.set(out);
        try {
            synchronized (channel) {
                channel.writeAndFlush(packet);
            }
        } finally {
            capture.remove();
        }
        return out;
    }

    /** Pushes a serverbound packet through the pipeline; returns what reached the packet listener (empty = swallowed). */
    List<Packet<?>> inbound(Packet<?> packet) {
        List<Packet<?>> out = new ArrayList<>();
        inboundCapture.set(out);
        try {
            synchronized (channel) {
                channel.pipeline().fireChannelRead(packet);
            }
        } catch (RuntimeException e) {
            LOGGER.log(Level.WARNING, "Inbound handler failed for " + uuid, e);
            return List.of(packet);
        } finally {
            inboundCapture.remove();
        }
        Object details = readField(Connection.class, "disconnectionDetails", connection);
        if (details != null) {
            LOGGER.warning("Vanilla connection faulted on inbound " + packet.getClass().getSimpleName() + ": "
                    + ((net.minecraft.network.DisconnectionDetails) details).reason().getString() + " (channel open=" + channel.isOpen() + ")");
        }
        return out;
    }

    void close() {
        channel.close();
    }

    private static Object readField(Class<?> owner, String name, Object target) {
        try {
            java.lang.reflect.Field field = owner.getDeclaredField(name);
            field.setAccessible(true);
            return field.get(target);
        } catch (ReflectiveOperationException e) {
            return null;
        }
    }

    private void deliver(byte[] bytes) {
        prism.bridge.Native.packet(uuid.getMostSignificantBits(), uuid.getLeastSignificantBits(), bytes);
    }

    private final class Encoder extends ChannelOutboundHandlerAdapter {
        @Override
        public void write(ChannelHandlerContext ctx, Object msg, ChannelPromise promise) {
            if (!(msg instanceof Packet<?> packet)) {
                ctx.write(msg, promise);
                return;
            }
            try {
                byte[] bytes = NmsCore.encode(packet);
                List<byte[]> captured = capture.get();
                if (captured != null) {
                    captured.add(bytes);
                } else {
                    deliver(bytes);
                }
            } catch (RuntimeException e) {
                LOGGER.log(Level.WARNING, "Could not encode " + packet.getClass().getSimpleName() + " for " + uuid, e);
            }
            promise.trySuccess();
            core.publishInjection();
        }
    }
}
