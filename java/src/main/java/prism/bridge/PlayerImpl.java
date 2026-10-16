package prism.bridge;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import net.kyori.adventure.identity.Identity;
import net.kyori.adventure.text.Component;
import net.kyori.adventure.text.serializer.legacy.LegacyComponentSerializer;
import net.kyori.adventure.title.Title;
import org.bukkit.GameMode;
import org.bukkit.Location;
import org.bukkit.Server;
import org.bukkit.entity.EntityType;
import org.bukkit.entity.Player;
import org.bukkit.inventory.ItemStack;
import org.bukkit.event.player.PlayerTeleportEvent;
import org.bukkit.permissions.Permission;
import org.bukkit.permissions.PermissionAttachment;
import org.bukkit.permissions.PermissionAttachmentInfo;
import org.bukkit.permissions.PermissibleBase;
import org.bukkit.permissions.ServerOperator;
import org.bukkit.plugin.Plugin;

import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.Set;
import java.util.UUID;

/**
 * Backing object for {@link Player} (and {@link org.bukkit.OfflinePlayer})
 * proxies. Position/health/etc. are read through to PrismMC on every call;
 * permissions live here in a real PermissibleBase so plugin attachments work.
 */
public final class PlayerImpl implements ProxyBase.Backing, ProxyBase.HandleProvider, ServerOperator {
    private static final LegacyComponentSerializer LEGACY = ServerImpl.LEGACY;

    private final UUID uuid;
    private String name;
    private Player proxy;
    private final PermissibleBase permissions = new PermissibleBase(this);
    private final PdcImpl persistentData = new PdcImpl();
    private final Inventories.PlayerInventoryImpl inventory = new Inventories.PlayerInventoryImpl(this);
    private Inventories.ViewImpl openView;
    private Scoreboards.ScoreboardImpl scoreboard;
    private final java.util.Map<net.kyori.adventure.bossbar.BossBar, BossBars.AdventureBridge> bossBars = new java.util.concurrent.ConcurrentHashMap<>();
    private int heldSlot;
    private boolean pdcRestored;
    private volatile boolean op;
    private volatile boolean online;
    private Component displayName;
    private Component playerListName;
    private JsonObject snapshot = new JsonObject();

    PlayerImpl(UUID uuid, String name) {
        this.uuid = uuid;
        this.name = name;
        persistentData.onChange(() -> {
            if (online) {
                Main.rpc.api("player.setPluginData", "uuid", uuid, "data", Persistence.serialize(persistentData));
            }
        });
    }

    int heldSlot() {
        return heldSlot;
    }

    void setHeldSlotCached(int slot) {
        heldSlot = slot;
    }

    Inventories.ViewImpl openView() {
        return openView;
    }

    /** Re-send an open container after its Java-side contents changed. */
    void refreshOpenInventory(Inventories.InventoryImpl inventory) {
        if (openView != null && openView.top == inventory && online) {
            Main.rpc.api("player.updateInventory", "uuid", uuid, "windowId", openView.windowId, "items", inventory.itemsJson());
        }
    }

    void applyServerInventoryChange(int windowId, com.google.gson.JsonArray items) {
        if (openView != null && openView.windowId == windowId) {
            openView.top.applyFromServer(items);
        }
    }

    void closedByClient(int windowId) {
        Inventories.ViewImpl view = openView;
        if (view != null && view.windowId == windowId) {
            openView = null;
            view.top.viewers.remove(proxy);
            org.bukkit.Bukkit.getPluginManager().callEvent(new org.bukkit.event.inventory.InventoryCloseEvent(view));
        }
    }

    static UUID offlineUuid(String name) {
        return UUID.nameUUIDFromBytes(("OfflinePlayer:" + name).getBytes(StandardCharsets.UTF_8));
    }

    @Override
    public void attachProxy(Object proxy) {
        if (proxy instanceof Player player) {
            this.proxy = player;
        }
    }

    Player proxy() {
        return proxy;
    }

    @Override
    public Object nmsHandle() {
        prism.bridge.nms.Nms nms = prism.bridge.nms.Nms.get();
        return nms == null || !online ? null : nms.playerHandle(uuid, getName(), getEntityId());
    }

