package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import io.papermc.paper.threadedregions.scheduler.AsyncScheduler;
import io.papermc.paper.threadedregions.scheduler.GlobalRegionScheduler;
import io.papermc.paper.threadedregions.scheduler.RegionScheduler;
import net.kyori.adventure.audience.Audience;
import net.kyori.adventure.text.Component;
import net.kyori.adventure.text.serializer.legacy.LegacyComponentSerializer;
import org.bukkit.Bukkit;
import org.bukkit.GameMode;
import org.bukkit.Location;
import org.bukkit.OfflinePlayer;
import org.bukkit.Server;
import org.bukkit.UnsafeValues;
import org.bukkit.World;
import org.bukkit.command.CommandMap;
import org.bukkit.command.CommandSender;
import org.bukkit.command.ConsoleCommandSender;
import org.bukkit.command.PluginCommand;
import org.bukkit.command.SimpleCommandMap;
import org.bukkit.entity.Player;
import org.bukkit.plugin.PluginManager;
import org.bukkit.plugin.ServicesManager;
import org.bukkit.plugin.SimpleServicesManager;
import org.bukkit.plugin.messaging.Messenger;
import org.bukkit.plugin.messaging.StandardMessenger;
import org.bukkit.scheduler.BukkitScheduler;

import java.io.File;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.logging.Logger;

/** Backing object for the {@link Server} proxy. Every state read goes to PrismMC. */
public final class ServerImpl implements ProxyBase.Backing, ProxyBase.HandleProvider {
    static final LegacyComponentSerializer LEGACY = LegacyComponentSerializer.legacySection();

    private final JsonObject config;
    private final Logger logger = Logger.getLogger("");
    private final Map<UUID, PlayerImpl> players = new ConcurrentHashMap<>();
    private final Map<String, PlayerImpl> playersByName = new ConcurrentHashMap<>();
    private Server proxy;
    private SimpleCommandMap commandMap;
    private ServicesManager servicesManager;
    private Messenger messenger;
    private ConsoleCommandSender console;
    private WorldImpl overworld;
    private World overworldProxy;
    private BukkitScheduler schedulerProxy;
    private UnsafeValues unsafe;
    private GlobalRegionScheduler globalRegionScheduler;
    private AsyncScheduler asyncScheduler;
    private RegionScheduler regionScheduler;

    ServerImpl(JsonObject config) {
        this.config = config;
    }

    @Override
    public void attachProxy(Object proxy) {
        this.proxy = (Server) proxy;
    }

    void afterBukkitReady() {
        commandMap = new SimpleCommandMap(proxy, new HashMap<>());
        servicesManager = new SimpleServicesManager();
        messenger = new StandardMessenger();
        console = ProxyBase.create(ConsoleCommandSender.class, new ConsoleImpl());
        overworld = new WorldImpl(str("worldName", "world"), config.has("seed") ? config.get("seed").getAsLong() : 0);
        overworldProxy = ProxyBase.create(World.class, overworld);
        schedulerProxy = Main.scheduler;
        unsafe = ProxyBase.create(UnsafeValues.class, new UnsafeImpl());
        globalRegionScheduler = new FoliaSchedulers.Global();
        asyncScheduler = new FoliaSchedulers.Async();
        regionScheduler = new FoliaSchedulers.Region();
    }

    private String str(String key, String fallback) {
        return config.has(key) ? config.get(key).getAsString() : fallback;
    }

    private int num(String key, int fallback) {
        return config.has(key) ? config.get(key).getAsInt() : fallback;
    }

    // ----- players -----

    PlayerImpl playerByUuid(String uuid) {
        return players.get(UUID.fromString(uuid));
    }

    PlayerImpl ensurePlayer(JsonObject snapshot) {
        UUID uuid = UUID.fromString(snapshot.get("uuid").getAsString());
        PlayerImpl player = players.get(uuid);
        if (player == null) {
            player = new PlayerImpl(uuid, snapshot.get("name").getAsString());
            ProxyBase.create(Player.class, player);
            players.put(uuid, player);
            playersByName.put(player.getName().toLowerCase(java.util.Locale.ROOT), player);
        }
        player.applySnapshot(snapshot);
        return player;
    }

