package prism.bridge;

import io.papermc.paper.InternalAPIBridge;
import io.papermc.paper.plugin.lifecycle.event.LifecycleEventManager;
import io.papermc.paper.plugin.lifecycle.event.handler.configuration.LifecycleEventHandlerConfiguration;
import io.papermc.paper.plugin.provider.classloader.ClassLoaderAccess;
import io.papermc.paper.plugin.provider.classloader.ConfiguredPluginClassLoader;
import io.papermc.paper.plugin.provider.classloader.PaperClassLoaderStorage;
import io.papermc.paper.plugin.provider.classloader.PluginClassLoaderGroup;
import io.papermc.paper.registry.RegistryAccess;
import io.papermc.paper.registry.RegistryKey;
import net.kyori.adventure.text.Component;
import net.kyori.adventure.text.flattener.ComponentFlattener;
import org.bukkit.Keyed;
import org.bukkit.NamespacedKey;
import org.bukkit.Registry;
import org.bukkit.inventory.ItemStack;
import org.bukkit.plugin.Plugin;
import org.bukkit.plugin.java.JavaPlugin;

import java.util.Collections;
import java.util.Iterator;
import java.util.function.BooleanSupplier;
import java.util.stream.Stream;

/**
 * The three services paper-api discovers through ServiceLoader. These are the
 * only static injection points between the API jar and a server.
 */
public final class ServicesImpl {
    private ServicesImpl() {
    }

    /** {@code io.papermc.paper.InternalAPIBridge} */
    public static final class Bridge implements InternalAPIBridge {
        @Override
        public LifecycleEventManager<Plugin> createPluginLifecycleEventManager(JavaPlugin plugin, BooleanSupplier registrationCheck) {
            return ProxyBase.create(LifecycleEventManager.class, new LifecycleImpl(plugin));
        }

        @Override
        public ItemStack createEmptyStack() {
            return new PrismItemStack();
        }

        @Override
        public ComponentFlattener componentFlattener() {
            return ComponentFlattener.basic();
        }

        @Override
        public String getTranslationKey(org.bukkit.entity.EntityType type) {
            return "entity." + type.getKey().getNamespace() + "." + type.getKey().getKey();
        }

        @Override
        public Component resolveWithContext(Component component, org.bukkit.command.CommandSender sender, org.bukkit.entity.Entity entity, boolean bypassPermissions) {
            return component;
        }

        // Everything below is out of scope for PrismMC's current gameplay model.

        @Override
        public org.bukkit.block.Biome constructLegacyCustomBiome() {
            return (org.bukkit.block.Biome) prism.bridge.Registries.keyedProxy(org.bukkit.block.Biome.class, NamespacedKey.minecraft("custom"));
        }

        @Override
        public io.papermc.paper.world.damagesource.CombatEntry createCombatEntry(org.bukkit.entity.LivingEntity entity, org.bukkit.damage.DamageSource source, float damage) {
            throw unsupported("createCombatEntry");
        }

        @Override
        public io.papermc.paper.world.damagesource.CombatEntry createCombatEntry(org.bukkit.damage.DamageSource source, float damage, io.papermc.paper.world.damagesource.FallLocationType fallLocationType, float fallDistance) {
            throw unsupported("createCombatEntry");
        }

        @Override
        public java.util.function.Predicate<io.papermc.paper.command.brigadier.CommandSourceStack> restricted(java.util.function.Predicate<io.papermc.paper.command.brigadier.CommandSourceStack> predicate, boolean allowFunctionsAndCommandBlocks) {
            return predicate;
        }

        @Override
        public io.papermc.paper.datacomponent.item.ResolvableProfile defaultMannequinProfile() {
            throw unsupported("defaultMannequinProfile");
        }

        @Override
        public <C> io.papermc.paper.persistence.PersistentDataKey<C> createPersistentDataKey(net.kyori.adventure.key.Key key, org.bukkit.persistence.PersistentDataType<?, C> type) {
            throw unsupported("createPersistentDataKey");
        }

