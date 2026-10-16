package prism.bridge;

import com.google.gson.JsonObject;
import io.papermc.paper.plugin.PermissionManager;
import io.papermc.paper.plugin.configuration.PluginMeta;
import org.bukkit.Server;
import org.bukkit.command.PluginCommandYamlParser;
import org.bukkit.event.Event;
import org.bukkit.event.EventHandler;
import org.bukkit.event.EventPriority;
import org.bukkit.event.HandlerList;
import org.bukkit.event.Listener;
import org.bukkit.event.server.PluginDisableEvent;
import org.bukkit.event.server.PluginEnableEvent;
import org.bukkit.permissions.Permissible;
import org.bukkit.permissions.Permission;
import org.bukkit.permissions.PermissionDefault;
import org.bukkit.plugin.AuthorNagException;
import org.bukkit.plugin.EventExecutor;
import org.bukkit.plugin.IllegalPluginAccessException;
import org.bukkit.plugin.InvalidDescriptionException;
import org.bukkit.plugin.InvalidPluginException;
import org.bukkit.plugin.Plugin;
import org.bukkit.plugin.PluginDescriptionFile;
import org.bukkit.plugin.PluginLoader;
import org.bukkit.plugin.PluginManager;
import org.bukkit.plugin.RegisteredListener;
import org.bukkit.plugin.UnknownDependencyException;
import org.bukkit.plugin.java.JavaPlugin;

import java.io.File;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.jar.JarFile;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;

/**
 * Plugin lifecycle, event bus, and permission registry. Loads plugin.yml
 * (and the plugin.yml-shaped parts of paper-plugin.yml) plugins from the
 * plugins folder in dependency order, the way the Paper server does.
 */
public final class PluginManagerImpl implements PluginManager {
    static final PluginLoader LOADER = new LoaderShim();

    private final Server server;
    private final Logger logger = Logger.getLogger("");
    private final Map<String, Plugin> lookupNames = new LinkedHashMap<>();
    private final List<Plugin> plugins = new ArrayList<>();
    private final Map<String, Permission> permissions = new HashMap<>();
    private final Map<Boolean, Set<Permission>> defaultPerms = new HashMap<>();
    private final Map<String, Map<Permissible, Boolean>> permSubs = new HashMap<>();
    private final Map<Boolean, Map<Permissible, Boolean>> defSubs = new HashMap<>();
    private Map<String, Boolean> lastListenerFlags = Map.of();

    PluginManagerImpl(ServerImpl server) {
        this.server = org.bukkit.Bukkit.getServer() != null ? org.bukkit.Bukkit.getServer() : null;
        defaultPerms.put(true, new LinkedHashSet<>());
        defaultPerms.put(false, new LinkedHashSet<>());
        defSubs.put(true, new WeakHashMap<>());
        defSubs.put(false, new WeakHashMap<>());
    }

    // ----- loading -----

    @Override
    public void registerInterface(Class<? extends PluginLoader> loader) {
    }

    @Override
    public Plugin getPlugin(String name) {
        return lookupNames.get(name.replace(' ', '_').toLowerCase(Locale.ROOT));
    }

    @Override
    public Plugin[] getPlugins() {
        return plugins.toArray(new Plugin[0]);
    }

    @Override
    public boolean isPluginEnabled(String name) {
        Plugin plugin = getPlugin(name);
        return plugin != null && plugin.isEnabled();
    }

    @Override
    public boolean isPluginEnabled(Plugin plugin) {
        return plugin != null && plugins.contains(plugin) && plugin.isEnabled();
    }

    @Override
    public Plugin[] loadPlugins(File directory) {
        File[] files = directory.listFiles((dir, name) -> name.toLowerCase(Locale.ROOT).endsWith(".jar"));
        return loadPlugins(files == null ? new File[0] : files);
    }