    @Override
    public Object nmsHandle() {
        prism.bridge.nms.Nms nms = prism.bridge.nms.Nms.get();
        return nms == null ? null : nms.server();
    }

    void removePlayer(UUID uuid) {
        if (prism.bridge.nms.Nms.get() != null) {
            prism.bridge.nms.Nms.get().removePlayer(uuid);
        }
        PlayerImpl player = players.remove(uuid);
        if (player != null) {
            playersByName.remove(player.getName().toLowerCase(java.util.Locale.ROOT));
            player.markOffline();
        }
    }

    Collection<PlayerImpl> playerBackings() {
        return players.values();
    }

    /** Resolve a command sender reference from PrismMC: null/absent = console. */
    CommandSender senderFrom(JsonObject payload) {
        if (payload.has("sender") && !payload.get("sender").isJsonNull()) {
            PlayerImpl player = playerByUuid(payload.get("sender").getAsString());
            if (player != null) {
                return player.proxy();
            }
        }
        return console;
    }

    // ----- Server interface (called through the proxy) -----

    public String getName() {
        return "Paper";
    }

    public String getVersion() {
        return str("version", "26.3-CraftPP") + " (MC: " + str("mcVersion", "26.3") + ")";
    }

    public String getBukkitVersion() {
        return str("bukkitVersion", "26.3-R0.1-SNAPSHOT");
    }

    public String getMinecraftVersion() {
        return str("mcVersion", "26.3");
    }

    public Logger getLogger() {
        return logger;
    }

    public Collection<? extends Player> getOnlinePlayers() {
        List<Player> list = new ArrayList<>();
        // Refresh from PrismMC so newly joined players are visible even to async callers.
        JsonElement result = Main.rpc.api("server.players");
        for (JsonElement element : result.getAsJsonArray()) {
            list.add(ensurePlayer(element.getAsJsonObject()).proxy());
        }
        return list;
    }

    public int getMaxPlayers() {
        return num("maxPlayers", 20);
    }

    public int getPort() {
        return num("port", 25565);
    }

    public int getViewDistance() {
        return num("viewDistance", 10);
    }

    public int getSimulationDistance() {
        return num("simulationDistance", 10);
    }

    public String getIp() {
        return "";
    }

    public String getWorldType() {
        return "DEFAULT";
    }

    public boolean getGenerateStructures() {
        return true;
    }

    public boolean getAllowEnd() {
        return false;
    }

    public boolean getAllowNether() {
        return false;
    }

    public boolean getOnlineMode() {
        return config.has("onlineMode") && config.get("onlineMode").getAsBoolean();
    }

    public boolean getAllowFlight() {
        return false;
    }

    public boolean isHardcore() {
        return false;
    }

    public int getSpawnRadius() {
        return 16;
    }

    public String getMotd() {
        return Main.rpc.api("server.motd").getAsString();
    }

    public Component motd() {
        return LEGACY.deserialize(getMotd());
    }

    public void setMotd(String motd) {
        Main.rpc.api("server.setMotd", "text", java.util.Objects.requireNonNull(motd, "motd"));
    }

    public String getShutdownMessage() {
        return "Server closed";
    }

    public Component shutdownMessage() {
        return Component.text("Server closed");
    }

    public Player getPlayer(String name) {
        Player exact = getPlayerExact(name);
        if (exact != null) {
            return exact;
        }
        String lower = name.toLowerCase(java.util.Locale.ROOT);
        Player best = null;
        int bestDelta = Integer.MAX_VALUE;
        for (Player player : getOnlinePlayers()) {
            if (player.getName().toLowerCase(java.util.Locale.ROOT).startsWith(lower)) {
                int delta = player.getName().length() - lower.length();
                if (delta < bestDelta) {
                    best = player;
                    bestDelta = delta;
                }
            }
        }
        return best;
    }

    public Player getPlayerExact(String name) {
        for (Player player : getOnlinePlayers()) {
            if (player.getName().equalsIgnoreCase(name)) {
                return player;
            }
        }
        return null;
    }

    public Player getPlayer(UUID uuid) {
        for (Player player : getOnlinePlayers()) {
            if (player.getUniqueId().equals(uuid)) {
                return player;
            }
        }
        return null;
    }

