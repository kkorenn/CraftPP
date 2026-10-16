package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import org.bukkit.Axis;
import org.bukkit.Bukkit;
import org.bukkit.Material;
import org.bukkit.block.BlockFace;
import org.bukkit.block.data.BlockData;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Block states with properties. A BlockData proxy implements BlockData plus
 * every org.bukkit.block.data interface its properties satisfy (Directional
 * for "facing", Waterlogged for "waterlogged", Stairs for shape+half+facing...),
 * translating typed getters/setters to the vanilla property strings PrismMC
 * resolves to state ids.
 */
public final class BlockDataImpl implements InvocationHandler {
    private static final Map<String, JsonObject> DEFAULTS = new ConcurrentHashMap<>();

    final Material material;
    final Map<String, String> properties;
    final Map<String, List<String>> allowed;

    private BlockDataImpl(Material material, Map<String, String> properties, Map<String, List<String>> allowed) {
        this.material = material;
        this.properties = new java.util.TreeMap<>(properties); // Vanilla StateDefinition keeps properties sorted by name.
        this.allowed = allowed;
    }

    static BlockData of(Material material) {
        return fromString(material.getKey().asString());
    }

    static BlockData fromString(String text) {
        String key = text.contains("[") ? text.substring(0, text.indexOf('[')) : text;
        JsonObject json = DEFAULTS.computeIfAbsent(text.contains("[") ? text : key, t -> {
            JsonElement result = Main.rpc.api("world.blockDataOf", "text", t);
            return result != null && result.isJsonObject() ? result.getAsJsonObject() : null;
        });
        if (json == null) {
            throw new IllegalArgumentException("Could not parse block data: " + text);
        }
        return fromJson(json);
    }

    static BlockData fromJson(JsonObject json) {
        Material material = Material.matchMaterial(json.get("block").getAsString());
        if (material == null) {
            material = Material.AIR;
        }
        Map<String, String> properties = new LinkedHashMap<>();
        if (json.has("properties")) {
            for (Map.Entry<String, JsonElement> entry : json.getAsJsonObject("properties").entrySet()) {
                properties.put(entry.getKey(), entry.getValue().getAsString());
            }
        }
        Map<String, List<String>> allowed = new LinkedHashMap<>();
        if (json.has("allowed")) {
            for (Map.Entry<String, JsonElement> entry : json.getAsJsonObject("allowed").entrySet()) {
                List<String> values = new ArrayList<>();
                for (JsonElement value : entry.getValue().getAsJsonArray()) {
                    values.add(value.getAsString());
                }
                allowed.put(entry.getKey(), values);
            }
        }
        return create(new BlockDataImpl(material, properties, allowed));
    }

    static BlockDataImpl unwrap(BlockData data) {
        if (data != null && Proxy.isProxyClass(data.getClass()) && Proxy.getInvocationHandler(data) instanceof BlockDataImpl impl) {
            return impl;
        }
        return null;
    }

    private static BlockData create(BlockDataImpl handler) {
        Set<Class<?>> interfaces = new LinkedHashSet<>();
        interfaces.add(BlockData.class);
        for (Class<?> candidate : TYPED) {
            if (handler.satisfies(candidate)) {
                interfaces.add(candidate);
            }
        }
        return (BlockData) Proxy.newProxyInstance(BlockDataImpl.class.getClassLoader(), interfaces.toArray(new Class<?>[0]), handler);
    }

    /** Every typed block-data interface in the API that maps onto vanilla properties. */
    private static final List<Class<?>> TYPED = loadTyped();