    @Override
    public Plugin[] loadPlugins(File[] files) {
        Map<String, PluginDescriptionFile> descriptions = new LinkedHashMap<>();
        Map<String, File> jarByName = new LinkedHashMap<>();
        for (File file : files) {
            try {
                PluginDescriptionFile description = readDescription(file);
                if (descriptions.containsKey(description.getName())) {
                    logger.severe("Ambiguous plugin name `" + description.getName() + "' for files `" + file.getPath()
                            + "' and `" + jarByName.get(description.getName()).getPath() + "' in `" + file.getParent() + "'");
                    continue;
                }
                descriptions.put(description.getName(), description);
                jarByName.put(description.getName(), file);
            } catch (InvalidDescriptionException e) {
                logger.log(Level.SEVERE, "Could not load '" + file.getPath() + "' in folder '" + file.getParent() + "'", e);
            }
        }

        // Dependency-ordered load: a plugin loads after its (soft) dependencies
        // and before anything that lists it in loadbefore.
        Map<String, Set<String>> after = new HashMap<>();
        for (PluginDescriptionFile description : descriptions.values()) {
            Set<String> deps = after.computeIfAbsent(description.getName(), k -> new LinkedHashSet<>());
            for (String dep : description.getDepend()) {
                if (!descriptions.containsKey(dep)) {
                    logger.log(Level.SEVERE, "Could not load '" + jarByName.get(description.getName()).getPath() + "' in folder '"
                            + jarByName.get(description.getName()).getParent() + "'",
                            new UnknownDependencyException("Unknown dependency " + dep + ". Please download and install " + dep + " to run this plugin."));
                    deps.add("\0missing");
                }
                deps.add(dep);
            }
            for (String dep : description.getSoftDepend()) {
                if (descriptions.containsKey(dep)) {
                    deps.add(dep);
                }
            }
            for (String before : description.getLoadBefore()) {
                if (descriptions.containsKey(before)) {
                    after.computeIfAbsent(before, k -> new LinkedHashSet<>()).add(description.getName());
                }
            }
        }

        List<Plugin> loaded = new ArrayList<>();
        Set<String> done = new HashSet<>();
        Set<String> visiting = new HashSet<>();
        for (String name : descriptions.keySet()) {
            loadInOrder(name, descriptions, jarByName, after, done, visiting, loaded);
        }
        return loaded.toArray(new Plugin[0]);
    }

    private void loadInOrder(String name, Map<String, PluginDescriptionFile> descriptions, Map<String, File> jars,
                             Map<String, Set<String>> after, Set<String> done, Set<String> visiting, List<Plugin> loaded) {
        if (done.contains(name) || !descriptions.containsKey(name)) {
            return;
        }
        Set<String> deps = after.getOrDefault(name, Set.of());
        if (deps.contains("\0missing")) {
            done.add(name);
            return;
        }
        if (!visiting.add(name)) {
            logger.severe("Circular dependency detected involving plugin " + name);
            return;
        }
        for (String dep : deps) {
            loadInOrder(dep, descriptions, jars, after, done, visiting, loaded);
        }
        visiting.remove(name);
        done.add(name);
        for (String dep : descriptions.get(name).getDepend()) {
            if (getPlugin(dep) == null) {
                return; // a hard dependency failed to load
            }
        }
        try {
            Plugin plugin = loadPlugin(jars.get(name));
            if (plugin != null) {
                loaded.add(plugin);
            }
        } catch (InvalidPluginException | UnknownDependencyException e) {
            logger.log(Level.SEVERE, "Could not load '" + jars.get(name).getPath() + "' in folder '" + jars.get(name).getParent() + "'", e);
        }
    }

    static PluginDescriptionFile readDescription(File file) throws InvalidDescriptionException {
        try (JarFile jar = new JarFile(file)) {
            ZipEntry entry = jar.getEntry("paper-plugin.yml");
            if (entry != null) {
                try (InputStream stream = jar.getInputStream(entry)) {
                    return PaperPluginYaml.toDescription(stream);
                }
            }
            entry = jar.getEntry("plugin.yml");
            if (entry == null) {
                throw new InvalidDescriptionException(new java.io.FileNotFoundException("Jar does not contain plugin.yml"));
            }
            try (InputStream stream = jar.getInputStream(entry)) {
                return new PluginDescriptionFile(stream);
            }
        } catch (java.io.IOException e) {
            throw new InvalidDescriptionException(e);
        }
    }