    public List<Player> matchPlayer(String name) {
        List<Player> matches = new ArrayList<>();
        String lower = name.toLowerCase(java.util.Locale.ROOT);
        for (Player player : getOnlinePlayers()) {
            if (player.getName().equalsIgnoreCase(name)) {
                return List.of(player);
            }
            if (player.getName().toLowerCase(java.util.Locale.ROOT).contains(lower)) {
                matches.add(player);
            }
        }
        return matches;
    }

    public UUID getPlayerUniqueId(String name) {
        Player player = getPlayerExact(name);
        return player != null ? player.getUniqueId() : PlayerImpl.offlineUuid(name);
    }

    public OfflinePlayer getOfflinePlayer(String name) {
        Player online = getPlayerExact(name);
        if (online != null) {
            return online;
        }
        return ProxyBase.create(OfflinePlayer.class, new PlayerImpl(PlayerImpl.offlineUuid(name), name));
    }

    public OfflinePlayer getOfflinePlayer(UUID uuid) {
        Player online = getPlayer(uuid);
        if (online != null) {
            return online;
        }
        return ProxyBase.create(OfflinePlayer.class, new PlayerImpl(uuid, null));
    }

    public OfflinePlayer getOfflinePlayerIfCached(String name) {
        return getPlayerExact(name);
    }

    public OfflinePlayer[] getOfflinePlayers() {
        return new OfflinePlayer[0];
    }

    public java.util.Set<OfflinePlayer> getOperators() {
        return UserLists.players("ops");
    }

    public java.util.Set<OfflinePlayer> getWhitelistedPlayers() {
        return UserLists.players("whitelist");
    }

    public boolean hasWhitelist() {
        return Main.rpc.api("server.hasWhitelist").getAsBoolean();
    }

    public void setWhitelist(boolean value) {
        Main.rpc.api("server.setWhitelist", "value", value);
    }

    public boolean isWhitelistEnforced() {
        return Main.rpc.api("server.whitelistEnforced").getAsBoolean();
    }

    public void setWhitelistEnforced(boolean value) {
        Main.rpc.api("server.setWhitelistEnforced", "value", value);
    }

    public void reloadWhitelist() {
        Main.rpc.api("server.listReload", "list", "whitelist");
    }

    public java.util.Set<OfflinePlayer> getBannedPlayers() {
        return UserLists.players("bans");
    }

    public java.util.Set<String> getIPBans() {
        java.util.Set<String> ips = new java.util.LinkedHashSet<>();
        for (JsonElement row : UserLists.rows("ipbans")) ips.add(row.getAsJsonObject().get("ip").getAsString());
        return ips;
    }

    public void banIP(String address) {
        UserLists.banList(true).addBan(address, null, (java.util.Date) null, null);
    }

    public void banIP(java.net.InetAddress address) {
        banIP(address.getHostAddress());
    }

    public void unbanIP(String address) {
        UserLists.banList(true).pardon(address);
    }

    public void unbanIP(java.net.InetAddress address) {
        unbanIP(address.getHostAddress());
    }

    @SuppressWarnings("unchecked")
    public <T extends org.bukkit.BanList<?>> T getBanList(org.bukkit.BanList.Type type) {
        return (T) UserLists.banList(type == org.bukkit.BanList.Type.IP);
    }

    @SuppressWarnings("unchecked")
    public <B extends org.bukkit.BanList<E>, E> B getBanList(io.papermc.paper.ban.BanListType<B> type) {
        return (B) UserLists.banList(type == io.papermc.paper.ban.BanListType.IP);
    }

    // ----- managers -----

    public PluginManager getPluginManager() {
        return Main.pluginManager;
    }

    public BukkitScheduler getScheduler() {
        return schedulerProxy;
    }

    public ServicesManager getServicesManager() {
        return servicesManager;
    }

    public Messenger getMessenger() {
        return messenger;
    }

    public CommandMap getCommandMap() {
        return commandMap;
    }

    SimpleCommandMap commandMap() {
        return commandMap;
    }

    public PluginCommand getPluginCommand(String name) {
        org.bukkit.command.Command command = commandMap.getCommand(name);
        return command instanceof PluginCommand pluginCommand ? pluginCommand : null;
    }

