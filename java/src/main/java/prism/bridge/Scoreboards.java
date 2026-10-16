package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import net.kyori.adventure.text.Component;
import net.kyori.adventure.text.format.NamedTextColor;
import net.kyori.adventure.text.format.TextColor;
import org.bukkit.ChatColor;
import org.bukkit.OfflinePlayer;
import org.bukkit.entity.Entity;
import org.bukkit.scoreboard.Criteria;
import org.bukkit.scoreboard.DisplaySlot;
import org.bukkit.scoreboard.NameTagVisibility;
import org.bukkit.scoreboard.Objective;
import org.bukkit.scoreboard.RenderType;
import org.bukkit.scoreboard.Score;
import org.bukkit.scoreboard.Scoreboard;
import org.bukkit.scoreboard.ScoreboardManager;
import org.bukkit.scoreboard.Team;

import java.util.ArrayList;
import java.util.Collection;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Scoreboards. State lives here; PrismMC only sends packets. The main board
 * goes to every player; a board from getNewScoreboard() reaches only the
 * players it is assigned to via Player.setScoreboard.
 */
final class Scoreboards {
    private Scoreboards() {
    }

    static final class ManagerImpl implements ScoreboardManager {
        final ScoreboardImpl main = new ScoreboardImpl(true);

        @Override
        public Scoreboard getMainScoreboard() {
            return main;
        }

        @Override
        public Scoreboard getNewScoreboard() {
            return new ScoreboardImpl(false);
        }
    }

    static final class ScoreboardImpl implements Scoreboard {
        final boolean main;
        final Map<String, ObjectiveImpl> objectives = new LinkedHashMap<>();
        final Map<DisplaySlot, ObjectiveImpl> displays = new ConcurrentHashMap<>();
        final Map<String, TeamImpl> teams = new LinkedHashMap<>();
        final Set<UUID> viewers = ConcurrentHashMap.newKeySet();

        ScoreboardImpl(boolean main) {
            this.main = main;
        }

        /** Null = everyone online (the main board); otherwise the assigned players. */
        JsonArray viewerList() {
            if (main) {
                return null;
            }
            JsonArray array = new JsonArray();
            viewers.forEach(uuid -> array.add(uuid.toString()));
            return array;
        }

        void send(String method, JsonObject args) {
            JsonArray list = viewerList();
            if (list != null) {
                if (list.isEmpty()) {
                    return;
                }
                args.add("viewers", list);
            }
            JsonObject payload = new JsonObject();
            payload.addProperty("m", method);
            payload.add("a", args);
            Main.rpc.call("api", payload);
        }

        /** Replays the whole board to one player (join, or after setScoreboard). */
        void sendAllTo(UUID uuid) {
            JsonArray only = new JsonArray();
            only.add(uuid.toString());
            for (ObjectiveImpl objective : objectives.values()) {
                JsonObject add = objective.packetArgs("add");
                add.add("viewers", only);
                call("scoreboard.objective", add);
                for (Map.Entry<String, Integer> score : objective.scores.entrySet()) {
                    JsonObject set = new JsonObject();
                    set.addProperty("objective", objective.name);
                    set.addProperty("entry", score.getKey());
                    set.addProperty("value", score.getValue());
                    String custom = objective.customNames.get(score.getKey());
                    if (custom != null) set.addProperty("display", custom);
                    set.add("viewers", only);
                    call("scoreboard.score", set);
                }
            }
            for (Map.Entry<DisplaySlot, ObjectiveImpl> display : displays.entrySet()) {
                JsonObject args = new JsonObject();
                args.addProperty("op", "display");
                args.addProperty("name", display.getValue().name);
                args.addProperty("slot", slotId(display.getKey()));
                args.add("viewers", only);
                call("scoreboard.objective", args);
            }
            for (TeamImpl team : teams.values()) {
                JsonObject add = team.packetArgs("add");
                add.add("viewers", only);
                call("scoreboard.team", add);
            }
        }

        /** Removes everything this board showed from one player. */
        void clearFrom(UUID uuid) {
            JsonArray only = new JsonArray();
            only.add(uuid.toString());
            for (ObjectiveImpl objective : objectives.values()) {
                JsonObject remove = new JsonObject();
                remove.addProperty("op", "remove");
                remove.addProperty("name", objective.name);
                remove.add("viewers", only);
                call("scoreboard.objective", remove);
            }
            for (TeamImpl team : teams.values()) {
                JsonObject remove = new JsonObject();
                remove.addProperty("op", "remove");
                remove.addProperty("name", team.name);
                remove.add("viewers", only);
                call("scoreboard.team", remove);
            }
        }

