package prism.bridge;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import org.bukkit.BanEntry;
import org.bukkit.BanList;
import org.bukkit.Bukkit;
import org.bukkit.OfflinePlayer;

import java.lang.reflect.Method;
import java.net.InetAddress;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.time.Duration;
import java.time.Instant;
import java.util.Date;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;

/**
 * Ban lists over CraftPP's vanilla files (banned-players.json, banned-ips.json), shared with /ban and /ban-ip.
 * BanList is generic with many overloads; Bans implements them by argument type (ProxyBase.Overloads).
 */
final class UserLists {
    private UserLists() {
    }

    private static final ThreadLocal<SimpleDateFormat> DATE = ThreadLocal.withInitial(() -> new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.ROOT));

    static String format(Date date) {
        return date == null ? "forever" : DATE.get().format(date);
    }

    static Date parse(String text) {
        if (text == null || text.isEmpty() || text.equals("forever")) return null;
        try {
            return DATE.get().parse(text);
        } catch (ParseException e) {
            return null;
        }
    }

    static Iterable<JsonElement> rows(String list) {
        return Main.rpc.api("server.list", "list", list).getAsJsonArray();
    }

    static Set<OfflinePlayer> players(String list) {
        Set<OfflinePlayer> players = new LinkedHashSet<>();
        for (JsonElement row : rows(list)) {
            JsonObject r = row.getAsJsonObject();
            String uuid = r.get("uuid").getAsString();
            players.add(uuid.isEmpty() ? Bukkit.getOfflinePlayer(r.get("name").getAsString()) : Bukkit.getOfflinePlayer(UUID.fromString(uuid)));
        }
        return players;
    }

    static boolean contains(String list, UUID uuid, String name) {
        for (JsonElement row : rows(list)) {
            JsonObject r = row.getAsJsonObject();
            if ((uuid != null && r.get("uuid").getAsString().equals(uuid.toString()))
                    || (name != null && r.get("name").getAsString().equalsIgnoreCase(name))) return true;
        }
        return false;
    }

    static void add(String list, UUID uuid, String name) {
        Main.rpc.api("server.listAdd", "list", list, "uuid", uuid == null ? "" : uuid.toString(), "name", name == null ? "" : name);
    }

    static void remove(String list, UUID uuid, String name) {
        Main.rpc.api("server.listRemove", "list", list, "uuid", uuid == null ? "" : uuid.toString(), "name", name == null ? "" : name);
    }

    static BanList<?> banList(boolean ip) {
        return ip ? ProxyBase.create(org.bukkit.ban.IpBanList.class, new Bans(true))
                : ProxyBase.create(org.bukkit.ban.ProfileBanList.class, new Bans(false));
    }

    /** One ban list (profile or IP). */
    static final class Bans implements ProxyBase.Overloads {
        private final boolean ip;

        Bans(boolean ip) {
            this.ip = ip;
        }

        private String list() {
            return ip ? "ipbans" : "bans";
        }

        /** (uuid, name) of a profile target, or (null, ip) of an address target. */
        private String[] key(Object target) {
            if (target == null) throw new IllegalArgumentException("Target cannot be null");
            if (ip) {
                String address = target instanceof InetAddress inet ? inet.getHostAddress() : target.toString();
                return new String[] {null, address};
            }
            if (target instanceof org.bukkit.profile.PlayerProfile profile) {
                return new String[] {profile.getUniqueId() == null ? null : profile.getUniqueId().toString(), profile.getName()};
            }
            String text = target.toString();
            try {
                return new String[] {UUID.fromString(text).toString(), null};
            } catch (IllegalArgumentException notUuid) {
                OfflinePlayer player = Bukkit.getOfflinePlayer(text);
                return new String[] {player.getUniqueId().toString(), text};
            }
        }

        private JsonObject find(String[] key) {
            for (JsonElement row : rows(list())) {
                JsonObject r = row.getAsJsonObject();
                if (ip ? r.get("ip").getAsString().equals(key[1])
                        : (key[0] != null && r.get("uuid").getAsString().equals(key[0]))
                        || (key[1] != null && r.get("name").getAsString().equalsIgnoreCase(key[1]))) return r;
            }
            return null;
        }

        private BanEntry<?> entry(JsonObject row) {
            return row == null ? null : ProxyBase.create(BanEntry.class, new Entry(ip, row));
        }

        private BanEntry<?> add(Object target, String reason, Date expires, String source) {
            String[] key = key(target);
            Main.rpc.api("server.listAdd", "list", list(), "uuid", key[0] == null ? "" : key[0], "name", key[1] == null ? "" : key[1],
                    "ip", ip ? key[1] : "", "reason", reason == null ? "Banned by an operator." : reason,
                    "source", source == null ? "Server" : source, "expires", format(expires), "created", format(new Date()));
            return entry(find(key));
        }

        @Override
        public Object overload(Method method, Object[] args) {
            return switch (method.getName()) {
                case "getBanEntry" -> entry(find(key(args[0])));
                case "addBan" -> {
                    Object when = args[2];
                    Date expires = when == null ? null : when instanceof Date d ? d : when instanceof Instant i ? Date.from(i)
                            : Date.from(Instant.now().plus((Duration) when));
                    yield add(args[0], (String) args[1], expires, (String) args[3]);
                }
                case "getBanEntries", "getEntries" -> {
                    Set<BanEntry<?>> entries = new HashSet<>();
                    for (JsonElement row : rows(list())) entries.add(entry(row.getAsJsonObject()));
                    yield entries;
                }
                case "isBanned" -> find(key(args[0])) != null;
                case "pardon" -> {
                    String[] key = key(args[0]);
                    Main.rpc.api("server.listRemove", "list", list(), "uuid", key[0] == null ? "" : key[0], "name", key[1] == null ? "" : key[1],
                            "ip", ip ? key[1] : "");
                    yield null;
                }
                default -> ProxyBase.UNHANDLED;
            };
        }
    }

    /** One ban row; setters change the copy, save() writes it back. */
    public static final class Entry {
        private final boolean ip;
        private final String uuid, name, address;
        private Date created, expiration;
        private String source, reason;

        Entry(boolean ip, JsonObject row) {
            this.ip = ip;
            uuid = row.get("uuid").getAsString();
            name = row.get("name").getAsString();
            address = row.get("ip").getAsString();
            created = parse(row.get("created").getAsString());
            expiration = parse(row.get("expires").getAsString());
            source = row.get("source").getAsString();
            reason = row.get("reason").getAsString();
        }

        public String getTarget() {
            return ip ? address : name;
        }

        public Object getBanTarget() {
            if (ip) {
                try {
                    return InetAddress.getByName(address);
                } catch (java.net.UnknownHostException e) {
                    return null;
                }
            }
            return Bukkit.createProfile(uuid.isEmpty() ? null : UUID.fromString(uuid), name);
        }

        public Date getCreated() {
            return created == null ? null : (Date) created.clone();
        }

        public void setCreated(Date created) {
            this.created = created;
        }

        public String getSource() {
            return source;
        }

        public void setSource(String source) {
            this.source = source;
        }

        public Date getExpiration() {
            return expiration == null ? null : (Date) expiration.clone();
        }

        public void setExpiration(Date expiration) {
            this.expiration = expiration;
        }

        public String getReason() {
            return reason;
        }

        public void setReason(String reason) {
            this.reason = reason;
        }

        public void save() {
            Main.rpc.api("server.listAdd", "list", ip ? "ipbans" : "bans", "uuid", uuid, "name", name, "ip", address, "reason", reason,
                    "source", source, "expires", format(expiration), "created", format(created));
        }

        public void remove() {
            Main.rpc.api("server.listRemove", "list", ip ? "ipbans" : "bans", "uuid", uuid, "name", name, "ip", address);
        }
    }
}
