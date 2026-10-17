package prism.bridge;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.papermc.paper.registry.RegistryKey;
import io.papermc.paper.registry.tag.Tag;
import io.papermc.paper.registry.tag.TagKey;
import net.kyori.adventure.key.Key;
import net.kyori.adventure.text.Component;
import org.bukkit.Keyed;
import org.bukkit.Material;
import org.bukkit.NamespacedKey;
import org.bukkit.Registry;
import org.bukkit.enchantments.Enchantment;
import org.bukkit.enchantments.EnchantmentTarget;
import org.bukkit.entity.EntityType;
import org.bukkit.inventory.ItemStack;
import org.bukkit.potion.PotionEffect;
import org.bukkit.potion.PotionEffectType;
import org.bukkit.potion.PotionEffectTypeCategory;

import java.io.InputStreamReader;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.stream.Stream;

/**
 * Real registries. Static registries (sounds, effects, attributes, menus...)
 * come from the vanilla 26.3 registries report bundled with the bridge;
 * data-driven ones (biomes, enchantments, damage types, variants...) come
 * from PrismMC's synced registry catalog. Elements are lightweight keyed
 * objects: interface types are proxies, the abstract API classes get small
 * concrete subclasses with vanilla-shaped defaults.
 */
final class Registries {
    private static final Map<String, List<String>> KEYS = new LinkedHashMap<>();
    private static final Map<String, RegistryImpl<?>> BY_PATH = new HashMap<>();
    private static final Map<Class<?>, RegistryImpl<?>> BY_TYPE = new HashMap<>();
    private static final Map<String, Class<?>> TYPES = new HashMap<>();
    private static boolean loaded;

    private Registries() {
    }

    /** Loads the bundled static registries once; merges PrismMC's synced registries whenever they arrive. */
    static synchronized void load(JsonObject fromServer) {
        if (!loaded) {
            loaded = true;
            loadStatic();
        }
        if (fromServer != null) {
            for (Map.Entry<String, JsonElement> entry : fromServer.entrySet()) {
                List<String> keys = new ArrayList<>();
                for (JsonElement element : entry.getValue().getAsJsonArray()) {
                    keys.add(element.getAsString());
                }
                KEYS.put(entry.getKey(), keys);
                RegistryImpl<?> existing = BY_PATH.get(entry.getKey());
                if (existing != null && !existing.type.isEnum()) {
                    existing.keys.clear();
                    existing.keys.addAll(keys);
                }
            }
        }
        for (Map.Entry<String, Class<?>> entry : TYPES.entrySet()) {
            RegistryImpl<?> registry = registryFor(entry.getKey());
            BY_TYPE.putIfAbsent(entry.getValue(), registry);
        }
    }

    private static RegistryImpl<?> registryFor(String path) {
        // Not computeIfAbsent: building one registry can initialise API classes that ask for another.
        RegistryImpl<?> registry = BY_PATH.get(path);
        if (registry == null) {
            registry = build(path, TYPES.get(path));
            BY_PATH.put(path, registry);
        }
        return registry;
    }

    private static void loadStatic() {
        for (Field field : RegistryKey.class.getFields()) {
            if (field.getType() != RegistryKey.class) {
                continue;
            }
            try {
                RegistryKey<?> key = (RegistryKey<?>) field.get(null);
                Class<?> type = elementType(field);
                if (type != null) {
                    TYPES.put(key.key().asString(), type);
                }
            } catch (ReflectiveOperationException | RuntimeException ignored) {
            }
        }
        try (InputStreamReader reader = new InputStreamReader(
                Registries.class.getClassLoader().getResourceAsStream("registries-26.3.json"), StandardCharsets.UTF_8)) {
            JsonObject report = JsonParser.parseReader(reader).getAsJsonObject();
            for (Map.Entry<String, JsonElement> entry : report.entrySet()) {
                JsonObject entries = entry.getValue().getAsJsonObject().getAsJsonObject("entries");
                String[] ordered = new String[entries.size()];
                for (Map.Entry<String, JsonElement> element : entries.entrySet()) {
                    int id = element.getValue().getAsJsonObject().get("protocol_id").getAsInt();
                    if (id >= 0 && id < ordered.length) {
                        ordered[id] = element.getKey();
                    }
                }
                List<String> keys = new ArrayList<>();
                for (String key : ordered) {
                    if (key != null) {
                        keys.add(key);
                    }
                }
                KEYS.put(entry.getKey(), keys);
            }
        } catch (Exception e) {
            Main.logThrowable(java.util.logging.Logger.getLogger(""), "Could not read the bundled registry report", e);
        }
    }

