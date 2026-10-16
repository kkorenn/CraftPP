package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * Per-block and per-item facts from the official 26.3 jar
 * (tools/jar-truth/MaterialTruth.java): what Material delegates to BlockType
 * and ItemType. Loaded once from material-properties-26.3.json.
 */
final class MaterialProperties {
    record BlockInfo(boolean air, boolean solid, boolean occluding, float hardness, float blastResistance, float friction,
                     boolean gravity, boolean flammable, boolean collidable, int light, boolean blockEntity) {
    }

    record ItemInfo(int maxStackSize, int maxDamage, boolean edible, boolean record, int fuel, float compost, String slot, String remainder, String rarity) {
    }

    private static final Map<String, BlockInfo> BLOCKS = new HashMap<>();
    private static final Map<String, ItemInfo> ITEMS = new HashMap<>();
    private static final BlockInfo UNKNOWN_BLOCK = new BlockInfo(false, true, true, 1.0f, 1.0f, 0.6f, false, false, true, 0, false);
    private static final ItemInfo UNKNOWN_ITEM = new ItemInfo(64, 0, false, false, 0, 0f, "", "", "common");
    private static boolean loaded;

    private MaterialProperties() {
    }

    private static synchronized void load() {
        if (loaded) {
            return;
        }
        loaded = true;
        try (InputStreamReader reader = new InputStreamReader(
                MaterialProperties.class.getClassLoader().getResourceAsStream("material-properties-26.3.json"), StandardCharsets.UTF_8)) {
            JsonObject root = JsonParser.parseReader(reader).getAsJsonObject();
            for (Map.Entry<String, JsonElement> entry : root.getAsJsonObject("blocks").entrySet()) {
                JsonArray v = entry.getValue().getAsJsonArray();
                BLOCKS.put(entry.getKey(), new BlockInfo(v.get(0).getAsInt() == 1, v.get(1).getAsInt() == 1, v.get(2).getAsInt() == 1,
                        v.get(3).getAsFloat(), v.get(4).getAsFloat(), v.get(5).getAsFloat(), v.get(6).getAsInt() == 1, v.get(7).getAsInt() == 1,
                        v.get(8).getAsInt() == 1, v.get(9).getAsInt(), v.get(10).getAsInt() == 1));
            }
            for (Map.Entry<String, JsonElement> entry : root.getAsJsonObject("items").entrySet()) {
                JsonObject v = entry.getValue().getAsJsonObject();
                ITEMS.put(entry.getKey(), new ItemInfo(v.get("stack").getAsInt(), v.get("damage").getAsInt(), v.get("edible").getAsInt() == 1,
                        v.get("record").getAsInt() == 1, v.get("fuel").getAsInt(), v.get("compost").getAsFloat(), v.get("slot").getAsString(),
                        v.get("remainder").getAsString(), v.get("rarity").getAsString()));
            }
        } catch (Exception e) {
            Main.logThrowable(java.util.logging.Logger.getLogger(""), "Could not read material-properties-26.3.json", e);
        }
    }

    static BlockInfo block(String key) {
        load();
        return BLOCKS.getOrDefault(key, UNKNOWN_BLOCK);
    }

    static boolean isBlock(String key) {
        load();
        return BLOCKS.containsKey(key);
    }

    static ItemInfo item(String key) {
        load();
        return ITEMS.getOrDefault(key, UNKNOWN_ITEM);
    }

    static boolean isItem(String key) {
        load();
        return ITEMS.containsKey(key);
    }
}
