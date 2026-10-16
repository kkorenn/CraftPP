package io.papermc.paper.command.brigadier.argument;

import com.mojang.brigadier.StringReader;
import com.mojang.brigadier.arguments.ArgumentType;
import com.mojang.brigadier.context.CommandContext;
import com.mojang.brigadier.exceptions.CommandSyntaxException;
import com.mojang.brigadier.exceptions.SimpleCommandExceptionType;
import com.mojang.brigadier.suggestion.Suggestions;
import com.mojang.brigadier.suggestion.SuggestionsBuilder;
import io.papermc.paper.command.brigadier.CommandSourceStack;
import io.papermc.paper.command.brigadier.argument.range.DoubleRangeProvider;
import io.papermc.paper.command.brigadier.argument.range.IntegerRangeProvider;
import io.papermc.paper.command.brigadier.argument.resolvers.BlockPositionResolver;
import io.papermc.paper.command.brigadier.argument.resolvers.FinePositionResolver;
import io.papermc.paper.command.brigadier.argument.resolvers.selector.EntitySelectorArgumentResolver;
import io.papermc.paper.command.brigadier.argument.resolvers.selector.PlayerSelectorArgumentResolver;
import io.papermc.paper.math.BlockPosition;
import io.papermc.paper.math.FinePosition;
import io.papermc.paper.math.Position;
import net.kyori.adventure.key.Key;
import net.kyori.adventure.text.Component;
import net.kyori.adventure.text.format.NamedTextColor;
import net.kyori.adventure.text.serializer.gson.GsonComponentSerializer;
import org.bukkit.Bukkit;
import org.bukkit.GameMode;
import org.bukkit.Location;
import org.bukkit.NamespacedKey;
import org.bukkit.World;
import org.bukkit.entity.Entity;
import org.bukkit.entity.Player;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.function.Function;

/**
 * PrismMC's {@code VanillaArgumentProvider} (package-private interface, so it
 * lives here). Each argument type parses the vanilla syntax on the Java side
 * and carries the vanilla parser key so PrismMC can declare it to clients.
 */
public final class PrismVanillaArgumentProvider implements VanillaArgumentProvider {
    /** An argument type that knows its vanilla parser id for the client command tree. */
    public static final class VanillaType<T> implements ArgumentType<T> {
        private final String parserKey;
        private final int flags;
        private final String registry;
        private final Parser<T> parser;
        private final Function<CommandSourceStack, Collection<String>> suggester;

        interface Parser<T> {
            T parse(StringReader reader) throws CommandSyntaxException;
        }

        VanillaType(String parserKey, int flags, String registry, Parser<T> parser, Function<CommandSourceStack, Collection<String>> suggester) {
            this.parserKey = parserKey;
            this.flags = flags;
            this.registry = registry;
            this.parser = parser;
            this.suggester = suggester;
        }

        public String parserKey() {
            return parserKey;
        }

        public int flags() {
            return flags;
        }

        public String registry() {
            return registry;
        }

        @Override
        public T parse(StringReader reader) throws CommandSyntaxException {
            return parser.parse(reader);
        }

        @Override
        public <S> CompletableFuture<Suggestions> listSuggestions(CommandContext<S> context, SuggestionsBuilder builder) {
            if (suggester == null || !(context.getSource() instanceof CommandSourceStack source)) {
                return Suggestions.empty();
            }
            String remaining = builder.getRemaining().toLowerCase(Locale.ROOT);
            for (String suggestion : suggester.apply(source)) {
                if (suggestion.toLowerCase(Locale.ROOT).startsWith(remaining)) {
                    builder.suggest(suggestion);
                }
            }
            return builder.buildFuture();
        }

        @Override
        public Collection<String> getExamples() {
            return List.of();
        }
    }

    private static final SimpleCommandExceptionType NO_PLAYER = new SimpleCommandExceptionType(() -> "No player was found");
    private static final SimpleCommandExceptionType NO_ENTITY = new SimpleCommandExceptionType(() -> "No entity was found");

    private static String readWord(StringReader reader) {
        int start = reader.getCursor();
        while (reader.canRead() && reader.peek() != ' ') {
            reader.skip();
        }
        return reader.getString().substring(start, reader.getCursor());
    }

    private static Collection<String> playerNames(CommandSourceStack source) {
        List<String> names = new ArrayList<>();
        for (Player player : Bukkit.getOnlinePlayers()) {
            names.add(player.getName());
        }
        names.add("@a");
        names.add("@p");
        names.add("@s");
        names.add("@r");
        return names;
    }

