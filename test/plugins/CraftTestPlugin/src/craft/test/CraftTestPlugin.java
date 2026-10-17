package craft.test;

import org.bukkit.Bukkit;
import org.bukkit.ChatColor;
import org.bukkit.GameMode;
import org.bukkit.Location;
import org.bukkit.Material;
import org.bukkit.block.Block;
import org.bukkit.command.Command;
import org.bukkit.command.CommandSender;
import org.bukkit.entity.Player;
import org.bukkit.event.EventHandler;
import org.bukkit.event.Listener;
import org.bukkit.event.block.BlockBreakEvent;
import org.bukkit.event.block.BlockPlaceEvent;
import org.bukkit.event.player.AsyncPlayerChatEvent;
import org.bukkit.event.player.PlayerCommandPreprocessEvent;
import org.bukkit.event.player.PlayerJoinEvent;
import org.bukkit.event.player.PlayerMoveEvent;
import org.bukkit.event.player.PlayerQuitEvent;
import org.bukkit.plugin.java.JavaPlugin;
import org.bukkit.scheduler.BukkitRunnable;

import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

public final class CraftTestPlugin extends JavaPlugin implements Listener {
    private final AtomicInteger ticks = new AtomicInteger();

    private static void rejects(Runnable action) {
        try { action.run(); } catch (IllegalArgumentException expected) { return; }
        throw new AssertionError("Invalid API input was accepted");
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    @Override
    public void onLoad() {
        getLogger().info("onLoad on " + Bukkit.getName() + " " + Bukkit.getBukkitVersion());
    }

    @Override
    public void onEnable() {
        saveDefaultConfig();
        getServer().getPluginManager().registerEvents(this, this);
        getServer().getScheduler().runTaskTimer(this, ticks::incrementAndGet, 1L, 1L);
        getServer().getScheduler().runTaskLater(this, () -> getLogger().info("delayed task ran after " + ticks.get() + " ticks"), 10L);
        getLogger().info("enabled; greeting=" + getConfig().getString("greeting") + " primary=" + Bukkit.isPrimaryThread()
                + " version=" + Bukkit.getVersion());
    }

    @Override
    public void onDisable() {
        getLogger().info("disabled after " + ticks.get() + " ticks");
    }

    @Override
    public boolean onCommand(CommandSender sender, Command command, String label, String[] args) {
        switch (command.getName()) {
            case "crafttest" -> {
                String who = sender instanceof Player p ? p.getName() + " " + p.getUniqueId() + " gm=" + p.getGameMode()
                        + " op=" + p.isOp() + " world=" + p.getWorld().getName() : "console";
                sender.sendMessage(ChatColor.GREEN + "crafttest ok: " + String.join(" ", args) + " | " + who
                        + " | online=" + Bukkit.getOnlinePlayers().size());
                if (args.length > 0 && args[0].equals("worldapi")) {
                    org.bukkit.World world = Bukkit.getWorlds().getFirst();
                    long age = world.getGameTime();
                    world.setFullTime(9007199254740993L);
                    check(world.getFullTime() == 9007199254740993L, "64-bit time precision");
                    world.setFullTime(-1);
                    check(world.getTime() == 23999, "negative time normalization");
                    world.setTime(0);
                    check(world.getFullTime() == 0, "negative time rollover");
                    world.setFullTime(61000);
                    world.setTime(1000);
                    check(world.getFullTime() == 73000, "setTime rolls forward");
                    world.setTime(25000);
                    check(world.getFullTime() == 73000, "setTime normalizes input");
                    check(world.getGameTime() == age, "day time must not change game age");
                    world.setDifficulty(org.bukkit.Difficulty.HARD);
                    check(world.getDifficulty() == org.bukkit.Difficulty.HARD, "difficulty round trip");
                    check(world.setSpawnLocation(new Location(world, 12, 90, -7, 135, 0)), "spawn setter");
                    Location spawn = world.getSpawnLocation();
                    check(spawn.getBlockX() == 12 && spawn.getBlockY() == 90 && spawn.getBlockZ() == -7
                            && spawn.getYaw() == 135, "spawn round trip");
                    check(!world.setSpawnLocation(0, world.getMaxHeight(), 0), "invalid spawn rejected");
                    check(world.getSpawnLocation().getBlockX() == 12, "invalid spawn leaves state intact");
                    sender.sendMessage("worldapi ok");
                }
                if (args.length > 0 && args[0].equals("borderapi")) {
                    org.bukkit.World world = Bukkit.getWorlds().getFirst();
                    org.bukkit.WorldBorder border = world.getWorldBorder();
                    border.reset();
                    check(border.getSize() == border.getMaxSize() && border.getWarningTimeTicks() == 300, "border defaults");
                    border.setCenter(100.123456789, -50.987654321);
                    check(border.getCenter().getX() == 100.123456789 && border.getCenter().getZ() == -50.987654321,
                            "border center precision");
                    border.setCenter(100, -50);
                    border.setSize(200);
                    check(border.isInside(new Location(world, 0, 80, -150)), "minimum edge included");
                    check(!border.isInside(new Location(world, 200, 80, -50)), "maximum edge excluded");
                    check(!border.isInside(new Location(null, 100, 80, -50)), "wrong world rejected");
                    border.setDamageAmount(0.125);
                    border.setDamageBuffer(3.5);
                    border.setWarningDistance(9);
                    border.setWarningTime(2);
                    check(border.getWarningTimeTicks() == 40, "seconds converted to ticks");
                    border.setWarningTimeTicks(37);
                    check(border.getWarningTimeTicks() == 37 && border.getWarningTime() == 1, "subsecond warning precision");
                    check(border.getDamageAmount() == 0.125 && border.getDamageBuffer() == 3.5 && border.getWarningDistance() == 9,
                            "border settings round trip");
                    rejects(() -> border.setSize(0));
                    rejects(() -> border.setSize(Double.NaN));
                    rejects(() -> border.setCenter(30000000, 0));
                    rejects(() -> border.setWarningTimeTicks(-1));
                    rejects(() -> border.setDamageAmount(-1));
                    check(border.getSize() == 200 && border.getCenter().getX() == 100, "invalid writes preserve state");
                    border.changeSize(240, 100);
                    getServer().getScheduler().runTaskLater(this, () -> {
                        check(border.getSize() == 240, "millisecond resize completed");
                        border.setSize(260, 1L);
                        getServer().getScheduler().runTaskLater(this, () -> {
                            check(border.getSize() == 260, "seconds resize completed");
                            sender.sendMessage("borderapi ok");
                        }, 22);
                    }, 4);
                    Bukkit.setMotd("CraftPP plugin API test");
                    check(Bukkit.getMotd().equals("CraftPP plugin API test"), "MOTD round trip");
                    Bukkit.setDefaultGameMode(GameMode.ADVENTURE);
                    check(Bukkit.getDefaultGameMode() == GameMode.ADVENTURE, "default game mode round trip");
                }
                if (args.length > 0 && args[0].equals("lightapi")) {
                    org.bukkit.World world = Bukkit.getWorlds().getFirst();
                    Block torch = world.getBlockAt(40, -60, 40);
                    torch.setType(Material.TORCH);
                    getServer().getScheduler().runTaskLater(this, () -> {
                        check(torch.getLightFromBlocks() == 14, "torch block light");
                        check(world.getBlockAt(45, -60, 40).getLightFromBlocks() == 9, "light propagation");
                        check(world.getBlockAt(40, -61, 40).getLightFromSky() == 0, "opaque ground blocks sky");
                        check(torch.getLightFromSky() == 15, "open sky light");
                        torch.setType(Material.AIR);
                        getServer().getScheduler().runTaskLater(this, () -> {
                            check(torch.getLightFromBlocks() == 0, "removed torch clears light");
                            sender.sendMessage("lightapi ok");
                        }, 2);
                    }, 2);
                }
                if (args.length > 0 && args[0].equals("worldverify")) {
                    org.bukkit.World world = Bukkit.getWorlds().getFirst();
                    Location spawn = world.getSpawnLocation();
                    check(world.getFullTime() >= 73000 && world.getFullTime() < 100000, "clock persisted");
                    check(world.getGameTime() < world.getFullTime(), "separate game age persisted");
                    check(world.getDifficulty() == org.bukkit.Difficulty.HARD, "difficulty persisted");
                    check(spawn.getBlockX() == 12 && spawn.getBlockY() == 90 && spawn.getBlockZ() == -7
                            && spawn.getYaw() == 135, "spawn persisted");
                    org.bukkit.WorldBorder border = world.getWorldBorder();
                    check(border.getSize() == 260 && border.getCenter().getX() == 100 && border.getCenter().getZ() == -50,
                            "border position and size persisted");
                    check(border.getWarningTimeTicks() == 37 && border.getWarningDistance() == 9
                            && border.getDamageAmount() == 0.125 && border.getDamageBuffer() == 3.5, "border settings persisted");
                    sender.sendMessage("worldverify ok");
                }
                if (sender instanceof Player p && args.length > 0 && args[0].equals("creative")) {
                    p.setGameMode(GameMode.CREATIVE);
                }
                if (sender instanceof Player p && args.length > 0 && args[0].equals("tp")) {
                    p.teleport(new Location(p.getWorld(), 100.5, 80, 100.5));
                }
                if (sender instanceof Player p && args.length > 0 && args[0].equals("lookup")) {
                    Player byName = Bukkit.getPlayer(p.getName());
                    Player byId = Bukkit.getPlayer(p.getUniqueId());
                    sender.sendMessage("lookup " + (byName == p) + " " + (byId == p));
                }
                if (args.length > 0 && args[0].equals("broadcast")) {
                    Bukkit.broadcastMessage(ChatColor.GOLD + "broadcast from plugin");
                }
                if (sender instanceof Player p && args.length > 0 && args[0].equals("kick")) {
                    p.kickPlayer("kicked by crafttest");
                }
                return true;
            }
            case "craftop" -> {
                sender.sendMessage("craftop ok");
                return true;
            }
            case "craftblock" -> {
                if (!(sender instanceof Player p)) {
                    return false;
                }
                Location l = p.getLocation();
                Block block = p.getWorld().getBlockAt(l.getBlockX(), l.getBlockY() - 1, l.getBlockZ());
                Material before = block.getType();
                block.setType(Material.DIAMOND_BLOCK);
                sender.sendMessage("craftblock " + before + " -> " + block.getType());
                return true;
            }
            case "craftsched" -> {
                Bukkit.getScheduler().runTaskAsynchronously(this, () -> {
                    boolean primary = Bukkit.isPrimaryThread();
                    Bukkit.getScheduler().runTask(this, () -> sender.sendMessage("sched async=" + !primary + " back on main=" + Bukkit.isPrimaryThread()));
                });
                new BukkitRunnable() {
                    int runs;

                    @Override
                    public void run() {
                        if (++runs == 3) {
                            sender.sendMessage("sched repeating ran 3 times");
                            cancel();
                        }
                    }
                }.runTaskTimer(this, 1L, 2L);
                return true;
            }
            default -> {
                return false;
            }
        }
    }

    @Override
    public List<String> onTabComplete(CommandSender sender, Command command, String alias, String[] args) {
        return command.getName().equals("crafttest") && args.length == 1
                ? List.of("creative", "tp", "lookup", "broadcast", "kick").stream().filter(s -> s.startsWith(args[0])).toList()
                : List.of();
    }

    @EventHandler
    public void onJoin(PlayerJoinEvent event) {
        event.setJoinMessage(ChatColor.AQUA + "[CraftTest] " + event.getPlayer().getName() + " joined");
    }

    @EventHandler
    public void onQuit(PlayerQuitEvent event) {
        event.setQuitMessage(ChatColor.GRAY + "[CraftTest] " + event.getPlayer().getName() + " left");
    }

    @EventHandler
    public void onChat(AsyncPlayerChatEvent event) {
        if (event.getMessage().contains("forbidden")) {
            event.setCancelled(true);
            return;
        }
        event.setFormat("[%1$s] %2$s");
        event.setMessage(event.getMessage() + " (async=" + event.isAsynchronous() + ")");
    }

    @EventHandler
    public void onPreprocess(PlayerCommandPreprocessEvent event) {
        if (event.getMessage().startsWith("/blocked")) {
            event.setCancelled(true);
            event.getPlayer().sendMessage("blocked by preprocess");
        } else if (event.getMessage().startsWith("/rewrite-me")) {
            event.setMessage("/crafttest rewritten");
        }
    }

    @EventHandler
    public void onBreak(BlockBreakEvent event) {
        if (event.getBlock().getType() == Material.BEDROCK || event.getBlock().getType() == Material.DIAMOND_BLOCK) {
            event.setCancelled(true);
            event.getPlayer().sendMessage("break cancelled: " + event.getBlock().getType());
        }
    }

    @EventHandler
    public void onPlace(BlockPlaceEvent event) {
        if (event.getBlock().getY() > 100) {
            event.setCancelled(true);
            event.getPlayer().sendMessage("place cancelled at y=" + event.getBlock().getY());
        }
    }

    @EventHandler
    public void onMove(PlayerMoveEvent event) {
        Location to = event.getTo();
        if (to != null && to.getX() > 1000) { // invisible wall at x=1000
            event.setCancelled(true);
            event.getPlayer().sendMessage("move cancelled");
        }
    }
}
