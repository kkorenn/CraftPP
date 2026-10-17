package prism.bridge;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import io.papermc.paper.event.player.AsyncChatEvent;
import net.kyori.adventure.audience.Audience;
import net.kyori.adventure.chat.SignedMessage;
import net.kyori.adventure.text.Component;
import org.bukkit.Bukkit;
import org.bukkit.Location;
import org.bukkit.Material;
import org.bukkit.block.Block;
import org.bukkit.block.BlockState;
import org.bukkit.entity.Player;
import org.bukkit.event.Event;
import org.bukkit.event.block.BlockBreakEvent;
import org.bukkit.event.block.BlockPlaceEvent;
import org.bukkit.event.player.AsyncPlayerChatEvent;
import org.bukkit.event.player.AsyncPlayerPreLoginEvent;
import org.bukkit.event.player.PlayerCommandPreprocessEvent;
import org.bukkit.event.player.PlayerInteractEvent;
import org.bukkit.event.player.PlayerJoinEvent;
import org.bukkit.event.player.PlayerKickEvent;
import org.bukkit.event.player.PlayerLoginEvent;
import org.bukkit.event.player.PlayerMoveEvent;
import org.bukkit.event.player.PlayerQuitEvent;
import org.bukkit.event.server.ServerListPingEvent;
import org.bukkit.inventory.EquipmentSlot;
import org.bukkit.inventory.ItemStack;

import java.net.InetAddress;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

/** Builds Bukkit events from PrismMC's hook payloads and reports the outcome. */
final class Events {
    private Events() {
    }

    /** Hook name → event class, so PrismMC only pays for events somebody listens to. */
    static final Map<String, Class<? extends Event>> HOOKABLE = new LinkedHashMap<>();

    static {
        HOOKABLE.put("prelogin", AsyncPlayerPreLoginEvent.class);
        HOOKABLE.put("login", PlayerLoginEvent.class);
        HOOKABLE.put("join", PlayerJoinEvent.class);
        HOOKABLE.put("quit", PlayerQuitEvent.class);
        HOOKABLE.put("chat", AsyncChatEvent.class);
        HOOKABLE.put("legacychat", AsyncPlayerChatEvent.class);
        HOOKABLE.put("command", PlayerCommandPreprocessEvent.class);
        HOOKABLE.put("blockbreak", BlockBreakEvent.class);
        HOOKABLE.put("blockplace", BlockPlaceEvent.class);
        HOOKABLE.put("move", PlayerMoveEvent.class);
        HOOKABLE.put("ping", ServerListPingEvent.class);
        HOOKABLE.put("kick", PlayerKickEvent.class);
        HOOKABLE.put("interact", PlayerInteractEvent.class);
        HOOKABLE.put("interactentity", org.bukkit.event.player.PlayerInteractEntityEvent.class);
        HOOKABLE.put("attack", org.bukkit.event.entity.EntityDamageByEntityEvent.class);
        HOOKABLE.put("invclick", org.bukkit.event.inventory.InventoryClickEvent.class);
        HOOKABLE.put("invclose", org.bukkit.event.inventory.InventoryCloseEvent.class);
        HOOKABLE.put("helditem", org.bukkit.event.player.PlayerItemHeldEvent.class);
        HOOKABLE.put("drop", org.bukkit.event.player.PlayerDropItemEvent.class);
        HOOKABLE.put("chunkload", org.bukkit.event.world.ChunkLoadEvent.class);
        HOOKABLE.put("entitydeath", org.bukkit.event.entity.EntityDeathEvent.class);
    }

    private static Block blockOrNull(JsonObject p) {
        return p.has("x") ? block(p, "x", "y", "z") : null;
    }

    private static org.bukkit.block.BlockFace face(int face) {
        return switch (face) {
            case 0 -> org.bukkit.block.BlockFace.DOWN;
            case 1 -> org.bukkit.block.BlockFace.UP;
            case 2 -> org.bukkit.block.BlockFace.NORTH;
            case 3 -> org.bukkit.block.BlockFace.SOUTH;
            case 4 -> org.bukkit.block.BlockFace.WEST;
            case 5 -> org.bukkit.block.BlockFace.EAST;
            default -> org.bukkit.block.BlockFace.SELF;
        };
    }