    private static List<Player> resolvePlayers(String selector, CommandSourceStack source, boolean single) throws CommandSyntaxException {
        List<Player> online = new ArrayList<>(Bukkit.getOnlinePlayers());
        List<Player> result = new ArrayList<>();
        String base = selector.contains("[") ? selector.substring(0, selector.indexOf('[')) : selector;
        switch (base) {
            case "@a" -> result.addAll(online);
            case "@s" -> {
                if (source.getExecutor() instanceof Player player) result.add(player);
            }
            case "@r" -> {
                if (!online.isEmpty()) result.add(online.get(new java.util.Random().nextInt(online.size())));
            }
            case "@p", "@n" -> {
                Location here = source.getLocation();
                online.stream().min(java.util.Comparator.comparingDouble(player -> player.getLocation().distanceSquared(here))).ifPresent(result::add);
            }
            default -> {
                Player player = Bukkit.getPlayerExact(base);
                if (player == null) {
                    try {
                        player = Bukkit.getPlayer(UUID.fromString(base));
                    } catch (IllegalArgumentException ignored) {
                    }
                }
                if (player != null) result.add(player);
            }
        }
        if (result.isEmpty()) {
            throw NO_PLAYER.create();
        }
        return single ? List.of(result.get(0)) : result;
    }

    private static List<Entity> resolveEntities(String selector, CommandSourceStack source, boolean single) throws CommandSyntaxException {
        String base = selector.contains("[") ? selector.substring(0, selector.indexOf('[')) : selector;
        List<Entity> result = new ArrayList<>();
        if (base.equals("@e")) {
            result.addAll(Bukkit.getWorlds().get(0).getEntities());
        } else {
            try {
                result.addAll(resolvePlayers(selector, source, single));
            } catch (CommandSyntaxException e) {
                try {
                    Entity entity = Bukkit.getEntity(UUID.fromString(base));
                    if (entity != null) result.add(entity);
                } catch (IllegalArgumentException ignored) {
                }
            }
        }
        if (result.isEmpty()) {
            throw NO_ENTITY.create();
        }
        return single ? List.of(result.get(0)) : result;
    }

    private static double coordinate(StringReader reader, double base) throws CommandSyntaxException {
        if (reader.canRead() && reader.peek() == '~') {
            reader.skip();
            if (!reader.canRead() || reader.peek() == ' ') {
                return base;
            }
            return base + reader.readDouble();
        }
        return reader.readDouble();
    }

    @Override
    public ArgumentType<EntitySelectorArgumentResolver> entity() {
        return new VanillaType<>("minecraft:entity", 0x03, null, reader -> {
            String selector = readWord(reader);
            return source -> resolveEntities(selector, source, true);
        }, PrismVanillaArgumentProvider::playerNames);
    }

    @Override
    public ArgumentType<PlayerSelectorArgumentResolver> player() {
        return new VanillaType<>("minecraft:entity", 0x03, null, reader -> {
            String selector = readWord(reader);
            return source -> resolvePlayers(selector, source, true);
        }, PrismVanillaArgumentProvider::playerNames);
    }

    @Override
    public ArgumentType<EntitySelectorArgumentResolver> entities() {
        return new VanillaType<>("minecraft:entity", 0x00, null, reader -> {
            String selector = readWord(reader);
            return source -> resolveEntities(selector, source, false);
        }, PrismVanillaArgumentProvider::playerNames);
    }

    @Override
    public ArgumentType<PlayerSelectorArgumentResolver> players() {
        return new VanillaType<>("minecraft:entity", 0x02, null, reader -> {
            String selector = readWord(reader);
            return source -> resolvePlayers(selector, source, false);
        }, PrismVanillaArgumentProvider::playerNames);
    }

    @Override
    public ArgumentType<io.papermc.paper.command.brigadier.argument.resolvers.PlayerProfileListResolver> playerProfiles() {
        return new VanillaType<>("minecraft:game_profile", -1, null, reader -> {
            String name = readWord(reader);
            return source -> List.of(Bukkit.createProfile(name));
        }, PrismVanillaArgumentProvider::playerNames);
    }

