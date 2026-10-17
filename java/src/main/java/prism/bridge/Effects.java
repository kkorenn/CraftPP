package prism.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import org.bukkit.Color;
import org.bukkit.Location;
import org.bukkit.Particle;
import org.bukkit.SoundCategory;
import org.bukkit.block.data.BlockData;
import org.bukkit.entity.Entity;
import org.bukkit.entity.Player;
import org.bukkit.inventory.ItemStack;

import java.lang.reflect.Method;
import java.util.List;
import java.util.UUID;

/**
 * Sounds and particles for Player and World: every playSound / stopSound / spawnParticle overload (Bukkit and
 * Adventure) is parsed here by argument types (ProxyBase.Overloads) and sent as one CraftPP API call.
 */
final class Effects {
    private Effects() {
    }

    /** viewer: the player (Player methods) or null (World methods: everyone in range). */
    static Object overload(Method method, Object[] args, UUID viewer) {
        return switch (method.getName()) {
            case "playSound" -> {
                playSound(args, viewer);
                yield null;
            }
            case "stopSound", "stopAllSounds" -> {
                if (viewer == null) yield ProxyBase.UNHANDLED;
                stopSound(args, viewer);
                yield null;
            }
            case "spawnParticle" -> {
                spawnParticle(args, viewer);
                yield null;
            }
            default -> ProxyBase.UNHANDLED;
        };
    }

    // ----- sounds -----

    private static String soundKey(Object sound) {
        if (sound instanceof String name) return name;
        if (sound instanceof org.bukkit.Sound bukkit) return org.bukkit.Registry.SOUNDS.getKeyOrThrow(bukkit).asString();
        if (sound instanceof net.kyori.adventure.key.Keyed keyed) return keyed.key().asString();
        throw new IllegalArgumentException("Not a sound: " + sound);
    }

    /**
     * playSound(Location|Entity, Sound|String, [SoundCategory], float volume, float pitch, [long seed]),
     * playSound(adventure Sound), playSound(adventure Sound, double x, double y, double z),
     * playSound(adventure Sound, Sound.Emitter).
     */
    private static void playSound(Object[] args, UUID viewer) {
        Object first = args[0];
        if (first instanceof net.kyori.adventure.sound.Sound sound) {
            int category = sound.source().ordinal();  // Sound.Source and SoundSource share the vanilla order
            long seed = sound.seed().orElse(java.util.concurrent.ThreadLocalRandom.current().nextLong());
            if (args.length == 4) {
                send(viewer, sound.name().asString(), category, sound.volume(), sound.pitch(), seed,
                        (Double) args[1], (Double) args[2], (Double) args[3], null);
            } else if (args.length == 2 && args[1] instanceof Entity entity) {
                Location at = entity.getLocation();
                send(viewer, sound.name().asString(), category, sound.volume(), sound.pitch(), seed, at.getX(), at.getY(), at.getZ(), entity);
            } else {
                // Emitter.self(): at the listener (a player) or nowhere for worlds, as Paper does.
                if (viewer == null) return;
                Player player = org.bukkit.Bukkit.getPlayer(viewer);
                if (player == null) return;
                send(viewer, sound.name().asString(), category, sound.volume(), sound.pitch(), seed, 0, 0, 0, player);
            }
            return;
        }
        int i = 1;
        String key = soundKey(args[i++]);
        int category = SoundCategory.MASTER.ordinal();
        if (args[i] instanceof SoundCategory c) {
            category = c.ordinal();
            i++;
        }
        float volume = (Float) args[i++], pitch = (Float) args[i++];
        long seed = i < args.length ? (Long) args[i] : java.util.concurrent.ThreadLocalRandom.current().nextLong();
        if (first instanceof Location location) {
            send(viewer, key, category, volume, pitch, seed, location.getX(), location.getY(), location.getZ(), null);
        } else if (first instanceof Entity entity) {
            Location at = entity.getLocation();
            send(viewer, key, category, volume, pitch, seed, at.getX(), at.getY(), at.getZ(), entity);
        }
    }

    private static void send(UUID viewer, String sound, int category, float volume, float pitch, long seed,
                             double x, double y, double z, Entity entity) {
        Main.rpc.api(viewer == null ? "world.playSound" : "player.playSound", "uuid", viewer, "sound", sound, "category", category,
                "volume", volume, "pitch", pitch, "seed", seed, "x", x, "y", y, "z", z, "entity", entity == null ? null : entity.getEntityId());
    }

    /** stopSound(Sound|String, [SoundCategory]), stopSound(SoundCategory), stopAllSounds(), stopSound(SoundStop). */
    private static void stopSound(Object[] args, UUID viewer) {
        String sound = null;
        Integer category = null;
        for (Object arg : args) {
            if (arg instanceof SoundCategory c) category = c.ordinal();
            else if (arg instanceof net.kyori.adventure.sound.SoundStop stop) {
                if (stop.sound() != null) sound = stop.sound().asString();
                if (stop.source() != null) category = stop.source().ordinal();
            } else if (arg instanceof net.kyori.adventure.sound.Sound adventure) {
                sound = adventure.name().asString();
                category = adventure.source().ordinal();
            } else if (arg != null) sound = soundKey(arg);
        }
        Main.rpc.api("player.stopSound", "uuid", viewer, "sound", sound, "category", category);
    }

    // ----- particles -----