        private static void call(String method, JsonObject args) {
            JsonObject payload = new JsonObject();
            payload.addProperty("m", method);
            payload.add("a", args);
            Main.rpc.call("api", payload);
        }

        static int slotId(DisplaySlot slot) {
            return slot.ordinal(); // Bukkit and vanilla share the same order (list, sidebar, below name, team colours).
        }

        @Override
        public Objective registerNewObjective(String name, String criteria, Component displayName, RenderType renderType) {
            return registerNewObjective(name, Criteria.create(criteria), displayName, renderType);
        }

        @Override
        public Objective registerNewObjective(String name, Criteria criteria, Component displayName, RenderType renderType) {
            if (objectives.containsKey(name)) {
                throw new IllegalArgumentException("An objective of name '" + name + "' already exists");
            }
            ObjectiveImpl objective = new ObjectiveImpl(this, name, criteria, displayName == null ? name : ServerImpl.LEGACY.serialize(displayName), renderType);
            objectives.put(name, objective);
            send("scoreboard.objective", objective.packetArgs("add"));
            return objective;
        }

        @Override
        public Objective registerNewObjective(String name, String criteria, String displayName, RenderType renderType) {
            return registerNewObjective(name, Criteria.create(criteria), displayName == null ? null : ServerImpl.LEGACY.deserialize(displayName), renderType);
        }

        @Override
        public Objective getObjective(String name) {
            return objectives.get(name);
        }

        @Override
        public Set<Objective> getObjectivesByCriteria(String criteria) {
            Set<Objective> result = new LinkedHashSet<>();
            for (ObjectiveImpl objective : objectives.values()) {
                if (objective.criteria.getName().equals(criteria)) result.add(objective);
            }
            return result;
        }

        @Override
        public Set<Objective> getObjectivesByCriteria(Criteria criteria) {
            return getObjectivesByCriteria(criteria.getName());
        }

        @Override
        public Set<Objective> getObjectives() {
            return new LinkedHashSet<>(objectives.values());
        }

        @Override
        public Objective getObjective(DisplaySlot slot) {
            return displays.get(slot);
        }

        @Override
        public Set<Score> getScores(OfflinePlayer player) {
            return getScores(player.getName());
        }

        @Override
        public Set<Score> getScores(String entry) {
            Set<Score> scores = new LinkedHashSet<>();
            for (ObjectiveImpl objective : objectives.values()) {
                scores.add(objective.getScore(entry));
            }
            return scores;
        }

        @Override
        public void resetScores(OfflinePlayer player) {
            resetScores(player.getName());
        }

        @Override
        public void resetScores(String entry) {
            for (ObjectiveImpl objective : objectives.values()) {
                objective.scores.remove(entry);
            }
            JsonObject args = new JsonObject();
            args.addProperty("entry", entry);
            args.addProperty("reset", true);
            send("scoreboard.score", args);
        }

        @Override
        public Team getPlayerTeam(OfflinePlayer player) {
            return getEntryTeam(player.getName());
        }

        @Override
        public Team getEntryTeam(String entry) {
            for (TeamImpl team : teams.values()) {
                if (team.entries.contains(entry)) return team;
            }
            return null;
        }

        @Override
        public Team getTeam(String name) {
            return teams.get(name);
        }

        @Override
        public Set<Team> getTeams() {
            return new LinkedHashSet<>(teams.values());
        }

        @Override
        public Team registerNewTeam(String name) {
            if (teams.containsKey(name)) {
                throw new IllegalArgumentException("Team name '" + name + "' is already in use");
            }
            TeamImpl team = new TeamImpl(this, name);
            teams.put(name, team);
            send("scoreboard.team", team.packetArgs("add"));
            return team;
        }

        @Override
        public Set<OfflinePlayer> getPlayers() {
            Set<OfflinePlayer> players = new LinkedHashSet<>();
            for (String entry : getEntries()) {
                players.add(org.bukkit.Bukkit.getOfflinePlayer(entry));
            }
            return players;
        }

        @Override
        public Set<String> getEntries() {
            Set<String> entries = new LinkedHashSet<>();
            for (ObjectiveImpl objective : objectives.values()) {
                entries.addAll(objective.scores.keySet());
            }
            return entries;
        }

