package prism.bridge;

import com.google.gson.JsonObject;
import org.bukkit.Location;
import org.bukkit.World;
import org.bukkit.WorldBorder;

/** World border over PrismMC's border state and packets. */
final class WorldBorderImpl implements WorldBorder {
    private final World world;

    WorldBorderImpl(World world) {
        this.world = world;
    }

    private JsonObject state() {
        return Main.rpc.api("world.border").getAsJsonObject();
    }

    private void set(Object... keyValues) {
        Main.rpc.api("world.setBorder", keyValues);
    }

    @Override
    public World getWorld() {
        return world;
    }

    @Override
    public void reset() {
        set("size", 59999968.0, "centerX", 0.0, "centerZ", 0.0, "warningDistance", 5, "warningTimeTicks", 300, "damageAmount", 0.2, "damageBuffer", 5.0);
    }

    @Override
    public double getSize() {
        return state().get("size").getAsDouble();
    }

    @Override
    public void setSize(double size) {
        setSize(size, 0);
    }

    @Override
    public void setSize(double size, long seconds) {
        if (seconds < 0) throw new IllegalArgumentException("seconds must be non-negative");
        setSizeTicks(size, Math.multiplyExact(seconds, 20L));
    }

    @Override
    public void setSize(double size, java.util.concurrent.TimeUnit unit, long time) {
        java.util.Objects.requireNonNull(unit, "unit");
        if (time < 0) throw new IllegalArgumentException("time must be non-negative");
        changeSize(size, unit.toMillis(time));
    }

    @Override
    public void changeSize(double size, long milliseconds) {
        if (milliseconds < 0) throw new IllegalArgumentException("milliseconds must be non-negative");
        setSizeTicks(size, milliseconds / 50 + (milliseconds % 50 == 0 ? 0 : 1));
    }

    private void setSizeTicks(double size, long ticks) {
        if (!Double.isFinite(size) || size < 1 || size > getMaxSize()) throw new IllegalArgumentException("Invalid border size");
        set("size", size, "ticks", Long.toString(ticks));
    }

    @Override
    public Location getCenter() {
        JsonObject state = state();
        return new Location(world, state.get("centerX").getAsDouble(), 0, state.get("centerZ").getAsDouble());
    }

    @Override
    public void setCenter(double x, double z) {
        if (!Double.isFinite(x) || !Double.isFinite(z) || Math.abs(x) > getMaxCenterCoordinate() || Math.abs(z) > getMaxCenterCoordinate())
            throw new IllegalArgumentException("Invalid border center");
        set("centerX", x, "centerZ", z);
    }

    @Override
    public void setCenter(Location location) {
        setCenter(location.getX(), location.getZ());
    }

    @Override
    public double getDamageBuffer() {
        return state().get("damageBuffer").getAsDouble();
    }

    @Override
    public void setDamageBuffer(double blocks) {
        if (!Double.isFinite(blocks) || blocks < 0) throw new IllegalArgumentException("Invalid damage buffer");
        set("damageBuffer", blocks);
    }

    @Override
    public double getDamageAmount() {
        return state().get("damageAmount").getAsDouble();
    }

    @Override
    public void setDamageAmount(double damage) {
        if (!Double.isFinite(damage) || damage < 0) throw new IllegalArgumentException("Invalid damage amount");
        set("damageAmount", damage);
    }

    @Override
    public int getWarningTime() {
        return getWarningTimeTicks() / 20;
    }

    @Override
    public void setWarningTime(int seconds) {
        if (seconds < 0) throw new IllegalArgumentException("Invalid warning time");
        setWarningTimeTicks(Math.multiplyExact(seconds, 20));
    }

    @Override
    public int getWarningTimeTicks() {
        return state().get("warningTimeTicks").getAsInt();
    }

    @Override
    public void setWarningTimeTicks(int ticks) {
        if (ticks < 0) throw new IllegalArgumentException("Invalid warning time");
        set("warningTimeTicks", ticks);
    }

    @Override
    public int getWarningDistance() {
        return state().get("warningDistance").getAsInt();
    }

    @Override
    public void setWarningDistance(int distance) {
        if (distance < 0) throw new IllegalArgumentException("Invalid warning distance");
        set("warningDistance", distance);
    }

    @Override
    public boolean isInside(Location location) {
        JsonObject state = state();
        double half = state.get("size").getAsDouble() / 2;
        double centerX = state.get("centerX").getAsDouble();
        double centerZ = state.get("centerZ").getAsDouble();
        return world.equals(location.getWorld())
                && location.getX() >= Math.max(-getMaxCenterCoordinate(), centerX - half)
                && location.getX() < Math.min(getMaxCenterCoordinate(), centerX + half)
                && location.getZ() >= Math.max(-getMaxCenterCoordinate(), centerZ - half)
                && location.getZ() < Math.min(getMaxCenterCoordinate(), centerZ + half);
    }

    @Override
    public double getMaxSize() {
        return 59999968;
    }

    @Override
    public double getMaxCenterCoordinate() {
        return 29999984;
    }
}