    @Override
    public Plugin loadPlugin(File file) throws InvalidPluginException, UnknownDependencyException {
        PluginDescriptionFile description;
        try {
            description = readDescription(file);
        } catch (InvalidDescriptionException e) {
            throw new InvalidPluginException(e);
        }
        File dataFolder = new File(file.getParentFile(), description.getName());
        logger.info("[" + description.getName() + "] Loading server plugin " + description.getFullName());
        PluginClassLoaderImpl loader;
        try {
            loader = new PluginClassLoaderImpl(file, description, dataFolder);
        } catch (java.net.MalformedURLException e) {
            throw new InvalidPluginException(e);
        }
        BootstrapContextImpl context = new BootstrapContextImpl(description, dataFolder, file);
        // plugin.yml `libraries:` and paper-plugin.yml loaders add jars from Maven before the main class loads.
        for (java.nio.file.Path library : Libraries.resolve(description, PaperPluginYaml.LOADERS.get(description.getName()), loader, context)) {
            loader.addLibrary(library);
        }
        JavaPlugin plugin;
        try {
            String bootstrapperName = PaperPluginYaml.BOOTSTRAPPERS.get(description.getName());
            io.papermc.paper.plugin.bootstrap.PluginBootstrap bootstrapper = null;
            if (bootstrapperName != null) {
                bootstrapper = (io.papermc.paper.plugin.bootstrap.PluginBootstrap) Class.forName(bootstrapperName, true, loader).getDeclaredConstructor().newInstance();
                bootstrapper.bootstrap(ProxyBase.create(io.papermc.paper.plugin.bootstrap.BootstrapContext.class, context));
            }
            Class<?> mainClass = Class.forName(description.getMain(), true, loader);
            Class<? extends JavaPlugin> pluginClass;
            try {
                pluginClass = mainClass.asSubclass(JavaPlugin.class);
            } catch (ClassCastException e) {
                throw new InvalidPluginException("main class `" + description.getMain() + "' does not extend JavaPlugin", e);
            }
            plugin = bootstrapper != null
                    ? (JavaPlugin) bootstrapper.createPlugin(ProxyBase.create(io.papermc.paper.plugin.bootstrap.PluginProviderContext.class, context))
                    : pluginClass.getDeclaredConstructor().newInstance();
        } catch (InvalidPluginException e) {
            closeQuietly(loader);
            throw e;
        } catch (ClassNotFoundException e) {
            closeQuietly(loader);
            throw new InvalidPluginException("Cannot find main class `" + description.getMain() + "'", e);
        } catch (ReflectiveOperationException e) {
            closeQuietly(loader);
            throw new InvalidPluginException("Abnormal plugin type", e.getCause() == null ? e : e.getCause());
        } catch (LinkageError e) {
            closeQuietly(loader);
            throw new InvalidPluginException("Plugin main class failed to link: " + e.getMessage(), e);
        }
        if (loader.getPlugin() == null) {
            closeQuietly(loader);
            throw new InvalidPluginException("Plugin main class did not initialize through its class loader");
        }
        plugins.add(plugin);
        lookupNames.put(description.getName().replace(' ', '_').toLowerCase(Locale.ROOT), plugin);
        for (String provided : description.getProvides()) {
            lookupNames.putIfAbsent(provided.replace(' ', '_').toLowerCase(Locale.ROOT), plugin);
        }
        for (Permission permission : description.getPermissions()) {
            try {
                addPermission(permission, false);
            } catch (IllegalArgumentException e) {
                logger.warning("Plugin " + description.getFullName() + " tried to register permission '" + permission.getName() + "' but it's already registered");
            }
        }
        try {
            plugin.onLoad();
        } catch (Throwable t) {
            logger.log(Level.SEVERE, "Error initializing plugin '" + file.getName() + "' in folder '" + file.getParent() + "' (Is it up to date?)", t);
        }
        return plugin;
    }