        @Override
        public void clearSlot(DisplaySlot slot) {
            ObjectiveImpl previous = displays.remove(slot);
            if (previous != null) {
                previous.slot = null;
            }
            JsonObject args = new JsonObject();
            args.addProperty("op", "display");
            args.addProperty("name", "");
            args.addProperty("slot", slotId(slot));
            send("scoreboard.objective", args);
        }

        @Override
        public Set<Score> getScoresFor(Entity entity) {
            return getScores(entity.getScoreboardEntryName());
        }

        @Override
        public void resetScoresFor(Entity entity) {
            resetScores(entity.getScoreboardEntryName());
        }

        @Override
        public Team getEntityTeam(Entity entity) {
            return getEntryTeam(entity.getScoreboardEntryName());
        }
    }

    static final class ObjectiveImpl implements Objective {
        final ScoreboardImpl board;
        final String name;
        final Criteria criteria;
        String displayName;
        RenderType renderType;
        DisplaySlot slot;
        boolean registered = true;
        final Map<String, Integer> scores = new LinkedHashMap<>();
        final Map<String, String> customNames = new ConcurrentHashMap<>();

        ObjectiveImpl(ScoreboardImpl board, String name, Criteria criteria, String displayName, RenderType renderType) {
            this.board = board;
            this.name = name;
            this.criteria = criteria;
            this.displayName = displayName;
            this.renderType = renderType == null ? RenderType.INTEGER : renderType;
        }

        JsonObject packetArgs(String op) {
            JsonObject args = new JsonObject();
            args.addProperty("op", op);
            args.addProperty("name", name);
            args.addProperty("displayName", displayName);
            args.addProperty("renderType", renderType == RenderType.HEARTS ? 1 : 0);
            return args;
        }

        private void checkState() {
            if (!registered) {
                throw new IllegalStateException("Unregistered scoreboard component");
            }
        }

        @Override
        public String getName() {
            return name;
        }

        @Override
        public Component displayName() {
            return ServerImpl.LEGACY.deserialize(displayName);
        }

        @Override
        public void displayName(Component value) {
            displayName = value == null ? name : ServerImpl.LEGACY.serialize(value);
            board.send("scoreboard.objective", packetArgs("update"));
        }

        @Override
        public String getDisplayName() {
            return displayName;
        }

        @Override
        public void setDisplayName(String value) {
            displayName = value == null ? name : value;
            board.send("scoreboard.objective", packetArgs("update"));
        }

        @Override
        public String getCriteria() {
            return criteria.getName();
        }

        @Override
        public Criteria getTrackedCriteria() {
            return criteria;
        }

        @Override
        public boolean isModifiable() {
            return !criteria.isReadOnly();
        }

        @Override
        public Scoreboard getScoreboard() {
            return registered ? board : null;
        }

        @Override
        public void unregister() {
            checkState();
            registered = false;
            board.objectives.remove(name);
            board.displays.values().remove(this);
            JsonObject args = new JsonObject();
            args.addProperty("op", "remove");
            args.addProperty("name", name);
            board.send("scoreboard.objective", args);
        }

        @Override
        public void setDisplaySlot(DisplaySlot displaySlot) {
            checkState();
            if (slot != null) {
                board.displays.remove(slot);
            }
            slot = displaySlot;
            if (displaySlot != null) {
                ObjectiveImpl previous = board.displays.put(displaySlot, this);
                if (previous != null && previous != this) {
                    previous.slot = null;
                }
                JsonObject args = new JsonObject();
                args.addProperty("op", "display");
                args.addProperty("name", name);
                args.addProperty("slot", ScoreboardImpl.slotId(displaySlot));
                board.send("scoreboard.objective", args);
            }
        }

        @Override
        public DisplaySlot getDisplaySlot() {
            return slot;
        }

        @Override
        public void setRenderType(RenderType type) {
            renderType = type;
            board.send("scoreboard.objective", packetArgs("update"));
        }

        @Override
        public RenderType getRenderType() {
            return renderType;
        }

        @Override
        public Score getScore(OfflinePlayer player) {
            return getScore(player.getName());
        }

        @Override
        public Score getScore(String entry) {
            checkState();
            return new ScoreImpl(this, entry);
        }

        @Override
        public Score getScoreFor(Entity entity) {
            return getScore(entity.getScoreboardEntryName());
        }

