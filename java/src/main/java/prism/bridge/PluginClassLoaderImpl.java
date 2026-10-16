package prism.bridge;

import io.papermc.paper.plugin.configuration.PluginMeta;
import io.papermc.paper.plugin.provider.classloader.ClassLoaderAccess;
import io.papermc.paper.plugin.provider.classloader.ConfiguredPluginClassLoader;
import io.papermc.paper.plugin.provider.classloader.PluginClassLoaderGroup;
import org.bukkit.plugin.PluginDescriptionFile;
import org.bukkit.plugin.java.JavaPlugin;

import java.io.File;
import java.io.IOException;
import java.net.MalformedURLException;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Parent-first plugin class loader (Bukkit semantics): the API and its
 * libraries win, then the plugin jar, then classes from other loaded plugins.
 */
public final class PluginClassLoaderImpl extends URLClassLoader implements ConfiguredPluginClassLoader {
    static final Group GROUP = new Group();

    private final PluginDescriptionFile description;
    private final File file;
    private final File dataFolder;
    private final Map<String, Class<?>> classes = new ConcurrentHashMap<>();
    private JavaPlugin plugin;

    PluginClassLoaderImpl(File file, PluginDescriptionFile description, File dataFolder) throws MalformedURLException {
        super(new URL[] {file.toURI().toURL()}, PluginClassLoaderImpl.class.getClassLoader());
        this.file = file;
        this.description = description;
        this.dataFolder = dataFolder;
        GROUP.add(this);
    }

    static {
        ClassLoader.registerAsParallelCapable();
    }

    @Override
    public PluginMeta getConfiguration() {
        return description;
    }

    void addLibrary(java.nio.file.Path path) {
        try {
            addURL(path.toUri().toURL());
        } catch (MalformedURLException ignored) {
        }
    }

    @Override
    protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        return loadClass(name, resolve, true, true);
    }

    @Override
    public Class<?> loadClass(String name, boolean resolve, boolean checkGlobal, boolean checkLibraries) throws ClassNotFoundException {
        try {
            return super.loadClass(name, resolve);
        } catch (ClassNotFoundException ignored) {
        }
        if (checkGlobal) {
            Class<?> global = GROUP.getClassByName(name, resolve, this);
            if (global != null) {
                return global;
            }
        }
        throw new ClassNotFoundException(name);
    }

    @Override
    protected Class<?> findClass(String name) throws ClassNotFoundException {
        Class<?> cached = classes.get(name);
        if (cached != null) {
            return cached;
        }
        Class<?> found = super.findClass(name);
        classes.put(name, found);
        return found;
    }

    /** Class from this plugin's jar only (used by the group for cross-plugin lookups). */
    Class<?> findLocal(String name) {
        Class<?> cached = classes.get(name);
        if (cached != null) {
            return cached;
        }
        try {
            return findClass(name);
        } catch (ClassNotFoundException e) {
            return null;
        }
    }

    @Override
    public void init(JavaPlugin javaPlugin) {
        if (plugin != null) {
            throw new IllegalStateException("Plugin already initialized: " + description.getName());
        }
        plugin = javaPlugin;
        javaPlugin.init(PluginManagerImpl.LOADER, org.bukkit.Bukkit.getServer(), description, dataFolder, file, this);
    }

    @Override
    public JavaPlugin getPlugin() {
        return plugin;
    }

    @Override
    public PluginClassLoaderGroup getGroup() {
        return GROUP;
    }

    File getFile() {
        return file;
    }

    @Override
    public void close() throws IOException {
        GROUP.remove(this);
        super.close();
    }

    @Override
    public String toString() {
        return "PluginClassLoader{" + description.getName() + "}";
    }

    /** One shared group: every plugin can see every other plugin's classes. */
    static final class Group implements PluginClassLoaderGroup {
        private final CopyOnWriteArrayList<PluginClassLoaderImpl> loaders = new CopyOnWriteArrayList<>();

        @Override
        public Class<?> getClassByName(String name, boolean resolve, ConfiguredPluginClassLoader requester) {
            for (PluginClassLoaderImpl loader : loaders) {
                if (loader == requester) {
                    continue;
                }
                Class<?> found = loader.findLocal(name);
                if (found != null) {
                    return found;
                }
            }
            return null;
        }

        @Override
        public void remove(ConfiguredPluginClassLoader loader) {
            loaders.remove(loader);
        }

        @Override
        public void add(ConfiguredPluginClassLoader loader) {
            if (loader instanceof PluginClassLoaderImpl impl && !loaders.contains(impl)) {
                loaders.add(impl);
            }
        }

        @Override
        public ClassLoaderAccess getAccess() {
            return loader -> true;
        }
    }
}