    void applySnapshot(JsonObject snapshot) {
        this.snapshot = snapshot;
        if (prism.bridge.nms.Nms.get() != null) {
            prism.bridge.nms.Nms.get().updatePlayer(uuid, snapshot);
        }
        if (snapshot.has("name")) {
            name = snapshot.get("name").getAsString();
        }
        if (snapshot.has("op")) {
            setOpCached(snapshot.get("op").getAsBoolean());
        }
        if (snapshot.has("heldSlot")) {
            heldSlot = snapshot.get("heldSlot").getAsInt();
        }
        if (!pdcRestored && snapshot.has("pluginData") && !snapshot.get("pluginData").isJsonNull()) {
            pdcRestored = true;
            Persistence.restore(snapshot.get("pluginData").getAsString(), persistentData);
        }
        online = true;
    }

    void markOffline() {
        online = false;
    }

    void setOpCached(boolean value) {
        if (op != value) {
            op = value;
            permissions.recalculatePermissions();
        }
    }

    private JsonObject state() {
        if (!online) {
            return snapshot;
        }
        JsonElement fresh = Main.rpc.api("player.state", "uuid", uuid);
        if (fresh != null && fresh.isJsonObject()) {
            snapshot = fresh.getAsJsonObject();
        }
        return snapshot;
    }

    private double num(JsonObject object, String key, double fallback) {
        return object.has(key) ? object.get(key).getAsDouble() : fallback;
    }

    // ----- identity -----

    public String getName() {
        return name;
    }

    public UUID getUniqueId() {
        return uuid;
    }

    public Identity identity() {
        return Identity.identity(uuid);
    }

    public Server getServer() {
        return org.bukkit.Bukkit.getServer();
    }

    public boolean isOnline() {
        return online;
    }

    public boolean isConnected() {
        return online;
    }

    public boolean isValid() {
        return online;
    }

    public boolean isDead() {
        return getHealth() <= 0;
    }

    public boolean hasPlayedBefore() {
        return true;
    }

    public long getFirstPlayed() {
        return 0;
    }

    public long getLastPlayed() {
        return 0;
    }

    public long getLastSeen() {
        return 0;
    }

    public long getLastLogin() {
        return 0;
    }

    public Player getPlayer() {
        return online ? proxy : null;
    }

    public EntityType getType() {
        return EntityType.PLAYER;
    }

    public int getEntityId() {
        int id = (int) num(snapshot, "entityId", 0);
        return id != 0 ? id : (int) num(state(), "entityId", 0);
    }

    public String getDisplayName() {
        return displayName == null ? name : LEGACY.serialize(displayName);
    }

    public void setDisplayName(String value) {
        displayName = value == null ? null : LEGACY.deserialize(value);
    }

    public Component displayName() {
        return displayName == null ? Component.text(name) : displayName;
    }

    public void displayName(Component value) {
        displayName = value;
    }

    public String getPlayerListName() {
        return playerListName == null ? name : LEGACY.serialize(playerListName);
    }

    public void setPlayerListName(String value) {
        playerListName = value == null ? null : LEGACY.deserialize(value);
    }

    public Component playerListName() {
        return playerListName == null ? Component.text(name) : playerListName;
    }

    public void playerListName(Component value) {
        playerListName = value;
    }

    public Component name() {
        return Component.text(name);
    }

    public String getCustomName() {
        return null;
    }

    public Component customName() {
        return null;
    }

    public InetSocketAddress getAddress() {
        JsonObject state = snapshot;
        String ip = state.has("ip") ? state.get("ip").getAsString() : "127.0.0.1";
        return InetSocketAddress.createUnresolved(ip, (int) num(state, "port", 0));
    }

    public String getLocale() {
        return "en_us";
    }

    public java.util.Locale locale() {
        return java.util.Locale.US;
    }

    public String getClientBrandName() {
        return "vanilla";
    }

    public int getProtocolVersion() {
        return 776;
    }

    public int getPing() {
        return (int) num(snapshot, "ping", 0);
    }

    // ----- messaging -----