        @Override
        public boolean willAutoUpdateDisplay() {
            return true;
        }

        @Override
        public void setAutoUpdateDisplay(boolean value) {
        }

        @Override
        public io.papermc.paper.scoreboard.numbers.NumberFormat numberFormat() {
            return null;
        }

        @Override
        public void numberFormat(io.papermc.paper.scoreboard.numbers.NumberFormat format) {
        }

        void setScore(String entry, int value) {
            scores.put(entry, value);
            JsonObject args = new JsonObject();
            args.addProperty("objective", name);
            args.addProperty("entry", entry);
            args.addProperty("value", value);
            String custom = customNames.get(entry);
            if (custom != null) args.addProperty("display", custom);
            board.send("scoreboard.score", args);
        }

        void resetScore(String entry) {
            scores.remove(entry);
            JsonObject args = new JsonObject();
            args.addProperty("objective", name);
            args.addProperty("entry", entry);
            args.addProperty("reset", true);
            board.send("scoreboard.score", args);
        }
    }

    static final class ScoreImpl implements Score {
        final ObjectiveImpl objective;
        final String entry;

        ScoreImpl(ObjectiveImpl objective, String entry) {
            this.objective = objective;
            this.entry = entry;
        }

        @Override
        public OfflinePlayer getPlayer() {
            return org.bukkit.Bukkit.getOfflinePlayer(entry);
        }

        @Override
        public String getEntry() {
            return entry;
        }

        @Override
        public Objective getObjective() {
            return objective;
        }

        @Override
        public int getScore() {
            return objective.scores.getOrDefault(entry, 0);
        }

        @Override
        public void setScore(int score) {
            objective.setScore(entry, score);
        }

        @Override
        public boolean isScoreSet() {
            return objective.scores.containsKey(entry);
        }

        @Override
        public Scoreboard getScoreboard() {
            return objective.board;
        }

        @Override
        public void resetScore() {
            objective.resetScore(entry);
        }

        @Override
        public boolean isTriggerable() {
            return false;
        }

        @Override
        public void setTriggerable(boolean triggerable) {
        }

        @Override
        public Component customName() {
            String custom = objective.customNames.get(entry);
            return custom == null ? null : ServerImpl.LEGACY.deserialize(custom);
        }

        @Override
        public void customName(Component name) {
            if (name == null) objective.customNames.remove(entry);
            else objective.customNames.put(entry, ServerImpl.LEGACY.serialize(name));
            if (isScoreSet()) objective.setScore(entry, getScore());
        }

        @Override
        public io.papermc.paper.scoreboard.numbers.NumberFormat numberFormat() {
            return null;
        }

        @Override
        public void numberFormat(io.papermc.paper.scoreboard.numbers.NumberFormat format) {
        }
    }

    static final class TeamImpl implements Team {
        final ScoreboardImpl board;
        final String name;
        String displayName;
        String prefix = "";
        String suffix = "";
        NamedTextColor color;
        boolean friendlyFire = true;
        boolean seeInvisibles = true;
        Team.OptionStatus nameTagVisibility = Team.OptionStatus.ALWAYS;
        Team.OptionStatus collisionRule = Team.OptionStatus.ALWAYS;
        Team.OptionStatus deathMessages = Team.OptionStatus.ALWAYS;
        final Set<String> entries = new LinkedHashSet<>();
        boolean registered = true;

        TeamImpl(ScoreboardImpl board, String name) {
            this.board = board;
            this.name = name;
            this.displayName = name;
        }

        JsonObject packetArgs(String op) {
            JsonObject args = new JsonObject();
            args.addProperty("op", op);
            args.addProperty("name", name);
            args.addProperty("displayName", displayName);
            args.addProperty("prefix", prefix);
            args.addProperty("suffix", suffix);
            args.addProperty("visibility", statusId(nameTagVisibility));
            args.addProperty("collision", statusId(collisionRule));
            args.addProperty("color", color == null ? -1 : colorId(color));
            args.addProperty("options", (friendlyFire ? 1 : 0) | (seeInvisibles ? 2 : 0));
            JsonArray members = new JsonArray();
            entries.forEach(members::add);
            args.add("entries", members);
            return args;
        }

        private static int statusId(Team.OptionStatus status) {
            return switch (status) {
                case ALWAYS -> 0;
                case NEVER -> 1;
                case FOR_OTHER_TEAMS -> 2;
                case FOR_OWN_TEAM -> 3;
            };
        }

