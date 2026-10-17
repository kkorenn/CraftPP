package prism.bridge;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import org.bukkit.Location;
import org.bukkit.NamespacedKey;
import org.bukkit.damage.DamageEffect;
import org.bukkit.damage.DamageScaling;
import org.bukkit.damage.DamageSource;
import org.bukkit.damage.DamageType;
import org.bukkit.damage.DeathMessageType;
import org.bukkit.entity.Entity;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import java.util.function.Consumer;

/** Damage types (vanilla data/vanilla/minecraft/damage_type/*.json), damage effects and DamageSource builders. */
final class Damage {
    private Damage() {
    }

    static final class TypeImpl implements DamageType {
        private final NamespacedKey key;
        private final JsonObject json;

        TypeImpl(NamespacedKey key) {
            this.key = key;
            JsonObject loaded = new JsonObject();
            Path file = Path.of("data/vanilla", key.getNamespace(), "damage_type", key.getKey() + ".json");
            try {
                loaded = JsonParser.parseString(Files.readString(file)).getAsJsonObject();
            } catch (Exception missing) {
                // datapack-less run (tools/tests): vanilla defaults below
            }
            json = loaded;
        }

        private String text(String field, String fallback) {
            return json.has(field) ? json.get(field).getAsString() : fallback;
        }

        @Override
        public NamespacedKey getKey() {
            return key;
        }

        @Override
        public String getTranslationKey() {
            return "death.attack." + text("message_id", key.getKey());
        }

        public String translationKey() {
            return getTranslationKey();
        }

        @Override
        public DamageScaling getDamageScaling() {
            return DamageScaling.valueOf(text("scaling", "never").toUpperCase(Locale.ROOT));
        }

        @Override
        public DamageEffect getDamageEffect() {
            return effect(text("effects", "hurt"));
        }

        @Override
        public DeathMessageType getDeathMessageType() {
            return DeathMessageType.valueOf(text("death_message_type", "default").toUpperCase(Locale.ROOT));
        }

        @Override
        public float getExhaustion() {
            return json.has("exhaustion") ? json.get("exhaustion").getAsFloat() : 0f;
        }

        @Override
        public boolean equals(Object other) {
            return other instanceof TypeImpl type && type.key.equals(key);
        }

        @Override
        public int hashCode() {
            return key.hashCode();
        }

        @Override
        public String toString() {
            return "CraftDamageType{" + key + "}";
        }
    }

    /** DamageEffects: hurt sound per effect (DamageEffects enum in vanilla). */
    static DamageEffect effect(String key) {
        String sound = switch (key) {
            case "thorns" -> "enchant.thorns.hit";
            case "drowning" -> "entity.player.hurt_drown";
            case "burning" -> "entity.player.hurt_on_fire";
            case "poking" -> "entity.player.hurt_sweet_berry_bush";
            case "freezing" -> "entity.player.hurt_freeze";
            default -> "entity.player.hurt";
        };
        org.bukkit.Sound resolved = org.bukkit.Registry.SOUNDS.get(NamespacedKey.minecraft(sound));
        return () -> resolved;
    }

    static final class Builder implements DamageSource.Builder {
        private final DamageType type;
        private Entity causing, direct;
        private Location location;

        Builder(DamageType type) {
            this.type = java.util.Objects.requireNonNull(type, "DamageType cannot be null");
        }

        @Override
        public DamageSource.Builder withCausingEntity(Entity entity) {
            causing = java.util.Objects.requireNonNull(entity, "Entity cannot be null");
            return this;
        }

        @Override
        public DamageSource.Builder withDirectEntity(Entity entity) {
            direct = java.util.Objects.requireNonNull(entity, "Entity cannot be null");
            return this;
        }

        @Override
        public DamageSource.Builder withDamageLocation(Location location) {
            this.location = java.util.Objects.requireNonNull(location, "Location cannot be null").clone();
            return this;
        }

        @Override
        public DamageSource.Builder withDamageContext(Consumer<net.kyori.adventure.pointer.Pointers.Builder> context) {
            return this;
        }

        @Override
        public DamageSource build() {
            if (location != null && direct != null) throw new IllegalArgumentException("Cannot build a DamageSource with both a location and a direct entity");
            return new SourceImpl(type, causing == null ? direct : causing, direct == null ? causing : direct, location);
        }
    }

    record SourceImpl(DamageType type, Entity causing, Entity direct, Location location) implements DamageSource {
        @Override
        public DamageType getDamageType() {
            return type;
        }

        @Override
        public Entity getCausingEntity() {
            return causing;
        }

        @Override
        public Entity getDirectEntity() {
            return direct;
        }

        @Override
        public Location getDamageLocation() {
            return location == null ? null : location.clone();
        }

        @Override
        public Location getSourceLocation() {
            return location != null ? location.clone() : direct != null ? direct.getLocation() : null;
        }

        @Override
        public boolean isIndirect() {
            return causing != direct;
        }

        @Override
        public float getFoodExhaustion() {
            return type.getExhaustion();
        }

        @Override
        public boolean scalesWithDifficulty() {
            return switch (type.getDamageScaling()) {
                case NEVER -> false;
                case WHEN_CAUSED_BY_LIVING_NON_PLAYER -> causing instanceof org.bukkit.entity.LivingEntity && !(causing instanceof org.bukkit.entity.Player);
                case ALWAYS -> true;
            };
        }

        @Override
        public net.kyori.adventure.pointer.Pointers getDamageContext() {
            return net.kyori.adventure.pointer.Pointers.empty();
        }
    }
}
