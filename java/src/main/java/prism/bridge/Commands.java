package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import org.bukkit.command.Command;
import org.bukkit.command.CommandException;
import org.bukkit.command.CommandSender;
import org.bukkit.command.PluginCommand;
import org.bukkit.entity.Player;
import org.bukkit.plugin.Plugin;

import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.logging.Level;
import java.util.logging.Logger;

/** Plugin command dispatch, tab completion, and the per-player visible command list. */
final class Commands {
    private Commands() {
    }

    static JsonElement dispatch(JsonObject p) {
        CommandSender sender = Main.server.senderFrom(p);
        String line = p.get("line").getAsString();
        JsonObject result = new JsonObject();
        if (Brigadier.dispatch(sender, line)) {
            result.addProperty("handled", true);
            return result;
        }
        try {
            result.addProperty("handled", Main.server.commandMap().dispatch(sender, line));
        } catch (CommandException e) {
            Logger.getLogger("").log(Level.SEVERE, e.getMessage(), e.getCause() == null ? e : e.getCause());
            sender.sendMessage("§cAn unexpected error occurred trying to execute that command");
            result.addProperty("handled", true);
        }
        return result;
    }

    static JsonElement tabComplete(JsonObject p) {
        CommandSender sender = Main.server.senderFrom(p);
        String label = p.get("line").getAsString().split(" ", 2)[0];
        if (Brigadier.hasRoot(label)) {
            return Brigadier.suggest(sender, p.get("line").getAsString());
        }
        JsonArray completions = new JsonArray();
        try {
            List<String> list = Main.server.commandMap().tabComplete(sender, p.get("line").getAsString());
            if (list != null) {
                list.forEach(completions::add);
            }
        } catch (RuntimeException e) {
            Logger.getLogger("").log(Level.WARNING, "Tab completion failed for '" + p.get("line").getAsString() + "'", e);
        }
        JsonObject result = new JsonObject();
        result.add("completions", completions);
        return result;
    }

    /** Commands (and aliases) this sender may see, per Command.testPermissionSilent. */
    static JsonElement visibleCommands(JsonObject p) {
        CommandSender sender;
        if (p.has("uuid") && !p.get("uuid").isJsonNull()) {
            PlayerImpl player = Main.server.playerByUuid(p.get("uuid").getAsString());
            if (player == null) {
                player = new PlayerImpl(UUID.fromString(p.get("uuid").getAsString()), p.get("name").getAsString());
                ProxyBase.create(Player.class, player);
                player.setOpCached(p.has("op") && p.get("op").getAsBoolean());
            }
            sender = player.proxy();
        } else {
            sender = Main.server.getConsoleSender();
        }
        JsonArray names = new JsonArray();
        for (Map.Entry<String, Command> entry : Main.server.commandMap().getKnownCommands().entrySet()) {
            if (entry.getValue().testPermissionSilent(sender)) {
                names.add(entry.getKey());
            }
        }
        JsonObject result = new JsonObject();
        result.add("commands", names);
        result.add("brigadier", Brigadier.exportTree(sender));
        result.add("labels", allLabels());
        return result;
    }

    /** Every label the bridge answers: Bukkit map commands and Brigadier roots. */
    static JsonArray allLabels() {
        JsonArray labels = new JsonArray();
        Main.server.commandMap().getKnownCommands().keySet().forEach(labels::add);
        Brigadier.roots().forEach(labels::add);
        return labels;
    }

    /** Every known command label with its help metadata. */
    static JsonArray registeredCommands() {
        JsonArray commands = new JsonArray();
        for (String root : Brigadier.roots()) {
            JsonObject item = new JsonObject();
            item.addProperty("label", root);
            item.addProperty("name", root);
            item.addProperty("description", Brigadier.descriptions().getOrDefault(root, ""));
            item.addProperty("usage", "/" + root);
            item.addProperty("plugin", "");
            commands.add(item);
        }
        for (Map.Entry<String, Command> entry : Main.server.commandMap().getKnownCommands().entrySet()) {
            Command command = entry.getValue();
            JsonObject item = new JsonObject();
            item.addProperty("label", entry.getKey());
            item.addProperty("name", command.getName());
            item.addProperty("description", command.getDescription());
            item.addProperty("usage", command.getUsage());
            item.addProperty("permission", command.getPermission());
            item.addProperty("plugin", command instanceof PluginCommand pluginCommand ? pluginCommand.getPlugin().getName() : "");
            commands.add(item);
        }
        return commands;
    }

    static JsonObject describePlugin(Plugin plugin) {
        JsonObject item = new JsonObject();
        item.addProperty("name", plugin.getName());
        item.addProperty("version", plugin.getDescription().getVersion());
        item.addProperty("fullName", plugin.getDescription().getFullName());
        item.addProperty("enabled", plugin.isEnabled());
        item.addProperty("description", plugin.getDescription().getDescription());
        item.addProperty("website", plugin.getDescription().getWebsite());
        JsonArray authors = new JsonArray();
        plugin.getDescription().getAuthors().forEach(authors::add);
        item.add("authors", authors);
        return item;
    }

    static JsonArray describePlugins() {
        JsonArray plugins = new JsonArray();
        for (Plugin plugin : Main.pluginManager.getPlugins()) {
            plugins.add(describePlugin(plugin));
        }
        return plugins;
    }
}
