package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.mojang.brigadier.CommandDispatcher;
import com.mojang.brigadier.ParseResults;
import com.mojang.brigadier.arguments.ArgumentType;
import com.mojang.brigadier.arguments.BoolArgumentType;
import com.mojang.brigadier.arguments.DoubleArgumentType;
import com.mojang.brigadier.arguments.FloatArgumentType;
import com.mojang.brigadier.arguments.IntegerArgumentType;
import com.mojang.brigadier.arguments.LongArgumentType;
import com.mojang.brigadier.arguments.StringArgumentType;
import com.mojang.brigadier.builder.LiteralArgumentBuilder;
import com.mojang.brigadier.builder.RequiredArgumentBuilder;
import com.mojang.brigadier.exceptions.CommandSyntaxException;
import com.mojang.brigadier.suggestion.Suggestions;
import com.mojang.brigadier.tree.ArgumentCommandNode;
import com.mojang.brigadier.tree.CommandNode;
import com.mojang.brigadier.tree.LiteralCommandNode;
import io.papermc.paper.command.brigadier.BasicCommand;
import io.papermc.paper.command.brigadier.CommandRegistrationFlag;
import io.papermc.paper.command.brigadier.CommandSourceStack;
import io.papermc.paper.command.brigadier.Commands;
import io.papermc.paper.command.brigadier.argument.CustomArgumentType;
import io.papermc.paper.plugin.configuration.PluginMeta;
import io.papermc.paper.plugin.lifecycle.event.registrar.ReloadableRegistrarEvent;
import org.bukkit.Location;
import org.bukkit.command.CommandSender;
import org.bukkit.entity.Entity;
import org.bukkit.entity.Player;

import java.util.ArrayList;
import java.util.Collection;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletableFuture;

/**
 * Paper's Brigadier command API: a real dispatcher, the Commands registrar
 * plugins reach through LifecycleEvents.COMMANDS, and the node serializer
 * that lets PrismMC declare plugin trees to clients.
 */
final class Brigadier {
    private Brigadier() {
    }

    static CommandDispatcher<CommandSourceStack> DISPATCHER = new CommandDispatcher<>();
    private static final Map<String, String> ROOT_OWNERS = new LinkedHashMap<>();
    private static final Map<String, String> DESCRIPTIONS = new LinkedHashMap<>();

    static boolean hasRoot(String label) {
        return DISPATCHER.getRoot().getChild(label) != null;
    }

    static Set<String> roots() {
        return DISPATCHER.getRoot().getChildren().stream().map(CommandNode::getName).collect(java.util.stream.Collectors.toCollection(LinkedHashSet::new));
    }

    static void clear() {
        DISPATCHER = new CommandDispatcher<>();
        ROOT_OWNERS.clear();
        DESCRIPTIONS.clear();
    }

    static CommandSourceStack source(CommandSender sender) {
        Entity executor = sender instanceof Entity entity ? entity : null;
        Location location = executor instanceof Player player && !player.isOnline() ? Main.server.overworld().getSpawnLocation()
                : executor != null ? executor.getLocation() : Main.server.overworld().getSpawnLocation();
        return new SourceImpl(sender, executor, location);
    }

    /** True when a Brigadier root handled the line. */
    static boolean dispatch(CommandSender sender, String line) {
        String label = line.split(" ", 2)[0];
        if (!hasRoot(label)) {
            return false;
        }
        CommandSourceStack source = source(sender);
        try {
            DISPATCHER.execute(line, source);
        } catch (CommandSyntaxException e) {
            sender.sendMessage("§c" + e.getMessage());
        } catch (RuntimeException e) {
            java.util.logging.Logger.getLogger("").log(java.util.logging.Level.SEVERE,
                    "Unhandled exception executing command '" + label + "' in plugin " + ROOT_OWNERS.getOrDefault(label, "?"), e);
            sender.sendMessage("§cAn unexpected error occurred trying to execute that command");
        }
        return true;
    }