    private static Class<?> elementType(Field field) {
        if (field.getGenericType() instanceof ParameterizedType parameterized) {
            var argument = parameterized.getActualTypeArguments()[0];
            if (argument instanceof Class<?> clazz) {
                return clazz;
            }
            if (argument instanceof ParameterizedType nested && nested.getRawType() instanceof Class<?> raw) {
                return raw;
            }
        }
        return null;
    }

    static synchronized RegistryImpl<?> byKey(RegistryKey<?> key) {
        load(null);
        return registryFor(key.key().asString());
    }

    static synchronized RegistryImpl<?> byType(Class<?> type) {
        load(null);
        RegistryImpl<?> registry = BY_TYPE.get(type);
        if (registry != null) {
            return registry;
        }
        for (Map.Entry<Class<?>, RegistryImpl<?>> entry : BY_TYPE.entrySet()) {
            if (entry.getKey().isAssignableFrom(type) || type.isAssignableFrom(entry.getKey())) {
                return entry.getValue();
            }
        }
        return new RegistryImpl<>(type.getSimpleName().toLowerCase(Locale.ROOT), type, Map.of());
    }

    static Enchantment enchantment(String key) {
        Object value = byKey(RegistryKey.ENCHANTMENT).get(NamespacedKey.fromString(key));
        return value instanceof Enchantment enchantment ? enchantment : null;
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static RegistryImpl<?> build(String path, Class<?> type) {
        List<String> keys = new ArrayList<>(KEYS.getOrDefault(path, List.of()));
        Map<NamespacedKey, Object> values = new LinkedHashMap<>();
        if (type != null && type.isEnum()) {
            keys.clear();
            for (Object constant : type.getEnumConstants()) {
                if (constant instanceof Keyed keyed) {
                    try {
                        values.put(keyed.getKey(), constant);
                        keys.add(keyed.getKey().asString());
                    } catch (RuntimeException ignored) {
                        // EntityType.UNKNOWN and friends have no key.
                    }
                }
            }
            return new RegistryImpl(path, type, keys, values, key -> null);
        }
        Class<?> elementType = type == null ? Keyed.class : type;
        // Registries PrismMC does not sync (structures, keyed API constants) still
        // resolve every constant the API declares: derive keys from its static fields.
        for (Field field : elementType.getDeclaredFields()) {
            if (elementType != org.bukkit.GameRule.class && java.lang.reflect.Modifier.isStatic(field.getModifiers()) && field.getType() == elementType) {
                String derived = "minecraft:" + field.getName().toLowerCase(Locale.ROOT);
                if (!keys.contains(derived)) {
                    keys.add(derived);
                }
            }
        }
        // Elements are created on first lookup: API classes (GameEvent, Enchantment...)
        // resolve their static constants through this registry while it is built.
        java.util.function.Function<NamespacedKey, Object> factory = key -> create(elementType, key);
        return new RegistryImpl(path, elementType, keys, values, factory);
    }

    private static Object create(Class<?> type, NamespacedKey key) {
        if (type == org.bukkit.GameRule.class) return GameRulesBridge.create(key);
        if (type == Enchantment.class) {
            return new EnchantmentImpl(key);
        }
        if (type == PotionEffectType.class) {
            return new PotionEffectTypeImpl(key);
        }
        if (type == org.bukkit.GameEvent.class) {
            return new GameEventImpl(key);
        }
        if (type == org.bukkit.MusicInstrument.class) {
            return new MusicInstrumentImpl(key);
        }
        if (type.isInterface()) {
            return keyedProxy(type, key);
        }
        return null;
    }

    /** A Keyed element that answers identity/translation queries and defers the rest to sensible defaults. */
    static Object keyedProxy(Class<?> type, NamespacedKey key) {
        KeyedHandler handler = new KeyedHandler(type, key);
        List<Class<?>> interfaces = new ArrayList<>();
        interfaces.add(type);
        // API constants cast registry elements to nested Typed views (ItemType.Typed, MenuType.Typed...).
        for (Class<?> nested : type.getDeclaredClasses()) {
            if (nested.isInterface() && type.isAssignableFrom(nested) && java.lang.reflect.Modifier.isPublic(nested.getModifiers())) {
                interfaces.add(nested);
            }
        }
        interfaces.add(KeyedElement.class);
        return Proxy.newProxyInstance(Registries.class.getClassLoader(), interfaces.toArray(new Class<?>[0]), handler);
    }

    /** Marker so proxies can be recognised and compared. */
    interface KeyedElement {
        NamespacedKey prismKey();
    }

    private static final class KeyedHandler implements InvocationHandler {
        private final Class<?> type;
        private final NamespacedKey key;

        KeyedHandler(Class<?> type, NamespacedKey key) {
            this.type = type;
            this.key = key;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            String name = method.getName();
            switch (name) {
                case "prismKey", "getKey", "getKeyOrThrow", "getKeyOrNull" -> {
                    return key;
                }
                case "key" -> {
                    return key;
                }
                case "asString" -> {
                    return key.asString();
                }
                case "translationKey", "getTranslationKey" -> {
                    return translationPrefix() + key.getNamespace() + "." + key.getKey().replace('/', '.');
                }
                case "name" -> {
                    return key.getKey().toUpperCase(Locale.ROOT);
                }
                case "ordinal" -> {
                    return 0;
                }
                case "compareTo" -> {
                    return key.asString().compareTo(((Keyed) args[0]).getKey().asString());
                }
                case "equals" -> {
                    return args[0] instanceof KeyedElement other && other.prismKey().equals(key);
                }
                case "hashCode" -> {
                    return key.hashCode();
                }
                case "toString" -> {
                    return type.getSimpleName() + "{" + key + "}";
                }
                default -> {
                }
            }
            if (type == org.bukkit.inventory.ItemType.class || type == org.bukkit.block.BlockType.class) {
                Object handled = itemTypeCall(name, args);
                if (handled != NO_VALUE) {
                    return handled;
                }
            }
            if (type == org.bukkit.attribute.Attribute.class) {
                if (name.equals("getDefaultValue")) {
                    return attributeDefault(key.getKey());
                }
                if (name.equals("getSentiment")) {
                    return org.bukkit.attribute.Attribute.Sentiment.POSITIVE;
                }
            }
            if (type == org.bukkit.inventory.MenuType.class) {
                if (name.equals("typed")) {
                    return proxy;
                }
                if (name.equals("create") && args != null && args.length == 2 && args[0] instanceof org.bukkit.entity.HumanEntity human) {
                    org.bukkit.event.inventory.InventoryType inventoryType = menuInventoryType(key.getKey());
                    String title = args[1] instanceof Component component ? ServerImpl.LEGACY.serialize(component) : String.valueOf(args[1]);
                    Inventories.InventoryImpl inventory = new Inventories.InventoryImpl(null, inventoryType, Inventories.defaultSize(inventoryType), title);
                    if (key.getKey().startsWith("generic_9x")) {
                        inventory = new Inventories.InventoryImpl(null, inventoryType, 9 * Integer.parseInt(key.getKey().substring(10)), title);
                    }
                    if (ProxyBase.unwrap(human) instanceof PlayerImpl player) {
                        return new Inventories.ViewImpl(inventory, player, inventoryType, title);
                    }
                }
                if (name.equals("getInventoryViewClass")) {
                    return org.bukkit.inventory.InventoryView.class;
                }
            }
            if (method.isDefault()) {
                return InvocationHandler.invokeDefault(proxy, method, args);
            }
            Class<?> returnType = method.getReturnType();
            if (returnType == boolean.class) {
                return false;
            }
            if (returnType == int.class || returnType == short.class) {
                return 0;
            }
            if (returnType == float.class || returnType == double.class) {
                return 0.0f;
            }
            if (returnType == String.class) {
                return key.getKey();
            }
            if (returnType == Component.class) {
                return Component.translatable(translationPrefix() + key.getNamespace() + "." + key.getKey());
            }
            if (returnType == java.util.Optional.class) {
                return java.util.Optional.empty();
            }
            if (Collection.class.isAssignableFrom(returnType)) {
                return returnType.isAssignableFrom(java.util.Set.class) ? java.util.Set.of() : List.of();
            }
            if (returnType == Material.class) {
                Material material = Material.matchMaterial(key.asString());
                return material == null ? Material.AIR : material;
            }
            return null;
        }

        private static final Object NO_VALUE = new Object();

        /**
         * Material delegates its property queries (isSolid, getMaxStackSize...) to
         * these registry elements, so the answers must come from the jar-truth
         * table rather than from Material - that would recurse forever.
         */
        private Object itemTypeCall(String name, Object[] args) {
            Material material = Material.matchMaterial(key.asString());
            MaterialProperties.BlockInfo block = MaterialProperties.block(key.asString());
            MaterialProperties.ItemInfo item = MaterialProperties.item(key.asString());
            switch (name) {
                case "asMaterial" -> {
                    return material;
                }
                case "typed" -> {
                    return proxyOf();
                }
                case "getMaxStackSize" -> {
                    return item.maxStackSize();
                }
                case "getMaxDurability" -> {
                    return (short) item.maxDamage();
                }
                case "createItemStack" -> {
                    return new PrismItemStack(material == null ? Material.AIR : material, args == null || args.length == 0 ? 1 : (Integer) args[0]);
                }
                case "createBlockData" -> {
                    return args == null || args.length == 0 ? BlockDataImpl.fromString(key.asString())
                            : args[0] instanceof String data ? BlockDataImpl.fromString(key.asString() + data) : BlockDataImpl.fromString(key.asString());
                }
                case "hasBlockType" -> {
                    return MaterialProperties.isBlock(key.asString());
                }
                case "hasItemType" -> {
                    return MaterialProperties.isItem(key.asString());
                }
                case "getBlockType" -> {
                    return byKey(RegistryKey.BLOCK).get(key);
                }
                case "getItemType" -> {
                    return byKey(RegistryKey.ITEM).get(key);
                }
                case "isEdible" -> {
                    return item.edible();
                }
                case "isRecord" -> {
                    return item.record();
                }
                case "isFuel" -> {
                    return item.fuel() > 0;
                }
                case "getBurnDuration" -> {
                    return item.fuel();
                }
                case "isCompostable" -> {
                    return item.compost() > 0;
                }
                case "getCompostChance" -> {
                    return item.compost();
                }
                case "getCraftingRemainingItem" -> {
                    return item.remainder().isEmpty() ? null : byKey(RegistryKey.ITEM).get(NamespacedKey.fromString(item.remainder()));
                }
                case "getEquipmentSlot" -> {
                    return switch (item.slot()) {
                        case "head" -> org.bukkit.inventory.EquipmentSlot.HEAD;
                        case "chest" -> org.bukkit.inventory.EquipmentSlot.CHEST;
                        case "legs" -> org.bukkit.inventory.EquipmentSlot.LEGS;
                        case "feet" -> org.bukkit.inventory.EquipmentSlot.FEET;
                        case "offhand" -> org.bukkit.inventory.EquipmentSlot.OFF_HAND;
                        case "body" -> org.bukkit.inventory.EquipmentSlot.BODY;
                        default -> org.bukkit.inventory.EquipmentSlot.HAND;
                    };
                }
                case "getItemRarity" -> {
                    return switch (item.rarity()) {
                        case "uncommon" -> org.bukkit.inventory.ItemRarity.UNCOMMON;
                        case "rare" -> org.bukkit.inventory.ItemRarity.RARE;
                        case "epic" -> org.bukkit.inventory.ItemRarity.EPIC;
                        default -> org.bukkit.inventory.ItemRarity.COMMON;
                    };
                }
                case "getItemMetaClass" -> {
                    return org.bukkit.inventory.meta.ItemMeta.class;
                }
                case "isEnabledByFeature" -> {
                    return true;
                }
                case "getDefaultAttributeModifiers" -> {
                    return com.google.common.collect.ImmutableMultimap.of();
                }
                case "getCreativeCategory" -> {
                    return null;
                }
                case "isSolid" -> {
                    return block.solid();
                }
                case "isAir" -> {
                    return block.air();
                }
                case "isOccluding" -> {
                    return block.occluding();
                }
                case "isFlammable", "isBurnable" -> {
                    return block.flammable();
                }
                case "isCollidable" -> {
                    return block.collidable();
                }
                case "hasGravity" -> {
                    return block.gravity();
                }
                case "isInteractable" -> {
                    return block.blockEntity();
                }
                case "getHardness" -> {
                    return block.hardness();
                }
                case "getBlastResistance" -> {
                    return block.blastResistance();
                }
                case "getSlipperiness" -> {
                    return block.friction();
                }
                case "getLightEmission" -> {
                    return block.light();
                }
                case "getBlockDataClass" -> {
                    return org.bukkit.block.data.BlockData.class;
                }
                default -> {
                    return NO_VALUE;
                }
            }
        }

        private Object proxyOf() {
            return byKey(type == org.bukkit.inventory.ItemType.class ? RegistryKey.ITEM : RegistryKey.BLOCK).get(key);
        }

        private String translationPrefix() {
            String simple = type.getSimpleName().toLowerCase(Locale.ROOT);
            if (type == org.bukkit.inventory.ItemType.class) {
                return "item.";
            }
            if (type == org.bukkit.block.BlockType.class) {
                return "block.";
            }
            if (type == org.bukkit.block.Biome.class) {
                return "biome.";
            }
            if (type == org.bukkit.attribute.Attribute.class) {
                return "attribute.name.";
            }
            if (type == org.bukkit.Sound.class) {
                return "subtitles.";
            }
            return simple + ".";
        }

        private static org.bukkit.event.inventory.InventoryType menuInventoryType(String menu) {
            return switch (menu) {
                case "generic_3x3" -> org.bukkit.event.inventory.InventoryType.DISPENSER;
                case "hopper" -> org.bukkit.event.inventory.InventoryType.HOPPER;
                case "shulker_box" -> org.bukkit.event.inventory.InventoryType.SHULKER_BOX;
                case "furnace" -> org.bukkit.event.inventory.InventoryType.FURNACE;
                case "blast_furnace" -> org.bukkit.event.inventory.InventoryType.BLAST_FURNACE;
                case "smoker" -> org.bukkit.event.inventory.InventoryType.SMOKER;
                case "anvil" -> org.bukkit.event.inventory.InventoryType.ANVIL;
                case "crafting" -> org.bukkit.event.inventory.InventoryType.WORKBENCH;
                case "enchantment" -> org.bukkit.event.inventory.InventoryType.ENCHANTING;
                case "brewing_stand" -> org.bukkit.event.inventory.InventoryType.BREWING;
                case "beacon" -> org.bukkit.event.inventory.InventoryType.BEACON;
                case "lectern" -> org.bukkit.event.inventory.InventoryType.LECTERN;
                case "loom" -> org.bukkit.event.inventory.InventoryType.LOOM;
                case "cartography_table" -> org.bukkit.event.inventory.InventoryType.CARTOGRAPHY;
                case "grindstone" -> org.bukkit.event.inventory.InventoryType.GRINDSTONE;
                case "stonecutter" -> org.bukkit.event.inventory.InventoryType.STONECUTTER;
                case "merchant" -> org.bukkit.event.inventory.InventoryType.MERCHANT;
                case "smithing" -> org.bukkit.event.inventory.InventoryType.SMITHING;
                case "crafter_3x3" -> org.bukkit.event.inventory.InventoryType.CRAFTER;
                default -> org.bukkit.event.inventory.InventoryType.CHEST;
            };
        }

        private static double attributeDefault(String path) {
            return switch (path) {
                case "max_health" -> 20.0;
                case "movement_speed" -> 0.1;
                case "attack_damage" -> 2.0;
                case "attack_speed" -> 4.0;
                case "armor", "armor_toughness", "knockback_resistance", "luck" -> 0.0;
                case "follow_range" -> 32.0;
                case "flying_speed" -> 0.4;
                case "scale", "gravity", "step_height" -> path.equals("gravity") ? 0.08 : path.equals("step_height") ? 0.6 : 1.0;
                case "safe_fall_distance" -> 3.0;
                case "fall_damage_multiplier", "jump_strength" -> path.equals("jump_strength") ? 0.42 : 1.0;
                case "block_interaction_range" -> 4.5;
                case "entity_interaction_range" -> 3.0;
                case "block_break_speed", "mining_efficiency" -> path.equals("block_break_speed") ? 1.0 : 0.0;
                case "max_absorption" -> 0.0;
                default -> 1.0;
            };
        }
    }

    /** Concrete Registry over an ordered key list; elements materialise lazily. */
    static final class RegistryImpl<T extends Keyed> implements Registry<T> {
        private final String path;
        final Class<?> type;
        final List<String> keys;
        private final Map<NamespacedKey, T> values;
        private final java.util.function.Function<NamespacedKey, Object> factory;
        private boolean materialized;

        @SuppressWarnings("unchecked")
        RegistryImpl(String path, Class<?> type, List<String> keys, Map<NamespacedKey, ?> values, java.util.function.Function<NamespacedKey, Object> factory) {
            this.path = path;
            this.type = type;
            this.keys = keys;
            this.values = (Map<NamespacedKey, T>) values;
            this.factory = factory;
        }

        RegistryImpl(String path, Class<?> type, Map<NamespacedKey, ?> values) {
            this(path, type, new ArrayList<>(), values, key -> null);
        }

        @Override
        @SuppressWarnings("unchecked")
        public T get(NamespacedKey key) {
            T cached = values.get(key);
            if (cached != null || key == null) {
                return cached;
            }
            if (!keys.contains(key.asString())) {
                return null;
            }
            Object created = factory.apply(key);
            if (created == null) {
                return null;
            }
            synchronized (values) {
                T existing = values.get(key);
                if (existing != null) {
                    return existing; // created recursively by a static initialiser meanwhile
                }
                values.put(key, (T) created);
                return (T) created;
            }
        }

        private void materialize() {
            if (!materialized) {
                for (String key : keys) {
                    get(NamespacedKey.fromString(key));
                }
                materialized = true;
            }
        }

        @Override
        public NamespacedKey getKey(T value) {
            return value.getKey();
        }

        @Override
        public boolean hasTag(TagKey<T> key) {
            return false;
        }

        @Override
        public Tag<T> getTag(TagKey<T> key) {
            throw new java.util.NoSuchElementException("No tag " + key.key() + " in " + path + " (CraftPP does not sync tags to plugins yet)");
        }

        @Override
        public Collection<Tag<T>> getTags() {
            return Collections.emptyList();
        }

        @Override
        public Stream<T> stream() {
            materialize();
            return new ArrayList<>(values.values()).stream();
        }

        @Override
        public Stream<NamespacedKey> keyStream() {
            materialize();
            return new ArrayList<>(values.keySet()).stream();
        }

        @Override
        public int size() {
            return keys.isEmpty() ? values.size() : keys.size();
        }

        @Override
        public Iterator<T> iterator() {
            materialize();
            return new ArrayList<>(values.values()).iterator();
        }

        @Override
        public String toString() {
            return "Registry{" + path + ", " + values.size() + " entries}";
        }
    }

    // ----- concrete elements for the abstract API classes -----

    static final class EnchantmentImpl extends Enchantment {
        private static final Map<String, Integer> MAX_LEVELS = Map.ofEntries(
                Map.entry("sharpness", 5), Map.entry("smite", 5), Map.entry("bane_of_arthropods", 5), Map.entry("knockback", 2),
                Map.entry("fire_aspect", 2), Map.entry("looting", 3), Map.entry("sweeping_edge", 3), Map.entry("efficiency", 5),
                Map.entry("silk_touch", 1), Map.entry("unbreaking", 3), Map.entry("fortune", 3), Map.entry("power", 5),
                Map.entry("punch", 2), Map.entry("flame", 1), Map.entry("infinity", 1), Map.entry("luck_of_the_sea", 3),
                Map.entry("lure", 3), Map.entry("loyalty", 3), Map.entry("impaling", 5), Map.entry("riptide", 3),
                Map.entry("channeling", 1), Map.entry("multishot", 1), Map.entry("quick_charge", 3), Map.entry("piercing", 4),
                Map.entry("mending", 1), Map.entry("vanishing_curse", 1), Map.entry("binding_curse", 1), Map.entry("protection", 4),
                Map.entry("fire_protection", 4), Map.entry("feather_falling", 4), Map.entry("blast_protection", 4),
                Map.entry("projectile_protection", 4), Map.entry("respiration", 3), Map.entry("aqua_affinity", 1),
                Map.entry("thorns", 3), Map.entry("depth_strider", 3), Map.entry("frost_walker", 2), Map.entry("soul_speed", 3),
                Map.entry("swift_sneak", 3), Map.entry("density", 5), Map.entry("breach", 4), Map.entry("wind_burst", 3));
        private final NamespacedKey key;

        EnchantmentImpl(NamespacedKey key) {
            this.key = key;
        }

        @Override
        public NamespacedKey getKey() {
            return key;
        }

        @Override
        public String getName() {
            return key.getKey().toUpperCase(Locale.ROOT);
        }

        @Override
        public int getMaxLevel() {
            return MAX_LEVELS.getOrDefault(key.getKey(), 1);
        }

        @Override
        public int getStartLevel() {
            return 1;
        }

        @Override
        public EnchantmentTarget getItemTarget() {
            return EnchantmentTarget.ALL;
        }

        @Override
        public boolean isTreasure() {
            return key.getKey().endsWith("_curse") || key.getKey().equals("mending") || key.getKey().equals("frost_walker") || key.getKey().equals("soul_speed") || key.getKey().equals("swift_sneak");
        }

        @Override
        public boolean isCursed() {
            return key.getKey().endsWith("_curse");
        }

        @Override
        public boolean conflictsWith(Enchantment other) {
            return false;
        }

        @Override
        public boolean canEnchantItem(ItemStack item) {
            return true;
        }

        @Override
        public Component displayName(int level) {
            return Component.translatable("enchantment." + key.getNamespace() + "." + key.getKey());
        }

        @Override
        public boolean isTradeable() {
            return !isCursed();
        }

        @Override
        public boolean isDiscoverable() {
            return !isCursed();
        }

        @Override
        public int getMinModifiedCost(int level) {
            return level * 10;
        }

        @Override
        public int getMaxModifiedCost(int level) {
            return level * 10 + 50;
        }

        @Override
        public int getAnvilCost() {
            return 1;
        }

        @Override
        public io.papermc.paper.enchantments.EnchantmentRarity getRarity() {
            return io.papermc.paper.enchantments.EnchantmentRarity.COMMON;
        }

        @Override
        public float getDamageIncrease(int level, org.bukkit.entity.EntityCategory category) {
            return key.getKey().equals("sharpness") ? 0.5f * level + 0.5f : 0;
        }

        @Override
        public float getDamageIncrease(int level, EntityType type) {
            return getDamageIncrease(level, org.bukkit.entity.EntityCategory.NONE);
        }

        @Override
        public java.util.Set<org.bukkit.inventory.EquipmentSlotGroup> getActiveSlotGroups() {
            return java.util.Set.of(org.bukkit.inventory.EquipmentSlotGroup.ANY);
        }

        @Override
        public Component description() {
            return displayName(1);
        }

        @Override
        public io.papermc.paper.registry.set.RegistryKeySet<org.bukkit.inventory.ItemType> getSupportedItems() {
            return io.papermc.paper.registry.set.RegistrySet.keySet(RegistryKey.ITEM, List.of());
        }

        @Override
        public io.papermc.paper.registry.set.RegistryKeySet<org.bukkit.inventory.ItemType> getPrimaryItems() {
            return getSupportedItems();
        }

        @Override
        public int getWeight() {
            return 10;
        }

        @Override
        public io.papermc.paper.registry.set.RegistryKeySet<Enchantment> getExclusiveWith() {
            return io.papermc.paper.registry.set.RegistrySet.keySet(RegistryKey.ENCHANTMENT, List.of());
        }

        @Override
        public String translationKey() {
            return "enchantment." + key.getNamespace() + "." + key.getKey();
        }

        @Override
        public String getTranslationKey() {
            return translationKey();
        }

        @Override
        public boolean equals(Object other) {
            return other instanceof Enchantment enchantment && enchantment.getKey().equals(key);
        }

        @Override
        public int hashCode() {
            return key.hashCode();
        }

        @Override
        public String toString() {
            return "Enchantment{" + key + "}";
        }
    }

    static final class PotionEffectTypeImpl extends PotionEffectType {
        private final NamespacedKey key;
        private final int id;

        PotionEffectTypeImpl(NamespacedKey key) {
            this.key = key;
            this.id = KEYS.getOrDefault("minecraft:mob_effect", List.of()).indexOf(key.asString()) + 1;
        }

        @Override
        public NamespacedKey getKey() {
            return key;
        }

        @Override
        public PotionEffect createEffect(int duration, int amplifier) {
            return new PotionEffect(this, duration, amplifier);
        }

        @Override
        public boolean isInstant() {
            return key.getKey().equals("instant_health") || key.getKey().equals("instant_damage") || key.getKey().equals("saturation");
        }

        @Override
        public PotionEffectTypeCategory getCategory() {
            return switch (key.getKey()) {
                case "slowness", "mining_fatigue", "instant_damage", "nausea", "blindness", "hunger", "weakness", "poison", "wither",
                     "levitation", "bad_omen", "darkness", "infested", "oozing", "weaving", "wind_charged", "unluck" -> PotionEffectTypeCategory.HARMFUL;
                case "glowing", "bad_luck", "trial_omen", "raid_omen" -> PotionEffectTypeCategory.NEUTRAL;
                default -> PotionEffectTypeCategory.BENEFICIAL;
            };
        }

        @Override
        public org.bukkit.Color getColor() {
            return org.bukkit.Color.fromRGB(0x3E, 0x8E, 0xE5);
        }

        @Override
        public double getDurationModifier() {
            return 1.0;
        }

        @Override
        public int getId() {
            return id;
        }

        @Override
        public String getName() {
            return key.getKey().toUpperCase(Locale.ROOT);
        }

        @Override
        public Map<org.bukkit.attribute.Attribute, org.bukkit.attribute.AttributeModifier> getEffectAttributes() {
            return Map.of();
        }

        @Override
        public double getAttributeModifierAmount(org.bukkit.attribute.Attribute attribute, int amplifier) {
            return 0;
        }

        @Override
        public PotionEffectType.Category getEffectCategory() {
            return switch (getCategory()) {
                case HARMFUL -> PotionEffectType.Category.HARMFUL;
                case NEUTRAL -> PotionEffectType.Category.NEUTRAL;
                default -> PotionEffectType.Category.BENEFICIAL;
            };
        }

        @Override
        public String translationKey() {
            return "effect." + key.getNamespace() + "." + key.getKey();
        }

        @Override
        public String getTranslationKey() {
            return translationKey();
        }

        @Override
        public boolean equals(Object other) {
            return other instanceof PotionEffectType type && type.getKey().equals(key);
        }

        @Override
        public int hashCode() {
            return key.hashCode();
        }

        @Override
        public String toString() {
            return "PotionEffectType{" + key + "}";
        }
    }

    static final class GameEventImpl extends org.bukkit.GameEvent {
        private final NamespacedKey key;

        GameEventImpl(NamespacedKey key) {
            this.key = key;
        }

        @Override
        public NamespacedKey getKey() {
            return key;
        }

        @Override
        public int getRange() {
            return 16;
        }

        @Override
        public int getVibrationLevel() {
            return 1;
        }

        @Override
        public boolean equals(Object other) {
            return other instanceof org.bukkit.GameEvent event && event.getKey().equals(key);
        }

        @Override
        public int hashCode() {
            return key.hashCode();
        }
    }

    static final class MusicInstrumentImpl extends org.bukkit.MusicInstrument {
        private final NamespacedKey key;

        MusicInstrumentImpl(NamespacedKey key) {
            this.key = key;
        }

        @Override
        public NamespacedKey getKey() {
            return key;
        }

        @Override
        public float getDuration() {
            return 7;
        }

        @Override
        public float getRange() {
            return 256;
        }

        @Override
        public Component description() {
            return Component.translatable("instrument." + key.getNamespace() + "." + key.getKey());
        }

        @Override
        public org.bukkit.Sound getSound() {
            Object sound = byKey(RegistryKey.SOUND_EVENT).get(NamespacedKey.minecraft("item.goat_horn.sound.0"));
            return sound instanceof org.bukkit.Sound value ? value : null;
        }

        @Override
        public String translationKey() {
            return "instrument." + key.getNamespace() + "." + key.getKey();
        }

        @Override
        public boolean equals(Object other) {
            return other instanceof org.bukkit.MusicInstrument instrument && instrument.getKey().equals(key);
        }

        @Override
        public int hashCode() {
            return key.hashCode();
        }
    }

    static List<String> keysOf(String path) {
        return KEYS.getOrDefault(path, List.of());
    }

    static Key keyOf(String text) {
        return Key.key(text);
    }
}