    public ConsoleCommandSender getConsoleSender() {
        return console;
    }

    public CommandSender createCommandSender(java.util.function.Consumer<? super Component> feedback) {
        return ProxyBase.create(CommandSender.class, new ConsoleImpl(feedback));
    }

    public UnsafeValues getUnsafe() {
        return unsafe;
    }

    public GlobalRegionScheduler getGlobalRegionScheduler() {
        return globalRegionScheduler;
    }

    public AsyncScheduler getAsyncScheduler() {
        return asyncScheduler;
    }

    public RegionScheduler getRegionScheduler() {
        return regionScheduler;
    }

    public boolean isOwnedByCurrentRegion(Location location) {
        return Main.isServerThread();
    }

    public boolean isOwnedByCurrentRegion(org.bukkit.entity.Entity entity) {
        return Main.isServerThread();
    }

    public boolean isGlobalTickThread() {
        return Main.isServerThread();
    }

    public boolean isPrimaryThread() {
        return Main.isServerThread();
    }

    public boolean isStopping() {
        return Main.rpc.isClosed();
    }

    public int getCurrentTick() {
        return (int) Main.scheduler.currentTick();
    }

    public double[] getTPS() {
        return new double[] {20.0, 20.0, 20.0};
    }

    public long[] getTickTimes() {
        return new long[100];
    }

    public double getAverageTickTime() {
        return 0.0;
    }

    public boolean isPaused() {
        return false;
    }

    public boolean isTickingWorlds() {
        return true;
    }

    // ----- worlds -----

    public List<World> getWorlds() {
        return List.of(overworldProxy);
    }

    public World getWorld(String name) {
        return overworld.getName().equalsIgnoreCase(name) ? overworldProxy : null;
    }

    public World getWorld(UUID uid) {
        return overworld.getUID().equals(uid) ? overworldProxy : null;
    }

    public World getWorld(net.kyori.adventure.key.Key key) {
        return key.value().equals(overworld.getName()) || key.asString().equals("minecraft:overworld") ? overworldProxy : null;
    }

    public World getWorld(org.bukkit.NamespacedKey key) {
        return getWorld((net.kyori.adventure.key.Key) key);
    }

    World overworld() {
        return overworldProxy;
    }

    public File getWorldContainer() {
        return new File(str("serverDir", "."));
    }

    public File getPluginsFolder() {
        return new File(str("pluginsDir", "plugins"));
    }

    public void savePlayers() {
    }

    // ----- messaging -----

    public Iterable<? extends Audience> audiences() {
        List<Audience> audiences = new ArrayList<>(getOnlinePlayers());
        audiences.add(console);
        return audiences;
    }

    public int broadcastMessage(String message) {
        Main.rpc.api("server.broadcast", "text", message);
        return players.size();
    }

    public int broadcast(String message, String permission) {
        int count = 0;
        for (Player player : getOnlinePlayers()) {
            if (player.hasPermission(permission)) {
                player.sendMessage(message);
                count++;
            }
        }
        console.sendMessage(message);
        return count;
    }

    public int broadcast(Component message) {
        return broadcastMessage(LEGACY.serialize(message));
    }

    public int broadcast(Component message, String permission) {
        return broadcast(LEGACY.serialize(message), permission);
    }

    public boolean dispatchCommand(CommandSender sender, String commandLine) {
        if (commandMap.dispatch(sender, commandLine)) {
            return true;
        }
        Object backing = ProxyBase.unwrap(sender);
        String senderUuid = backing instanceof PlayerImpl player ? player.getUniqueId().toString() : null;
        return Main.rpc.api("server.dispatch", "sender", senderUuid, "line", commandLine).getAsBoolean();
    }

    public void shutdown() {
        Main.rpc.api("server.shutdown");
    }

    public void reload() {
        Main.rpc.api("server.dispatch", "sender", null, "line", "reload confirm");
    }

    public void reloadData() {
    }

    public GameMode getDefaultGameMode() {
        GameMode mode = GameMode.getByValue(Main.rpc.api("server.defaultGameMode").getAsInt());
        return mode == null ? GameMode.SURVIVAL : mode;
    }

