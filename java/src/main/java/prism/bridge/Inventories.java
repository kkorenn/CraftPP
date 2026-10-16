package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import net.kyori.adventure.text.Component;
import org.bukkit.Location;
import org.bukkit.Material;
import org.bukkit.entity.HumanEntity;
import org.bukkit.event.inventory.ClickType;
import org.bukkit.event.inventory.InventoryAction;
import org.bukkit.event.inventory.InventoryType;
import org.bukkit.inventory.EquipmentSlot;
import org.bukkit.inventory.Inventory;
import org.bukkit.inventory.InventoryHolder;
import org.bukkit.inventory.InventoryView;
import org.bukkit.inventory.ItemStack;
import org.bukkit.inventory.MenuType;
import org.bukkit.inventory.PlayerInventory;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.ListIterator;
import java.util.Map;

/** Bukkit inventories: plain chest-style containers and the player inventory synced with PrismMC. */
final class Inventories {
    private Inventories() {
    }

    static String menuTypeFor(InventoryType type, int size) {
        return switch (type) {
            case CHEST, BARREL, ENDER_CHEST -> "generic_9x" + Math.max(1, Math.min(6, size / 9));
            case DISPENSER, DROPPER -> "generic_3x3";
            case HOPPER -> "hopper";
            case SHULKER_BOX -> "shulker_box";
            case FURNACE -> "furnace";
            case BLAST_FURNACE -> "blast_furnace";
            case SMOKER -> "smoker";
            case WORKBENCH -> "crafting";
            case ENCHANTING -> "enchantment";
            case BREWING -> "brewing_stand";
            case ANVIL -> "anvil";
            case SMITHING, SMITHING_NEW -> "smithing";
            case BEACON -> "beacon";
            case LECTERN -> "lectern";
            case LOOM -> "loom";
            case CARTOGRAPHY -> "cartography_table";
            case GRINDSTONE -> "grindstone";
            case STONECUTTER -> "stonecutter";
            case MERCHANT -> "merchant";
            case CRAFTER -> "crafter_3x3";
            default -> "generic_9x" + Math.max(1, Math.min(6, Math.max(9, size) / 9));
        };
    }

    static int defaultSize(InventoryType type) {
        return switch (type) {
            case CHEST, ENDER_CHEST, BARREL, SHULKER_BOX -> 27;
            case DISPENSER, DROPPER, CRAFTER -> 9;
            case HOPPER, BREWING -> 5;
            case FURNACE, BLAST_FURNACE, SMOKER, ANVIL, GRINDSTONE, CARTOGRAPHY, MERCHANT -> 3;
            case WORKBENCH -> 10;
            case ENCHANTING, STONECUTTER -> 2;
            case BEACON, LECTERN -> 1;
            case LOOM, SMITHING, SMITHING_NEW -> 4;
            case PLAYER -> 41;
            default -> 27;
        };
    }

    /** Container inventories live entirely on the Java side; PrismMC just renders them. */
    static class InventoryImpl implements Inventory {
        final ItemStack[] slots;
        final InventoryType type;
        final InventoryHolder holder;
        String title;
        final List<HumanEntity> viewers = new ArrayList<>();
        int maxStackSize = 99;

        InventoryImpl(InventoryHolder holder, InventoryType type, int size, String title) {
            this.holder = holder;
            this.type = type;
            this.slots = new ItemStack[size];
            this.title = title;
        }

        @Override
        public int getSize() {
            return slots.length;
        }

        @Override
        public int getMaxStackSize() {
            return maxStackSize;
        }

        @Override
        public void setMaxStackSize(int size) {
            maxStackSize = size;
        }

        @Override
        public ItemStack getItem(int index) {
            return index < 0 || index >= slots.length || slots[index] == null ? null : slots[index];
        }

        @Override
        public void setItem(int index, ItemStack item) {
            if (index >= 0 && index < slots.length) {
                slots[index] = item == null || item.getType().isAir() || item.getAmount() <= 0 ? null : item.clone();
                changed();
            }
        }

        void changed() {
            for (HumanEntity viewer : new ArrayList<>(viewers)) {
                if (ProxyBase.unwrap(viewer) instanceof PlayerImpl player) {
                    player.refreshOpenInventory(this);
                }
            }
        }

