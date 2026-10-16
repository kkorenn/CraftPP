package prism.bridge.nms;

import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;

/**
 * ServerPlayer allocated without its constructor (there is no MinecraftServer
 * to construct against); the overrides answer from PrismMC's state instead of
 * the unset vanilla fields.
 */
public class PrismServerPlayer extends ServerPlayer {
    String prismName = "";

    private PrismServerPlayer() {
        super(null, null, null, null); // Never run: instances come from Unsafe.allocateInstance.
    }

    @Override
    public ServerLevel level() {
        return (ServerLevel) Nms.get().level();
    }

    @Override
    public Component getName() {
        return Component.literal(prismName);
    }

    @Override
    public Component getDisplayName() {
        return Component.literal(prismName);
    }

    @Override
    public Component getTabListDisplayName() {
        return null; // Vanilla: no custom tab name.
    }

    @Override
    public String toString() {
        return "PrismServerPlayer[" + prismName + "/" + getUUID() + "]";
    }
}