    public void setDefaultGameMode(GameMode mode) {
        Main.rpc.api("server.setDefaultGameMode", "mode", java.util.Objects.requireNonNull(mode, "mode").getValue());
    }

    public int getIdleTimeout() {
        return 0;
    }

    public void setIdleTimeout(int threshold) {
    }

    public String getPermissionMessage() {
        return "I'm sorry, but you do not have permission to perform this command. Please contact the server administrators if you believe that this is in error.";
    }

    public Component permissionMessage() {
        return Component.text(getPermissionMessage());
    }

    public org.bukkit.util.CachedServerIcon getServerIcon() {
        return null;
    }

    public boolean suggestPlayerNamesWhenNullTabCompletions() {
        return true; // paper-global.yml commands.suggest-player-names-when-null-tab-completions default.
    }

    public boolean getHideOnlinePlayers() {
        return false;
    }

    public boolean isResourcePackRequired() {
        return false;
    }

    public String getResourcePack() {
        return "";
    }

    public String getResourcePackHash() {
        return "";
    }

    public String getResourcePackPrompt() {
        return "";
    }

    public boolean isEnforcingSecureProfiles() {
        return false;
    }

    public boolean isAcceptingTransfers() {
        return false;
    }

    public boolean shouldSendChatPreviews() {
        return false;
    }

    public int getMaxWorldSize() {
        return 29999984;
    }

    public boolean isLoggingIPs() {
        return true;
    }

    public java.util.Set<String> getInitialEnabledPacks() {
        return java.util.Set.of("vanilla");
    }

    public java.util.Set<String> getInitialDisabledPacks() {
        return Collections.emptySet();
    }

    public int getTicksPerAnimalSpawns() {
        return 400;
    }

    public int getTicksPerMonsterSpawns() {
        return 1;
    }

    public int getMonsterSpawnLimit() {
        return 70;
    }

    public int getAnimalSpawnLimit() {
        return 10;
    }

    public int getWaterAnimalSpawnLimit() {
        return 5;
    }

    public int getAmbientSpawnLimit() {
        return 15;
    }

    public int getConnectionThrottle() {
        return 4000;
    }

    public java.util.Map<String, String[]> getCommandAliases() {
        return new HashMap<>();
    }

    public void sendPluginMessage(org.bukkit.plugin.Plugin source, String channel, byte[] message) {
    }

    public java.util.Set<String> getListeningPluginChannels() {
        return Collections.emptySet();
    }

    public com.destroystokyo.paper.profile.PlayerProfile createProfile(UUID uuid) {
        return createProfile(uuid, null);
    }

    public com.destroystokyo.paper.profile.PlayerProfile createProfile(String name) {
        return createProfile(null, name);
    }

    public com.destroystokyo.paper.profile.PlayerProfile createProfile(UUID uuid, String name) {
        return ProxyBase.create(com.destroystokyo.paper.profile.PlayerProfile.class, new ProfileImpl(uuid, name));
    }

    public com.destroystokyo.paper.profile.PlayerProfile createProfileExact(UUID uuid, String name) {
        return createProfile(uuid, name);
    }

    private final org.bukkit.inventory.ItemFactory itemFactory = ProxyBase.create(org.bukkit.inventory.ItemFactory.class, new ItemFactoryImpl());
    private final Scoreboards.ManagerImpl scoreboardManager = new Scoreboards.ManagerImpl();
    private final Map<org.bukkit.NamespacedKey, BossBars.KeyedBarImpl> keyedBossBars = new ConcurrentHashMap<>();

    public org.bukkit.inventory.ItemFactory getItemFactory() {
        return itemFactory;
    }

    public org.bukkit.scoreboard.ScoreboardManager getScoreboardManager() {
        return scoreboardManager;
    }

    Scoreboards.ManagerImpl scoreboards() {
        return scoreboardManager;
    }

    public org.bukkit.inventory.Inventory createInventory(org.bukkit.inventory.InventoryHolder owner, org.bukkit.event.inventory.InventoryType type) {
        return new Inventories.InventoryImpl(owner, type, Inventories.defaultSize(type), type.getDefaultTitle());
    }

    public org.bukkit.inventory.Inventory createInventory(org.bukkit.inventory.InventoryHolder owner, org.bukkit.event.inventory.InventoryType type, Component title) {
        return new Inventories.InventoryImpl(owner, type, Inventories.defaultSize(type), LEGACY.serialize(title));
    }