        @Override
        public HashMap<Integer, ItemStack> addItem(ItemStack... items) {
            HashMap<Integer, ItemStack> leftover = new HashMap<>();
            for (int i = 0; i < items.length; i++) {
                ItemStack remaining = items[i] == null ? null : items[i].clone();
                if (remaining == null || remaining.getType().isAir()) {
                    continue;
                }
                for (int slot = 0; slot < slots.length && remaining.getAmount() > 0; slot++) {
                    ItemStack existing = slots[slot];
                    if (existing != null && existing.isSimilar(remaining) && existing.getAmount() < existing.getMaxStackSize()) {
                        int move = Math.min(remaining.getAmount(), existing.getMaxStackSize() - existing.getAmount());
                        existing.setAmount(existing.getAmount() + move);
                        remaining.setAmount(remaining.getAmount() - move);
                    }
                }
                for (int slot = 0; slot < slots.length && remaining.getAmount() > 0; slot++) {
                    if (slots[slot] == null) {
                        int move = Math.min(remaining.getAmount(), remaining.getMaxStackSize());
                        ItemStack placed = remaining.clone();
                        placed.setAmount(move);
                        slots[slot] = placed;
                        remaining.setAmount(remaining.getAmount() - move);
                    }
                }
                if (remaining.getAmount() > 0) {
                    leftover.put(i, remaining);
                }
            }
            changed();
            return leftover;
        }

        @Override
        public HashMap<Integer, ItemStack> removeItem(ItemStack... items) {
            HashMap<Integer, ItemStack> leftover = new HashMap<>();
            for (int i = 0; i < items.length; i++) {
                int toRemove = items[i] == null ? 0 : items[i].getAmount();
                for (int slot = 0; slot < slots.length && toRemove > 0; slot++) {
                    if (slots[slot] != null && slots[slot].isSimilar(items[i])) {
                        int take = Math.min(toRemove, slots[slot].getAmount());
                        slots[slot].setAmount(slots[slot].getAmount() - take);
                        if (slots[slot].getAmount() <= 0) {
                            slots[slot] = null;
                        }
                        toRemove -= take;
                    }
                }
                if (toRemove > 0) {
                    ItemStack rest = items[i].clone();
                    rest.setAmount(toRemove);
                    leftover.put(i, rest);
                }
            }
            changed();
            return leftover;
        }

        @Override
        public HashMap<Integer, ItemStack> removeItemAnySlot(ItemStack... items) {
            return removeItem(items);
        }

        @Override
        public ItemStack[] getContents() {
            ItemStack[] copy = new ItemStack[slots.length];
            for (int i = 0; i < slots.length; i++) {
                copy[i] = slots[i] == null ? null : slots[i].clone();
            }
            return copy;
        }

        @Override
        public void setContents(ItemStack[] items) {
            for (int i = 0; i < slots.length; i++) {
                slots[i] = i < items.length && items[i] != null && !items[i].getType().isAir() ? items[i].clone() : null;
            }
            changed();
        }

        @Override
        public ItemStack[] getStorageContents() {
            return getContents();
        }

        @Override
        public void setStorageContents(ItemStack[] items) {
            setContents(items);
        }

        @Override
        public boolean contains(Material material) {
            return first(material) >= 0;
        }

        @Override
        public boolean contains(ItemStack item) {
            return item != null && first(item) >= 0;
        }

        @Override
        public boolean contains(Material material, int amount) {
            int total = 0;
            for (ItemStack slot : slots) {
                if (slot != null && slot.getType() == material) {
                    total += slot.getAmount();
                }
            }
            return total >= amount;
        }

        @Override
        public boolean contains(ItemStack item, int amount) {
            int count = 0;
            for (ItemStack slot : slots) {
                if (slot != null && slot.equals(item)) {
                    count++;
                }
            }
            return count >= amount;
        }

        @Override
        public boolean containsAtLeast(ItemStack item, int amount) {
            int total = 0;
            for (ItemStack slot : slots) {
                if (slot != null && slot.isSimilar(item)) {
                    total += slot.getAmount();
                }
            }
            return total >= amount;
        }

        @Override
        public HashMap<Integer, ? extends ItemStack> all(Material material) {
            HashMap<Integer, ItemStack> result = new HashMap<>();
            for (int i = 0; i < slots.length; i++) {
                if (slots[i] != null && slots[i].getType() == material) {
                    result.put(i, slots[i]);
                }
            }
            return result;
        }