    private static void closeQuietly(PluginClassLoaderImpl loader) {
        try {
            loader.close();
        } catch (java.io.IOException ignored) {
        }
    }

    // ----- enable / disable -----

    @Override
    public void enablePlugin(Plugin plugin) {
        if (plugin.isEnabled()) {
            return;
        }
        JavaPlugin javaPlugin = (JavaPlugin) plugin;
        List<org.bukkit.command.Command> commands = PluginCommandYamlParser.parse(plugin);
        if (!commands.isEmpty()) {
            Main.server.commandMap().registerAll(plugin.getDescription().getName(), commands);
        }
        plugin.getLogger().info("Enabling " + plugin.getDescription().getFullName());
        try {
            setEnabled(javaPlugin, true);
        } catch (Throwable t) {
            logger.log(Level.SEVERE, "Error occurred while enabling " + plugin.getDescription().getFullName() + " (Is it up to date?)", t);
            try {
                disablePlugin(plugin);
            } catch (Throwable ignored) {
            }
            return;
        }
        HandlerList.bakeAll();
        callEvent(new PluginEnableEvent(plugin));
    }

    @Override
    public void disablePlugin(Plugin plugin) {
        if (!plugin.isEnabled()) {
            return;
        }
        ServicesImpl.LifecycleImpl.forget(plugin);
        plugin.getLogger().info("Disabling " + plugin.getDescription().getFullName());
        callEvent(new PluginDisableEvent(plugin));
        try {
            setEnabled((JavaPlugin) plugin, false);
        } catch (Throwable t) {
            logger.log(Level.SEVERE, "Error occurred while disabling " + plugin.getDescription().getFullName() + " (Is it up to date?)", t);
        }
        Main.scheduler.cancelTasks(plugin);
        org.bukkit.Bukkit.getServicesManager().unregisterAll(plugin);
        HandlerList.unregisterAll(plugin);
        org.bukkit.Bukkit.getMessenger().unregisterIncomingPluginChannel(plugin);
        org.bukkit.Bukkit.getMessenger().unregisterOutgoingPluginChannel(plugin);
        publishListenerFlags();
    }

    private static void setEnabled(JavaPlugin plugin, boolean enabled) throws ReflectiveOperationException {
        Method method = JavaPlugin.class.getDeclaredMethod("setEnabled", boolean.class);
        method.setAccessible(true);
        method.invoke(plugin, enabled);
    }

    @Override
    public void disablePlugins() {
        Plugin[] all = getPlugins();
        for (int i = all.length - 1; i >= 0; i--) {
            disablePlugin(all[i]);
        }
    }

    @Override
    public void clearPlugins() {
        disablePlugins();
        plugins.clear();
        lookupNames.clear();
        HandlerList.unregisterAll();
        permissions.clear();
        defaultPerms.get(true).clear();
        defaultPerms.get(false).clear();
    }

    // ----- events -----

    @Override
    public void callEvent(Event event) {
        if (event.isAsynchronous()) {
            if (Main.isServerThread()) {
                throw new IllegalStateException(event.getEventName() + " may only be triggered asynchronously.");
            }
        } else if (!Main.isServerThread()) {
            throw new IllegalStateException(event.getEventName() + " may only be triggered synchronously.");
        }
        HandlerList handlers = event.getHandlers();
        for (RegisteredListener registration : handlers.getRegisteredListeners()) {
            if (!registration.getPlugin().isEnabled()) {
                continue;
            }
            try {
                registration.callEvent(event);
            } catch (AuthorNagException e) {
                Plugin plugin = registration.getPlugin();
                if (plugin.isNaggable()) {
                    plugin.setNaggable(false);
                    logger.log(Level.SEVERE, "Nag author(s): '" + plugin.getDescription().getAuthors() + "' of '" + plugin.getDescription().getFullName() + "' about the following: " + e.getMessage());
                }
            } catch (Throwable t) {
                logger.log(Level.SEVERE, "Could not pass event " + event.getEventName() + " to " + registration.getPlugin().getDescription().getFullName(), t);
            }
        }
    }