    static JsonObject suggest(CommandSender sender, String line) {
        JsonObject result = new JsonObject();
        JsonArray completions = new JsonArray();
        result.add("completions", completions);
        ParseResults<CommandSourceStack> parse = DISPATCHER.parse(line, source(sender));
        try {
            Suggestions suggestions = DISPATCHER.getCompletionSuggestions(parse).get();
            result.addProperty("start", suggestions.getRange().getStart());
            suggestions.getList().forEach(suggestion -> completions.add(suggestion.getText()));
        } catch (Exception ignored) {
        }
        return result;
    }

    // ----- registrar -----

    static final class CommandsImpl implements Commands {
        private final PluginMeta owner;

        CommandsImpl(PluginMeta owner) {
            this.owner = owner;
        }

        @Override
        public CommandDispatcher<CommandSourceStack> getDispatcher() {
            return DISPATCHER;
        }

        @Override
        public Set<String> register(LiteralCommandNode<CommandSourceStack> node, String description, Collection<String> aliases) {
            return register(owner, node, description, aliases);
        }

        @Override
        public Set<String> register(PluginMeta meta, LiteralCommandNode<CommandSourceStack> node, String description, Collection<String> aliases) {
            return registerWithFlags(meta, node, description, aliases, Set.of());
        }

        @Override
        public Set<String> registerWithFlags(PluginMeta meta, LiteralCommandNode<CommandSourceStack> node, String description, Collection<String> aliases, Set<CommandRegistrationFlag> flags) {
            Set<String> registered = new LinkedHashSet<>();
            String pluginName = meta == null ? "unknown" : meta.getName().toLowerCase(Locale.ROOT);
            List<String> labels = new ArrayList<>();
            labels.add(node.getLiteral());
            labels.addAll(aliases);
            for (String label : labels) {
                for (String candidate : new String[] {label, pluginName + ":" + label}) {
                    if (hasRoot(candidate)) {
                        continue;
                    }
                    LiteralCommandNode<CommandSourceStack> copy = candidate.equals(node.getLiteral()) ? node : alias(node, candidate);
                    DISPATCHER.getRoot().addChild(copy);
                    ROOT_OWNERS.put(candidate, meta == null ? "?" : meta.getDisplayName());
                    if (description != null) {
                        DESCRIPTIONS.put(candidate, description);
                    }
                    registered.add(candidate);
                }
            }
            return registered;
        }

        private static LiteralCommandNode<CommandSourceStack> alias(LiteralCommandNode<CommandSourceStack> node, String label) {
            LiteralArgumentBuilder<CommandSourceStack> builder = LiteralArgumentBuilder.<CommandSourceStack>literal(label)
                    .requires(node.getRequirement());
            if (node.getCommand() != null) {
                builder.executes(node.getCommand());
            }
            for (CommandNode<CommandSourceStack> child : node.getChildren()) {
                builder.then(child);
            }
            if (node.getRedirect() != null) {
                builder.redirect(node.getRedirect());
            }
            return builder.build();
        }

        @Override
        public Set<String> register(String label, String description, Collection<String> aliases, BasicCommand command) {
            return register(owner, label, description, aliases, command);
        }