    public org.bukkit.inventory.Inventory createInventory(org.bukkit.inventory.InventoryHolder owner, org.bukkit.event.inventory.InventoryType type, String title) {
        return new Inventories.InventoryImpl(owner, type, Inventories.defaultSize(type), title);
    }

    public org.bukkit.inventory.Inventory createInventory(org.bukkit.inventory.InventoryHolder owner, int size) {
        return createInventory(owner, size, "Chest");
    }

    public org.bukkit.inventory.Inventory createInventory(org.bukkit.inventory.InventoryHolder owner, int size, Component title) {
        return createInventory(owner, size, LEGACY.serialize(title));
    }

    public org.bukkit.inventory.Inventory createInventory(org.bukkit.inventory.InventoryHolder owner, int size, String title) {
        if (size % 9 != 0 || size < 9 || size > 54) {
            throw new IllegalArgumentException("Chests must have a size that is a multiple of 9 between 9 and 54 slots (got " + size + ")");
        }
        return new Inventories.InventoryImpl(owner, org.bukkit.event.inventory.InventoryType.CHEST, size, title);
    }

    public org.bukkit.boss.BossBar createBossBar(String title, org.bukkit.boss.BarColor color, org.bukkit.boss.BarStyle style, org.bukkit.boss.BarFlag... flags) {
        return new BossBars.BarImpl(title, color, style, flags);
    }

    public org.bukkit.boss.KeyedBossBar createBossBar(org.bukkit.NamespacedKey key, String title, org.bukkit.boss.BarColor color, org.bukkit.boss.BarStyle style, org.bukkit.boss.BarFlag... flags) {
        BossBars.KeyedBarImpl bar = new BossBars.KeyedBarImpl(key, title, color, style, flags);
        keyedBossBars.put(key, bar);
        return bar;
    }

    public java.util.Iterator<org.bukkit.boss.KeyedBossBar> getBossBars() {
        return new ArrayList<org.bukkit.boss.KeyedBossBar>(keyedBossBars.values()).iterator();
    }

    public org.bukkit.boss.KeyedBossBar getBossBar(org.bukkit.NamespacedKey key) {
        return keyedBossBars.get(key);
    }

    public boolean removeBossBar(org.bukkit.NamespacedKey key) {
        BossBars.KeyedBarImpl bar = keyedBossBars.remove(key);
        if (bar != null) {
            bar.removeAll();
            return true;
        }
        return false;
    }

    public org.bukkit.block.data.BlockData createBlockData(org.bukkit.Material material) {
        return BlockDataImpl.of(material);
    }

    public org.bukkit.block.data.BlockData createBlockData(org.bukkit.Material material, java.util.function.Consumer<? super org.bukkit.block.data.BlockData> consumer) {
        org.bukkit.block.data.BlockData data = createBlockData(material);
        if (consumer != null) {
            consumer.accept(data);
        }
        return data;
    }

    public org.bukkit.block.data.BlockData createBlockData(String data) {
        return BlockDataImpl.fromString(data);
    }

    public org.bukkit.block.data.BlockData createBlockData(org.bukkit.Material material, String data) {
        return BlockDataImpl.fromString(material == null ? data : material.getKey().asString() + (data == null ? "" : data));
    }

    @SuppressWarnings("unchecked")
    public <T extends org.bukkit.Keyed> org.bukkit.Registry<T> getRegistry(Class<T> type) {
        Registries.load(null);
        return (org.bukkit.Registry<T>) Registries.byType(type);
    }

    public org.bukkit.entity.Entity getEntity(UUID uuid) {
        return overworld.getEntity(uuid);
    }

    private final Map<String, org.bukkit.scoreboard.Criteria> criteria = new ConcurrentHashMap<>();