    @Override
    public void registerEvents(Listener listener, Plugin plugin) {
        if (!plugin.isEnabled()) {
            throw new IllegalPluginAccessException("Plugin attempted to register " + listener + " while not enabled");
        }
        for (Map.Entry<Class<? extends Event>, Set<RegisteredListener>> entry : createRegisteredListeners(listener, plugin).entrySet()) {
            getEventListeners(getRegistrationClass(entry.getKey())).registerAll(entry.getValue());
        }
        publishListenerFlags();
    }

    @Override
    public void registerEvent(Class<? extends Event> event, Listener listener, EventPriority priority, EventExecutor executor, Plugin plugin) {
        registerEvent(event, listener, priority, executor, plugin, false);
    }

    @Override
    public void registerEvent(Class<? extends Event> event, Listener listener, EventPriority priority, EventExecutor executor, Plugin plugin, boolean ignoreCancelled) {
        if (!plugin.isEnabled()) {
            throw new IllegalPluginAccessException("Plugin attempted to register " + event + " while not enabled");
        }
        getEventListeners(event).register(new RegisteredListener(listener, executor, priority, plugin, ignoreCancelled));
        publishListenerFlags();
    }

    static Map<Class<? extends Event>, Set<RegisteredListener>> createRegisteredListeners(Listener listener, Plugin plugin) {
        Map<Class<? extends Event>, Set<RegisteredListener>> result = new HashMap<>();
        Set<Method> methods = new HashSet<>();
        try {
            Collections.addAll(methods, listener.getClass().getMethods());
            Collections.addAll(methods, listener.getClass().getDeclaredMethods());
        } catch (NoClassDefFoundError e) {
            plugin.getLogger().severe("Plugin " + plugin.getDescription().getFullName() + " has failed to register events for "
                    + listener.getClass() + " because " + e.getMessage() + " does not exist.");
            return result;
        }
        for (Method method : methods) {
            EventHandler handler = method.getAnnotation(EventHandler.class);
            if (handler == null || method.isBridge() || method.isSynthetic()) {
                continue;
            }
            if (method.getParameterCount() != 1 || !Event.class.isAssignableFrom(method.getParameterTypes()[0])) {
                plugin.getLogger().severe(plugin.getDescription().getFullName() + " attempted to register an invalid EventHandler method signature \""
                        + method.toGenericString() + "\" in " + listener.getClass());
                continue;
            }
            Class<? extends Event> eventClass = method.getParameterTypes()[0].asSubclass(Event.class);
            method.setAccessible(true);
            EventExecutor executor = EventExecutor.create(method, eventClass);
            result.computeIfAbsent(eventClass, k -> new HashSet<>())
                    .add(new RegisteredListener(listener, executor, handler.priority(), plugin, handler.ignoreCancelled()));
        }
        return result;
    }

    private static HandlerList getEventListeners(Class<? extends Event> type) {
        try {
            Method method = getRegistrationClass(type).getDeclaredMethod("getHandlerList");
            method.setAccessible(true);
            return (HandlerList) method.invoke(null);
        } catch (Exception e) {
            throw new IllegalPluginAccessException(e.toString());
        }
    }

    private static Class<? extends Event> getRegistrationClass(Class<? extends Event> clazz) {
        try {
            clazz.getDeclaredMethod("getHandlerList");
            return clazz;
        } catch (NoSuchMethodException e) {
            if (clazz.getSuperclass() != null && !clazz.getSuperclass().equals(Event.class)
                    && Event.class.isAssignableFrom(clazz.getSuperclass())) {
                return getRegistrationClass(clazz.getSuperclass().asSubclass(Event.class));
            }
        }
        throw new IllegalPluginAccessException("Unable to find handler list for event " + clazz.getName() + ". Static getHandlerList method required!");
    }

