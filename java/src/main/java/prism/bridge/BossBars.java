package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import net.kyori.adventure.bossbar.BossBar;
import org.bukkit.NamespacedKey;
import org.bukkit.boss.BarColor;
import org.bukkit.boss.BarFlag;
import org.bukkit.boss.BarStyle;
import org.bukkit.boss.KeyedBossBar;
import org.bukkit.entity.Player;

import java.util.ArrayList;
import java.util.EnumSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;

/** Bukkit boss bars, plus the Adventure BossBar bridge used by Audience.showBossBar. */
final class BossBars {
    private BossBars() {
    }

    static int colorId(BarColor color) {
        return color.ordinal(); // pink, blue, red, green, yellow, purple, white - same order as vanilla.
    }

    static int styleId(BarStyle style) {
        return style.ordinal(); // solid, 6, 10, 12, 20.
    }

    static int adventureColor(BossBar.Color color) {
        return switch (color) {
            case PINK -> 0;
            case BLUE -> 1;
            case RED -> 2;
            case GREEN -> 3;
            case YELLOW -> 4;
            case PURPLE -> 5;
            default -> 6;
        };
    }

    static int adventureOverlay(BossBar.Overlay overlay) {
        return switch (overlay) {
            case NOTCHED_6 -> 1;
            case NOTCHED_10 -> 2;
            case NOTCHED_12 -> 3;
            case NOTCHED_20 -> 4;
            default -> 0;
        };
    }

    static int adventureFlags(Set<BossBar.Flag> flags) {
        int bits = 0;
        if (flags.contains(BossBar.Flag.DARKEN_SCREEN)) bits |= 1;
        if (flags.contains(BossBar.Flag.PLAY_BOSS_MUSIC)) bits |= 2;
        if (flags.contains(BossBar.Flag.CREATE_WORLD_FOG)) bits |= 4;
        return bits;
    }

    private static JsonArray viewers(Iterable<UUID> uuids) {
        JsonArray array = new JsonArray();
        for (UUID uuid : uuids) array.add(uuid.toString());
        return array;
    }

    /** Bukkit-style bar: state here, one set of viewers, packets through PrismMC. */
    static class BarImpl implements org.bukkit.boss.BossBar {
        final UUID id = UUID.randomUUID();
        String title;
        BarColor color;
        BarStyle style;
        final Set<BarFlag> flags = EnumSet.noneOf(BarFlag.class);
        double progress = 1.0;
        boolean visible = true;
        final Set<UUID> players = new LinkedHashSet<>();

        BarImpl(String title, BarColor color, BarStyle style, BarFlag... barFlags) {
            this.title = title == null ? "" : title;
            this.color = color == null ? BarColor.WHITE : color;
            this.style = style == null ? BarStyle.SOLID : style;
            for (BarFlag flag : barFlags) flags.add(flag);
        }

        private int flagBits() {
            int bits = 0;
            if (flags.contains(BarFlag.DARKEN_SKY)) bits |= 1;
            if (flags.contains(BarFlag.PLAY_BOSS_MUSIC)) bits |= 2;
            if (flags.contains(BarFlag.CREATE_FOG)) bits |= 4;
            return bits;
        }

        private JsonObject full() {
            JsonObject args = new JsonObject();
            args.addProperty("id", id.toString());
            args.addProperty("title", title);
            args.addProperty("progress", (float) progress);
            args.addProperty("color", colorId(color));
            args.addProperty("style", styleId(style));
            args.addProperty("flags", flagBits());
            return args;
        }

        private void showTo(Iterable<UUID> targets) {
            JsonObject args = full();
            args.add("viewers", viewers(targets));
            call("bossbar.show", args);
        }

        private void hideFrom(Iterable<UUID> targets) {
            JsonObject args = new JsonObject();
            args.addProperty("id", id.toString());
            args.add("viewers", viewers(targets));
            call("bossbar.hide", args);
        }

        private void update() {
            if (!visible || players.isEmpty()) return;
            JsonObject args = full();
            args.add("viewers", viewers(players));
            call("bossbar.update", args);
        }

        static void call(String method, JsonObject args) {
            JsonObject payload = new JsonObject();
            payload.addProperty("m", method);
            payload.add("a", args);
            Main.rpc.call("api", payload);
        }

        @Override
        public String getTitle() {
            return title;
        }

        @Override
        public void setTitle(String value) {
            title = value == null ? "" : value;
            update();
        }

        @Override
        public BarColor getColor() {
            return color;
        }

        @Override
        public void setColor(BarColor value) {
            color = value;
            update();
        }