        @Override
        public HashMap<Integer, ? extends ItemStack> all(ItemStack item) {
            HashMap<Integer, ItemStack> result = new HashMap<>();
            for (int i = 0; i < slots.length; i++) {
                if (slots[i] != null && slots[i].equals(item)) {
                    result.put(i, slots[i]);
                }
            }
            return result;
        }

        @Override
        public int first(Material material) {
            for (int i = 0; i < slots.length; i++) {
                if (slots[i] != null && slots[i].getType() == material) {
                    return i;
                }
            }
            return -1;
        }

        @Override
        public int first(ItemStack item) {
            for (int i = 0; i < slots.length; i++) {
                if (slots[i] != null && slots[i].equals(item)) {
                    return i;
                }
            }
            return -1;
        }

        @Override
        public int firstEmpty() {
            for (int i = 0; i < slots.length; i++) {
                if (slots[i] == null) {
                    return i;
                }
            }
            return -1;
        }

        @Override
        public boolean isEmpty() {
            for (ItemStack slot : slots) {
                if (slot != null) {
                    return false;
                }
            }
            return true;
        }

        @Override
        public void remove(Material material) {
            for (int i = 0; i < slots.length; i++) {
                if (slots[i] != null && slots[i].getType() == material) {
                    slots[i] = null;
                }
            }
            changed();
        }

        @Override
        public void remove(ItemStack item) {
            for (int i = 0; i < slots.length; i++) {
                if (slots[i] != null && slots[i].equals(item)) {
                    slots[i] = null;
                }
            }
            changed();
        }

        @Override
        public void clear(int index) {
            setItem(index, null);
        }

        @Override
        public void clear() {
            java.util.Arrays.fill(slots, null);
            changed();
        }

        @Override
        public int close() {
            int count = viewers.size();
            for (HumanEntity viewer : new ArrayList<>(viewers)) {
                viewer.closeInventory();
            }
            return count;
        }

        @Override
        public List<HumanEntity> getViewers() {
            return new ArrayList<>(viewers);
        }

        @Override
        public InventoryType getType() {
            return type;
        }

        @Override
        public InventoryHolder getHolder() {
            return holder;
        }

        @Override
        public InventoryHolder getHolder(boolean useSnapshot) {
            return holder;
        }

        @Override
        public ListIterator<ItemStack> iterator() {
            return new ArrayList<>(java.util.Arrays.asList(getContents())).listIterator();
        }

        @Override
        public ListIterator<ItemStack> iterator(int index) {
            return new ArrayList<>(java.util.Arrays.asList(getContents())).listIterator(index);
        }

        @Override
        public Location getLocation() {
            return null;
        }

        JsonArray itemsJson() {
            JsonArray array = new JsonArray();
            for (ItemStack slot : slots) {
                array.add(PrismItemStack.jsonOrNull(slot));
            }
            return array;
        }

        void applyFromServer(JsonArray items) {
            for (int i = 0; i < slots.length && i < items.size(); i++) {
                PrismItemStack stack = PrismItemStack.fromJson(items.get(i));
                slots[i] = stack == null ? null : stack;
            }
        }
    }

    /**
     * The player's own inventory. Bukkit slot numbering (0-8 hotbar, 9-35
     * main, 36-39 armor boots..helmet, 40 offhand) maps onto PrismMC's
     * protocol slots on every read/write.
     */
    static final class PlayerInventoryImpl extends InventoryImpl implements PlayerInventory {
        private final PlayerImpl player;

        PlayerInventoryImpl(PlayerImpl player) {
            super(null, InventoryType.PLAYER, 41, "Inventory");
            this.player = player;
        }

        static int toProtocol(int bukkitSlot) {
            if (bukkitSlot >= 0 && bukkitSlot <= 8) return 36 + bukkitSlot;
            if (bukkitSlot >= 9 && bukkitSlot <= 35) return bukkitSlot;
            return switch (bukkitSlot) {
                case 36 -> 8; // boots
                case 37 -> 7;
                case 38 -> 6;
                case 39 -> 5; // helmet
                case 40 -> 45; // offhand
                default -> -1;
            };
        }