    public void sendMessage(String message) {
        Native.sendMessage(uuid.getMostSignificantBits(), uuid.getLeastSignificantBits(), Rpc.utf8(message));
    }

    public void sendMessage(String... messages) {
        for (String message : messages) {
            sendMessage(message);
        }
    }

    public void sendMessage(UUID sender, String message) {
        sendMessage(message);
    }

    public void sendMessage(UUID sender, String... messages) {
        sendMessage(messages);
    }

    public void sendMessage(Component message) {
        sendMessage(LEGACY.serialize(message));
    }


    public void sendPlainMessage(String message) {
        sendMessage(Component.text(message));
    }

    public void sendRichMessage(String message) {
        sendMessage(net.kyori.adventure.text.minimessage.MiniMessage.miniMessage().deserialize(message));
    }

    public void sendRawMessage(String message) {
        sendMessage(message);
    }

    public void sendRawMessage(UUID sender, String message) {
        sendMessage(message);
    }

    public void sendActionBar(Component message) {
        Main.rpc.api("player.actionBar", "uuid", uuid, "text", LEGACY.serialize(message));
    }

    public void sendActionBar(String message) {
        Main.rpc.api("player.actionBar", "uuid", uuid, "text", message);
    }

    public void showTitle(Title title) {
        Title.Times times = title.times();
        Main.rpc.api("player.title", "uuid", uuid,
                "title", LEGACY.serialize(title.title()),
                "subtitle", LEGACY.serialize(title.subtitle()),
                "fadeIn", times == null ? 10 : times.fadeIn().toMillis() / 50,
                "stay", times == null ? 70 : times.stay().toMillis() / 50,
                "fadeOut", times == null ? 20 : times.fadeOut().toMillis() / 50);
    }

    public void sendTitle(String title, String subtitle, int fadeIn, int stay, int fadeOut) {
        Main.rpc.api("player.title", "uuid", uuid, "title", title == null ? "" : title,
                "subtitle", subtitle == null ? "" : subtitle, "fadeIn", fadeIn, "stay", stay, "fadeOut", fadeOut);
    }

    public void sendTitle(String title, String subtitle) {
        sendTitle(title, subtitle, 10, 70, 20);
    }

    public void resetTitle() {
        Main.rpc.api("player.resetTitle", "uuid", uuid);
    }

    public void clearTitle() {
        resetTitle();
    }

    public void playSound(Location location, String sound, float volume, float pitch) {
        Main.rpc.api("player.playSound", "uuid", uuid, "sound", sound,
                "x", location.getX(), "y", location.getY(), "z", location.getZ(), "volume", volume, "pitch", pitch);
    }

    public void playSound(Location location, org.bukkit.Sound sound, float volume, float pitch) {
        playSound(location, sound.getKey().asString(), volume, pitch);
    }

    public void playSound(Location location, org.bukkit.Sound sound, org.bukkit.SoundCategory category, float volume, float pitch) {
        playSound(location, sound, volume, pitch);
    }

    public void playSound(Location location, String sound, org.bukkit.SoundCategory category, float volume, float pitch) {
        playSound(location, sound, volume, pitch);
    }

    public void playSound(net.kyori.adventure.sound.Sound sound) {
        Location here = getLocation();
        playSound(here, sound.name().asString(), sound.volume(), sound.pitch());
    }

    public void playSound(net.kyori.adventure.sound.Sound sound, double x, double y, double z) {
        playSound(new Location(getWorld(), x, y, z), sound.name().asString(), sound.volume(), sound.pitch());
    }

    public void kick(Component message) {
        Main.rpc.api("player.kick", "uuid", uuid, "text", message == null ? "Kicked from the server." : LEGACY.serialize(message));
    }

    public void kick() {
        kick(null);
    }

    public void kick(Component message, org.bukkit.event.player.PlayerKickEvent.Cause cause) {
        kick(message);
    }

    public void kickPlayer(String message) {
        Main.rpc.api("player.kick", "uuid", uuid, "text", message == null ? "Kicked from the server." : message);
    }

    public void disconnect(Component message) {
        kick(message);
    }

    public void chat(String message) {
        Main.rpc.api("player.chat", "uuid", uuid, "text", message);
    }