        @Override
        public Set<String> register(PluginMeta meta, String label, String description, Collection<String> aliases, BasicCommand command) {
            LiteralArgumentBuilder<CommandSourceStack> builder = LiteralArgumentBuilder.<CommandSourceStack>literal(label)
                    .requires(source -> command.canUse(source.getSender()))
                    .executes(context -> {
                        command.execute(context.getSource(), new String[0]);
                        return 1;
                    });
            RequiredArgumentBuilder<CommandSourceStack, String> args = RequiredArgumentBuilder.<CommandSourceStack, String>argument("args", StringArgumentType.greedyString())
                    .suggests((context, suggestionsBuilder) -> {
                        String remaining = suggestionsBuilder.getRemaining();
                        String[] parts = remaining.isEmpty() ? new String[] {""} : remaining.split(" ", -1);
                        int start = suggestionsBuilder.getStart() + remaining.lastIndexOf(' ') + 1;
                        var offset = suggestionsBuilder.createOffset(start);
                        for (String suggestion : command.suggest(context.getSource(), parts)) {
                            offset.suggest(suggestion);
                        }
                        return offset.buildFuture();
                    })
                    .executes(context -> {
                        String raw = StringArgumentType.getString(context, "args");
                        command.execute(context.getSource(), raw.isEmpty() ? new String[0] : raw.split(" "));
                        return 1;
                    });
            return register(meta, builder.then(args).build(), description, aliases);
        }
    }

    /** The ReloadableRegistrarEvent<Commands> handed to lifecycle handlers. */
    static Object commandsEvent(PluginMeta owner, ReloadableRegistrarEvent.Cause cause) {
        CommandsImpl registrar = new CommandsImpl(owner);
        return ProxyBase.create(ReloadableRegistrarEvent.class, new Object() {
            public Commands registrar() {
                return registrar;
            }

            public ReloadableRegistrarEvent.Cause cause() {
                return cause;
            }

            @Override
            public String toString() {
                return "CommandsLifecycleEvent{" + cause + "}";
            }
        });
    }

    // ----- source stack -----

    static final class SourceImpl implements CommandSourceStack {
        private final CommandSender sender;
        private final Entity executor;
        private final Location location;

        SourceImpl(CommandSender sender, Entity executor, Location location) {
            this.sender = sender;
            this.executor = executor;
            this.location = location;
        }

        @Override
        public Location getLocation() {
            return location.clone();
        }

        @Override
        public CommandSender getSender() {
            return sender;
        }

        @Override
        public Entity getExecutor() {
            return executor;
        }

        @Override
        public Player getPlayerOrThrow() throws CommandSyntaxException {
            if (executor instanceof Player player) {
                return player;
            }
            throw new com.mojang.brigadier.exceptions.SimpleCommandExceptionType(() -> "A player is required to run this command here").create();
        }

        @Override
        public Entity getEntityOrThrow() throws CommandSyntaxException {
            if (executor != null) {
                return executor;
            }
            throw new com.mojang.brigadier.exceptions.SimpleCommandExceptionType(() -> "An entity is required to run this command here").create();
        }

        @Override
        public CommandSourceStack withLocation(Location newLocation) {
            return new SourceImpl(sender, executor, newLocation);
        }

        @Override
        public CommandSourceStack withExecutor(Entity newExecutor) {
            return new SourceImpl(sender, newExecutor, location);
        }
    }

    // ----- client tree export -----

    /** Serialises every Brigadier root into the node list PrismMC appends to the client command tree. */
    static JsonArray exportTree(CommandSender viewer) {
        JsonArray roots = new JsonArray();
        CommandSourceStack source = source(viewer);
        for (CommandNode<CommandSourceStack> root : DISPATCHER.getRoot().getChildren()) {
            if (!root.canUse(source)) {
                continue;
            }
            JsonArray nodes = new JsonArray();
            Map<CommandNode<CommandSourceStack>, Integer> indices = new IdentityHashMap<>();
            serialize(root, nodes, indices, source, 0);
            JsonObject entry = new JsonObject();
            entry.addProperty("root", root.getName());
            entry.add("nodes", nodes);
            roots.add(entry);
        }
        return roots;
    }

