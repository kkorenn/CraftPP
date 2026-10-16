package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import org.bukkit.NamespacedKey;
import org.bukkit.persistence.PersistentDataContainer;

import java.util.Base64;
import java.util.Map;

/**
 * PersistentDataContainer <-> JSON. Values are stored in their primitive
 * form, tagged with the primitive type so they read back exactly.
 * PrismMC keeps the JSON in its world store for players and the world.
 */
final class Persistence {
    private Persistence() {
    }

    static JsonObject toJson(PdcImpl container) {
        JsonObject json = new JsonObject();
        for (Map.Entry<NamespacedKey, Object> entry : container.rawEntries()) {
            JsonObject slot = new JsonObject();
            Object value = entry.getValue();
            switch (value) {
                case Byte b -> { slot.addProperty("t", "b"); slot.addProperty("v", b); }
                case Short s -> { slot.addProperty("t", "s"); slot.addProperty("v", s); }
                case Integer i -> { slot.addProperty("t", "i"); slot.addProperty("v", i); }
                case Long l -> { slot.addProperty("t", "l"); slot.addProperty("v", l); }
                case Float f -> { slot.addProperty("t", "f"); slot.addProperty("v", f); }
                case Double d -> { slot.addProperty("t", "d"); slot.addProperty("v", d); }
                case String str -> { slot.addProperty("t", "str"); slot.addProperty("v", str); }
                case byte[] bytes -> { slot.addProperty("t", "ba"); slot.addProperty("v", Base64.getEncoder().encodeToString(bytes)); }
                case int[] ints -> { slot.addProperty("t", "ia"); JsonArray array = new JsonArray(); for (int i : ints) array.add(i); slot.add("v", array); }
                case long[] longs -> { slot.addProperty("t", "la"); JsonArray array = new JsonArray(); for (long l : longs) array.add(l); slot.add("v", array); }
                case PdcImpl nested -> { slot.addProperty("t", "c"); slot.add("v", toJson(nested)); }
                case PersistentDataContainer[] containers -> {
                    slot.addProperty("t", "ca");
                    JsonArray array = new JsonArray();
                    for (PersistentDataContainer nested : containers) {
                        array.add(nested instanceof PdcImpl impl ? toJson(impl) : new JsonObject());
                    }
                    slot.add("v", array);
                }
                default -> { continue; }
            }
            json.add(entry.getKey().asString(), slot);
        }
        return json;
    }

    static String serialize(PdcImpl container) {
        return container.isEmpty() ? null : toJson(container).toString();
    }

    static void fromJson(JsonObject json, PdcImpl into) {
        if (json == null) {
            return;
        }
        for (Map.Entry<String, JsonElement> entry : json.entrySet()) {
            NamespacedKey key = NamespacedKey.fromString(entry.getKey());
            if (key == null || !entry.getValue().isJsonObject()) {
                continue;
            }
            JsonObject slot = entry.getValue().getAsJsonObject();
            JsonElement v = slot.get("v");
            Object value = switch (slot.get("t").getAsString()) {
                case "b" -> v.getAsByte();
                case "s" -> v.getAsShort();
                case "i" -> v.getAsInt();
                case "l" -> v.getAsLong();
                case "f" -> v.getAsFloat();
                case "d" -> v.getAsDouble();
                case "str" -> v.getAsString();
                case "ba" -> Base64.getDecoder().decode(v.getAsString());
                case "ia" -> { JsonArray array = v.getAsJsonArray(); int[] ints = new int[array.size()]; for (int i = 0; i < ints.length; i++) ints[i] = array.get(i).getAsInt(); yield ints; }
                case "la" -> { JsonArray array = v.getAsJsonArray(); long[] longs = new long[array.size()]; for (int i = 0; i < longs.length; i++) longs[i] = array.get(i).getAsLong(); yield longs; }
                case "c" -> { PdcImpl nested = new PdcImpl(); fromJson(v.getAsJsonObject(), nested); yield nested; }
                case "ca" -> {
                    JsonArray array = v.getAsJsonArray();
                    PersistentDataContainer[] containers = new PersistentDataContainer[array.size()];
                    for (int i = 0; i < containers.length; i++) {
                        PdcImpl nested = new PdcImpl();
                        fromJson(array.get(i).getAsJsonObject(), nested);
                        containers[i] = nested;
                    }
                    yield containers;
                }
                default -> null;
            };
            if (value != null) {
                into.putRaw(key, value);
            }
        }
    }

    static void restore(String serialized, PdcImpl into) {
        if (serialized == null || serialized.isEmpty()) {
            return;
        }
        try {
            fromJson(JsonParser.parseString(serialized).getAsJsonObject(), into);
        } catch (RuntimeException ignored) {
        }
    }
}
