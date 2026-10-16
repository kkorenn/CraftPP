package prism.bridge.nms;

import io.netty.channel.ChannelHandlerContext;
import net.minecraft.network.Connection;
import net.minecraft.network.protocol.PacketFlow;

import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * Vanilla Connection whose fault handling does not close the in-memory
 * channel: PrismMC owns the real socket, so a plugin handler throwing on a
 * packet is logged (with the cause vanilla would hide behind a disconnect)
 * and the pipeline stays usable.
 */
final class PrismConnection extends Connection {
    private static final Logger LOGGER = Logger.getLogger("NMS");

    PrismConnection() {
        super(PacketFlow.SERVERBOUND);
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) {
        LOGGER.log(Level.WARNING, "Packet handler failed in the NMS pipeline (packet kept)", cause);
    }
}