        @Override
        public BarStyle getStyle() {
            return style;
        }

        @Override
        public void setStyle(BarStyle value) {
            style = value;
            update();
        }

        @Override
        public void removeFlag(BarFlag flag) {
            flags.remove(flag);
            update();
        }

        @Override
        public void addFlag(BarFlag flag) {
            flags.add(flag);
            update();
        }

        @Override
        public boolean hasFlag(BarFlag flag) {
            return flags.contains(flag);
        }

        @Override
        public void setProgress(double value) {
            progress = Math.max(0, Math.min(1, value));
            update();
        }

        @Override
        public double getProgress() {
            return progress;
        }

        @Override
        public void addPlayer(Player player) {
            if (players.add(player.getUniqueId()) && visible) {
                showTo(List.of(player.getUniqueId()));
            }
        }

        @Override
        public void removePlayer(Player player) {
            if (players.remove(player.getUniqueId())) {
                hideFrom(List.of(player.getUniqueId()));
            }
        }

        @Override
        public void removeAll() {
            hideFrom(new ArrayList<>(players));
            players.clear();
        }

        @Override
        public List<Player> getPlayers() {
            List<Player> list = new ArrayList<>();
            for (UUID uuid : players) {
                Player player = org.bukkit.Bukkit.getPlayer(uuid);
                if (player != null) list.add(player);
            }
            return list;
        }

        @Override
        public void setVisible(boolean value) {
            if (visible == value) return;
            visible = value;
            if (visible) showTo(players);
            else hideFrom(players);
        }

        @Override
        public boolean isVisible() {
            return visible;
        }

        @Override
        public void show() {
            setVisible(true);
        }

        @Override
        public void hide() {
            setVisible(false);
        }
    }

    static final class KeyedBarImpl extends BarImpl implements KeyedBossBar {
        private final NamespacedKey key;

        KeyedBarImpl(NamespacedKey key, String title, BarColor color, BarStyle style, BarFlag... flags) {
            super(title, color, style, flags);
            this.key = key;
        }

        @Override
        public NamespacedKey getKey() {
            return key;
        }
    }

    /** Adventure BossBar shown to one player: PrismMC packets keyed by the bar's identity. */
    static final class AdventureBridge implements BossBar.Listener {
        private final BossBar bar;
        private final UUID id = UUID.randomUUID();
        private final Set<UUID> viewers = new LinkedHashSet<>();

        AdventureBridge(BossBar bar) {
            this.bar = bar;
            bar.addListener(this);
        }

        private JsonObject full() {
            JsonObject args = new JsonObject();
            args.addProperty("id", id.toString());
            args.addProperty("title", ServerImpl.LEGACY.serialize(bar.name()));
            args.addProperty("progress", bar.progress());
            args.addProperty("color", adventureColor(bar.color()));
            args.addProperty("style", adventureOverlay(bar.overlay()));
            args.addProperty("flags", adventureFlags(bar.flags()));
            return args;
        }

        void show(UUID viewer) {
            viewers.add(viewer);
            JsonObject args = full();
            args.add("viewers", viewers(List.of(viewer)));
            BarImpl.call("bossbar.show", args);
        }

        void hide(UUID viewer) {
            if (viewers.remove(viewer)) {
                JsonObject args = new JsonObject();
                args.addProperty("id", id.toString());
                args.add("viewers", viewers(List.of(viewer)));
                BarImpl.call("bossbar.hide", args);
            }
            if (viewers.isEmpty()) {
                bar.removeListener(this);
            }
        }

        boolean isEmpty() {
            return viewers.isEmpty();
        }

        private void update() {
            if (viewers.isEmpty()) return;
            JsonObject args = full();
            args.add("viewers", viewers(viewers));
            BarImpl.call("bossbar.update", args);
        }

        @Override
        public void bossBarNameChanged(BossBar bar, net.kyori.adventure.text.Component oldName, net.kyori.adventure.text.Component newName) {
            update();
        }

        @Override
        public void bossBarProgressChanged(BossBar bar, float oldProgress, float newProgress) {
            update();
        }

        @Override
        public void bossBarColorChanged(BossBar bar, BossBar.Color oldColor, BossBar.Color newColor) {
            update();
        }

        @Override
        public void bossBarOverlayChanged(BossBar bar, BossBar.Overlay oldOverlay, BossBar.Overlay newOverlay) {
            update();
        }

        @Override
        public void bossBarFlagsChanged(BossBar bar, Set<BossBar.Flag> flagsAdded, Set<BossBar.Flag> flagsRemoved) {
            update();
        }
    }
}