    private static List<Class<?>> loadTyped() {
        List<Class<?>> types = new ArrayList<>();
        String[] names = {
                "Ageable", "AnaloguePowerable", "Attachable", "Bisected", "Brushable", "Directional", "FaceAttachable", "Hangable", "Hatchable",
                "Levelled", "Lightable", "MultipleFacing", "Openable", "Orientable", "Powerable", "Rail", "Rotatable", "Snowable", "Waterlogged",
                "type.Bamboo", "type.Barrel", "type.Bed", "type.Beehive", "type.Bell", "type.BigDripleaf", "type.BrewingStand", "type.BubbleColumn",
                "type.Cake", "type.Campfire", "type.Candle", "type.CaveVines", "type.Chest", "type.CommandBlock", "type.Comparator", "type.CoralWallFan",
                "type.DaylightDetector", "type.Dispenser", "type.Door", "type.EndPortalFrame", "type.EnderChest", "type.Farmland", "type.Fence",
                "type.Fire", "type.Furnace", "type.Gate", "type.Grindstone", "type.Hopper", "type.Jigsaw", "type.Jukebox", "type.Ladder", "type.Lantern",
                "type.Leaves", "type.Lectern", "type.Light", "type.LightningRod", "type.NoteBlock", "type.Observer", "type.Piston", "type.PistonHead",
                "type.PointedDripstone", "type.RedstoneRail", "type.RedstoneWallTorch", "type.RedstoneWire", "type.Repeater", "type.RespawnAnchor",
                "type.Sapling", "type.Scaffolding", "type.SculkCatalyst", "type.SculkSensor", "type.SculkShrieker", "type.SeaPickle", "type.Sign",
                "type.Slab", "type.SmallDripleaf", "type.Snow", "type.Stairs", "type.StructureBlock", "type.Switch", "type.TNT", "type.TechnicalPiston",
                "type.TrapDoor", "type.Tripwire", "type.TripwireHook", "type.TurtleEgg", "type.Wall", "type.WallSign", "type.HangingSign", "type.WallHangingSign",
                "type.CopperBulb", "type.Crafter", "type.TrialSpawner", "type.Vault", "type.DecoratedPot", "type.ChiseledBookshelf", "type.Chain",
                "type.GlowLichen", "type.Cocoa", "type.Beacon", "type.Bamboo", "type.MossyCarpet",
        };
        for (String name : names) {
            try {
                types.add(Class.forName("org.bukkit.block.data." + name));
            } catch (ClassNotFoundException ignored) {
            }
        }
        return types;
    }

    /** A typed interface applies when every property its getters read exists on this block. */
    private boolean satisfies(Class<?> type) {
        int required = 0;
        if (type.getDeclaredMethods().length == 0) {
            // ponytail: marker interfaces (Ladder = Directional+Waterlogged) match by name only; a
            // per-block table from the jar would be exact.
            String marker = toSnake(type.getSimpleName());
            if (!material.getKey().getKey().contains(marker)) {
                return false;
            }
        }
        for (Method method : type.getMethods()) {
            if (method.getDeclaringClass() == BlockData.class || method.getDeclaringClass() == Object.class || method.isDefault()) {
                continue;
            }
            if (Set.class.isAssignableFrom(method.getReturnType()) || java.util.Collection.class.isAssignableFrom(method.getReturnType())) {
                continue; // getShapes()/getFaces(): allowed values, not a property
            }
            String property = propertyFor(method);
            if (property == null) {
                continue;
            }
            required++;
            if (property.equals("multiple_facing")) {
                if (!properties.containsKey("north") && !properties.containsKey("up")) {
                    return false;
                }
                continue;
            }
            String value = properties.get(property);
            if (value == null) {
                return false;
            }
            // Same property name, different enum (Stairs.Shape vs Rail.Shape): the value must be a constant.
            Class<?> returned = method.getReturnType();
            if (returned.isEnum() && !hasConstant(returned, value)) {
                return false;
            }
        }
        return required > 0;
    }

    private static boolean hasConstant(Class<?> type, String value) {
        String wanted = value.toUpperCase(Locale.ROOT);
        for (Object constant : type.getEnumConstants()) {
            if (((Enum<?>) constant).name().equals(wanted)) {
                return true;
            }
        }
        return false;
    }