    /**
     * spawnParticle(Particle, Location | double x, y, z, int count, [double offsetX, offsetY, offsetZ], [double extra],
     * [T data], [boolean force]) and Paper's World.spawnParticle(Particle, List receivers, Player source, x, y, z, count,
     * offsetX, offsetY, offsetZ, extra, data, force).
     */
    private static void spawnParticle(Object[] args, UUID viewer) {
        Particle particle = (Particle) args[0];
        int i = 1;
        JsonArray viewers = null;
        if (args[i] instanceof List<?> receivers) {
            viewers = new JsonArray();
            for (Object receiver : receivers) viewers.add(((Player) receiver).getUniqueId().toString());
            i += 2;  // receivers, source (Paper's ParticleBuilder: the source only filters visibility)
        } else if (args[i] == null && args.length == 13) {
            i += 2;
        }
        double x, y, z;
        if (args[i] instanceof Location location) {
            x = location.getX();
            y = location.getY();
            z = location.getZ();
            i++;
        } else {
            x = (Double) args[i++];
            y = (Double) args[i++];
            z = (Double) args[i++];
        }
        int count = (Integer) args[i++];
        double dx = 0, dy = 0, dz = 0, extra = 1;
        if (i + 2 < args.length && args[i] instanceof Double && args[i + 1] instanceof Double && args[i + 2] instanceof Double) {
            dx = (Double) args[i++];
            dy = (Double) args[i++];
            dz = (Double) args[i++];
        }
        if (i < args.length && args[i] instanceof Double speed) {
            extra = speed;
            i++;
        }
        Object data = null;
        boolean force = false;
        for (; i < args.length; i++) {
            if (args[i] instanceof Boolean b && i == args.length - 1) force = b;
            else data = args[i];
        }
        if (viewer != null) {
            viewers = new JsonArray();
            viewers.add(viewer.toString());
        }
        JsonObject call = new JsonObject();
        call.addProperty("type", particle.getKey().asString());
        call.addProperty("x", x);
        call.addProperty("y", y);
        call.addProperty("z", z);
        call.addProperty("count", count);
        call.addProperty("dx", dx);
        call.addProperty("dy", dy);
        call.addProperty("dz", dz);
        call.addProperty("speed", extra);
        call.addProperty("force", force);
        call.add("options", options(particle, data));
        if (viewers != null) call.add("viewers", viewers);
        JsonObject payload = new JsonObject();
        payload.addProperty("m", "world.particle");
        payload.add("a", call);
        Main.rpc.call("api", payload);
    }

    private static JsonArray field(String type, Object value) {
        JsonArray f = new JsonArray();
        f.add(type);
        if (value instanceof JsonElement element) f.add(element);
        else if (value instanceof Number number) f.add(number);
        else f.add(String.valueOf(value));
        return f;
    }

    /** Bukkit particle data -> the vanilla option codec as typed fields (see api_display.cpp write_options). */
    private static JsonArray options(Particle particle, Object data) {
        JsonArray out = new JsonArray();
        Class<?> expected = particle.getDataType();
        if (expected == Void.class) return out;
        if (data == null) throw new IllegalArgumentException("missing required data " + expected.getName() + " for particle " + particle);
        if (!expected.isInstance(data)) {
            throw new IllegalArgumentException("data (" + data.getClass().getName() + ") should be " + expected.getName());
        }
        switch (data) {
            case Particle.DustTransition transition -> {
                out.add(field("i", transition.getColor().asRGB()));
                out.add(field("i", transition.getToColor().asRGB()));
                out.add(field("f", transition.getSize()));
            }
            case Particle.DustOptions dust -> {
                out.add(field("i", dust.getColor().asRGB()));
                out.add(field("f", dust.getSize()));
            }
            case BlockData block -> out.add(field("block", block.getAsString()));
            case ItemStack item -> out.add(field("item", PrismItemStack.jsonOrNull(item)));
            case Color color -> out.add(field("i", color.asARGB()));
            case Float value -> out.add(field("f", value));
            case Integer value -> out.add(field("v", value));
            case Particle.Spell spell -> {
                out.add(field("i", spell.getColor().asRGB()));
                out.add(field("f", spell.getPower()));
            }
            case Particle.Trail trail -> {
                out.add(field("d", trail.getTarget().getX()));
                out.add(field("d", trail.getTarget().getY()));
                out.add(field("d", trail.getTarget().getZ()));
                out.add(field("i", trail.getColor().asARGB()));
                out.add(field("v", trail.getDuration()));
            }
            case org.bukkit.Vibration vibration -> {
                if (vibration.getDestination() instanceof org.bukkit.Vibration.Destination.BlockDestination block) {
                    out.add(field("v", 0));
                    JsonArray pos = new JsonArray();
                    pos.add("pos");
                    pos.add(block.getLocation().getBlockX());
                    pos.add(block.getLocation().getBlockY());
                    pos.add(block.getLocation().getBlockZ());
                    out.add(pos);
                } else if (vibration.getDestination() instanceof org.bukkit.Vibration.Destination.EntityDestination entity) {
                    out.add(field("v", 1));
                    out.add(field("v", entity.getEntity().getEntityId()));
                    out.add(field("f", 0f));
                }
                out.add(field("v", vibration.getArrivalTime()));
            }
            default -> throw new UnsupportedOperationException("CraftPP does not implement particle data " + data.getClass().getName() + " yet");
        }
        return out;
    }
}