        @Override
        public com.destroystokyo.paper.SkinParts.Mutable allSkinParts() {
            throw unsupported("allSkinParts");
        }

        @Override
        public Component defaultMannequinDescription() {
            return Component.empty();
        }

        @Override
        public <MODERN, LEGACY> org.bukkit.GameRule<LEGACY> legacyGameRuleBridge(org.bukkit.GameRule<MODERN> modern, java.util.function.Function<LEGACY, MODERN> toModern, java.util.function.Function<MODERN, LEGACY> toLegacy, Class<LEGACY> legacyType) {
            throw unsupported("legacyGameRuleBridge");
        }

        @Override
        public java.util.Set<org.bukkit.entity.Pose> validMannequinPoses() {
            return java.util.Set.of();
        }

        @Override
        public io.papermc.paper.entity.poi.PoiType.Occupancy createOccupancy(String name) {
            throw unsupported("createOccupancy");
        }

        @Override
        public org.bukkit.damage.DamageSource.Builder createDamageSourceBuilder(org.bukkit.damage.DamageType type) {
            throw unsupported("createDamageSourceBuilder");
        }

        @Override
        public org.bukkit.damage.DamageEffect getDamageEffect(String key) {
            throw unsupported("getDamageEffect");
        }

        @Override
        public org.bukkit.entity.SpawnCategory getSpawnCategory(org.bukkit.entity.EntityType type) {
            return org.bukkit.entity.SpawnCategory.MISC;
        }

        @Override
        public ItemStack deserializeItem(byte[] data) {
            throw unsupported("deserializeItem");
        }

        @Override
        public boolean hasDefaultEntityAttributes(NamespacedKey key) {
            return false;
        }

        @Override
        public org.bukkit.attribute.Attributable getDefaultEntityAttributes(NamespacedKey key) {
            throw unsupported("getDefaultEntityAttributes");
        }

        @Override
        public String getStatisticCriteriaKey(org.bukkit.Statistic statistic) {
            return "minecraft.custom:minecraft." + statistic.getKey().getKey();
        }

        private static UnsupportedOperationException unsupported(String what) {
            return new UnsupportedOperationException("CraftPP does not implement " + what + " yet");
        }
    }

    /** Lifecycle event handlers, kept per owner and fired by name (e.g. "commands") from Bootstrap. */
    static final class LifecycleImpl {
        static final java.util.List<Registration> REGISTRATIONS = new java.util.concurrent.CopyOnWriteArrayList<>();
        private final Object owner;

        record Registration(Object owner, String type, io.papermc.paper.plugin.lifecycle.event.handler.LifecycleEventHandler<?> handler, int priority) {
        }

        LifecycleImpl(Object owner) {
            this.owner = owner;
        }

        public void registerEventHandler(LifecycleEventHandlerConfiguration<?> configuration) {
            if (configuration instanceof io.papermc.paper.plugin.lifecycle.event.types.PrismLifecycleEventTypeProvider.Captured captured) {
                REGISTRATIONS.add(new Registration(owner, captured.typeName(), captured.handler(), captured.priority()));
            }
        }

        public <E extends io.papermc.paper.plugin.lifecycle.event.LifecycleEvent> void registerEventHandler(
                io.papermc.paper.plugin.lifecycle.event.types.LifecycleEventType<?, ? extends E, ?> type,
                io.papermc.paper.plugin.lifecycle.event.handler.LifecycleEventHandler<? super E> handler) {
            REGISTRATIONS.add(new Registration(owner, type.name(), handler, 0));
        }