    public boolean performCommand(String command) {
        return org.bukkit.Bukkit.dispatchCommand(proxy, command);
    }

    // ----- world / position -----

    public org.bukkit.World getWorld() {
        return Main.server.overworld();
    }

    public Location getLocation() {
        if (online) { // hot path: typed native, no JSON
            double[] pos = new double[5];
            if (Native.playerPos(uuid.getMostSignificantBits(), uuid.getLeastSignificantBits(), pos)) {
                return new Location(getWorld(), pos[0], pos[1], pos[2], (float) pos[3], (float) pos[4]);
            }
        }
        JsonObject state = state();
        return new Location(getWorld(), num(state, "x", 0), num(state, "y", 64), num(state, "z", 0),
                (float) num(state, "yaw", 0), (float) num(state, "pitch", 0));
    }

    public Location getLocation(Location target) {
        Location location = getLocation();
        if (target == null) {
            return location;
        }
        target.setWorld(location.getWorld());
        target.setX(location.getX());
        target.setY(location.getY());
        target.setZ(location.getZ());
        target.setYaw(location.getYaw());
        target.setPitch(location.getPitch());
        return target;
    }

    public Location getEyeLocation() {
        return getLocation().add(0, 1.62, 0);
    }

    public double getEyeHeight() {
        return 1.62;
    }

    public double getEyeHeight(boolean ignorePose) {
        return 1.62;
    }

    public org.bukkit.util.Vector getVelocity() {
        return new org.bukkit.util.Vector();
    }

    public void setVelocity(org.bukkit.util.Vector velocity) {
    }

    public boolean isOnGround() {
        return snapshot.has("onGround") && snapshot.get("onGround").getAsBoolean();
    }

    // ----- inventory -----

    public org.bukkit.inventory.PlayerInventory getInventory() {
        return inventory;
    }

    public org.bukkit.inventory.Inventory getEnderChest() {
        return enderChest;
    }

    private final Inventories.InventoryImpl enderChest = new Inventories.InventoryImpl(null, org.bukkit.event.inventory.InventoryType.ENDER_CHEST, 27, "Ender Chest");

    public org.bukkit.inventory.InventoryView getOpenInventory() {
        return openView != null ? openView : new Inventories.ViewImpl(inventory, this, org.bukkit.event.inventory.InventoryType.PLAYER, "Inventory");
    }

    public org.bukkit.inventory.InventoryView openInventory(org.bukkit.inventory.Inventory target) {
        if (!(target instanceof Inventories.InventoryImpl impl)) {
            throw new IllegalArgumentException("Only inventories created through Bukkit.createInventory can be opened");
        }
        Inventories.ViewImpl view = new Inventories.ViewImpl(impl, this, impl.type, impl.title);
        openInventory(view);
        return openView;
    }

    public void openInventory(org.bukkit.inventory.InventoryView target) {
        if (!(target instanceof Inventories.ViewImpl view)) {
            throw new IllegalArgumentException("Unsupported inventory view");
        }
        if (openView != null) {
            closeInventory();
        }
        org.bukkit.event.inventory.InventoryOpenEvent event = new org.bukkit.event.inventory.InventoryOpenEvent(view);
        org.bukkit.Bukkit.getPluginManager().callEvent(event);
        if (event.isCancelled()) {
            return;
        }
        com.google.gson.JsonElement result = Main.rpc.api("player.openInventory", "uuid", uuid, "type", Inventories.menuTypeFor(view.top.type, view.top.getSize()),
                "title", view.title, "items", view.top.itemsJson());
        view.windowId = result.getAsInt();
        openView = view;
        view.top.viewers.add(proxy);
    }

    public void closeInventory() {
        closeInventory(org.bukkit.event.inventory.InventoryCloseEvent.Reason.PLUGIN);
    }

    public void closeInventory(org.bukkit.event.inventory.InventoryCloseEvent.Reason reason) {
        Inventories.ViewImpl view = openView;
        if (view == null) {
            return;
        }
        openView = null;
        view.top.viewers.remove(proxy);
        if (online) {
            Main.rpc.api("player.closeInventory", "uuid", uuid);
        }
        org.bukkit.Bukkit.getPluginManager().callEvent(new org.bukkit.event.inventory.InventoryCloseEvent(view, reason));
    }