    /** True when at least one plugin listens for the event class (by simple name). */
    boolean hasListeners(Class<? extends Event> type) {
        try {
            return getEventListeners(type).getRegisteredListeners().length > 0;
        } catch (RuntimeException e) {
            return false;
        }
    }

    /** Tell PrismMC which of its hookable events are worth a round trip. */
    void publishListenerFlags() {
        Map<String, Boolean> flags = new LinkedHashMap<>();
        for (Map.Entry<String, Class<? extends Event>> entry : Events.HOOKABLE.entrySet()) {
            flags.put(entry.getKey(), hasListeners(entry.getValue()));
        }
        if (flags.equals(lastListenerFlags)) {
            return;
        }
        lastListenerFlags = flags;
        JsonObject payload = new JsonObject();
        flags.forEach(payload::addProperty);
        Main.rpc.notify("listeners", payload);
    }

    // ----- permissions (Bukkit SimplePluginManager semantics) -----

    @Override
    public Permission getPermission(String name) {
        return permissions.get(name.toLowerCase(Locale.ROOT));
    }

    @Override
    public void addPermission(Permission perm) {
        addPermission(perm, true);
    }

    public void addPermission(Permission perm, boolean dirty) {
        String name = perm.getName().toLowerCase(Locale.ROOT);
        if (permissions.containsKey(name)) {
            throw new IllegalArgumentException("The permission " + name + " is already defined!");
        }
        permissions.put(name, perm);
        calculatePermissionDefault(perm, dirty);
    }

    @Override
    public void addPermissions(List<Permission> perms) {
        for (Permission perm : perms) {
            addPermission(perm, false);
        }
        dirtyPermissibles();
    }

    @Override
    public void clearPermissions() {
        permissions.clear();
        defaultPerms.get(true).clear();
        defaultPerms.get(false).clear();
    }

    @Override
    public Set<Permission> getDefaultPermissions(boolean op) {
        return Collections.unmodifiableSet(defaultPerms.get(op));
    }

    @Override
    public void removePermission(Permission perm) {
        removePermission(perm.getName());
    }

    @Override
    public void removePermission(String name) {
        permissions.remove(name.toLowerCase(Locale.ROOT));
    }

    @Override
    public void recalculatePermissionDefaults(Permission perm) {
        if (perm != null && permissions.containsKey(perm.getName().toLowerCase(Locale.ROOT))) {
            defaultPerms.get(true).remove(perm);
            defaultPerms.get(false).remove(perm);
            calculatePermissionDefault(perm, true);
        }
    }

    private void calculatePermissionDefault(Permission perm, boolean dirty) {
        if (perm.getDefault() == PermissionDefault.OP || perm.getDefault() == PermissionDefault.TRUE) {
            defaultPerms.get(true).add(perm);
            if (dirty) {
                dirtyPermissibles(true);
            }
        }
        if (perm.getDefault() == PermissionDefault.NOT_OP || perm.getDefault() == PermissionDefault.TRUE) {
            defaultPerms.get(false).add(perm);
            if (dirty) {
                dirtyPermissibles(false);
            }
        }
    }

    private void dirtyPermissibles() {
        dirtyPermissibles(true);
        dirtyPermissibles(false);
    }

    private void dirtyPermissibles(boolean op) {
        for (Permissible permissible : new ArrayList<>(getDefaultPermSubscriptions(op))) {
            permissible.recalculatePermissions();
        }
    }

    @Override
    public void subscribeToPermission(String permission, Permissible permissible) {
        permSubs.computeIfAbsent(permission.toLowerCase(Locale.ROOT), k -> new WeakHashMap<>()).put(permissible, true);
    }