    private static org.bukkit.entity.Entity entityRef(JsonObject p, String idKey, String playerKey) {
        if (p.has(playerKey) && !p.get(playerKey).isJsonNull()) {
            PlayerImpl player = Main.server.playerByUuid(p.get(playerKey).getAsString());
            if (player != null) {
                return player.proxy();
            }
        }
        EntityImpl entity = EntityImpl.byId(p.get(idKey).getAsInt());
        return entity == null ? null : entity.proxy();
    }

    static boolean isAsync(String name) {
        return name.equals("prelogin") || name.equals("chat") || name.equals("ping");
    }

    static void restorePluginData(PlayerImpl player, JsonObject snapshot) {
        player.applySnapshot(snapshot);
    }

    static JsonElement fire(String name, JsonObject p) {
        JsonObject result = new JsonObject();
        switch (name) {
            case "prelogin" -> {
                UUID uuid = UUID.fromString(p.get("uuid").getAsString());
                InetAddress address = address(p);
                AsyncPlayerPreLoginEvent event = new AsyncPlayerPreLoginEvent(p.get("name").getAsString(), address, address, uuid,
                        false, Bukkit.createProfile(uuid, p.get("name").getAsString()));
                call(event);
                result.addProperty("allow", event.getLoginResult() == AsyncPlayerPreLoginEvent.Result.ALLOWED);
                result.addProperty("message", legacy(event.kickMessage()));
            }
            case "login" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                PlayerLoginEvent event = new PlayerLoginEvent(player.proxy(), p.has("host") ? p.get("host").getAsString() : "", address(p));
                call(event);
                result.addProperty("allow", event.getResult() == PlayerLoginEvent.Result.ALLOWED);
                result.addProperty("message", legacy(event.kickMessage()));
                if (event.getResult() != PlayerLoginEvent.Result.ALLOWED) {
                    Main.server.removePlayer(player.getUniqueId());
                }
            }
            case "join" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                player.sendScoreboard();
                PlayerJoinEvent event = new PlayerJoinEvent(player.proxy(), Component.translatable("multiplayer.player.joined", Component.text(player.getName())));
                call(event);
                if (prism.bridge.nms.Nms.get() != null) {
                    prism.bridge.nms.Nms.get().afterJoin(player.getUniqueId()); // NMS plugins injected their pipeline handlers in the event.
                }
                Component message = event.joinMessage();
                result.add("message", message == null ? null : Rpc.GSON.toJsonTree(joinLeaveText(message, player.getName(), " joined the game")));
            }
            case "quit" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                PlayerQuitEvent event = new PlayerQuitEvent(player.proxy(), Component.translatable("multiplayer.player.left", Component.text(player.getName())), PlayerQuitEvent.QuitReason.DISCONNECTED);
                call(event);
                Component message = event.quitMessage();
                result.add("message", message == null ? null : Rpc.GSON.toJsonTree(joinLeaveText(message, player.getName(), " left the game")));
                Main.server.removePlayer(player.getUniqueId());
            }
            case "kick" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                PlayerKickEvent event = new PlayerKickEvent(player.proxy(), ServerImpl.LEGACY.deserialize(p.get("reason").getAsString()),
                        Component.translatable("multiplayer.player.left", Component.text(player.getName())), PlayerKickEvent.Cause.KICK_COMMAND);
                call(event);
                result.addProperty("cancelled", event.isCancelled());
                result.addProperty("reason", legacy(event.reason()));
                Component message = event.leaveMessage();
                result.add("message", Rpc.GSON.toJsonTree(joinLeaveText(message, player.getName(), " left the game")));
            }
            case "chat" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                String text = p.get("message").getAsString();
                Component message = Component.text(text);
                Set<Audience> viewers = new HashSet<>(Main.server.getOnlinePlayers());
                viewers.add(Bukkit.getConsoleSender());
                AsyncChatEvent event = new AsyncChatEvent(true, player.proxy(), viewers, io.papermc.paper.chat.ChatRenderer.defaultRenderer(),
                        message, message, SignedMessage.system(text, null));
                boolean cancelled = false;
                String line = null;
                if (Main.pluginManager.hasListeners(AsyncChatEvent.class)) {
                    call(event);
                    cancelled = event.isCancelled();
                    message = event.message();
                    if (!cancelled) {
                        Component rendered = event.renderer().render(player.proxy(), player.displayName(), message, Bukkit.getConsoleSender());
                        line = renderChat(rendered, player, message);
                    }
                }
                if (!cancelled && Main.pluginManager.hasListeners(AsyncPlayerChatEvent.class)) {
                    AsyncPlayerChatEvent legacyEvent = new AsyncPlayerChatEvent(true, player.proxy(), ServerImpl.LEGACY.serialize(message), new HashSet<>(Main.server.getOnlinePlayers()));
                    call(legacyEvent);
                    cancelled = legacyEvent.isCancelled();
                    if (!cancelled) {
                        line = String.format(legacyEvent.getFormat(), player.getDisplayName(), legacyEvent.getMessage());
                    }
                }
                result.addProperty("cancelled", cancelled);
                if (line != null) {
                    result.addProperty("line", line);
                }
            }
            case "command" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                PlayerCommandPreprocessEvent event = new PlayerCommandPreprocessEvent(player.proxy(), p.get("line").getAsString(), new HashSet<>(Main.server.getOnlinePlayers()));
                call(event);
                result.addProperty("cancelled", event.isCancelled());
                result.addProperty("line", event.getMessage());
            }
            case "blockbreak" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                Block block = block(p, "x", "y", "z");
                BlockBreakEvent event = new BlockBreakEvent(block, player.proxy());
                call(event);
                result.addProperty("cancelled", event.isCancelled());
            }
            case "blockplace" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                Block placed = block(p, "x", "y", "z");
                Block against = block(p, "ax", "ay", "az");
                BlockState replaced = ProxyBase.create(BlockState.class,
                        new BlockImpl.StateImpl((BlockImpl) ProxyBase.unwrap(placed), BlockImpl.materialFromKey(p.get("replaced").getAsString())));
                Material item = BlockImpl.materialFromKey(p.get("item").getAsString());
                ItemStack stack = new PrismItemStack(item, 1);
                BlockPlaceEvent event = new BlockPlaceEvent(placed, replaced, against, stack, player.proxy(), true, EquipmentSlot.HAND);
                call(event);
                result.addProperty("cancelled", event.isCancelled() || !event.canBuild());
            }
            case "move" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                Location from = location(p.getAsJsonObject("from"));
                Location to = location(p.getAsJsonObject("to"));
                PlayerMoveEvent event = new PlayerMoveEvent(player.proxy(), from, to);
                call(event);
                result.addProperty("cancelled", event.isCancelled());
                Location target = event.getTo();
                if (!event.isCancelled() && (target.getX() != to.getX() || target.getY() != to.getY() || target.getZ() != to.getZ()
                        || target.getYaw() != to.getYaw() || target.getPitch() != to.getPitch())) {
                    JsonObject moved = new JsonObject();
                    moved.addProperty("x", target.getX());
                    moved.addProperty("y", target.getY());
                    moved.addProperty("z", target.getZ());
                    moved.addProperty("yaw", target.getYaw());
                    moved.addProperty("pitch", target.getPitch());
                    result.add("to", moved);
                }
            }
            case "interact" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                org.bukkit.event.block.Action action = org.bukkit.event.block.Action.valueOf(p.get("action").getAsString());
                ItemStack item = p.has("item") && !p.get("item").isJsonNull() ? PrismItemStack.fromJson(p.get("item")) : null;
                Block block = blockOrNull(p);
                PlayerInteractEvent event = new PlayerInteractEvent(player.proxy(), action, item, block,
                        block == null ? org.bukkit.block.BlockFace.SELF : face(p.get("face").getAsInt()),
                        p.has("offHand") && p.get("offHand").getAsBoolean() ? EquipmentSlot.OFF_HAND : EquipmentSlot.HAND);
                call(event);
                result.addProperty("cancelled", event.useInteractedBlock() == org.bukkit.event.Event.Result.DENY
                        || event.useItemInHand() == org.bukkit.event.Event.Result.DENY);
            }
            case "interactentity" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                org.bukkit.entity.Entity target = entityRef(p, "targetId", "targetPlayer");
                if (target != null) {
                    org.bukkit.event.player.PlayerInteractEntityEvent event = new org.bukkit.event.player.PlayerInteractEntityEvent(player.proxy(), target,
                            p.has("offHand") && p.get("offHand").getAsBoolean() ? EquipmentSlot.OFF_HAND : EquipmentSlot.HAND);
                    call(event);
                    result.addProperty("cancelled", event.isCancelled());
                }
            }
            case "attack" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                org.bukkit.entity.Entity target = entityRef(p, "targetId", "targetPlayer");
                if (target != null) {
                    org.bukkit.event.entity.EntityDamageByEntityEvent event = new org.bukkit.event.entity.EntityDamageByEntityEvent(player.proxy(), target,
                            org.bukkit.event.entity.EntityDamageEvent.DamageCause.ENTITY_ATTACK, p.get("damage").getAsDouble());
                    call(event);
                    result.addProperty("cancelled", event.isCancelled());
                    result.addProperty("damage", event.getFinalDamage());
                }
            }
            case "invclick" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                int windowId = p.get("windowId").getAsInt();
                Inventories.ViewImpl view = player.openView();
                if (view == null || view.windowId != windowId) {
                    view = new Inventories.ViewImpl((Inventories.InventoryImpl) player.getInventory(), player, org.bukkit.event.inventory.InventoryType.PLAYER, "Inventory");
                }
                int rawSlot = p.get("slot").getAsInt();
                org.bukkit.event.inventory.ClickType click = Inventories.clickType(p.get("mode").getAsInt(), p.get("button").getAsInt());
                ItemStack current = p.has("current") && !p.get("current").isJsonNull() ? PrismItemStack.fromJson(p.get("current")) : null;
                org.bukkit.event.inventory.InventoryAction action = Inventories.action(click, current, view.getCursor());
                org.bukkit.event.inventory.InventoryClickEvent event = click == org.bukkit.event.inventory.ClickType.NUMBER_KEY
                        ? new org.bukkit.event.inventory.InventoryClickEvent(view, view.getSlotType(rawSlot), rawSlot, click, action, p.get("button").getAsInt())
                        : new org.bukkit.event.inventory.InventoryClickEvent(view, view.getSlotType(rawSlot), rawSlot, click, action);
                if (current != null) {
                    event.setCurrentItem(current);
                }
                call(event);
                result.addProperty("cancelled", event.isCancelled());
            }
            case "invclose" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                player.closedByClient(p.get("windowId").getAsInt());
            }
            case "helditem" -> {
                PlayerImpl player = Main.server.ensurePlayer(p);
                org.bukkit.event.player.PlayerItemHeldEvent event = new org.bukkit.event.player.PlayerItemHeldEvent(player.proxy(), p.get("previous").getAsInt(), p.get("slot").getAsInt());
                call(event);
                player.setHeldSlotCached(event.isCancelled() ? p.get("previous").getAsInt() : p.get("slot").getAsInt());
                result.addProperty("cancelled", event.isCancelled());
            }
            case "drop" -> {  // CraftPP already spawned the item; cancelling removes it and restores the slot
                PlayerImpl player = Main.server.ensurePlayer(p);
                EntityImpl entity = EntityImpl.byId(p.get("entityId").getAsInt());
                if (entity != null && entity.proxy() instanceof org.bukkit.entity.Item item) {
                    org.bukkit.event.player.PlayerDropItemEvent event = new org.bukkit.event.player.PlayerDropItemEvent(player.proxy(), item);
                    call(event);
                    result.addProperty("cancelled", event.isCancelled());
                }
            }
            case "chunkload" -> {
                org.bukkit.Chunk chunk = Main.server.overworld().getChunkAt(p.get("x").getAsInt(), p.get("z").getAsInt());
                call(new org.bukkit.event.world.ChunkLoadEvent(chunk, p.has("isNew") && p.get("isNew").getAsBoolean()));
            }
            case "entitydeath" -> {
                EntityImpl entity = EntityImpl.fromSnapshot(p);
                if (entity.proxy() instanceof org.bukkit.entity.LivingEntity living) {
                    call(new org.bukkit.event.entity.EntityDeathEvent(living, org.bukkit.damage.DamageSource.builder(org.bukkit.damage.DamageType.GENERIC).build(), new java.util.ArrayList<>()));
                }
                EntityImpl.forget(entity.id());
            }
            case "ping" -> {
                ServerListPingEvent event = new ServerListPingEvent(p.has("host") ? p.get("host").getAsString() : "", address(p),
                        p.get("motd").getAsString(), p.get("online").getAsInt(), p.get("max").getAsInt());
                call(event);
                result.addProperty("motd", legacy(event.motd()));
                result.addProperty("max", event.getMaxPlayers());
            }
            default -> throw new IllegalArgumentException("Unknown event hook " + name);
        }
        return result;
    }

    /** Typed PlayerMoveEvent (see Main.move): null accepts the move, else where to put the player. */
    static double[] move(java.util.UUID uuid, double fx, double fy, double fz, float fyaw, float fpitch,
                         double tx, double ty, double tz, float tyaw, float tpitch) {
        PlayerImpl player = Main.server.playerByUuid(uuid.toString());
        if (player == null) {
            return null;
        }
        org.bukkit.World world = Main.server.overworld();
        Location from = new Location(world, fx, fy, fz, fyaw, fpitch);
        Location to = new Location(world, tx, ty, tz, tyaw, tpitch);
        PlayerMoveEvent event = new PlayerMoveEvent(player.proxy(), from, to);
        call(event);
        Location target = event.isCancelled() ? from : event.getTo();
        if (!event.isCancelled() && target.getX() == tx && target.getY() == ty && target.getZ() == tz
                && target.getYaw() == tyaw && target.getPitch() == tpitch) {
            return null;
        }
        return new double[] {target.getX(), target.getY(), target.getZ(), target.getYaw(), target.getPitch()};
    }

    private static void call(Event event) {
        Bukkit.getPluginManager().callEvent(event);
    }

    private static String legacy(Component component) {
        return component == null ? "" : ServerImpl.LEGACY.serialize(component);
    }

    /** Vanilla join/leave messages are translatable; render them the way the client would. */
    private static String joinLeaveText(Component message, String name, String suffix) {
        if (message instanceof net.kyori.adventure.text.TranslatableComponent translatable
                && translatable.key().startsWith("multiplayer.player.")) {
            return "§e" + name + suffix;
        }
        return ServerImpl.LEGACY.serialize(message);
    }

    private static String renderChat(Component rendered, PlayerImpl player, Component message) {
        if (rendered instanceof net.kyori.adventure.text.TranslatableComponent translatable && translatable.key().equals("chat.type.text")) {
            return "<" + player.getDisplayName() + "> " + ServerImpl.LEGACY.serialize(message);
        }
        return ServerImpl.LEGACY.serialize(rendered);
    }

    private static InetAddress address(JsonObject p) {
        try {
            return InetAddress.getByName(p.has("ip") ? p.get("ip").getAsString() : "127.0.0.1");
        } catch (java.net.UnknownHostException e) {
            return InetAddress.getLoopbackAddress();
        }
    }

    private static Block block(JsonObject p, String xKey, String yKey, String zKey) {
        return Main.server.overworld().getBlockAt(p.get(xKey).getAsInt(), p.get(yKey).getAsInt(), p.get(zKey).getAsInt());
    }

    private static Location location(JsonObject o) {
        return new Location(Main.server.overworld(), o.get("x").getAsDouble(), o.get("y").getAsDouble(), o.get("z").getAsDouble(),
                o.get("yaw").getAsFloat(), o.get("pitch").getAsFloat());
    }
}