    @Override
    public ArgumentType<BlockPositionResolver> blockPosition() {
        return new VanillaType<>("minecraft:block_pos", -1, null, reader -> {
            String[] parts = new String[3];
            for (int i = 0; i < 3; i++) {
                parts[i] = readWord(reader);
                if (i < 2) reader.skipWhitespace();
            }
            return source -> {
                Location base = source.getLocation();
                double[] values = new double[3];
                double[] bases = {base.getX(), base.getY(), base.getZ()};
                for (int i = 0; i < 3; i++) {
                    values[i] = coordinate(new StringReader(parts[i]), Math.floor(bases[i]));
                }
                return Position.block((int) Math.floor(values[0]), (int) Math.floor(values[1]), (int) Math.floor(values[2]));
            };
        }, source -> List.of("~ ~ ~"));
    }

    @Override
    public ArgumentType<io.papermc.paper.command.brigadier.argument.resolvers.ColumnBlockPositionResolver> columnBlockPosition() {
        throw unsupported("columnBlockPosition");
    }

    @Override
    public ArgumentType<FinePositionResolver> finePosition(boolean centerIntegers) {
        return new VanillaType<>("minecraft:vec3", -1, null, reader -> {
            String[] parts = new String[3];
            for (int i = 0; i < 3; i++) {
                parts[i] = readWord(reader);
                if (i < 2) reader.skipWhitespace();
            }
            return source -> {
                Location base = source.getLocation();
                double[] bases = {base.getX(), base.getY(), base.getZ()};
                double[] values = new double[3];
                for (int i = 0; i < 3; i++) {
                    values[i] = coordinate(new StringReader(parts[i]), bases[i]);
                    if (centerIntegers && !parts[i].contains(".") && !parts[i].startsWith("~")) {
                        values[i] += 0.5;
                    }
                }
                return Position.fine(values[0], values[1], values[2]);
            };
        }, source -> List.of("~ ~ ~"));
    }

    @Override
    public ArgumentType<io.papermc.paper.command.brigadier.argument.resolvers.ColumnFinePositionResolver> columnFinePosition(boolean centerIntegers) {
        throw unsupported("columnFinePosition");
    }

    @Override
    public ArgumentType<io.papermc.paper.command.brigadier.argument.resolvers.RotationResolver> rotation() {
        throw unsupported("rotation");
    }

    @Override
    public ArgumentType<io.papermc.paper.command.brigadier.argument.resolvers.AngleResolver> angle() {
        throw unsupported("angle");
    }

    @Override
    public ArgumentType<AxisSet> axes() {
        throw unsupported("axes");
    }

    @Override
    public ArgumentType<org.bukkit.block.BlockState> blockState() {
        throw unsupported("blockState");
    }

    @Override
    public ArgumentType<io.papermc.paper.command.brigadier.argument.predicate.BlockInWorldPredicate> blockInWorldPredicate() {
        throw unsupported("blockInWorldPredicate");
    }

    @Override
    public ArgumentType<org.bukkit.inventory.ItemStack> itemStack() {
        return new VanillaType<>("minecraft:item_stack", -1, null, reader -> {
            String text = readWord(reader);
            String key = text.contains("[") ? text.substring(0, text.indexOf('[')) : text;
            org.bukkit.Material material = org.bukkit.Material.matchMaterial(key);
            if (material == null) {
                throw new SimpleCommandExceptionType(() -> "Unknown item '" + key + "'").create();
            }
            return new org.bukkit.inventory.ItemStack(material);
        }, source -> List.of("minecraft:stone", "minecraft:diamond"));
    }

    @Override
    public ArgumentType<io.papermc.paper.command.brigadier.argument.predicate.ItemStackPredicate> itemStackPredicate() {
        throw unsupported("itemStackPredicate");
    }

    @Override
    public ArgumentType<NamedTextColor> namedColor() {
        return new VanillaType<>("minecraft:color", -1, null, reader -> {
            String name = readWord(reader);
            NamedTextColor color = NamedTextColor.NAMES.value(name.toLowerCase(Locale.ROOT));
            if (color == null) {
                throw new SimpleCommandExceptionType(() -> "Unknown color '" + name + "'").create();
            }
            return color;
        }, source -> new ArrayList<>(NamedTextColor.NAMES.keys()));
    }

    @Override
    public ArgumentType<net.kyori.adventure.text.format.TextColor> hexColor() {
        return new VanillaType<>("minecraft:hex_color", -1, null, reader -> net.kyori.adventure.text.format.TextColor.color(Integer.parseInt(readWord(reader).replace("#", ""), 16)), null);
    }