    public org.bukkit.scoreboard.Criteria getScoreboardCriteria(String name) {
        return criteria.computeIfAbsent(name, n -> new org.bukkit.scoreboard.Criteria() {
            @Override
            public String getName() {
                return n;
            }

            @Override
            public boolean isReadOnly() {
                return n.equals("health") || n.equals("deathCount") || n.equals("playerKillCount") || n.equals("totalKillCount")
                        || n.equals("xp") || n.equals("level") || n.equals("food") || n.equals("air") || n.equals("armor") || n.startsWith("minecraft.");
            }

            @Override
            public org.bukkit.scoreboard.RenderType getDefaultRenderType() {
                return n.equals("health") ? org.bukkit.scoreboard.RenderType.HEARTS : org.bukkit.scoreboard.RenderType.INTEGER;
            }

            @Override
            public String toString() {
                return "Criteria{" + n + "}";
            }
        });
    }

    public org.bukkit.inventory.Recipe getRecipe(org.bukkit.NamespacedKey key) {
        return null;
    }

    public org.bukkit.inventory.Recipe getCraftingRecipe(org.bukkit.inventory.ItemStack[] matrix, World world) {
        return null;
    }

    public boolean addRecipe(org.bukkit.inventory.Recipe recipe) {
        return false;
    }

    public boolean removeRecipe(org.bukkit.NamespacedKey key) {
        return false;
    }

    public java.util.Iterator<org.bukkit.inventory.Recipe> recipeIterator() {
        return java.util.Collections.emptyIterator();
    }

    public org.bukkit.ServerTickManager getServerTickManager() {
        return ProxyBase.create(org.bukkit.ServerTickManager.class, new TickManagerImpl());
    }

    public org.bukkit.util.CachedServerIcon loadServerIcon(java.io.File file) {
        return null;
    }

    public org.bukkit.util.CachedServerIcon loadServerIcon(java.awt.image.BufferedImage image) {
        return null;
    }

    public org.bukkit.map.MapView createMap(World world) {
        throw new UnsupportedOperationException("CraftPP does not implement maps yet");
    }

    public org.bukkit.inventory.MenuType getMenuType(org.bukkit.NamespacedKey key) {
        return null;
    }

    /** ItemFactory: metas are ItemMetaImpl proxies regardless of material. */
    static final class ItemFactoryImpl {
        public org.bukkit.inventory.meta.ItemMeta getItemMeta(org.bukkit.Material material) {
            return material == null || material.isAir() ? null : ItemMetaImpl.create();
        }

        public boolean isApplicable(org.bukkit.inventory.meta.ItemMeta meta, org.bukkit.inventory.ItemStack stack) {
            return meta == null || ItemMetaImpl.unwrap(meta) != null;
        }

        public boolean isApplicable(org.bukkit.inventory.meta.ItemMeta meta, org.bukkit.Material material) {
            return meta == null || ItemMetaImpl.unwrap(meta) != null;
        }

        public boolean equals(org.bukkit.inventory.meta.ItemMeta first, org.bukkit.inventory.meta.ItemMeta second) {
            return java.util.Objects.equals(ItemMetaImpl.unwrap(first), ItemMetaImpl.unwrap(second));
        }

        public org.bukkit.inventory.meta.ItemMeta asMetaFor(org.bukkit.inventory.meta.ItemMeta meta, org.bukkit.inventory.ItemStack stack) {
            return meta;
        }

        public org.bukkit.inventory.meta.ItemMeta asMetaFor(org.bukkit.inventory.meta.ItemMeta meta, org.bukkit.Material material) {
            return meta;
        }

        public org.bukkit.Color getDefaultLeatherColor() {
            return org.bukkit.Color.fromRGB(0xA0, 0x65, 0x40);
        }

        public org.bukkit.inventory.ItemStack createItemStack(String input) {
            String key = input.contains("[") ? input.substring(0, input.indexOf('[')) : input;
            org.bukkit.Material material = org.bukkit.Material.matchMaterial(key);
            if (material == null) {
                throw new IllegalArgumentException("Unknown item " + input);
            }
            return new PrismItemStack(material, 1);
        }

        public org.bukkit.Material getSpawnEgg(org.bukkit.entity.EntityType type) {
            return org.bukkit.Material.matchMaterial(type.getKey().getKey() + "_spawn_egg");
        }

        public org.bukkit.inventory.ItemStack enchantItem(org.bukkit.inventory.ItemStack item, int level, boolean allowTreasures) {
            return item;
        }