    public ItemStack getItemOnCursor() {
        return openView != null ? openView.getCursor() : new PrismItemStack(org.bukkit.Material.AIR, 0);
    }

    public void setItemOnCursor(ItemStack item) {
        if (openView != null) {
            openView.setCursor(item);
        }
    }

    public ItemStack getItemInHand() {
        return inventory.getItemInMainHand();
    }

    public void setItemInHand(ItemStack item) {
        inventory.setItemInMainHand(item);
    }

    public org.bukkit.inventory.EntityEquipment getEquipment() {
        return ProxyBase.create(org.bukkit.inventory.EntityEquipment.class, new PlayerEquipment());
    }

    public java.util.Map<Integer, ItemStack> give(ItemStack... items) {
        return inventory.addItem(items);
    }

    public org.bukkit.inventory.ItemStack getActiveItem() {
        return inventory.getItemInMainHand();
    }

    public org.bukkit.GameMode getPreviousGameMode() {
        return null;
    }

    public void updateInventory() {
        if (openView != null) {
            refreshOpenInventory(openView.top);
        }
    }

    public int getExpToLevel() {
        int level = getLevel();
        return level >= 30 ? 112 + (level - 30) * 9 : level >= 15 ? 37 + (level - 15) * 5 : 7 + level * 2;
    }

    public void damage(double amount) {
        Main.rpc.api("player.damage", "uuid", uuid, "amount", amount);
    }

    public void damage(double amount, org.bukkit.entity.Entity source) {
        damage(amount);
    }

    /** Equipment view over the inventory's armor and hand slots. */
    private final class PlayerEquipment {
        public ItemStack getItem(org.bukkit.inventory.EquipmentSlot slot) {
            return inventory.getItem(slot);
        }

        public void setItem(org.bukkit.inventory.EquipmentSlot slot, ItemStack item) {
            inventory.setItem(slot, item);
        }

        public void setItem(org.bukkit.inventory.EquipmentSlot slot, ItemStack item, boolean silent) {
            inventory.setItem(slot, item);
        }

        public ItemStack getItemInMainHand() {
            return inventory.getItemInMainHand();
        }

        public void setItemInMainHand(ItemStack item) {
            inventory.setItemInMainHand(item);
        }

        public void setItemInMainHand(ItemStack item, boolean silent) {
            inventory.setItemInMainHand(item);
        }

        public ItemStack getItemInOffHand() {
            return inventory.getItemInOffHand();
        }

        public void setItemInOffHand(ItemStack item) {
            inventory.setItemInOffHand(item);
        }

        public void setItemInOffHand(ItemStack item, boolean silent) {
            inventory.setItemInOffHand(item);
        }

        public ItemStack getItemInHand() {
            return inventory.getItemInMainHand();
        }

        public void setItemInHand(ItemStack item) {
            inventory.setItemInMainHand(item);
        }

        public ItemStack getHelmet() {
            return inventory.getHelmet();
        }

        public void setHelmet(ItemStack item) {
            inventory.setHelmet(item);
        }

        public void setHelmet(ItemStack item, boolean silent) {
            inventory.setHelmet(item);
        }

        public ItemStack getChestplate() {
            return inventory.getChestplate();
        }

        public void setChestplate(ItemStack item) {
            inventory.setChestplate(item);
        }

        public void setChestplate(ItemStack item, boolean silent) {
            inventory.setChestplate(item);
        }

        public ItemStack getLeggings() {
            return inventory.getLeggings();
        }

        public void setLeggings(ItemStack item) {
            inventory.setLeggings(item);
        }

        public void setLeggings(ItemStack item, boolean silent) {
            inventory.setLeggings(item);
        }

        public ItemStack getBoots() {
            return inventory.getBoots();
        }

        public void setBoots(ItemStack item) {
            inventory.setBoots(item);
        }

        public void setBoots(ItemStack item, boolean silent) {
            inventory.setBoots(item);
        }

        public ItemStack[] getArmorContents() {
            return inventory.getArmorContents();
        }

