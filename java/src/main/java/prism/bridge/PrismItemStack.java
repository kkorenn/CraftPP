package prism.bridge;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import io.papermc.paper.persistence.PersistentDataContainerView;
import net.kyori.adventure.text.Component;
import org.bukkit.Material;
import org.bukkit.enchantments.Enchantment;
import org.bukkit.inventory.ItemStack;
import org.bukkit.inventory.meta.ItemMeta;
import org.bukkit.persistence.PersistentDataContainer;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;

/**
 * The server-side ItemStack the API delegates to: material, count, and an
 * ItemMetaImpl holding the data components PrismMC understands.
 */
public final class PrismItemStack extends ItemStack {
    private Material type = Material.AIR;
    private int amount = 1;
    private ItemMetaImpl meta = new ItemMetaImpl();

    public PrismItemStack() {
        super();
    }

    PrismItemStack(Material type, int amount) {
        super();
        this.type = type == null ? Material.AIR : type;
        this.amount = amount;
    }

    static PrismItemStack of(ItemStack stack) {
        if (stack == null || stack.getType().isAir()) {
            return new PrismItemStack(Material.AIR, 0);
        }
        if (stack instanceof PrismItemStack prism) {
            return prism;
        }
        PrismItemStack copy = new PrismItemStack(stack.getType(), stack.getAmount());
        ItemMetaImpl backing = ItemMetaImpl.unwrap(stack.getItemMeta());
        if (backing != null) {
            copy.meta = ItemMetaImpl.unwrap(backing.clone());
        }
        return copy;
    }

    // ----- JSON exchanged with PrismMC -----

    JsonObject toJson() {
        JsonObject json = new JsonObject();
        json.addProperty("item", type.getKey().asString());
        json.addProperty("count", amount);
        if (!meta.isEmpty()) {
            json.add("meta", meta.toJson());
        }
        return json;
    }

    static PrismItemStack fromJson(JsonElement element) {
        if (element == null || !element.isJsonObject()) {
            return null;
        }
        JsonObject json = element.getAsJsonObject();
        Material material = Material.matchMaterial(json.get("item").getAsString());
        if (material == null || material.isAir()) {
            return null;
        }
        PrismItemStack stack = new PrismItemStack(material, json.has("count") ? json.get("count").getAsInt() : 1);
        if (json.has("meta") && json.get("meta").isJsonObject()) {
            stack.meta = ItemMetaImpl.fromJson(json.getAsJsonObject("meta"));
        }
        return stack;
    }

    static JsonElement jsonOrNull(ItemStack stack) {
        return stack == null || stack.getType().isAir() || stack.getAmount() <= 0 ? com.google.gson.JsonNull.INSTANCE : of(stack).toJson();
    }

    ItemMetaImpl metaBacking() {
        return meta;
    }

    // ----- ItemStack delegate surface -----

    @Override
    public Material getType() {
        return type;
    }

    @Override
    public void setType(Material type) {
        this.type = type == null ? Material.AIR : type;
    }

    @Override
    public ItemStack withType(Material type) {
        PrismItemStack copy = new PrismItemStack(type, amount);
        copy.meta = ItemMetaImpl.unwrap(meta.clone());
        return copy;
    }

    @Override
    public int getAmount() {
        return amount;
    }

    @Override
    public void setAmount(int amount) {
        this.amount = amount;
    }

    @Override
    public boolean isEmpty() {
        return type.isAir() || amount <= 0;
    }

    @Override
    public int getMaxStackSize() {
        return meta.maxStackSize != null ? meta.maxStackSize : type.getMaxStackSize();
    }

    @Override
    public boolean isSimilar(ItemStack stack) {
        if (stack == null) {
            return false;
        }
        PrismItemStack other = of(stack);
        return other.type == type && other.meta.equals(meta);
    }

    @Override
    public boolean equals(Object other) {
        return other instanceof ItemStack stack && stack.getAmount() == amount && isSimilar(stack);
    }

    @Override
    public int hashCode() {
        return type.hashCode() * 31 + amount;
    }

    @Override
    public ItemStack clone() {
        PrismItemStack copy = new PrismItemStack(type, amount);
        copy.meta = ItemMetaImpl.unwrap(meta.clone());
        return copy;
    }

    @Override
    public ItemMeta getItemMeta() {
        return meta.clone();
    }

    @Override
    public boolean hasItemMeta() {
        return !meta.isEmpty();
    }

    @Override
    public boolean setItemMeta(ItemMeta itemMeta) {
        if (itemMeta == null) {
            meta = new ItemMetaImpl();
            return true;
        }
        ItemMetaImpl backing = ItemMetaImpl.unwrap(itemMeta);
        if (backing == null) {
            return false;
        }
        meta = ItemMetaImpl.unwrap(backing.clone());
        return true;
    }

