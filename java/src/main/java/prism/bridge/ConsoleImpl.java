package prism.bridge;

import net.kyori.adventure.identity.Identity;
import net.kyori.adventure.text.Component;
import org.bukkit.Bukkit;
import org.bukkit.Server;
import org.bukkit.permissions.Permission;
import org.bukkit.permissions.PermissionAttachment;
import org.bukkit.permissions.PermissionAttachmentInfo;
import org.bukkit.permissions.PermissibleBase;
import org.bukkit.permissions.ServerOperator;
import org.bukkit.plugin.Plugin;

import java.util.Set;
import java.util.UUID;
import java.util.function.Consumer;
import java.util.logging.Logger;

/** Backing for the console sender (and feedback-capturing senders). */
public final class ConsoleImpl implements ServerOperator {
    private final Consumer<? super Component> feedback;
    private final PermissibleBase permissions = new PermissibleBase(this);
    private final Logger logger = Logger.getLogger("");

    ConsoleImpl() {
        this(null);
    }

    ConsoleImpl(Consumer<? super Component> feedback) {
        this.feedback = feedback;
    }

    public String getName() {
        return "CONSOLE";
    }

    public Component name() {
        return Component.text("CONSOLE");
    }

    public Server getServer() {
        return Bukkit.getServer();
    }

    public boolean isConversing() {
        return false;
    }

    public Identity identity() {
        return Identity.nil();
    }

    public void sendMessage(String message) {
        if (feedback != null) {
            feedback.accept(ServerImpl.LEGACY.deserialize(message));
        } else {
            logger.info(message);
        }
    }

    public void sendMessage(String... messages) {
        for (String message : messages) {
            sendMessage(message);
        }
    }

    public void sendMessage(UUID sender, String message) {
        sendMessage(message);
    }

    public void sendMessage(UUID sender, String... messages) {
        sendMessage(messages);
    }

    public void sendMessage(Component message) {
        if (feedback != null) {
            feedback.accept(message);
        } else {
            logger.info(ServerImpl.LEGACY.serialize(message));
        }
    }


    public void sendRawMessage(String message) {
        sendMessage(message);
    }

    public void sendRawMessage(UUID sender, String message) {
        sendMessage(message);
    }

    @Override
    public boolean isOp() {
        return true;
    }

    @Override
    public void setOp(boolean value) {
        throw new UnsupportedOperationException("Cannot change operator status of server console");
    }

    public boolean isPermissionSet(String name) {
        return permissions.isPermissionSet(name);
    }

    public boolean isPermissionSet(Permission perm) {
        return permissions.isPermissionSet(perm);
    }

    public boolean hasPermission(String name) {
        return true;
    }

    public boolean hasPermission(Permission perm) {
        return true;
    }

    public PermissionAttachment addAttachment(Plugin plugin, String name, boolean value) {
        return permissions.addAttachment(plugin, name, value);
    }

    public PermissionAttachment addAttachment(Plugin plugin) {
        return permissions.addAttachment(plugin);
    }

    public PermissionAttachment addAttachment(Plugin plugin, String name, boolean value, int ticks) {
        return permissions.addAttachment(plugin, name, value, ticks);
    }

    public PermissionAttachment addAttachment(Plugin plugin, int ticks) {
        return permissions.addAttachment(plugin, ticks);
    }

    public void removeAttachment(PermissionAttachment attachment) {
        permissions.removeAttachment(attachment);
    }

    public void recalculatePermissions() {
        permissions.recalculatePermissions();
    }

    public Set<PermissionAttachmentInfo> getEffectivePermissions() {
        return permissions.getEffectivePermissions();
    }

    @Override
    public String toString() {
        return "CraftConsoleCommandSender";
    }
}