        public void setArmorContents(ItemStack[] items) {
            inventory.setArmorContents(items);
        }

        public void clear() {
            inventory.setArmorContents(new ItemStack[4]);
            inventory.setItemInMainHand(null);
            inventory.setItemInOffHand(null);
        }

        public org.bukkit.entity.Entity getHolder() {
            return proxy;
        }

        public float getDropChance(org.bukkit.inventory.EquipmentSlot slot) {
            return 1.0f;
        }

        public void setDropChance(org.bukkit.inventory.EquipmentSlot slot, float chance) {
        }
    }

    // ----- scoreboard -----

    public org.bukkit.scoreboard.Scoreboard getScoreboard() {
        return scoreboard != null ? scoreboard : org.bukkit.Bukkit.getScoreboardManager().getMainScoreboard();
    }

    public void setScoreboard(org.bukkit.scoreboard.Scoreboard board) {
        if (!(board instanceof Scoreboards.ScoreboardImpl impl)) {
            throw new IllegalArgumentException("Unknown scoreboard implementation");
        }
        Scoreboards.ScoreboardImpl current = (Scoreboards.ScoreboardImpl) getScoreboard();
        if (current == impl) {
            return;
        }
        current.viewers.remove(uuid);
        if (online) {
            current.clearFrom(uuid);
        }
        scoreboard = impl.main ? null : impl;
        impl.viewers.add(uuid);
        if (online) {
            impl.sendAllTo(uuid);
        }
    }

    /** On join: replay whichever board this player sees. */
    void sendScoreboard() {
        if (online) {
            ((Scoreboards.ScoreboardImpl) getScoreboard()).sendAllTo(uuid);
        }
    }

    // ----- boss bars (Adventure) -----

    public void showBossBar(net.kyori.adventure.bossbar.BossBar bar) {
        bossBars.computeIfAbsent(bar, BossBars.AdventureBridge::new).show(uuid);
    }

    public void hideBossBar(net.kyori.adventure.bossbar.BossBar bar) {
        BossBars.AdventureBridge bridge = bossBars.remove(bar);
        if (bridge != null) {
            bridge.hide(uuid);
        }
    }

    public Iterable<? extends net.kyori.adventure.bossbar.BossBar> activeBossBars() {
        return new java.util.ArrayList<>(bossBars.keySet());
    }

    public org.bukkit.entity.Entity getSpectatorTarget() {
        return null;
    }

    public org.bukkit.block.Block getTargetBlockExact(int maxDistance) {
        Location eye = getEyeLocation();
        org.bukkit.util.Vector direction = eye.getDirection();
        for (int step = 0; step < maxDistance * 4; step++) {
            Location point = eye.clone().add(direction.clone().multiply(step / 4.0));
            org.bukkit.block.Block block = point.getBlock();
            if (!block.getType().isAir()) {
                return block;
            }
        }
        return null;
    }

    public org.bukkit.block.Block getTargetBlock(java.util.Set<org.bukkit.Material> transparent, int maxDistance) {
        org.bukkit.block.Block block = getTargetBlockExact(maxDistance);
        return block != null ? block : getEyeLocation().add(getEyeLocation().getDirection().multiply(maxDistance)).getBlock();
    }

    public org.bukkit.block.Block getTargetBlock(int maxDistance) {
        return getTargetBlockExact(maxDistance);
    }

    public boolean isSneaking() {
        return false;
    }

    public boolean isSprinting() {
        return false;
    }

    public boolean isFlying() {
        return false;
    }

    public boolean getAllowFlight() {
        return getGameMode() == GameMode.CREATIVE || getGameMode() == GameMode.SPECTATOR;
    }

    public void setAllowFlight(boolean value) {
    }

    public boolean isInsideVehicle() {
        return false;
    }

    public boolean teleport(Location location) {
        return teleport(location, PlayerTeleportEvent.TeleportCause.PLUGIN);
    }