        static int fromProtocol(int protocolSlot) {
            if (protocolSlot >= 36 && protocolSlot <= 44) return protocolSlot - 36;
            if (protocolSlot >= 9 && protocolSlot <= 35) return protocolSlot;
            return switch (protocolSlot) {
                case 8 -> 36;
                case 7 -> 37;
                case 6 -> 38;
                case 5 -> 39;
                case 45 -> 40;
                default -> -1;
            };
        }

        private void refresh() {
            JsonElement result = Main.rpc.api("player.inventory", "uuid", player.getUniqueId());
            if (result == null || !result.isJsonObject()) {
                return;
            }
            JsonArray items = result.getAsJsonObject().getAsJsonArray("items");
            java.util.Arrays.fill(slots, null);
            for (int protocol = 0; protocol < items.size(); protocol++) {
                int bukkit = fromProtocol(protocol);
                if (bukkit >= 0) {
                    PrismItemStack stack = PrismItemStack.fromJson(items.get(protocol));
                    slots[bukkit] = stack;
                }
            }
        }

        private void push(int bukkitSlot) {
            int protocol = toProtocol(bukkitSlot);
            if (protocol >= 0) {
                Main.rpc.api("player.setSlot", "uuid", player.getUniqueId(), "slot", protocol, "item", PrismItemStack.jsonOrNull(slots[bukkitSlot]));
            }
        }

        @Override
        public ItemStack getItem(int index) {
            refresh();
            return super.getItem(index);
        }

        @Override
        public void setItem(int index, ItemStack item) {
            if (index < 0 || index >= slots.length) {
                return;
            }
            slots[index] = item == null || item.getType().isAir() || item.getAmount() <= 0 ? null : item.clone();
            push(index);
        }

        @Override
        void changed() {
            for (int i = 0; i < slots.length; i++) {
                push(i);
            }
        }

        @Override
        public HashMap<Integer, ItemStack> addItem(ItemStack... items) {
            refresh();
            HashMap<Integer, ItemStack> leftover = super.addItem(items);
            return leftover;
        }

        @Override
        public HashMap<Integer, ItemStack> removeItem(ItemStack... items) {
            refresh();
            return super.removeItem(items);
        }

        @Override
        public ItemStack[] getContents() {
            refresh();
            return super.getContents();
        }

        @Override
        public ItemStack[] getStorageContents() {
            refresh();
            ItemStack[] storage = new ItemStack[36];
            System.arraycopy(super.getContents(), 0, storage, 0, 36);
            return storage;
        }

        @Override
        public void setStorageContents(ItemStack[] items) {
            for (int i = 0; i < 36; i++) {
                setItem(i, i < items.length ? items[i] : null);
            }
        }

        @Override
        public boolean isEmpty() {
            refresh();
            return super.isEmpty();
        }

        @Override
        public int firstEmpty() {
            refresh();
            for (int i = 0; i < 36; i++) {
                if (slots[i] == null) {
                    return i;
                }
            }
            return -1;
        }

        @Override
        public int first(Material material) {
            refresh();
            return super.first(material);
        }

        @Override
        public boolean contains(Material material) {
            refresh();
            return super.contains(material);
        }

        @Override
        public boolean contains(Material material, int amount) {
            refresh();
            return super.contains(material, amount);
        }

        @Override
        public boolean containsAtLeast(ItemStack item, int amount) {
            refresh();
            return super.containsAtLeast(item, amount);
        }

        @Override
        public void clear() {
            java.util.Arrays.fill(slots, null);
            changed();
        }

        @Override
        public ItemStack[] getArmorContents() {
            refresh();
            return new ItemStack[] {slots[36], slots[37], slots[38], slots[39]};
        }

        @Override
        public ItemStack[] getExtraContents() {
            refresh();
            return new ItemStack[] {slots[40]};
        }

        @Override
        public ItemStack getHelmet() {
            return getItem(39);
        }

        @Override
        public ItemStack getChestplate() {
            return getItem(38);
        }

        @Override
        public ItemStack getLeggings() {
            return getItem(37);
        }

        @Override
        public ItemStack getBoots() {
            return getItem(36);
        }

        @Override
        public void setItem(EquipmentSlot slot, ItemStack item) {
            setItem(slotIndex(slot), item);
        }