    @Override
    public void unsubscribeFromPermission(String permission, Permissible permissible) {
        Map<Permissible, Boolean> map = permSubs.get(permission.toLowerCase(Locale.ROOT));
        if (map != null) {
            map.remove(permissible);
            if (map.isEmpty()) {
                permSubs.remove(permission.toLowerCase(Locale.ROOT));
            }
        }
    }

    @Override
    public Set<Permissible> getPermissionSubscriptions(String permission) {
        Map<Permissible, Boolean> map = permSubs.get(permission.toLowerCase(Locale.ROOT));
        return map == null ? Set.of() : Collections.unmodifiableSet(new HashSet<>(map.keySet()));
    }

    @Override
    public void subscribeToDefaultPerms(boolean op, Permissible permissible) {
        defSubs.get(op).put(permissible, true);
    }

    @Override
    public void unsubscribeFromDefaultPerms(boolean op, Permissible permissible) {
        defSubs.get(op).remove(permissible);
    }

    @Override
    public Set<Permissible> getDefaultPermSubscriptions(boolean op) {
        return Collections.unmodifiableSet(new HashSet<>(defSubs.get(op).keySet()));
    }

    @Override
    public Set<Permission> getPermissions() {
        return new HashSet<>(permissions.values());
    }

    @Override
    public boolean useTimings() {
        return false;
    }

    @Override
    public boolean isTransitiveDependency(PluginMeta plugin, PluginMeta depend) {
        Set<String> seen = new HashSet<>();
        return isTransitiveDependency(plugin, depend.getName(), seen);
    }

    private boolean isTransitiveDependency(PluginMeta plugin, String target, Set<String> seen) {
        if (!seen.add(plugin.getName())) {
            return false;
        }
        List<String> deps = new ArrayList<>(plugin.getPluginDependencies());
        deps.addAll(plugin.getPluginSoftDependencies());
        for (String dep : deps) {
            if (dep.equals(target)) {
                return true;
            }
            Plugin next = getPlugin(dep);
            if (next != null && isTransitiveDependency(next.getPluginMeta(), target, seen)) {
                return true;
            }
        }
        return false;
    }

    @Override
    public void overridePermissionManager(Plugin plugin, PermissionManager permissionManager) {
        logger.warning(plugin.getDescription().getFullName() + " tried to override the permission manager; CraftPP does not support that yet.");
    }

    /** PluginLoader handed to JavaPlugin.init; only the parts plugins actually call. */
    public static final class LoaderShim implements PluginLoader {
        public LoaderShim() {
        }

        @Override
        public Plugin loadPlugin(File file) throws InvalidPluginException, UnknownDependencyException {
            return Main.pluginManager.loadPlugin(file);
        }

        @Override
        public PluginDescriptionFile getPluginDescription(File file) throws InvalidDescriptionException {
            return readDescription(file);
        }

        @Override
        public Pattern[] getPluginFileFilters() {
            return new Pattern[] {Pattern.compile("\\.jar$")};
        }

        @Override
        public Map<Class<? extends Event>, Set<RegisteredListener>> createRegisteredListeners(Listener listener, Plugin plugin) {
            return PluginManagerImpl.createRegisteredListeners(listener, plugin);
        }

        @Override
        public void enablePlugin(Plugin plugin) {
            Main.pluginManager.enablePlugin(plugin);
        }

        @Override
        public void disablePlugin(Plugin plugin) {
            Main.pluginManager.disablePlugin(plugin);
        }
    }

    static boolean isStatic(Method method) {
        return Modifier.isStatic(method.getModifiers());
    }

    /** BootstrapContext / PluginProviderContext for paper-plugin.yml bootstrappers and loaders. */
    static final class BootstrapContextImpl {
        private final PluginDescriptionFile description;
        private final File dataFolder;
        private final File source;

        BootstrapContextImpl(PluginDescriptionFile description, File dataFolder, File source) {
            this.description = description;
            this.dataFolder = dataFolder;
            this.source = source;
        }

