package prism.testplugin;

import io.papermc.paper.command.brigadier.Commands;
import io.papermc.paper.command.brigadier.argument.ArgumentTypes;
import io.papermc.paper.command.brigadier.argument.resolvers.selector.PlayerSelectorArgumentResolver;
import io.papermc.paper.event.player.AsyncChatEvent;
import io.papermc.paper.plugin.lifecycle.event.types.LifecycleEvents;
import net.kyori.adventure.bossbar.BossBar;
import net.kyori.adventure.text.Component;
import net.kyori.adventure.text.format.NamedTextColor;
import org.bukkit.Bukkit;
import org.bukkit.Material;
import org.bukkit.NamespacedKey;
import org.bukkit.Registry;
import org.bukkit.Sound;
import org.bukkit.block.data.BlockData;
import org.bukkit.block.data.type.Stairs;
import org.bukkit.command.Command;
import org.bukkit.command.CommandSender;
import org.bukkit.enchantments.Enchantment;
import org.bukkit.entity.EntityType;
import org.bukkit.entity.LivingEntity;
import org.bukkit.entity.Player;
import org.bukkit.event.EventHandler;
import org.bukkit.event.Listener;
import org.bukkit.event.block.BlockBreakEvent;
import org.bukkit.event.inventory.InventoryClickEvent;
import org.bukkit.event.player.PlayerInteractEvent;
import org.bukkit.event.player.PlayerJoinEvent;
import org.bukkit.event.player.PlayerQuitEvent;
import org.bukkit.inventory.Inventory;
import org.bukkit.inventory.ItemStack;
import org.bukkit.inventory.meta.ItemMeta;
import org.bukkit.persistence.PersistentDataType;
import org.bukkit.plugin.java.JavaPlugin;
import org.bukkit.potion.PotionEffectType;
import org.bukkit.scoreboard.Criteria;
import org.bukkit.scoreboard.DisplaySlot;
import org.bukkit.scoreboard.Objective;

import java.util.List;

public final class PrismTestPlugin extends JavaPlugin implements Listener {
    private int ticks;
    private final NamespacedKey visitsKey = new NamespacedKey("prismtest", "visits");

    @Override
    public void onLoad() {
        getLogger().info("onLoad on " + Bukkit.getName() + " " + Bukkit.getVersion());
    }

    @Override
    public void onEnable() {
        getServer().getPluginManager().registerEvents(this, this);
        getServer().getScheduler().runTaskTimer(this, () -> ticks++, 1L, 1L);
        getServer().getScheduler().runTaskLater(this, () -> getLogger().info("delayed task ran after " + ticks + " ticks"), 5L);
        saveDefaultConfig();
        getLogger().info("registries: sharpness max " + Enchantment.SHARPNESS.getMaxLevel() + ", speed=" + PotionEffectType.SPEED.getName()
                + ", sounds=" + Registry.SOUNDS.size() + ", plains=" + Registry.BIOME.get(NamespacedKey.minecraft("plains")) + ", click=" + Sound.UI_BUTTON_CLICK.getKey());
        getLifecycleManager().registerEventHandler(LifecycleEvents.COMMANDS, event -> event.registrar().register(
                Commands.literal("prismbrig")
                        .executes(context -> {
                            context.getSource().getSender().sendMessage("brig ok");
                            return 1;
                        })
                        .then(Commands.argument("target", ArgumentTypes.player())
                                .then(Commands.argument("times", com.mojang.brigadier.arguments.IntegerArgumentType.integer(1, 10))
                                        .executes(context -> {
                                            Player target = context.getArgument("target", PlayerSelectorArgumentResolver.class).resolve(context.getSource()).get(0);
                                            int times = com.mojang.brigadier.arguments.IntegerArgumentType.getInteger(context, "times");
                                            context.getSource().getSender().sendMessage("brig target " + target.getName() + " x" + times);
                                            return 1;
                                        })))
                        .build(), "Brigadier smoke command", List.of("pb")));
        getLogger().info("enabled; primary thread=" + Bukkit.isPrimaryThread());
    }

    @Override
    public void onDisable() {
        getLogger().info("disabled after " + ticks + " ticks");
    }