    @Override
    public ArgumentType<Component> component() {
        return new VanillaType<>("minecraft:component", -1, null, reader -> {
            String rest = reader.getRemaining();
            reader.setCursor(reader.getTotalLength());
            try {
                return GsonComponentSerializer.gson().deserialize(rest);
            } catch (RuntimeException e) {
                return Component.text(rest);
            }
        }, null);
    }

    @Override
    public ArgumentType<net.kyori.adventure.text.format.Style> style() {
        throw unsupported("style");
    }

    @Override
    public ArgumentType<io.papermc.paper.command.brigadier.argument.SignedMessageResolver> signedMessage() {
        throw unsupported("signedMessage");
    }

    @Override
    public ArgumentType<org.bukkit.scoreboard.DisplaySlot> scoreboardDisplaySlot() {
        return new VanillaType<>("minecraft:scoreboard_slot", -1, null, reader -> {
            String name = readWord(reader);
            return switch (name) {
                case "list" -> org.bukkit.scoreboard.DisplaySlot.PLAYER_LIST;
                case "sidebar" -> org.bukkit.scoreboard.DisplaySlot.SIDEBAR;
                case "belowName" -> org.bukkit.scoreboard.DisplaySlot.BELOW_NAME;
                default -> org.bukkit.scoreboard.DisplaySlot.SIDEBAR;
            };
        }, source -> List.of("list", "sidebar", "belowName"));
    }

    @Override
    public ArgumentType<NamespacedKey> namespacedKey() {
        return new VanillaType<>("minecraft:resource_location", -1, null, reader -> {
            String text = readWord(reader);
            NamespacedKey key = NamespacedKey.fromString(text);
            if (key == null) {
                throw new SimpleCommandExceptionType(() -> "Invalid key '" + text + "'").create();
            }
            return key;
        }, null);
    }

    @Override
    public ArgumentType<Key> key() {
        return new VanillaType<>("minecraft:resource_location", -1, null, reader -> Key.key(readWord(reader)), null);
    }

    @Override
    public ArgumentType<IntegerRangeProvider> integerRange() {
        return new VanillaType<>("minecraft:int_range", -1, null, reader -> {
            String text = readWord(reader);
            com.google.common.collect.Range<Integer> range = parseRange(text, Integer::parseInt);
            return () -> range;
        }, null);
    }

    @Override
    public ArgumentType<DoubleRangeProvider> doubleRange() {
        return new VanillaType<>("minecraft:float_range", -1, null, reader -> {
            String text = readWord(reader);
            com.google.common.collect.Range<Double> range = parseRange(text, Double::parseDouble);
            return () -> range;
        }, null);
    }

    private static <N extends Comparable<N>> com.google.common.collect.Range<N> parseRange(String text, Function<String, N> parse) {
        if (text.contains("..")) {
            String[] parts = text.split("\\.\\.", -1);
            if (parts[0].isEmpty()) return com.google.common.collect.Range.atMost(parse.apply(parts[1]));
            if (parts[1].isEmpty()) return com.google.common.collect.Range.atLeast(parse.apply(parts[0]));
            return com.google.common.collect.Range.closed(parse.apply(parts[0]), parse.apply(parts[1]));
        }
        return com.google.common.collect.Range.singleton(parse.apply(text));
    }

    @Override
    public ArgumentType<World> world() {
        return new VanillaType<>("minecraft:dimension", -1, null, reader -> {
            String name = readWord(reader);
            World world = Bukkit.getWorld(Key.key(name));
            if (world == null) {
                world = Bukkit.getWorld(name.contains(":") ? name.substring(name.indexOf(':') + 1) : name);
            }
            if (world == null) {
                throw new SimpleCommandExceptionType(() -> "Unknown dimension '" + name + "'").create();
            }
            return world;
        }, source -> List.of("minecraft:overworld"));
    }

    @Override
    public ArgumentType<GameMode> gameMode() {
        return new VanillaType<>("minecraft:game_mode", -1, null, reader -> {
            String name = readWord(reader);
            try {
                return GameMode.valueOf(name.toUpperCase(Locale.ROOT));
            } catch (IllegalArgumentException e) {
                throw new SimpleCommandExceptionType(() -> "Unknown game mode '" + name + "'").create();
            }
        }, source -> List.of("survival", "creative", "adventure", "spectator"));
    }