    public boolean teleport(Location location, PlayerTeleportEvent.TeleportCause cause) {
        if (!online) {
            return false;
        }
        PlayerTeleportEvent event = new PlayerTeleportEvent(proxy, getLocation(), location.clone(), cause);
        org.bukkit.Bukkit.getPluginManager().callEvent(event);
        if (event.isCancelled()) {
            return false;
        }
        Location to = event.getTo();
        Main.rpc.api("player.teleport", "uuid", uuid, "x", to.getX(), "y", to.getY(), "z", to.getZ(),
                "yaw", to.getYaw(), "pitch", to.getPitch());
        return true;
    }

    public boolean teleport(org.bukkit.entity.Entity destination) {
        return teleport(destination.getLocation());
    }

    public java.util.concurrent.CompletableFuture<Boolean> teleportAsync(Location location) {
        return java.util.concurrent.CompletableFuture.completedFuture(teleport(location));
    }

    public java.util.concurrent.CompletableFuture<Boolean> teleportAsync(Location location, PlayerTeleportEvent.TeleportCause cause) {
        return java.util.concurrent.CompletableFuture.completedFuture(teleport(location, cause));
    }

    // ----- game state -----

    public GameMode getGameMode() {
        GameMode mode = GameMode.getByValue((int) num(state(), "gamemode", 0));
        return mode == null ? GameMode.SURVIVAL : mode;
    }

    public void setGameMode(GameMode mode) {
        Main.rpc.api("player.setGameMode", "uuid", uuid, "mode", mode.getValue());
    }

    public double getHealth() {
        return num(state(), "health", 20);
    }

    public void setHealth(double health) {
        Main.rpc.api("player.setHealth", "uuid", uuid, "value", health);
    }

    public double getMaxHealth() {
        return 20;
    }

    public int getFoodLevel() {
        return (int) num(state(), "food", 20);
    }

    public void setFoodLevel(int value) {
        Main.rpc.api("player.setFood", "uuid", uuid, "value", value);
    }

    public float getSaturation() {
        return 5;
    }

    public void setSaturation(float value) {
    }

    public int getLevel() {
        return (int) num(state(), "level", 0);
    }

    public void setLevel(int level) {
        Main.rpc.api("player.setLevel", "uuid", uuid, "value", level);
    }

    public float getExp() {
        return (float) num(state(), "exp", 0);
    }

    public void setExp(float exp) {
        Main.rpc.api("player.setExp", "uuid", uuid, "value", exp);
    }

    public int getTotalExperience() {
        return (int) num(state(), "totalExp", 0);
    }

    public void giveExpLevels(int amount) {
        setLevel(getLevel() + amount);
    }

    public void giveExp(int amount) {
        Main.rpc.api("player.giveExp", "uuid", uuid, "value", amount);
    }

    public void setRemainingAir(int ticks) {
    }

    public int getRemainingAir() {
        return 300;
    }

    public int getMaximumAir() {
        return 300;
    }

    public void setFireTicks(int ticks) {
    }

    public int getFireTicks() {
        return 0;
    }

    public int getNoDamageTicks() {
        return 0;
    }

    public void setNoDamageTicks(int ticks) {
    }

    public float getWalkSpeed() {
        return 0.2f;
    }

    public void setWalkSpeed(float value) {
    }

    public float getFlySpeed() {
        return 0.1f;
    }

    public void setFlySpeed(float value) {
    }

    public boolean isInvulnerable() {
        return getGameMode() == GameMode.CREATIVE;
    }

    public void setInvulnerable(boolean value) {
    }

    public boolean isSleeping() {
        return false;
    }

    public java.util.Collection<org.bukkit.potion.PotionEffect> getActivePotionEffects() {
        return java.util.List.of();
    }

    public boolean hasPotionEffect(org.bukkit.potion.PotionEffectType type) {
        return false;
    }

    public io.papermc.paper.persistence.PersistentDataContainerView getPersistentDataContainer() {
        return persistentData;
    }

    public void sendBlockChange(Location location, org.bukkit.block.data.BlockData block) {
        Main.rpc.api("player.sendBlockChange", "uuid", uuid, "x", location.getBlockX(), "y", location.getBlockY(),
                "z", location.getBlockZ(), "block", block.getMaterial().getKey().asString());
    }