        public org.bukkit.inventory.ItemStack enchantItem(org.bukkit.entity.Entity entity, org.bukkit.inventory.ItemStack item, int level, boolean allowTreasures) {
            return item;
        }

        public org.bukkit.inventory.ItemStack enchantItem(World world, org.bukkit.inventory.ItemStack item, int level, boolean allowTreasures) {
            return item;
        }

        public Component displayName(org.bukkit.inventory.ItemStack stack) {
            return PrismItemStack.of(stack).effectiveName();
        }

        public String getI18NDisplayName(org.bukkit.inventory.ItemStack stack) {
            return stack.getType().name().toLowerCase(java.util.Locale.ROOT).replace('_', ' ');
        }

        public org.bukkit.inventory.ItemStack ensureServerConversions(org.bukkit.inventory.ItemStack stack) {
            return PrismItemStack.of(stack);
        }

        public net.kyori.adventure.text.event.HoverEvent<net.kyori.adventure.text.event.HoverEvent.ShowItem> asHoverEvent(org.bukkit.inventory.ItemStack stack, java.util.function.UnaryOperator<net.kyori.adventure.text.event.HoverEvent.ShowItem> op) {
            return net.kyori.adventure.text.event.HoverEvent.showItem(op.apply(net.kyori.adventure.text.event.HoverEvent.ShowItem.showItem(stack.getType().getKey(), stack.getAmount())));
        }
    }

    static final class TickManagerImpl {
        public boolean isRunningNormally() {
            return true;
        }

        public float getTickRate() {
            return 20;
        }

        public void setTickRate(float rate) {
        }

        public boolean isFrozen() {
            return false;
        }

        public void setFrozen(boolean frozen) {
        }

        public boolean isStepping() {
            return false;
        }

        public boolean isSprinting() {
            return false;
        }

        public boolean isFrozen(org.bukkit.entity.Entity entity) {
            return false;
        }

        public int getFrozenTicksToRun() {
            return 0;
        }

        public float getRemainingSprintTicks() {
            return 0;
        }
    }

    public Server.Spigot spigot() {
        return new Server.Spigot();
    }

    JsonObject configSnapshot() {
        return config;
    }

    /** Minimal UnsafeValues: enough for ItemStack construction and version checks. */
    static final class UnsafeImpl {
        public org.bukkit.Material fromLegacy(org.bukkit.material.MaterialData material, boolean itemPriority) {
            return material.getItemType();
        }

        public org.bukkit.Material fromLegacy(org.bukkit.Material material) {
            return material;
        }

        public org.bukkit.Material toLegacy(org.bukkit.Material material) {
            return material;
        }

        public int getDataVersion() {
            return 4900;
        }

        public String getTimingsServerName() {
            return "CraftPP";
        }

        public boolean isSupportedApiVersion(String apiVersion) {
            return true;
        }

        public void checkSupported(org.bukkit.plugin.PluginDescriptionFile description) {
        }

        public org.bukkit.inventory.ItemStack createEmptyStack() {
            return new PrismItemStack();
        }

        public int nextEntityId() {
            return java.util.concurrent.ThreadLocalRandom.current().nextInt(1 << 20, Integer.MAX_VALUE);
        }

        public byte[] processClass(org.bukkit.plugin.PluginDescriptionFile description, String path, byte[] clazz) {
            return clazz;
        }
    }

    /** Minimal PlayerProfile backing. */
    static final class ProfileImpl {
        private final UUID uuid;
        private final String name;

        ProfileImpl(UUID uuid, String name) {
            this.uuid = uuid;
            this.name = name;
        }

        public UUID getId() {
            return uuid;
        }

        public UUID getUniqueId() {
            return uuid;
        }

        public String getName() {
            return name;
        }

        public java.util.Set<com.destroystokyo.paper.profile.ProfileProperty> getProperties() {
            return new java.util.HashSet<>();
        }

        public boolean hasProperty(String property) {
            return false;
        }

        public boolean isComplete() {
            return uuid != null && name != null;
        }

        public boolean completeFromCache() {
            return isComplete();
        }

        public boolean complete(boolean textures) {
            return isComplete();
        }

        public boolean complete() {
            return isComplete();
        }

        @Override
        public String toString() {
            return "PlayerProfile{" + uuid + ", " + name + "}";
        }
    }
}