        private static final List<NamedTextColor> COLORS = List.of(NamedTextColor.BLACK, NamedTextColor.DARK_BLUE, NamedTextColor.DARK_GREEN,
                NamedTextColor.DARK_AQUA, NamedTextColor.DARK_RED, NamedTextColor.DARK_PURPLE, NamedTextColor.GOLD, NamedTextColor.GRAY,
                NamedTextColor.DARK_GRAY, NamedTextColor.BLUE, NamedTextColor.GREEN, NamedTextColor.AQUA, NamedTextColor.RED,
                NamedTextColor.LIGHT_PURPLE, NamedTextColor.YELLOW, NamedTextColor.WHITE);

        private static int colorId(NamedTextColor color) {
            int index = COLORS.indexOf(color);
            return index < 0 ? 15 : index;
        }

        private void update() {
            board.send("scoreboard.team", packetArgs("update"));
        }

        @Override
        public String getName() {
            return name;
        }

        @Override
        public Component displayName() {
            return ServerImpl.LEGACY.deserialize(displayName);
        }

        @Override
        public void displayName(Component value) {
            displayName = value == null ? name : ServerImpl.LEGACY.serialize(value);
            update();
        }

        @Override
        public Component prefix() {
            return ServerImpl.LEGACY.deserialize(prefix);
        }

        @Override
        public void prefix(Component value) {
            prefix = value == null ? "" : ServerImpl.LEGACY.serialize(value);
            update();
        }

        @Override
        public Component suffix() {
            return ServerImpl.LEGACY.deserialize(suffix);
        }

        @Override
        public void suffix(Component value) {
            suffix = value == null ? "" : ServerImpl.LEGACY.serialize(value);
            update();
        }

        @Override
        public boolean hasColor() {
            return color != null;
        }

        @Override
        public TextColor color() {
            if (color == null) throw new IllegalStateException("Team has no color");
            return color;
        }

        @Override
        public void color(NamedTextColor value) {
            color = value;
            update();
        }

        @Override
        public String getDisplayName() {
            return displayName;
        }

        @Override
        public void setDisplayName(String value) {
            displayName = value == null ? name : value;
            update();
        }

        @Override
        public String getPrefix() {
            return prefix;
        }

        @Override
        public void setPrefix(String value) {
            prefix = value == null ? "" : value;
            update();
        }

        @Override
        public String getSuffix() {
            return suffix;
        }

        @Override
        public void setSuffix(String value) {
            suffix = value == null ? "" : value;
            update();
        }

        @Override
        public ChatColor getColor() {
            if (color == null) return ChatColor.RESET;
            return ChatColor.valueOf(color.toString().toUpperCase(java.util.Locale.ROOT));
        }

        @Override
        public void setColor(ChatColor chatColor) {
            color = chatColor == null || chatColor == ChatColor.RESET ? null : NamedTextColor.NAMES.value(chatColor.name().toLowerCase(java.util.Locale.ROOT));
            update();
        }

        @Override
        public boolean allowFriendlyFire() {
            return friendlyFire;
        }

        @Override
        public void setAllowFriendlyFire(boolean value) {
            friendlyFire = value;
            update();
        }

        @Override
        public boolean canSeeFriendlyInvisibles() {
            return seeInvisibles;
        }

        @Override
        public void setCanSeeFriendlyInvisibles(boolean value) {
            seeInvisibles = value;
            update();
        }

        @Override
        public NameTagVisibility getNameTagVisibility() {
            return switch (nameTagVisibility) {
                case NEVER -> NameTagVisibility.NEVER;
                case FOR_OTHER_TEAMS -> NameTagVisibility.HIDE_FOR_OTHER_TEAMS;
                case FOR_OWN_TEAM -> NameTagVisibility.HIDE_FOR_OWN_TEAM;
                default -> NameTagVisibility.ALWAYS;
            };
        }

        @Override
        public void setNameTagVisibility(NameTagVisibility visibility) {
            nameTagVisibility = switch (visibility) {
                case NEVER -> Team.OptionStatus.NEVER;
                case HIDE_FOR_OTHER_TEAMS -> Team.OptionStatus.FOR_OTHER_TEAMS;
                case HIDE_FOR_OWN_TEAM -> Team.OptionStatus.FOR_OWN_TEAM;
                default -> Team.OptionStatus.ALWAYS;
            };
            update();
        }