        @Override
        public ItemStack getItem(EquipmentSlot slot) {
            return getItem(slotIndex(slot));
        }

        private int slotIndex(EquipmentSlot slot) {
            return switch (slot) {
                case HAND -> getHeldItemSlot();
                case OFF_HAND -> 40;
                case FEET -> 36;
                case LEGS -> 37;
                case CHEST -> 38;
                case HEAD -> 39;
                default -> -1;
            };
        }

        @Override
        public void setArmorContents(ItemStack[] items) {
            for (int i = 0; i < 4; i++) {
                setItem(36 + i, i < items.length ? items[i] : null);
            }
        }

        @Override
        public void setExtraContents(ItemStack[] items) {
            setItem(40, items.length > 0 ? items[0] : null);
        }

        @Override
        public void setHelmet(ItemStack item) {
            setItem(39, item);
        }

        @Override
        public void setChestplate(ItemStack item) {
            setItem(38, item);
        }

        @Override
        public void setLeggings(ItemStack item) {
            setItem(37, item);
        }

        @Override
        public void setBoots(ItemStack item) {
            setItem(36, item);
        }

        @Override
        public ItemStack getItemInMainHand() {
            ItemStack item = getItem(getHeldItemSlot());
            return item == null ? new PrismItemStack(Material.AIR, 0) : item;
        }

        @Override
        public void setItemInMainHand(ItemStack item) {
            setItem(getHeldItemSlot(), item);
        }

        @Override
        public ItemStack getItemInOffHand() {
            ItemStack item = getItem(40);
            return item == null ? new PrismItemStack(Material.AIR, 0) : item;
        }

        @Override
        public void setItemInOffHand(ItemStack item) {
            setItem(40, item);
        }

        @Override
        public ItemStack getItemInHand() {
            return getItemInMainHand();
        }

        @Override
        public void setItemInHand(ItemStack item) {
            setItemInMainHand(item);
        }

        @Override
        public int getHeldItemSlot() {
            return player.heldSlot();
        }

        @Override
        public void setHeldItemSlot(int slot) {
            Main.rpc.api("player.setHeldSlot", "uuid", player.getUniqueId(), "slot", slot);
            player.setHeldSlotCached(slot);
        }

        @Override
        public HumanEntity getHolder() {
            return player.proxy();
        }

        @Override
        public HumanEntity getHolder(boolean useSnapshot) {
            return player.proxy();
        }
    }

    /** InventoryView over (top container, player inventory). */
    static final class ViewImpl implements InventoryView {
        final InventoryImpl top;
        final PlayerImpl player;
        final InventoryType type;
        String title;
        int windowId;
        ItemStack cursor;

        ViewImpl(InventoryImpl top, PlayerImpl player, InventoryType type, String title) {
            this.top = top;
            this.player = player;
            this.type = type;
            this.title = title;
        }

        @Override
        public Inventory getTopInventory() {
            return top;
        }

        @Override
        public Inventory getBottomInventory() {
            return player.getInventory();
        }

        @Override
        public HumanEntity getPlayer() {
            return player.proxy();
        }

        @Override
        public InventoryType getType() {
            return type;
        }

        @Override
        public void setItem(int slot, ItemStack item) {
            Inventory inventory = getInventory(slot);
            if (inventory != null) {
                inventory.setItem(convertSlot(slot), item);
            }
        }

        @Override
        public ItemStack getItem(int slot) {
            Inventory inventory = getInventory(slot);
            return inventory == null ? null : inventory.getItem(convertSlot(slot));
        }

        @Override
        public void setCursor(ItemStack item) {
            cursor = item;
        }

        @Override
        public ItemStack getCursor() {
            return cursor == null ? new PrismItemStack(Material.AIR, 0) : cursor;
        }

        @Override
        public Inventory getInventory(int rawSlot) {
            if (rawSlot < 0) {
                return null;
            }
            if (type == InventoryType.PLAYER) {
                return rawSlot < 46 ? getBottomInventory() : null;
            }
            return rawSlot < top.getSize() ? top : getBottomInventory();
        }