    public void sendBlockChange(Location location, org.bukkit.Material material, byte data) {
        Main.rpc.api("player.sendBlockChange", "uuid", uuid, "x", location.getBlockX(), "y", location.getBlockY(),
                "z", location.getBlockZ(), "block", material.getKey().asString());
    }

    public void setResourcePack(String url) {
    }

    public void setResourcePack(String url, byte[] hash) {
    }

    public void setPlayerTime(long time, boolean relative) {
    }

    public void resetPlayerTime() {
    }

    public void setPlayerWeather(org.bukkit.WeatherType type) {
    }

    public void resetPlayerWeather() {
    }

    public java.util.Set<Player> getHiddenPlayers() {
        return java.util.Set.of();
    }

    public void hidePlayer(Plugin plugin, Player player) {
    }

    public void showPlayer(Plugin plugin, Player player) {
    }

    public boolean canSee(Player player) {
        return true;
    }

    public boolean canSee(org.bukkit.entity.Entity entity) {
        return true;
    }

    public void setMetadata(String key, org.bukkit.metadata.MetadataValue value) {
        metadata.computeIfAbsent(key, k -> new java.util.ArrayList<>()).add(value);
    }

    public java.util.List<org.bukkit.metadata.MetadataValue> getMetadata(String key) {
        return metadata.getOrDefault(key, java.util.List.of());
    }

    public boolean hasMetadata(String key) {
        return metadata.containsKey(key) && !metadata.get(key).isEmpty();
    }

    public void removeMetadata(String key, Plugin plugin) {
        java.util.List<org.bukkit.metadata.MetadataValue> values = metadata.get(key);
        if (values != null) {
            values.removeIf(value -> value.getOwningPlugin() == plugin);
        }
    }

    private final java.util.Map<String, java.util.List<org.bukkit.metadata.MetadataValue>> metadata = new java.util.concurrent.ConcurrentHashMap<>();

    public java.util.Set<String> getScoreboardTags() {
        return new java.util.HashSet<>();
    }

    public boolean addScoreboardTag(String tag) {
        return false;
    }

    public boolean removeScoreboardTag(String tag) {
        return false;
    }

    // ----- permissions -----

    @Override
    public boolean isOp() {
        return op;
    }

    @Override
    public void setOp(boolean value) {
        Main.rpc.api("player.setOp", "uuid", uuid, "name", name, "op", value);
        setOpCached(value);
    }

    public boolean isPermissionSet(String name) {
        return permissions.isPermissionSet(name);
    }

    public boolean isPermissionSet(Permission perm) {
        return permissions.isPermissionSet(perm);
    }

    public boolean hasPermission(String name) {
        return permissions.hasPermission(name);
    }

    public boolean hasPermission(Permission perm) {
        return permissions.hasPermission(perm);
    }

    public PermissionAttachment addAttachment(Plugin plugin, String name, boolean value) {
        return permissions.addAttachment(plugin, name, value);
    }

    public PermissionAttachment addAttachment(Plugin plugin) {
        return permissions.addAttachment(plugin);
    }

    public PermissionAttachment addAttachment(Plugin plugin, String name, boolean value, int ticks) {
        return permissions.addAttachment(plugin, name, value, ticks);
    }

    public PermissionAttachment addAttachment(Plugin plugin, int ticks) {
        return permissions.addAttachment(plugin, ticks);
    }

    public void removeAttachment(PermissionAttachment attachment) {
        permissions.removeAttachment(attachment);
    }

    public void recalculatePermissions() {
        permissions.recalculatePermissions();
    }

    public Set<PermissionAttachmentInfo> getEffectivePermissions() {
        return permissions.getEffectivePermissions();
    }

    public boolean isBanned() {
        return false;
    }

    public boolean isWhitelisted() {
        return true;
    }

    public void setWhitelisted(boolean value) {
    }

    @Override
    public boolean equals(Object other) {
        Object unwrapped = ProxyBase.unwrap(other);
        return unwrapped instanceof PlayerImpl player && player.uuid.equals(uuid);
    }

    @Override
    public int hashCode() {
        return uuid.hashCode();
    }

    @Override
    public String toString() {
        return "CraftPlayer{name=" + name + "}";
    }
}
