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
    private boolean cancelRuleChange, rewriteRuleChange;

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
                if (args.length > 0 && args[0].equals("rulesapi")) {
                    org.bukkit.World world = Bukkit.getWorlds().getFirst();
                    check(world.getGameRules().length == 59 && org.bukkit.GameRule.values().length == 59, "59 real registry rules");
                    for (org.bukkit.GameRule<?> rule : org.bukkit.GameRule.values()) {
                        check(rule.getDefaultValue() != null && world.getGameRuleValue(rule) != null, "typed rule readable: " + rule);
                        check(world.getGameRuleDefault(rule).equals(rule.getDefaultValue()), "default matches registry: " + rule);
                    }
                    check(world.isGameRule("random_tick_speed") && world.isGameRule("minecraft:random_tick_speed")
                            && !world.isGameRule("not_a_rule"), "canonical and namespaced lookup");
                    check(!world.setGameRuleValue("not_a_rule", "true") && !world.setGameRuleValue("random_tick_speed", "-1")
                            && !world.setGameRuleValue("random_tick_speed", "2147483648")
                            && !world.setGameRuleValue("random_tick_speed", "1junk")
                            && !world.setGameRuleValue("random_tick_speed", " 1")
                            && !world.setGameRuleValue("keep_inventory", "banana"), "invalid rule writes rejected");
                    check(world.getGameRuleValue(org.bukkit.GameRules.RANDOM_TICK_SPEED) == 3, "invalid writes unchanged");
                    check(world.setGameRule(org.bukkit.GameRules.RANDOM_TICK_SPEED, 7), "typed integer setter");
                    check(world.getGameRuleValue(org.bukkit.GameRules.RANDOM_TICK_SPEED) == 7
                            && world.getGameRuleDefault(org.bukkit.GameRules.RANDOM_TICK_SPEED) == 3, "default independent of current value");
                    cancelRuleChange = true;
                    check(!world.setGameRule(org.bukkit.GameRules.KEEP_INVENTORY, true)
                            && !world.getGameRuleValue(org.bukkit.GameRules.KEEP_INVENTORY), "event cancels mutation");
                    cancelRuleChange = false;
                    rewriteRuleChange = true;
                    check(world.setGameRule(org.bukkit.GameRules.RANDOM_TICK_SPEED, 9)
                            && world.getGameRuleValue(org.bukkit.GameRules.RANDOM_TICK_SPEED) == 11, "event rewrites value");
                    rewriteRuleChange = false;
                    check(world.setGameRule(org.bukkit.GameRule.DISABLE_RAIDS, true)
                            && !world.getGameRuleValue(org.bukkit.GameRules.RAIDS)
                            && world.getGameRuleValue(org.bukkit.GameRule.DISABLE_RAIDS), "legacy inverted boolean conversion");
                    check(world.setGameRule(org.bukkit.GameRule.DO_FIRE_TICK, false)
                            && world.getGameRuleValue(org.bukkit.GameRules.FIRE_SPREAD_RADIUS_AROUND_PLAYER) == 0,
                            "legacy boolean-to-integer conversion");
                    check(world.setGameRule(org.bukkit.GameRules.ADVANCE_TIME, false), "pause clock rule");
                    check(world.setGameRule(org.bukkit.GameRules.IMMEDIATE_RESPAWN, true), "respawn client sync");
                    check(world.setGameRule(org.bukkit.GameRules.LIMITED_CRAFTING, true), "crafting client sync");
                    check(world.setGameRule(org.bukkit.GameRules.REDUCED_DEBUG_INFO, true), "debug client sync");
                    long full = world.getFullTime(), age = world.getGameTime();
                    getServer().getScheduler().runTaskLater(this, () -> {
                        check(world.getFullTime() == full && world.getGameTime() > age, "paused daylight keeps game age ticking");
                        check(world.setGameRule(org.bukkit.GameRules.ADVANCE_TIME, true), "resume clock rule");
                        sender.sendMessage("rulesapi ok");
                    }, 3);
                }
                if (sender instanceof Player p && args.length > 0 && args[0].equals("entityapi")) {
                    org.bukkit.World world = p.getWorld();
                    Location at = new Location(world, 3.5, -60, 3.5, 90, 0);
                    org.bukkit.entity.Entity spawned = world.spawnEntity(at, org.bukkit.entity.EntityType.COW);
                    check(spawned instanceof org.bukkit.entity.Cow cow && cow.getType() == org.bukkit.entity.EntityType.COW, "spawned a Cow proxy");
                    org.bukkit.entity.Cow cow = (org.bukkit.entity.Cow) spawned;
                    check(cow.isValid() && !cow.isDead() && cow.getMaxHealth() == 10 && cow.getHealth() == 10, "cow vitals: " + cow.getHealth());
                    check(Math.abs(cow.getLocation().getX() - 3.5) < 1e-9 && cow.getWidth() == 0.9f && cow.hasAI(), "cow position, size, AI");
                    check(world.getEntities().contains(cow) && world.getLivingEntities().contains(cow), "world lists the cow");
                    cow.setCustomName("§6Bessie");
                    cow.setCustomNameVisible(true);
                    check("§6Bessie".equals(cow.getCustomName()) && cow.isCustomNameVisible(), "custom name round trip");
                    cow.setHealth(4);
                    check(cow.getHealth() == 4, "setHealth");
                    rejects(() -> cow.setHealth(11));
                    cow.setMaxHealth(30);
                    cow.setHealth(25);
                    check(cow.getMaxHealth() == 30 && cow.getHealth() == 25, "max health attribute");
                    cow.setSilent(true);
                    cow.setGlowing(true);
                    cow.setGravity(false);
                    cow.setInvulnerable(true);
                    cow.setAI(false);
                    check(cow.isSilent() && cow.isGlowing() && !cow.hasGravity() && cow.isInvulnerable() && !cow.hasAI(), "entity flags");
                    check(cow.addScoreboardTag("milk") && cow.getScoreboardTags().contains("milk") && !cow.addScoreboardTag("milk"), "scoreboard tags");
                    cow.setVelocity(new org.bukkit.util.Vector(0, 0.5, 0));
                    check(Math.abs(cow.getVelocity().getY() - 0.5) < 1e-9, "velocity");
                    check(cow.teleport(new Location(world, 6.5, -60, 6.5, 180, 0)) && Math.abs(cow.getZ() - 6.5) < 1e-9
                            && cow.getYaw() == 180, "entity teleport");
                    cow.setInvulnerable(false);
                    cow.damage(5);
                    check(cow.getHealth() == 20, "damage: " + cow.getHealth());

                    org.bukkit.entity.Zombie zombie = world.spawn(new Location(world, 0.5, -60, 8.5), org.bukkit.entity.Zombie.class);
                    org.bukkit.inventory.ItemStack helmet = new org.bukkit.inventory.ItemStack(Material.DIAMOND_HELMET);
                    zombie.getEquipment().setHelmet(helmet);
                    check(zombie.getEquipment().getHelmet().getType() == Material.DIAMOND_HELMET, "zombie equipment");
                    zombie.setTarget(cow);
                    check(cow.equals(zombie.getTarget()), "mob target");
                    zombie.remove();
                    check(!zombie.isValid() && !world.getEntities().contains(zombie), "remove");

                    org.bukkit.inventory.ItemStack sword = new org.bukkit.inventory.ItemStack(Material.DIAMOND_SWORD);
                    org.bukkit.inventory.meta.ItemMeta meta = sword.getItemMeta();
                    meta.setDisplayName("§bExcalibur");
                    meta.setLore(List.of("§7Line one", "Line two"));
                    meta.addEnchant(org.bukkit.enchantments.Enchantment.SHARPNESS, 5, true);
                    meta.setUnbreakable(true);
                    meta.addItemFlags(org.bukkit.inventory.ItemFlag.HIDE_ENCHANTS);
                    sword.setItemMeta(meta);
                    p.getInventory().setItem(4, sword);
                    org.bukkit.inventory.ItemStack back = p.getInventory().getItem(4);
                    org.bukkit.inventory.meta.ItemMeta got = back.getItemMeta();
                    check(back.getType() == Material.DIAMOND_SWORD && "§bExcalibur".equals(got.getDisplayName()), "item name survives the server: " + got.getDisplayName());
                    check(List.of("§7Line one", "Line two").equals(got.getLore()), "lore survives: " + got.getLore());
                    check(got.getEnchantLevel(org.bukkit.enchantments.Enchantment.SHARPNESS) == 5 && got.isUnbreakable()
                            && got.hasItemFlag(org.bukkit.inventory.ItemFlag.HIDE_ENCHANTS), "enchants, unbreakable, flags survive");
                    org.bukkit.entity.Item dropped = world.dropItem(new Location(world, 1.5, -59, 1.5), sword);
                    check("§bExcalibur".equals(dropped.getItemStack().getItemMeta().getDisplayName()), "dropped item keeps meta");
                    dropped.remove();

                    check(world.getBiome(0, -60, 0) == org.bukkit.block.Biome.PLAINS, "flat world biome");
                    world.setBiome(0, -60, 0, org.bukkit.block.Biome.DESERT);
                    check(world.getBiome(1, -59, 2) == org.bukkit.block.Biome.DESERT && world.getBiome(4, -60, 0) == org.bukkit.block.Biome.PLAINS,
                            "setBiome changes one 4x4x4 cell");

                    org.bukkit.OfflinePlayer ghost = Bukkit.getOfflinePlayer("Ghost");
                    ghost.setWhitelisted(true);
                    check(ghost.isWhitelisted() && Bukkit.getWhitelistedPlayers().contains(ghost), "whitelist");
                    ghost.setWhitelisted(false);
                    org.bukkit.ban.ProfileBanList bans = Bukkit.getBanList(org.bukkit.BanList.Type.PROFILE);
                    bans.addBan(Bukkit.createProfile(ghost.getUniqueId(), "Ghost"), "griefing", (java.util.Date) null, "CraftTest");
                    check(ghost.isBanned() && bans.getBanEntry(Bukkit.createProfile(ghost.getUniqueId(), "Ghost")).getReason().equals("griefing"), "ban");
                    bans.pardon(Bukkit.createProfile(ghost.getUniqueId(), "Ghost"));
                    check(!ghost.isBanned(), "pardon");
                    Bukkit.banIP("10.1.2.3");
                    check(Bukkit.getIPBans().contains("10.1.2.3"), "ip ban");
                    Bukkit.unbanIP("10.1.2.3");
                    check(!Bukkit.getIPBans().contains("10.1.2.3"), "ip pardon");

                    org.bukkit.scoreboard.Scoreboard board = Bukkit.getScoreboardManager().getNewScoreboard();
                    org.bukkit.scoreboard.Objective side = board.registerNewObjective("crafttest", org.bukkit.scoreboard.Criteria.DUMMY, "§aCraft");
                    side.setDisplaySlot(org.bukkit.scoreboard.DisplaySlot.SIDEBAR);
                    side.getScore("apples").setScore(42);
                    board.registerNewTeam("red").setPrefix("§c[R] ");
                    p.setScoreboard(board);
                    org.bukkit.boss.BossBar bar = Bukkit.createBossBar("§eBoss", org.bukkit.boss.BarColor.RED, org.bukkit.boss.BarStyle.SEGMENTED_10);
                    bar.setProgress(0.25);
                    bar.addPlayer(p);
                    p.playSound(p.getLocation(), org.bukkit.Sound.BLOCK_ANVIL_LAND, org.bukkit.SoundCategory.BLOCKS, 0.5f, 1.5f);
                    p.playSound(cow, org.bukkit.Sound.ENTITY_COW_AMBIENT, 1f, 1f);
                    p.spawnParticle(org.bukkit.Particle.DUST, p.getLocation(), 7, new org.bukkit.Particle.DustOptions(org.bukkit.Color.RED, 2f));
                    p.spawnParticle(org.bukkit.Particle.BLOCK, p.getLocation(), 3, Material.STONE.createBlockData());
                    rejects(() -> p.spawnParticle(org.bukkit.Particle.DUST, p.getLocation(), 1));
                    cow.remove();
                    sender.sendMessage("entityapi ok");
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
                    check(world.getGameRuleValue(org.bukkit.GameRules.RANDOM_TICK_SPEED) == 11
                            && !world.getGameRuleValue(org.bukkit.GameRules.RAIDS)
                            && world.getGameRuleValue(org.bukkit.GameRules.IMMEDIATE_RESPAWN), "game rules persisted");
                    check(world.getGameRuleDefault(org.bukkit.GameRules.RANDOM_TICK_SPEED) == 3, "registry default survives restart");
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
    public void onGameRule(io.papermc.paper.event.world.WorldGameRuleChangeEvent event) {
        if (cancelRuleChange) event.setCancelled(true);
        if (rewriteRuleChange && event.getGameRule().getKey().getKey().equals("random_tick_speed")) event.setValue("11");
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