        @Override
        public int convertSlot(int rawSlot) {
            if (type == InventoryType.PLAYER) {
                return PlayerInventoryImpl.fromProtocol(rawSlot);
            }
            if (rawSlot < top.getSize()) {
                return rawSlot;
            }
            int index = rawSlot - top.getSize();
            return index < 27 ? 9 + index : index - 27;
        }

        @Override
        public InventoryType.SlotType getSlotType(int slot) {
            if (type == InventoryType.PLAYER) {
                return slot >= 5 && slot <= 8 ? InventoryType.SlotType.ARMOR : slot < 5 ? InventoryType.SlotType.CRAFTING : InventoryType.SlotType.CONTAINER;
            }
            if (slot >= top.getSize() + 27) {
                return InventoryType.SlotType.QUICKBAR;
            }
            return InventoryType.SlotType.CONTAINER;
        }

        @Override
        public void open() {
            player.openInventory(this);
        }

        @Override
        public void close() {
            player.closeInventory();
        }

        @Override
        public int countSlots() {
            return top.getSize() + 36;
        }

        @Override
        public boolean setProperty(InventoryView.Property property, int value) {
            return false;
        }

        @Override
        public String getTitle() {
            return title;
        }

        @Override
        public String getOriginalTitle() {
            return title;
        }

        @Override
        public void setTitle(String newTitle) {
            title = newTitle;
        }

        @Override
        public Component title() {
            return ServerImpl.LEGACY.deserialize(title);
        }

        @Override
        public MenuType getMenuType() {
            return null;
        }
    }

    static ClickType clickType(int mode, int button) {
        return switch (mode) {
            case 0 -> button == 1 ? ClickType.RIGHT : ClickType.LEFT;
            case 1 -> button == 1 ? ClickType.SHIFT_RIGHT : ClickType.SHIFT_LEFT;
            case 2 -> button == 40 ? ClickType.SWAP_OFFHAND : ClickType.NUMBER_KEY;
            case 3 -> ClickType.MIDDLE;
            case 4 -> button == 1 ? ClickType.CONTROL_DROP : ClickType.DROP;
            case 6 -> ClickType.DOUBLE_CLICK;
            default -> ClickType.UNKNOWN;
        };
    }

    static InventoryAction action(ClickType click, ItemStack current, ItemStack cursor) {
        boolean hasCurrent = current != null && !current.getType().isAir();
        boolean hasCursor = cursor != null && !cursor.getType().isAir();
        return switch (click) {
            case LEFT -> hasCursor ? (hasCurrent ? (cursor.isSimilar(current) ? InventoryAction.PLACE_SOME : InventoryAction.SWAP_WITH_CURSOR) : InventoryAction.PLACE_ALL)
                    : hasCurrent ? InventoryAction.PICKUP_ALL : InventoryAction.NOTHING;
            case RIGHT -> hasCursor ? InventoryAction.PLACE_ONE : hasCurrent ? InventoryAction.PICKUP_HALF : InventoryAction.NOTHING;
            case SHIFT_LEFT, SHIFT_RIGHT -> hasCurrent ? InventoryAction.MOVE_TO_OTHER_INVENTORY : InventoryAction.NOTHING;
            case NUMBER_KEY -> InventoryAction.HOTBAR_SWAP;
            case SWAP_OFFHAND -> InventoryAction.HOTBAR_SWAP;
            case MIDDLE -> InventoryAction.CLONE_STACK;
            case DROP -> hasCurrent ? InventoryAction.DROP_ONE_SLOT : InventoryAction.NOTHING;
            case CONTROL_DROP -> hasCurrent ? InventoryAction.DROP_ALL_SLOT : InventoryAction.NOTHING;
            case DOUBLE_CLICK -> InventoryAction.COLLECT_TO_CURSOR;
            default -> InventoryAction.UNKNOWN;
        };
    }

    static Map<String, Object> describe(InventoryImpl inventory) {
        Map<String, Object> map = new HashMap<>();
        map.put("type", menuTypeFor(inventory.type, inventory.getSize()));
        map.put("title", inventory.title);
        map.put("items", inventory.itemsJson());
        return map;
    }

    static JsonObject toJson(InventoryImpl inventory) {
        JsonObject json = new JsonObject();
        json.addProperty("type", menuTypeFor(inventory.type, inventory.getSize()));
        json.addProperty("title", inventory.title);
        json.add("items", inventory.itemsJson());
        return json;
    }
}
