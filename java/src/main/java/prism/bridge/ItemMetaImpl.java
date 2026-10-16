package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import net.kyori.adventure.text.Component;
import org.bukkit.NamespacedKey;
import org.bukkit.enchantments.Enchantment;
import org.bukkit.inventory.ItemFlag;
import org.bukkit.inventory.meta.Damageable;
import org.bukkit.inventory.meta.ItemMeta;
import org.bukkit.persistence.PersistentDataContainer;

import java.util.ArrayList;
import java.util.Collections;
import java.util.EnumSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Backing for ItemMeta (+ Damageable) proxies: the data components PrismMC
 * can send to clients - names, lore, enchantments, damage, unbreakable,
 * custom model data, glint, dye colour, tooltip flags - plus a persistent
 * data container that round-trips as JSON.
 */
public final class ItemMetaImpl implements ProxyBase.Backing, Cloneable {
    // 26.3 data component ids hidden by each ItemFlag (tooltip_display).
    private static final Map<ItemFlag, Integer> FLAG_COMPONENTS = Map.of(
            ItemFlag.HIDE_ENCHANTS, 13, ItemFlag.HIDE_ATTRIBUTES, 16, ItemFlag.HIDE_UNBREAKABLE, 4,
            ItemFlag.HIDE_DYE, 44, ItemFlag.HIDE_ARMOR_TRIM, 26, ItemFlag.HIDE_STORED_ENCHANTS, 20);

    String displayName;
    String itemName;
    List<String> lore = new ArrayList<>();
    final Map<Enchantment, Integer> enchants = new LinkedHashMap<>();
    final Set<ItemFlag> flags = EnumSet.noneOf(ItemFlag.class);
    boolean unbreakable;
    boolean hideTooltip;
    Integer customModelData;
    Boolean glint;
    Integer color;
    Integer maxStackSize;
    int damage;
    Integer maxDamage;
    PdcImpl persistentData = new PdcImpl();
    private ItemMeta proxy;

    @Override
    public void attachProxy(Object proxy) {
        this.proxy = (ItemMeta) proxy;
    }

    ItemMeta proxy() {
        return proxy;
    }

    static ItemMeta create() {
        ItemMetaImpl backing = new ItemMetaImpl();
        return ProxyBase.create(ItemMeta.class, backing, Damageable.class);
    }

    static ItemMetaImpl unwrap(ItemMeta meta) {
        return ProxyBase.unwrap(meta) instanceof ItemMetaImpl impl ? impl : null;
    }

    boolean isEmpty() {
        return displayName == null && itemName == null && lore.isEmpty() && enchants.isEmpty() && flags.isEmpty()
                && !unbreakable && !hideTooltip && customModelData == null && glint == null && color == null
                && maxStackSize == null && damage == 0 && maxDamage == null && persistentData.isEmpty();
    }

    // ----- JSON exchanged with PrismMC -----

    JsonObject toJson() {
        JsonObject json = new JsonObject();
        if (displayName != null) json.addProperty("name", displayName);
        if (itemName != null) json.addProperty("itemName", itemName);
        if (!lore.isEmpty()) {
            JsonArray lines = new JsonArray();
            lore.forEach(lines::add);
            json.add("lore", lines);
        }
        if (!enchants.isEmpty()) {
            JsonObject map = new JsonObject();
            enchants.forEach((enchantment, level) -> map.addProperty(enchantment.getKey().asString(), level));
            json.add("enchants", map);
        }
        if (damage > 0) json.addProperty("damage", damage);
        if (maxDamage != null) json.addProperty("maxDamage", maxDamage);
        if (maxStackSize != null) json.addProperty("maxStackSize", maxStackSize);
        if (unbreakable) json.addProperty("unbreakable", true);
        if (glint != null) json.addProperty("glint", glint);
        if (color != null) json.addProperty("color", color);
        if (customModelData != null) {
            JsonObject model = new JsonObject();
            JsonArray floats = new JsonArray();
            floats.add(customModelData.floatValue());
            model.add("floats", floats);
            json.add("customModelData", model);
        }
        if (hideTooltip) json.addProperty("hideTooltip", true);
        if (!flags.isEmpty()) {
            JsonArray hidden = new JsonArray();
            JsonArray names = new JsonArray();
            for (ItemFlag flag : flags) {
                Integer component = FLAG_COMPONENTS.get(flag);
                if (component != null) hidden.add(component);
                names.add(flag.name());
            }
            json.add("hidden", hidden);
            json.add("flags", names);
        }
        if (!persistentData.isEmpty()) json.add("pdc", Persistence.toJson(persistentData));
        return json;
    }