        @Override
        public Set<OfflinePlayer> getPlayers() {
            Set<OfflinePlayer> players = new LinkedHashSet<>();
            for (String entry : entries) players.add(org.bukkit.Bukkit.getOfflinePlayer(entry));
            return players;
        }

        @Override
        public Set<String> getEntries() {
            return new LinkedHashSet<>(entries);
        }

        @Override
        public int getSize() {
            return entries.size();
        }

        @Override
        public Scoreboard getScoreboard() {
            return registered ? board : null;
        }

        @Override
        public void addPlayer(OfflinePlayer player) {
            addEntry(player.getName());
        }

        @Override
        public void addEntry(String entry) {
            addEntries(List.of(entry));
        }

        @Override
        public void addEntities(Collection<Entity> entities) {
            List<String> names = new ArrayList<>();
            for (Entity entity : entities) names.add(entity.getScoreboardEntryName());
            addEntries(names);
        }

        @Override
        public void addEntries(Collection<String> newEntries) {
            for (String entry : newEntries) {
                Team previous = board.getEntryTeam(entry);
                if (previous instanceof TeamImpl other && other != this) {
                    other.removeEntry(entry);
                }
                entries.add(entry);
            }
            JsonObject args = new JsonObject();
            args.addProperty("op", "join");
            args.addProperty("name", name);
            JsonArray members = new JsonArray();
            newEntries.forEach(members::add);
            args.add("entries", members);
            board.send("scoreboard.team", args);
        }

        @Override
        public boolean removePlayer(OfflinePlayer player) {
            return removeEntry(player.getName());
        }

        @Override
        public boolean removeEntry(String entry) {
            return removeEntries(List.of(entry));
        }

        @Override
        public boolean removeEntities(Collection<Entity> entities) {
            List<String> names = new ArrayList<>();
            for (Entity entity : entities) names.add(entity.getScoreboardEntryName());
            return removeEntries(names);
        }

        @Override
        public boolean removeEntries(Collection<String> oldEntries) {
            boolean changed = false;
            JsonArray members = new JsonArray();
            for (String entry : oldEntries) {
                if (entries.remove(entry)) {
                    changed = true;
                    members.add(entry);
                }
            }
            if (changed) {
                JsonObject args = new JsonObject();
                args.addProperty("op", "leave");
                args.addProperty("name", name);
                args.add("entries", members);
                board.send("scoreboard.team", args);
            }
            return changed;
        }

        @Override
        public void unregister() {
            registered = false;
            board.teams.remove(name);
            JsonObject args = new JsonObject();
            args.addProperty("op", "remove");
            args.addProperty("name", name);
            board.send("scoreboard.team", args);
        }

        @Override
        public boolean hasPlayer(OfflinePlayer player) {
            return entries.contains(player.getName());
        }

        @Override
        public boolean hasEntry(String entry) {
            return entries.contains(entry);
        }

        @Override
        public Team.OptionStatus getOption(Team.Option option) {
            return switch (option) {
                case NAME_TAG_VISIBILITY -> nameTagVisibility;
                case COLLISION_RULE -> collisionRule;
                case DEATH_MESSAGE_VISIBILITY -> deathMessages;
            };
        }

        @Override
        public void setOption(Team.Option option, Team.OptionStatus status) {
            switch (option) {
                case NAME_TAG_VISIBILITY -> nameTagVisibility = status;
                case COLLISION_RULE -> collisionRule = status;
                case DEATH_MESSAGE_VISIBILITY -> deathMessages = status;
            }
            update();
        }

        @Override
        public void addEntity(Entity entity) {
            addEntry(entity.getScoreboardEntryName());
        }

        @Override
        public boolean removeEntity(Entity entity) {
            return removeEntry(entity.getScoreboardEntryName());
        }

        @Override
        public boolean hasEntity(Entity entity) {
            return hasEntry(entity.getScoreboardEntryName());
        }

        @Override
        public Iterable<? extends net.kyori.adventure.audience.Audience> audiences() {
            List<net.kyori.adventure.audience.Audience> members = new ArrayList<>();
            for (String entry : entries) {
                org.bukkit.entity.Player player = org.bukkit.Bukkit.getPlayerExact(entry);
                if (player != null) members.add(player);
            }
            return members;
        }
    }

    static Set<UUID> viewersOf(ScoreboardImpl board) {
        return new HashSet<>(board.viewers);
    }
}
