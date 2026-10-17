package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import org.bukkit.Bukkit;
import org.bukkit.Server;
import org.bukkit.plugin.Plugin;

import java.io.File;

/** Server bootstrap driven by PrismMC's init/enable requests. */
final class Bootstrap {
    private Bootstrap() {
    }

    static JsonElement init(JsonObject config) throws Exception {
        CraftClasses.generateAll(prism.bridge.nms.Nms.enabled());
        if (prism.bridge.nms.Nms.enabled()) {
            long start = System.nanoTime();
            Main.rpc.log("INFO", "", "Loading Minecraft " + config.get("mcVersion").getAsString() + " internals for NMS plugins");
            prism.bridge.nms.Nms.init();
            Main.rpc.log("INFO", "", String.format("Server internals ready (%.1fs)", (System.nanoTime() - start) / 1e9));
        }
        ServerImpl backing = new ServerImpl(config);
        Main.server = backing;
        Main.scheduler = new SchedulerImpl();
        Main.pluginManager = new PluginManagerImpl(backing);
        Server server = ProxyBase.create(Server.class, backing);
        Bukkit.setServer(server);
        backing.afterBukkitReady();
        com.google.gson.JsonElement registries = Main.rpc.api("server.registries");
        Registries.load(registries != null && registries.isJsonObject() ? registries.getAsJsonObject() : null);

        // Initialize the legacy superclass first: its static constants load GameRules,
        // which in turn constructs registry subclasses while superclass initialization is in progress.
        Class.forName("org.bukkit.GameRule");

        File pluginsFolder = backing.getPluginsFolder();
        pluginsFolder.mkdirs();
        Plugin[] loaded = Main.pluginManager.loadPlugins(pluginsFolder);
        JsonArray plugins = new JsonArray();
        for (Plugin plugin : loaded) {
            plugins.add(Commands.describePlugin(plugin));
        }
        JsonObject result = new JsonObject();
        result.add("plugins", plugins);
        return result;
    }

    static JsonElement enable() {
        // Paper fires the COMMANDS lifecycle event for bootstrap-time handlers
        // before enabling, and again for handlers plugins register in onEnable.
        fireCommands(io.papermc.paper.plugin.lifecycle.event.registrar.ReloadableRegistrarEvent.Cause.INITIAL);
        for (Plugin plugin : Main.pluginManager.getPlugins()) {
            if (!plugin.isEnabled()) {
                Main.pluginManager.enablePlugin(plugin);
            }
        }
        fireCommands(io.papermc.paper.plugin.lifecycle.event.registrar.ReloadableRegistrarEvent.Cause.INITIAL);
        Main.pluginManager.publishListenerFlags();
        return commandState();
    }

    private static final java.util.Set<Object> FIRED = java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<>());

    static void fireCommands(io.papermc.paper.plugin.lifecycle.event.registrar.ReloadableRegistrarEvent.Cause cause) {
        ServicesImpl.LifecycleImpl.fire("commands", owner -> {
            io.papermc.paper.plugin.configuration.PluginMeta meta = owner instanceof Plugin plugin ? plugin.getPluginMeta()
                    : owner instanceof PluginManagerImpl.BootstrapContextImpl context ? context.getConfiguration() : null;
            return Brigadier.commandsEvent(meta, cause);
        });
    }

    static JsonObject commandState() {
        JsonObject result = new JsonObject();
        result.add("commands", Commands.registeredCommands());
        result.add("plugins", Commands.describePlugins());
        result.add("brigadier", Brigadier.exportTree(Main.server.getConsoleSender()));
        return result;
    }
}
