package prism.bridge;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import org.bukkit.GameRule;
import org.bukkit.NamespacedKey;

import java.util.Objects;
import java.util.function.Function;

/** Registry identities and legacy rule conversions; values live in C++ game state. */
final class GameRulesBridge {
    private GameRulesBridge() {}

    static GameRule<?> create(NamespacedKey key) {
        JsonElement info = Main.rpc.api("world.gameRuleInfo", "rule", key.asString());
        if (info == null || info.isJsonNull()) return null;
        JsonObject data = info.getAsJsonObject();
        if (data.get("integer").getAsBoolean())
            return new Rule<>(key, Integer.class, data.get("default").getAsInt(), data.get("min").getAsInt(), data.get("max").getAsInt());
        return new Rule<>(key, Boolean.class, data.get("default").getAsBoolean(), 0, 1);
    }

    static <M, L> GameRule<L> legacy(GameRule<M> modern, Function<L, M> toModern, Function<M, L> toLegacy, Class<L> type) {
        return new Legacy<>(Objects.requireNonNull(modern, "modern rule"), toModern, toLegacy, type);
    }

    @SuppressWarnings("unchecked")
    static <T> T get(WorldImpl world, GameRule<T> rule) {
        Objects.requireNonNull(rule, "rule");
        if (rule instanceof Legacy<?, ?> legacy) return (T) readLegacy(world, legacy);
        String value = world.getGameRuleValue(rule.getKey().asString());
        if (value == null) return null;
        if (rule.getType() == Boolean.class) return (T) Boolean.valueOf(value);
        return (T) Integer.valueOf(value);
    }

    private static <M, L> L readLegacy(WorldImpl world, Legacy<M, L> rule) {
        M value = get(world, rule.modern);
        return value == null ? null : rule.fromModern.apply(value);
    }

    @SuppressWarnings("unchecked")
    static <T> boolean set(WorldImpl world, GameRule<T> rule, T value) {
        Objects.requireNonNull(rule, "rule");
        Objects.requireNonNull(value, "value");
        if (!rule.getType().isInstance(value)) throw new IllegalArgumentException("Wrong game rule value type");
        if (rule instanceof Legacy<?, ?> legacy) return writeLegacy(world, (Legacy<Object, T>) legacy, value);
        return world.setGameRuleValue(rule.getKey().asString(), value.toString());
    }

    private static <M, L> boolean writeLegacy(WorldImpl world, Legacy<M, L> rule, L value) {
        return set(world, rule.modern, rule.toModern.apply(value));
    }

    static boolean valid(GameRule<?> rule, String value) {
        if (!(rule instanceof Rule<?> modern) || value == null) return false;
        if (modern.type == Boolean.class) return value.equals("true") || value.equals("false");
        if (!value.matches("-?[0-9]+")) return false;
        try {
            int number = Integer.parseInt(value);
            return number >= modern.min && number <= modern.max;
        } catch (NumberFormatException invalid) {
            return false;
        }
    }

    private static final class Rule<T> extends GameRule<T> {
        private final NamespacedKey key;
        private final Class<T> type;
        private final T defaultValue;
        private final int min, max;

        Rule(NamespacedKey key, Class<T> type, T defaultValue, int min, int max) {
            this.key = key;
            this.type = type;
            this.defaultValue = defaultValue;
            this.min = min;
            this.max = max;
        }

        @Override public NamespacedKey getKey() { return key; }
        @Override public String getName() { return key.getKey(); }
        @Override public Class<T> getType() { return type; }
        @Override public T getDefaultValue() { return defaultValue; }
        @Override public String translationKey() { return "gamerule." + key.getNamespace() + "." + key.getKey(); }
        @Override public String toString() { return key.asString(); }
    }

    private static final class Legacy<M, L> extends GameRule<L> {
        private final GameRule<M> modern;
        private final Function<L, M> toModern;
        private final Function<M, L> fromModern;
        private final Class<L> type;

        Legacy(GameRule<M> modern, Function<L, M> toModern, Function<M, L> fromModern, Class<L> type) {
            this.modern = modern;
            this.toModern = Objects.requireNonNull(toModern);
            this.fromModern = Objects.requireNonNull(fromModern);
            this.type = Objects.requireNonNull(type);
        }

        @Override public NamespacedKey getKey() { return modern.getKey(); }
        @Override public String getName() { return modern.getName(); }
        @Override public Class<L> getType() { return type; }
        @Override public L getDefaultValue() { return fromModern.apply(modern.getDefaultValue()); }
        @Override public String translationKey() { return modern.translationKey(); }
    }
}