    @Override
    public ArgumentType<org.bukkit.HeightMap> heightMap() {
        return new VanillaType<>("minecraft:heightmap", -1, null, reader -> org.bukkit.HeightMap.valueOf(readWord(reader).toUpperCase(Locale.ROOT)),
                source -> List.of("world_surface", "motion_blocking", "motion_blocking_no_leaves", "ocean_floor"));
    }

    @Override
    public ArgumentType<UUID> uuid() {
        return new VanillaType<>("minecraft:uuid", -1, null, reader -> UUID.fromString(readWord(reader)), null);
    }

    @Override
    public ArgumentType<org.bukkit.scoreboard.Criteria> objectiveCriteria() {
        return new VanillaType<>("minecraft:objective_criteria", -1, null, reader -> org.bukkit.scoreboard.Criteria.create(readWord(reader)), source -> List.of("dummy"));
    }

    @Override
    public ArgumentType<io.papermc.paper.entity.LookAnchor> entityAnchor() {
        return new VanillaType<>("minecraft:entity_anchor", -1, null, reader -> {
            String name = readWord(reader);
            return name.equals("eyes") ? io.papermc.paper.entity.LookAnchor.EYES : io.papermc.paper.entity.LookAnchor.FEET;
        }, source -> List.of("feet", "eyes"));
    }

    @Override
    public ArgumentType<Integer> time(int minTicks) {
        return new VanillaType<>("minecraft:time", minTicks, null, reader -> {
            String text = readWord(reader);
            char unit = text.charAt(text.length() - 1);
            if (Character.isDigit(unit)) {
                return Integer.parseInt(text);
            }
            int value = Integer.parseInt(text.substring(0, text.length() - 1));
            return switch (unit) {
                case 's' -> value * 20;
                case 'd' -> value * 24000;
                default -> value;
            };
        }, source -> List.of("0t", "0s", "0d"));
    }

    @Override
    public ArgumentType<org.bukkit.block.structure.Mirror> templateMirror() {
        return new VanillaType<>("minecraft:template_mirror", -1, null, reader -> org.bukkit.block.structure.Mirror.valueOf(readWord(reader).toUpperCase(Locale.ROOT)),
                source -> List.of("none", "front_back", "left_right"));
    }

    @Override
    public ArgumentType<org.bukkit.block.structure.StructureRotation> templateRotation() {
        return new VanillaType<>("minecraft:template_rotation", -1, null, reader -> org.bukkit.block.structure.StructureRotation.valueOf(readWord(reader).toUpperCase(Locale.ROOT)),
                source -> List.of("none", "clockwise_90", "180", "counterclockwise_90"));
    }

    @Override
    public <T> ArgumentType<io.papermc.paper.registry.TypedKey<T>> resourceKey(io.papermc.paper.registry.RegistryKey<T> registryKey) {
        return new VanillaType<>("minecraft:resource_key", -1, registryKey.key().asString(),
                reader -> io.papermc.paper.registry.TypedKey.create(registryKey, Key.key(readWord(reader))), null);
    }

    @Override
    public <T> ArgumentType<T> resource(io.papermc.paper.registry.RegistryKey<T> registryKey) {
        return new VanillaType<>("minecraft:resource", -1, registryKey.key().asString(), reader -> {
            String text = readWord(reader);
            NamespacedKey key = NamespacedKey.fromString(text);
            @SuppressWarnings("unchecked")
            T value = key == null ? null : (T) io.papermc.paper.registry.RegistryAccess.registryAccess().getRegistry(castKey(registryKey)).get(key);
            if (value == null) {
                throw new SimpleCommandExceptionType(() -> "Unknown " + registryKey.key().value() + " '" + text + "'").create();
            }
            return value;
        }, null);
    }

    @SuppressWarnings("unchecked")
    private static <T> io.papermc.paper.registry.RegistryKey<? extends org.bukkit.Keyed> castKey(io.papermc.paper.registry.RegistryKey<T> key) {
        return (io.papermc.paper.registry.RegistryKey<? extends org.bukkit.Keyed>) key;
    }

    private static UnsupportedOperationException unsupported(String what) {
        return new UnsupportedOperationException("CraftPP does not implement ArgumentTypes." + what + " yet");
    }

    static BlockPosition blockPosition(int x, int y, int z) {
        return Position.block(x, y, z);
    }

    static FinePosition finePosition(double x, double y, double z) {
        return Position.fine(x, y, z);
    }
}