        public PluginMeta getConfiguration() {
            return description;
        }

        public java.nio.file.Path getDataDirectory() {
            return dataFolder.toPath();
        }

        public net.kyori.adventure.text.logger.slf4j.ComponentLogger getLogger() {
            return net.kyori.adventure.text.logger.slf4j.ComponentLogger.logger(description.getName());
        }

        public java.nio.file.Path getPluginSource() {
            return source.toPath();
        }

        public io.papermc.paper.plugin.lifecycle.event.LifecycleEventManager<io.papermc.paper.plugin.bootstrap.BootstrapContext> getLifecycleManager() {
            return ProxyBase.create(io.papermc.paper.plugin.lifecycle.event.LifecycleEventManager.class, new ServicesImpl.LifecycleImpl(this));
        }

        @Override
        public String toString() {
            return "BootstrapContext{" + description.getName() + "}";
        }
    }

    /** Maven-resolved plugin libraries (plugin.yml `libraries:` and paper-plugin loaders), cached under libraries/. */
    static final class Libraries {
        private Libraries() {
        }

        static List<java.nio.file.Path> resolve(PluginDescriptionFile description, String loaderClass, PluginClassLoaderImpl loader, BootstrapContextImpl context) {
            List<java.nio.file.Path> paths = new ArrayList<>();
            io.papermc.paper.plugin.loader.library.LibraryStore store = path -> {
                paths.add(path);
                Logger.getLogger("").info("[" + description.getName() + "] Loaded library " + path.getFileName());
            };
            List<String> coordinates = description.getLibraries();
            if (coordinates != null && !coordinates.isEmpty()) {
                try {
                    io.papermc.paper.plugin.loader.library.impl.MavenLibraryResolver resolver = new io.papermc.paper.plugin.loader.library.impl.MavenLibraryResolver();
                    resolver.addRepository(new org.eclipse.aether.repository.RemoteRepository.Builder("central", "default",
                            io.papermc.paper.plugin.loader.library.impl.MavenLibraryResolver.MAVEN_CENTRAL_DEFAULT_MIRROR).build());
                    for (String coordinate : coordinates) {
                        resolver.addDependency(new org.eclipse.aether.graph.Dependency(new org.eclipse.aether.artifact.DefaultArtifact(coordinate), null));
                    }
                    Logger.getLogger("").info("[" + description.getName() + "] Loading " + coordinates.size() + " libraries... please wait");
                    resolver.register(store);
                } catch (Throwable t) {
                    Logger.getLogger("").log(Level.SEVERE, "[" + description.getName() + "] Could not load libraries", t);
                }
            }
            if (loaderClass != null) {
                try {
                    io.papermc.paper.plugin.loader.PluginLoader pluginLoader = (io.papermc.paper.plugin.loader.PluginLoader) Class.forName(loaderClass, true, loader).getDeclaredConstructor().newInstance();
                    io.papermc.paper.plugin.loader.PluginClasspathBuilder builder = new io.papermc.paper.plugin.loader.PluginClasspathBuilder() {
                        @Override
                        public io.papermc.paper.plugin.loader.PluginClasspathBuilder addLibrary(io.papermc.paper.plugin.loader.library.ClassPathLibrary library) {
                            try {
                                library.register(store);
                            } catch (io.papermc.paper.plugin.loader.library.LibraryLoadingException e) {
                                Logger.getLogger("").log(Level.SEVERE, "[" + description.getName() + "] Could not load a library", e);
                            }
                            return this;
                        }

                        @Override
                        public io.papermc.paper.plugin.bootstrap.PluginProviderContext getContext() {
                            return ProxyBase.create(io.papermc.paper.plugin.bootstrap.PluginProviderContext.class, context);
                        }
                    };
                    pluginLoader.classloader(builder);
                } catch (Throwable t) {
                    Logger.getLogger("").log(Level.SEVERE, "[" + description.getName() + "] Plugin loader failed", t);
                }
            }
            return paths;
        }
    }
}