    static ItemMetaImpl fromJson(JsonObject json) {
        ItemMetaImpl meta = new ItemMetaImpl();
        if (json == null) {
            return meta;
        }
        if (json.has("name") && !json.get("name").isJsonNull()) meta.displayName = json.get("name").getAsString();
        if (json.has("itemName") && !json.get("itemName").isJsonNull()) meta.itemName = json.get("itemName").getAsString();
        if (json.has("lore")) {
            for (JsonElement line : json.getAsJsonArray("lore")) meta.lore.add(line.getAsString());
        }
        if (json.has("enchants")) {
            for (Map.Entry<String, JsonElement> entry : json.getAsJsonObject("enchants").entrySet()) {
                Enchantment enchantment = Registries.enchantment(entry.getKey());
                if (enchantment != null) meta.enchants.put(enchantment, entry.getValue().getAsInt());
            }
        }
        if (json.has("damage")) meta.damage = json.get("damage").getAsInt();
        if (json.has("maxDamage")) meta.maxDamage = json.get("maxDamage").getAsInt();
        if (json.has("maxStackSize")) meta.maxStackSize = json.get("maxStackSize").getAsInt();
        if (json.has("unbreakable")) meta.unbreakable = json.get("unbreakable").getAsBoolean();
        if (json.has("glint")) meta.glint = json.get("glint").getAsBoolean();
        if (json.has("color")) meta.color = json.get("color").getAsInt();
        if (json.has("hideTooltip")) meta.hideTooltip = json.get("hideTooltip").getAsBoolean();
        if (json.has("customModelData")) {
            JsonArray floats = json.getAsJsonObject("customModelData").getAsJsonArray("floats");
            if (floats != null && !floats.isEmpty()) meta.customModelData = (int) floats.get(0).getAsFloat();
        }
        if (json.has("flags")) {
            for (JsonElement flag : json.getAsJsonArray("flags")) {
                try {
                    meta.flags.add(ItemFlag.valueOf(flag.getAsString()));
                } catch (IllegalArgumentException ignored) {
                }
            }
        }
        if (json.has("pdc")) Persistence.fromJson(json.getAsJsonObject("pdc"), meta.persistentData);
        return meta;
    }

    // ----- ItemMeta -----

    public boolean hasDisplayName() {
        return displayName != null;
    }

    public boolean hasCustomName() {
        return displayName != null;
    }

    public String getDisplayName() {
        return displayName;
    }

    public void setDisplayName(String name) {
        displayName = name;
    }

    public Component displayName() {
        return displayName == null ? null : ServerImpl.LEGACY.deserialize(displayName);
    }

    public void displayName(Component name) {
        displayName = name == null ? null : ServerImpl.LEGACY.serialize(name);
    }

    public Component customName() {
        return displayName();
    }

    public void customName(Component name) {
        displayName(name);
    }

    public boolean hasItemName() {
        return itemName != null;
    }

    public String getItemName() {
        return itemName;
    }

    public void setItemName(String name) {
        itemName = name;
    }

    public Component itemName() {
        return itemName == null ? null : ServerImpl.LEGACY.deserialize(itemName);
    }

    public void itemName(Component name) {
        itemName = name == null ? null : ServerImpl.LEGACY.serialize(name);
    }

    public boolean hasLocalizedName() {
        return false;
    }

    public String getLocalizedName() {
        return null;
    }

    public void setLocalizedName(String name) {
    }

    public boolean hasLore() {
        return !lore.isEmpty();
    }

    public List<String> getLore() {
        return lore.isEmpty() ? null : new ArrayList<>(lore);
    }

    public void setLore(List<String> lines) {
        lore = lines == null ? new ArrayList<>() : new ArrayList<>(lines);
    }

    public List<Component> lore() {
        if (lore.isEmpty()) {
            return null;
        }
        List<Component> components = new ArrayList<>();
        for (String line : lore) components.add(ServerImpl.LEGACY.deserialize(line));
        return components;
    }

    public void lore(List<? extends Component> lines) {
        lore = new ArrayList<>();
        if (lines != null) {
            for (Component line : lines) lore.add(ServerImpl.LEGACY.serialize(line));
        }
    }

    public boolean hasCustomModelData() {
        return customModelData != null;
    }

    public int getCustomModelData() {
        return customModelData == null ? 0 : customModelData;
    }

    public void setCustomModelData(Integer data) {
        customModelData = data;
    }

    public boolean hasEnchants() {
        return !enchants.isEmpty();
    }

    public boolean hasEnchant(Enchantment enchantment) {
        return enchants.containsKey(enchantment);
    }

    public int getEnchantLevel(Enchantment enchantment) {
        return enchants.getOrDefault(enchantment, 0);
    }

    public Map<Enchantment, Integer> getEnchants() {
        return Collections.unmodifiableMap(new LinkedHashMap<>(enchants));
    }

    public boolean addEnchant(Enchantment enchantment, int level, boolean ignoreLevelRestriction) {
        if (!ignoreLevelRestriction && (level < enchantment.getStartLevel() || level > enchantment.getMaxLevel())) {
            return false;
        }
        Integer previous = enchants.put(enchantment, level);
        return previous == null || previous != level;
    }

    public boolean removeEnchant(Enchantment enchantment) {
        return enchants.remove(enchantment) != null;
    }

    public void removeEnchantments() {
        enchants.clear();
    }

    public boolean hasConflictingEnchant(Enchantment enchantment) {
        return false;
    }