    private static int serialize(CommandNode<CommandSourceStack> node, JsonArray nodes, Map<CommandNode<CommandSourceStack>, Integer> indices, CommandSourceStack source, int depth) {
        Integer existing = indices.get(node);
        if (existing != null) {
            return existing;
        }
        JsonObject json = new JsonObject();
        int index = nodes.size();
        nodes.add(json);
        indices.put(node, index);
        json.addProperty("name", node.getName());
        json.addProperty("executable", node.getCommand() != null);
        if (node instanceof ArgumentCommandNode<CommandSourceStack, ?> argument) {
            json.addProperty("type", "argument");
            describeParser(argument.getType(), json);
            if (argument.getCustomSuggestions() != null) {
                json.addProperty("suggestions", "minecraft:ask_server");
            }
        } else {
            json.addProperty("type", "literal");
        }
        JsonArray children = new JsonArray();
        json.add("children", children);
        if (depth < 32) {
            for (CommandNode<CommandSourceStack> child : node.getChildren()) {
                if (child.canUse(source)) {
                    children.add(serialize(child, nodes, indices, source, depth + 1));
                }
            }
        }
        if (node.getRedirect() != null && indices.containsKey(node.getRedirect())) {
            json.addProperty("redirect", indices.get(node.getRedirect()));
        }
        return index;
    }

    /** Parser id + properties in the encoding PrismMC writes; unknown types fall back to greedy strings with server suggestions. */
    private static void describeParser(ArgumentType<?> type, JsonObject json) {
        describeParserKey(type, json);
        if (json.has("parser")) {
            json.addProperty("parserId", Registries.keysOf("minecraft:command_argument_type").indexOf(json.get("parser").getAsString()));
        }
    }

    private static void describeParserKey(ArgumentType<?> type, JsonObject json) {
        switch (type) {
            case BoolArgumentType ignored -> json.addProperty("parser", "brigadier:bool");
            case IntegerArgumentType integer -> {
                json.addProperty("parser", "brigadier:integer");
                if (integer.getMinimum() != Integer.MIN_VALUE) json.addProperty("min", integer.getMinimum());
                if (integer.getMaximum() != Integer.MAX_VALUE) json.addProperty("max", integer.getMaximum());
            }
            case LongArgumentType number -> {
                json.addProperty("parser", "brigadier:long");
                if (number.getMinimum() != Long.MIN_VALUE) json.addProperty("min", number.getMinimum());
                if (number.getMaximum() != Long.MAX_VALUE) json.addProperty("max", number.getMaximum());
            }
            case FloatArgumentType number -> {
                json.addProperty("parser", "brigadier:float");
                if (number.getMinimum() != -Float.MAX_VALUE) json.addProperty("min", number.getMinimum());
                if (number.getMaximum() != Float.MAX_VALUE) json.addProperty("max", number.getMaximum());
            }
            case DoubleArgumentType number -> {
                json.addProperty("parser", "brigadier:double");
                if (number.getMinimum() != -Double.MAX_VALUE) json.addProperty("min", number.getMinimum());
                if (number.getMaximum() != Double.MAX_VALUE) json.addProperty("max", number.getMaximum());
            }
            case StringArgumentType string -> {
                json.addProperty("parser", "brigadier:string");
                json.addProperty("stringType", string.getType().ordinal());
            }
            case io.papermc.paper.command.brigadier.argument.PrismVanillaArgumentProvider.VanillaType<?> vanilla -> {
                json.addProperty("parser", vanilla.parserKey());
                if (vanilla.flags() >= 0) json.addProperty("flags", vanilla.flags());
                if (vanilla.registry() != null) json.addProperty("registry", vanilla.registry());
                json.addProperty("suggestions", "minecraft:ask_server");
            }
            case CustomArgumentType<?, ?> custom -> {
                describeParserKey(custom.getNativeType(), json);
                json.addProperty("suggestions", "minecraft:ask_server");
            }
            default -> {
                json.addProperty("parser", "brigadier:string");
                json.addProperty("stringType", 2);
                json.addProperty("suggestions", "minecraft:ask_server");
            }
        }
    }

    static Map<String, String> descriptions() {
        return DESCRIPTIONS;
    }

    static CompletableFuture<Suggestions> noSuggestions() {
        return Suggestions.empty();
    }
}