    @Override
    public short getDurability() {
        return (short) meta.damage;
    }

    @Override
    public void setDurability(short durability) {
        meta.damage = Math.max(0, durability);
    }

    @Override
    public Map<Enchantment, Integer> getEnchantments() {
        return meta.getEnchants();
    }

    @Override
    public boolean containsEnchantment(Enchantment enchantment) {
        return meta.hasEnchant(enchantment);
    }

    @Override
    public int getEnchantmentLevel(Enchantment enchantment) {
        return meta.getEnchantLevel(enchantment);
    }

    @Override
    public void addUnsafeEnchantment(Enchantment enchantment, int level) {
        meta.addEnchant(enchantment, level, true);
    }

    @Override
    public int removeEnchantment(Enchantment enchantment) {
        int level = meta.getEnchantLevel(enchantment);
        meta.removeEnchant(enchantment);
        return level;
    }

    @Override
    public void removeEnchantments() {
        meta.removeEnchantments();
    }

    @Override
    public Map<String, Object> serialize() {
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("v", 4900);
        result.put("type", type.name());
        if (amount != 1) {
            result.put("amount", amount);
        }
        if (!meta.isEmpty()) {
            result.put("meta", meta.serialize());
        }
        return result;
    }

    @Override
    public String getTranslationKey() {
        return type.getTranslationKey();
    }

    @Override
    public String translationKey() {
        return type.getTranslationKey();
    }

    @Override
    public Component effectiveName() {
        Component custom = meta.displayName();
        if (custom != null) {
            return custom;
        }
        Component named = meta.itemName();
        return named != null ? named : Component.translatable(type.getTranslationKey());
    }

    @Override
    public PersistentDataContainerView getPersistentDataContainer() {
        return meta.persistentData;
    }

    @Override
    public boolean editPersistentDataContainer(Consumer<PersistentDataContainer> consumer) {
        consumer.accept(meta.persistentData);
        return true;
    }

    @Override
    public byte[] serializeAsBytes() {
        return toJson().toString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
    }

    @Override
    public List<Component> computeTooltipLines(io.papermc.paper.inventory.tooltip.TooltipContext context, org.bukkit.entity.Player player) {
        List<Component> lines = new java.util.ArrayList<>();
        lines.add(effectiveName());
        List<Component> lore = meta.lore();
        if (lore != null) {
            lines.addAll(lore);
        }
        return lines;
    }

    @Override
    public boolean isRepairableBy(ItemStack repairMaterial) {
        return false;
    }

    @Override
    public int getMaxItemUseDuration(org.bukkit.entity.LivingEntity entity) {
        return 0;
    }

    @Override
    public boolean matchesWithoutData(ItemStack item, java.util.Set<io.papermc.paper.datacomponent.DataComponentType> excludeTypes, boolean ignoreCount) {
        return item != null && item.getType() == type && (ignoreCount || item.getAmount() == amount);
    }

    @Override
    public java.util.Set<io.papermc.paper.datacomponent.DataComponentType> getDataTypes() {
        return java.util.Set.of();
    }

    @Override
    public boolean hasData(io.papermc.paper.datacomponent.DataComponentType type) {
        return false;
    }

    @Override
    public boolean isDataOverridden(io.papermc.paper.datacomponent.DataComponentType type) {
        return false;
    }

    @Override
    public <T> T getData(io.papermc.paper.datacomponent.DataComponentType.Valued<T> type) {
        return null;
    }

    @Override
    public <T> void setData(io.papermc.paper.datacomponent.DataComponentType.Valued<T> type, T value) {
        throw new UnsupportedOperationException("CraftPP does not expose raw data components yet; use ItemMeta");
    }

    @Override
    public void setData(io.papermc.paper.datacomponent.DataComponentType.NonValued type) {
        throw new UnsupportedOperationException("CraftPP does not expose raw data components yet; use ItemMeta");
    }

    @Override
    public void unsetData(io.papermc.paper.datacomponent.DataComponentType type) {
    }

    @Override
    public void resetData(io.papermc.paper.datacomponent.DataComponentType type) {
    }

    @Override
    public void copyDataFrom(ItemStack source, java.util.function.Predicate<io.papermc.paper.datacomponent.DataComponentType> filter) {
        ItemMetaImpl backing = ItemMetaImpl.unwrap(source.getItemMeta());
        if (backing != null) {
            meta = ItemMetaImpl.unwrap(backing.clone());
        }
    }

    @Override
    public String toString() {
        return "ItemStack{" + type + " x " + amount + (meta.isEmpty() ? "" : ", " + meta.toJson()) + "}";
    }
}