    private static String propertyFor(Method method) {
        String name = method.getName();
        String bare;
        if (name.startsWith("get")) bare = name.substring(3);
        else if (name.startsWith("is")) bare = name.substring(2);
        else if (name.startsWith("set")) bare = name.substring(3);
        else if (name.startsWith("has")) bare = name.substring(3);
        else return null;
        if (bare.startsWith("Maximum") || bare.startsWith("Minimum")) {
            bare = bare.substring(7);
        }
        if (bare.isEmpty() || bare.equals("Faces") || bare.equals("AllowedFaces") || bare.equals("Face") && method.getParameterCount() > 0) {
            return "multiple_facing";
        }
        if (bare.equals("Height") && method.getParameterCount() > 0) {
            return "multiple_facing";
        }
        return switch (bare) {
            case "Facing" -> "facing";
            case "AttachedFace" -> "face";
            case "SignalFire" -> "signal_fire";
            case "HasRecord", "Record" -> "has_record";
            case "HasBook", "Book" -> "has_book";
            case "Bottle" -> "has_bottle_0";
            case "Charges" -> "charges";
            case "Berries" -> "berries";
            case "Flower" -> "flower";
            case "AttachedToAxis" -> "axis";
            default -> toSnake(bare);
        };
    }

    private static String toSnake(String camel) {
        StringBuilder builder = new StringBuilder();
        for (char c : camel.toCharArray()) {
            if (Character.isUpperCase(c) && builder.length() > 0) {
                builder.append('_');
            }
            builder.append(Character.toLowerCase(c));
        }
        return builder.toString();
    }