        /** Fire an event to every handler registered for the type name, in priority order. */
        @SuppressWarnings({"unchecked", "rawtypes"})
        static void fire(String typeName, java.util.function.Function<Object, Object> eventFactory) {
            java.util.List<Registration> matching = new java.util.ArrayList<>();
            for (Registration registration : REGISTRATIONS) {
                if (registration.type().equals(typeName)) {
                    matching.add(registration);
                }
            }
            matching.sort(java.util.Comparator.comparingInt(Registration::priority));
            for (Registration registration : matching) {
                try {
                    ((io.papermc.paper.plugin.lifecycle.event.handler.LifecycleEventHandler) registration.handler())
                            .run((io.papermc.paper.plugin.lifecycle.event.LifecycleEvent) eventFactory.apply(registration.owner()));
                } catch (Throwable t) {
                    java.util.logging.Logger.getLogger("").log(java.util.logging.Level.SEVERE, "Lifecycle event " + typeName + " failed for " + registration.owner(), t);
                }
            }
        }

        static void forget(Object owner) {
            REGISTRATIONS.removeIf(registration -> registration.owner() == owner);
        }
    }

    /** {@code io.papermc.paper.registry.RegistryAccess}: real registries from the vanilla report + PrismMC's synced catalog. */
    public static final class Registries implements RegistryAccess {
        @Override
        @SuppressWarnings("unchecked")
        public <T extends Keyed> Registry<T> getRegistry(Class<T> type) {
            prism.bridge.Registries.load(null);
            return (Registry<T>) prism.bridge.Registries.byType(type);
        }

        @Override
        @SuppressWarnings("unchecked")
        public <T extends Keyed> Registry<T> getRegistry(RegistryKey<T> key) {
            prism.bridge.Registries.load(null);
            return (Registry<T>) prism.bridge.Registries.byKey(key);
        }
    }

    static final class EmptyRegistry {
        public Object get(NamespacedKey key) {
            return null;
        }

        public NamespacedKey getKey(Object value) {
            return value instanceof Keyed keyed ? keyed.getKey() : null;
        }

        public boolean hasTag(io.papermc.paper.registry.tag.TagKey<?> key) {
            return false;
        }

        public io.papermc.paper.registry.tag.Tag<?> getTag(io.papermc.paper.registry.tag.TagKey<?> key) {
            throw new java.util.NoSuchElementException("No tag " + key + " (CraftPP registries are empty)");
        }

        public java.util.Collection<io.papermc.paper.registry.tag.Tag<?>> getTags() {
            return Collections.emptyList();
        }

        public Stream<?> stream() {
            return Stream.empty();
        }

        public Stream<NamespacedKey> keyStream() {
            return Stream.empty();
        }

        public int size() {
            return 0;
        }

        public Iterator<?> iterator() {
            return Collections.emptyIterator();
        }

        public Object match(String input) {
            return null;
        }
    }

    /** {@code io.papermc.paper.plugin.provider.classloader.PaperClassLoaderStorage} */
    public static final class ClassLoaders implements PaperClassLoaderStorage {
        @Override
        public PluginClassLoaderGroup registerSpigotGroup(org.bukkit.plugin.java.PluginClassLoader loader) {
            return PluginClassLoaderImpl.GROUP;
        }

        @Override
        public PluginClassLoaderGroup registerOpenGroup(ConfiguredPluginClassLoader loader) {
            PluginClassLoaderImpl.GROUP.add(loader);
            return PluginClassLoaderImpl.GROUP;
        }

        @Override
        public PluginClassLoaderGroup registerAccessBackedGroup(ConfiguredPluginClassLoader loader, ClassLoaderAccess access) {
            PluginClassLoaderImpl.GROUP.add(loader);
            return PluginClassLoaderImpl.GROUP;
        }

        @Override
        public void unregisterClassloader(ConfiguredPluginClassLoader loader) {
            PluginClassLoaderImpl.GROUP.remove(loader);
        }

        @Override
        public boolean registerUnsafePlugin(ConfiguredPluginClassLoader loader) {
            PluginClassLoaderImpl.GROUP.add(loader);
            return true;
        }
    }
}
