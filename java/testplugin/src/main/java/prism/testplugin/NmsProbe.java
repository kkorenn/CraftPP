package prism.testplugin;

import io.netty.channel.Channel;
import io.netty.channel.ChannelDuplexHandler;
import io.netty.channel.ChannelHandlerContext;
import net.minecraft.core.BlockPos;
import net.minecraft.network.chat.Component;
import net.minecraft.network.protocol.game.ClientboundSystemChatPacket;
import net.minecraft.network.protocol.game.ServerboundChatPacket;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import org.bukkit.Bukkit;
import org.bukkit.Location;
import org.bukkit.entity.EntityType;
import org.bukkit.entity.Player;

/** Kept in its own class so the plugin still loads when the server has no NMS. */
final class NmsProbe {
    private NmsProbe() {
    }

    static String run(Player player) {
        ServerPlayer handle = (ServerPlayer) handleOf(player);
        handle.connection.send(new ClientboundSystemChatPacket(Component.literal("nms ok " + handle.getUUID()), false));

        // World handle: block below the player through vanilla's Level API.
        ServerLevel level = (ServerLevel) handleOf(player.getWorld());
        Location below = player.getLocation().subtract(0, 1, 0);
        String block = level.getBlockState(new BlockPos(below.getBlockX(), below.getBlockY(), below.getBlockZ())).getBlock().toString();

        // Entity handle: a real NMS entity behind a Bukkit-spawned zombie.
        org.bukkit.entity.Entity zombie = player.getWorld().spawnEntity(player.getLocation(), EntityType.ZOMBIE);
        Entity nmsZombie = (Entity) handleOf(zombie);
        String entity = nmsZombie == null ? "none" : nmsZombie.getClass().getSimpleName() + "#" + nmsZombie.getId();

        // Inbound interception: swallow chat containing "nope" the way packet plugins do.
        Channel channel = handle.connection.connection.channel;
        if (!channel.pipeline().names().contains("prismtest_inbound")) {
            channel.pipeline().addBefore("packet_handler", "prismtest_inbound", new ChannelDuplexHandler() {
                @Override
                public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
                    if (msg instanceof ServerboundChatPacket chat && chat.message().contains("nope")) {
                        return;
                    }
                    super.channelRead(ctx, msg);
                }
            });
        }
        return "craft " + Bukkit.getServer().getClass().getName() + " / " + player.getClass().getName()
                + " id=" + handle.getId() + " name=" + handle.getGameProfile().name() + " level=" + level.dimension().identifier()
                + " below=" + block + " zombie=" + entity + " players=" + level.players().size();
    }

    private static Object handleOf(Object craft) {
        try {
            return craft.getClass().getMethod("getHandle").invoke(craft);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }
}