    public void addItemFlags(ItemFlag... itemFlags) {
        Collections.addAll(flags, itemFlags);
    }

    public void removeItemFlags(ItemFlag... itemFlags) {
        for (ItemFlag flag : itemFlags) flags.remove(flag);
    }

    public Set<ItemFlag> getItemFlags() {
        return EnumSet.copyOf(flags.isEmpty() ? EnumSet.noneOf(ItemFlag.class) : flags);
    }

    public boolean hasItemFlag(ItemFlag flag) {
        return flags.contains(flag);
    }

    public boolean isHideTooltip() {
        return hideTooltip;
    }

    public void setHideTooltip(boolean hide) {
        hideTooltip = hide;
    }

    public boolean isUnbreakable() {
        return unbreakable;
    }

    public void setUnbreakable(boolean value) {
        unbreakable = value;
    }

    public boolean hasEnchantmentGlintOverride() {
        return glint != null;
    }

    public Boolean getEnchantmentGlintOverride() {
        return glint;
    }

    public void setEnchantmentGlintOverride(Boolean value) {
        glint = value;
    }

    public boolean hasMaxStackSize() {
        return maxStackSize != null;
    }

    public int getMaxStackSize() {
        return maxStackSize == null ? 64 : maxStackSize;
    }

    public void setMaxStackSize(Integer value) {
        maxStackSize = value;
    }

    public boolean hasAttributeModifiers() {
        return false;
    }

    public boolean isFireResistant() {
        return false;
    }

    public void setFireResistant(boolean value) {
    }

    public boolean hasRarity() {
        return false;
    }

    public boolean hasFood() {
        return false;
    }

    public boolean hasTool() {
        return false;
    }

    public boolean hasItemModel() {
        return false;
    }

    public boolean hasTooltipStyle() {
        return false;
    }

    public boolean hasEnchantable() {
        return false;
    }

    public boolean hasUseRemainder() {
        return false;
    }

    public boolean hasUseCooldown() {
        return false;
    }

    public boolean hasEquippable() {
        return false;
    }

    public boolean hasJukeboxPlayable() {
        return false;
    }

    public boolean hasDamageResistant() {
        return false;
    }

    public boolean isGlider() {
        return false;
    }

    public void setGlider(boolean value) {
    }

    public PersistentDataContainer getPersistentDataContainer() {
        return persistentData;
    }

    public String getAsString() {
        return toJson().toString();
    }

    public String getAsComponentString() {
        return getAsString();
    }

    public void setVersion(int version) {
    }

    public Map<String, Object> serialize() {
        Map<String, Object> map = new LinkedHashMap<>();
        map.put("meta-type", "UNSPECIFIC");
        if (displayName != null) map.put("display-name", displayName);
        if (!lore.isEmpty()) map.put("lore", new ArrayList<>(lore));
        if (!enchants.isEmpty()) {
            Map<String, Integer> enchantMap = new LinkedHashMap<>();
            enchants.forEach((enchantment, level) -> enchantMap.put(enchantment.getKey().asString(), level));
            map.put("enchants", enchantMap);
        }
        if (unbreakable) map.put("Unbreakable", true);
        if (damage > 0) map.put("Damage", damage);
        if (customModelData != null) map.put("custom-model-data", customModelData);
        return map;
    }

    // ----- Damageable -----

    public boolean hasDamage() {
        return damage > 0;
    }

    public int getDamage() {
        return damage;
    }

    public void setDamage(int value) {
        damage = Math.max(0, value);
    }

    public boolean hasDamageValue() {
        return damage > 0;
    }

    public void resetDamage() {
        damage = 0;
    }

    public boolean hasMaxDamage() {
        return maxDamage != null;
    }

    public int getMaxDamage() {
        return maxDamage == null ? 0 : maxDamage;
    }

    public void setMaxDamage(Integer value) {
        maxDamage = value;
    }

    // ----- object -----

    public ItemMeta clone() {
        ItemMetaImpl copy = new ItemMetaImpl();
        copy.displayName = displayName;
        copy.itemName = itemName;
        copy.lore = new ArrayList<>(lore);
        copy.enchants.putAll(enchants);
        copy.flags.addAll(flags);
        copy.unbreakable = unbreakable;
        copy.hideTooltip = hideTooltip;
        copy.customModelData = customModelData;
        copy.glint = glint;
        copy.color = color;
        copy.maxStackSize = maxStackSize;
        copy.damage = damage;
        copy.maxDamage = maxDamage;
        persistentData.copyTo(copy.persistentData, true);
        return ProxyBase.create(ItemMeta.class, copy, Damageable.class);
    }

    @Override
    public boolean equals(Object other) {
        return ProxyBase.unwrap(other) instanceof ItemMetaImpl meta && meta.toJson().equals(toJson());
    }

    @Override
    public int hashCode() {
        return toJson().hashCode();
    }

    @Override
    public String toString() {
        return "ItemMeta" + toJson();
    }

    static NamespacedKey keyOf(String text) {
        return NamespacedKey.fromString(text);
    }
}