    @Override
    public boolean onCommand(CommandSender sender, Command command, String label, String[] args) {
        switch (command.getName()) {
            case "prismop" -> {
                sender.sendMessage("prismop ok");
                return true;
            }
            case "prismgui" -> {
                if (!(sender instanceof Player player)) {
                    sender.sendMessage("players only");
                    return true;
                }
                Inventory gui = Bukkit.createInventory(null, 27, "§bPrism GUI");
                ItemStack diamond = new ItemStack(Material.DIAMOND, 3);
                ItemMeta meta = diamond.getItemMeta();
                meta.setDisplayName("§6Shiny");
                meta.setLore(List.of("§7Click me"));
                meta.addEnchant(Enchantment.SHARPNESS, 2, true);
                diamond.setItemMeta(meta);
                gui.setItem(13, diamond);
                player.openInventory(gui);
                sender.sendMessage("gui opened with " + gui.getItem(13).getItemMeta().getDisplayName());
                return true;
            }
            case "prismnms" -> {
                if (!(sender instanceof Player player)) {
                    sender.sendMessage("players only");
                    return true;
                }
                sender.sendMessage(NmsProbe.run(player));
                return true;
            }
            case "prismspawn" -> {
                if (!(sender instanceof Player player)) {
                    sender.sendMessage("players only");
                    return true;
                }
                LivingEntity zombie = (LivingEntity) player.getWorld().spawnEntity(player.getLocation(), EntityType.ZOMBIE);
                zombie.setCustomName("§cBob");
                zombie.setCustomNameVisible(true);
                zombie.getPersistentDataContainer().set(visitsKey, PersistentDataType.INTEGER, 7);
                Objective objective = Bukkit.getScoreboardManager().getMainScoreboard().getObjective("prism") != null
                        ? Bukkit.getScoreboardManager().getMainScoreboard().getObjective("prism")
                        : Bukkit.getScoreboardManager().getMainScoreboard().registerNewObjective("prism", Criteria.DUMMY, Component.text("Prism", NamedTextColor.GOLD));
                objective.setDisplaySlot(DisplaySlot.SIDEBAR);
                objective.getScore(player.getName()).setScore(42);
                BossBar bar = BossBar.bossBar(Component.text("Prism bar"), 0.5f, BossBar.Color.BLUE, BossBar.Overlay.NOTCHED_10);
                player.showBossBar(bar);
                int visits = player.getPersistentDataContainer().getOrDefault(visitsKey, PersistentDataType.INTEGER, 0) + 1;
                player.getPersistentDataContainer().set(visitsKey, PersistentDataType.INTEGER, visits);
                BlockData stairs = Bukkit.createBlockData(Material.OAK_STAIRS);
                ((Stairs) stairs).setFacing(org.bukkit.block.BlockFace.EAST);
                ((Stairs) stairs).setHalf(org.bukkit.block.data.Bisected.Half.TOP);
                BlockData below = player.getLocation().subtract(0, 1, 0).getBlock().getBlockData();
                sender.sendMessage("spawned " + zombie.getType() + " id=" + zombie.getEntityId() + " name=" + zombie.getCustomName()
                        + " visits=" + visits + " stairs=" + stairs.getAsString() + " below=" + below.getMaterial()
                        + " chunk=" + player.getChunk().getX() + "," + player.getChunk().getZ() + " border=" + (int) player.getWorld().getWorldBorder().getSize()
                        + " biome=" + player.getLocation().getBlock().getBiome());
                return true;
            }
            default -> {
                sender.sendMessage(Component.text("prismtest ok: " + String.join(" ", args), NamedTextColor.GREEN));
                if (sender instanceof Player player) {
                    sender.sendMessage("you are at " + player.getLocation().getBlockX() + "," + player.getLocation().getBlockY() + "," + player.getLocation().getBlockZ()
                            + " in " + player.getWorld().getName() + " gm=" + player.getGameMode() + " op=" + player.isOp());
                }
                return true;
            }
        }
    }

    @EventHandler
    public void onJoin(PlayerJoinEvent event) {
        event.joinMessage(Component.text("[PrismTest] " + event.getPlayer().getName() + " joined", NamedTextColor.AQUA));
        getLogger().info("join event for " + event.getPlayer().getName() + " at " + event.getPlayer().getLocation());
    }

    @EventHandler
    public void onQuit(PlayerQuitEvent event) {
        event.quitMessage(Component.text("[PrismTest] " + event.getPlayer().getName() + " quit", NamedTextColor.GRAY));
    }

    @EventHandler
    public void onChat(AsyncChatEvent event) {
        event.message(Component.text("[chat-hooked] ").append(event.message()));
    }

    @EventHandler
    public void onBreak(BlockBreakEvent event) {
        if (event.getBlock().getType() == Material.BEDROCK || event.getBlock().getY() < -60) {
            event.setCancelled(true);
            event.getPlayer().sendMessage("[PrismTest] bedrock is protected");
        }
    }

    @EventHandler
    public void onClick(InventoryClickEvent event) {
        if (event.getView().getTitle().contains("Prism GUI")) {
            event.setCancelled(true);
            event.getWhoClicked().sendMessage("[PrismTest] clicked slot " + event.getRawSlot() + " " + event.getClick() + " item="
                    + (event.getCurrentItem() == null ? "none" : event.getCurrentItem().getType()));
        }
    }

    @EventHandler
    public void onInteract(PlayerInteractEvent event) {
        if (event.getClickedBlock() != null && event.getClickedBlock().getType() == Material.DIAMOND_BLOCK) {
            event.setCancelled(true);
            event.getPlayer().sendMessage("[PrismTest] diamond block interaction blocked");
        }
    }
}