    private static final Set<String> FACE_PROPERTIES = Set.of("north", "east", "south", "west", "up", "down");

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        String name = method.getName();
        switch (name) {
            case "getMaterial", "getPlacementMaterial" -> {
                return material;
            }
            case "getAsString" -> {
                boolean hideUnspecified = args != null && args.length == 1 && (Boolean) args[0];
                return asString(hideUnspecified);
            }
            case "clone" -> {
                return create(new BlockDataImpl(material, properties, allowed));
            }
            case "merge" -> {
                BlockDataImpl other = unwrap((BlockData) args[0]);
                Map<String, String> merged = new LinkedHashMap<>(properties);
                if (other != null) merged.putAll(other.properties);
                return create(new BlockDataImpl(material, merged, allowed));
            }
            case "matches" -> {
                BlockDataImpl other = unwrap((BlockData) args[0]);
                if (other == null || other.material != material) return false;
                for (Map.Entry<String, String> entry : other.properties.entrySet()) {
                    if (!entry.getValue().equals(properties.get(entry.getKey()))) return false;
                }
                return true;
            }
            case "copyTo" -> {
                BlockDataImpl other = unwrap((BlockData) args[0]);
                if (other != null) {
                    for (String key : other.properties.keySet()) {
                        if (properties.containsKey(key)) other.properties.put(key, properties.get(key));
                    }
                }
                return null;
            }
            case "equals" -> {
                BlockDataImpl other = unwrap((BlockData) args[0]);
                return other != null && other.material == material && other.properties.equals(properties);
            }
            case "hashCode" -> {
                return material.hashCode() * 31 + properties.hashCode();
            }
            case "toString" -> {
                return "CraftBlockData{" + asString(false) + "}";
            }
            case "getLightEmission" -> {
                return switch (material) {
                    case TORCH, WALL_TORCH, LANTERN, GLOWSTONE, SEA_LANTERN, SHROOMLIGHT, JACK_O_LANTERN, BEACON, END_ROD, LAVA, CAMPFIRE, FIRE -> 15;
                    case SOUL_TORCH, SOUL_WALL_TORCH, SOUL_LANTERN -> 10;
                    case REDSTONE_TORCH, ENCHANTING_TABLE, BREWING_STAND, END_PORTAL_FRAME -> 7;
                    case MAGMA_BLOCK -> 3;
                    default -> 0;
                };
            }
            case "isOccluding" -> {
                return material.isOccluding();
            }
            case "requiresCorrectToolForDrops" -> {
                return material.name().endsWith("_ORE") || material == Material.OBSIDIAN || material == Material.ANCIENT_DEBRIS;
            }
            case "isPreferredTool" -> {
                return true;
            }
            case "getPistonMoveReaction" -> {
                return org.bukkit.block.PistonMoveReaction.MOVE;
            }
            case "isSupported" -> {
                return true;
            }
            case "isFaceSturdy" -> {
                return material.isOccluding();
            }
            case "getCollisionShape" -> {
                throw new UnsupportedOperationException("CraftPP does not compute collision shapes yet");
            }
            case "getMapColor" -> {
                return org.bukkit.Color.GRAY;
            }
            case "rotate" -> {
                rotate((org.bukkit.block.structure.StructureRotation) args[0]);
                return null;
            }
            case "mirror" -> {
                mirror((org.bukkit.block.structure.Mirror) args[0]);
                return null;
            }
            case "createBlockState" -> {
                throw new UnsupportedOperationException("CraftPP block states need a block position; use Block.getState()");
            }
            case "getDestroySpeed" -> {
                return material.getHardness();
            }
            case "isRandomlyTicked" -> {
                return properties.containsKey("age") || properties.containsKey("stage");
            }
            case "isReplaceable" -> {
                return material.isAir() || material == Material.WATER || material == Material.LAVA || material == Material.SHORT_GRASS || material == Material.TALL_GRASS;
            }
            case "getSoundGroup" -> {
                throw new UnsupportedOperationException("CraftPP does not expose sound groups yet");
            }
            default -> {
            }
        }
        Object typed = typedCall(method, args);
        if (typed != NO_VALUE) {
            return typed;
        }
        if (method.isDefault()) {
            return InvocationHandler.invokeDefault(proxy, method, args);
        }
        throw new UnsupportedOperationException("CraftPP does not implement " + method.getDeclaringClass().getSimpleName() + "." + name + " for block data");
    }

    private static final Object NO_VALUE = new Object();

    /** Typed org.bukkit.block.data.* getters and setters over the property map. */
    private Object typedCall(Method method, Object[] args) {
        String name = method.getName();
        // MultipleFacing / Wall: one property per face.
        switch (name) {
            case "getFaces" -> {
                Set<BlockFace> faces = new LinkedHashSet<>();
                if (properties.containsKey("facing") && !properties.containsKey("north")) {
                    // Directional.getFaces(): the facings this block allows.
                    for (String value : allowed.getOrDefault("facing", List.of())) {
                        faces.add(BlockFace.valueOf(value.toUpperCase(Locale.ROOT)));
                    }
                    return faces;
                }
                for (String face : FACE_PROPERTIES) {
                    if ("true".equals(properties.get(face))) faces.add(BlockFace.valueOf(face.toUpperCase(Locale.ROOT)));
                }
                return faces;
            }
            case "getAllowedFaces" -> {
                Set<BlockFace> faces = new LinkedHashSet<>();
                for (String face : FACE_PROPERTIES) {
                    if (properties.containsKey(face)) faces.add(BlockFace.valueOf(face.toUpperCase(Locale.ROOT)));
                }
                return faces;
            }
            case "hasFace" -> {
                return "true".equals(properties.get(((BlockFace) args[0]).name().toLowerCase(Locale.ROOT)));
            }
            case "setFace" -> {
                properties.put(((BlockFace) args[0]).name().toLowerCase(Locale.ROOT), String.valueOf(args[1]));
                return null;
            }
            case "getHeight" -> {
                if (args != null && args.length == 1) {
                    String value = properties.getOrDefault(((BlockFace) args[0]).name().toLowerCase(Locale.ROOT), "none");
                    return enumValue(method.getReturnType(), value);
                }
            }
            case "setHeight" -> {
                if (args != null && args.length == 2) {
                    properties.put(((BlockFace) args[0]).name().toLowerCase(Locale.ROOT), ((Enum<?>) args[1]).name().toLowerCase(Locale.ROOT));
                    return null;
                }
            }
            default -> {
            }
        }
        String property = propertyFor(method);
        if (property == null || property.equals("multiple_facing")) {
            return NO_VALUE;
        }
        String bare = name.replaceFirst("^(get|is|set|has)", "");
        if (bare.startsWith("Maximum") || bare.startsWith("Minimum")) {
            List<String> values = allowed.getOrDefault(property, List.of());
            int extreme = bare.startsWith("Maximum") ? Integer.MIN_VALUE : Integer.MAX_VALUE;
            for (String value : values) {
                try {
                    int number = Integer.parseInt(value);
                    extreme = bare.startsWith("Maximum") ? Math.max(extreme, number) : Math.min(extreme, number);
                } catch (NumberFormatException ignored) {
                }
            }
            return extreme == Integer.MIN_VALUE || extreme == Integer.MAX_VALUE ? 0 : extreme;
        }
        if (bare.endsWith("s") && method.getReturnType() == Set.class) {
            // getShapes / getTypes...: allowed values as enums.
            Set<Object> result = new LinkedHashSet<>();
            Class<?> element = (Class<?>) ((java.lang.reflect.ParameterizedType) method.getGenericReturnType()).getActualTypeArguments()[0];
            for (String value : allowed.getOrDefault(property, List.of())) {
                Object converted = enumValue(element, value);
                if (converted != null) result.add(converted);
            }
            return result;
        }
        if (name.startsWith("set") && args != null && args.length == 1) {
            properties.put(property, toProperty(args[0]));
            return null;
        }
        if (!properties.containsKey(property)) {
            return NO_VALUE;
        }
        String value = properties.get(property);
        Class<?> type = method.getReturnType();
        if (type == boolean.class) return Boolean.parseBoolean(value);
        if (type == int.class) {
            try {
                return Integer.parseInt(value);
            } catch (NumberFormatException e) {
                return 0;
            }
        }
        if (type.isEnum()) return enumValue(type, value);
        if (type == String.class) return value;
        return NO_VALUE;
    }

    private static Object enumValue(Class<?> type, String value) {
        if (!type.isEnum()) {
            return null;
        }
        String wanted = value.toUpperCase(Locale.ROOT);
        for (Object constant : type.getEnumConstants()) {
            if (((Enum<?>) constant).name().equals(wanted)) {
                return constant;
            }
        }
        return type.getEnumConstants().length > 0 ? type.getEnumConstants()[0] : null;
    }

    private static String toProperty(Object value) {
        if (value instanceof Enum<?> constant) {
            return constant.name().toLowerCase(Locale.ROOT);
        }
        return String.valueOf(value);
    }

    private void rotate(org.bukkit.block.structure.StructureRotation rotation) {
        int steps = switch (rotation) {
            case CLOCKWISE_90 -> 1;
            case CLOCKWISE_180 -> 2;
            case COUNTERCLOCKWISE_90 -> 3;
            default -> 0;
        };
        String facing = properties.get("facing");
        if (facing != null) {
            List<String> ring = Arrays.asList("north", "east", "south", "west");
            int index = ring.indexOf(facing);
            if (index >= 0) properties.put("facing", ring.get((index + steps) % 4));
        }
        String axis = properties.get("axis");
        if (axis != null && steps % 2 == 1) {
            properties.put("axis", axis.equals("x") ? "z" : axis.equals("z") ? "x" : axis);
        }
    }

    private void mirror(org.bukkit.block.structure.Mirror mirror) {
        String facing = properties.get("facing");
        if (facing == null) {
            return;
        }
        if (mirror == org.bukkit.block.structure.Mirror.LEFT_RIGHT) {
            properties.put("facing", facing.equals("north") ? "south" : facing.equals("south") ? "north" : facing);
        } else if (mirror == org.bukkit.block.structure.Mirror.FRONT_BACK) {
            properties.put("facing", facing.equals("east") ? "west" : facing.equals("west") ? "east" : facing);
        }
    }

    String asString(boolean hideUnspecified) {
        if (properties.isEmpty()) {
            return material.getKey().asString();
        }
        StringBuilder builder = new StringBuilder(material.getKey().asString()).append('[');
        boolean first = true;
        for (Map.Entry<String, String> entry : properties.entrySet()) {
            if (!first) builder.append(',');
            builder.append(entry.getKey()).append('=').append(entry.getValue());
            first = false;
        }
        return builder.append(']').toString();
    }

    JsonObject propertiesJson() {
        JsonObject json = new JsonObject();
        properties.forEach(json::addProperty);
        return json;
    }

    static JsonArray listJson(List<String> values) {
        JsonArray array = new JsonArray();
        values.forEach(array::add);
        return array;
    }

    static Axis axisOf(String value) {
        return Axis.valueOf(value.toUpperCase(Locale.ROOT));
    }

    static Material materialOf(BlockData data) {
        return data == null ? Material.AIR : data.getMaterial();
    }

    static BlockData defaultFor(Material material) {
        return Bukkit.createBlockData(material);
    }
}
