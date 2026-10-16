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
        set("size", 59999968.0, "centerX", 0.0, "centerZ", 0.0, "warningDistance", 5, "warningTime", 15, "damageAmount", 0.2, "damageBuffer", 5.0);
    }

    @Override
    public double getSize() {
        return state().get("size").getAsDouble();
    }

    @Override
    public void setSize(double size) {
        set("size", size);
    }

    @Override
    public void setSize(double size, long seconds) {
        set("size", size, "seconds", seconds);
    }

    @Override
    public void setSize(double size, java.util.concurrent.TimeUnit unit, long time) {
        set("size", size, "seconds", unit.toSeconds(time));
    }

    @Override
    public void changeSize(double size, long milliseconds) {
        set("size", size, "seconds", milliseconds / 1000);
    }

    @Override
    public Location getCenter() {
        JsonObject state = state();
        return new Location(world, state.get("centerX").getAsDouble(), 0, state.get("centerZ").getAsDouble());
    }

    @Override
    public void setCenter(double x, double z) {
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
        set("damageBuffer", blocks);
    }

    @Override
    public double getDamageAmount() {
        return state().get("damageAmount").getAsDouble();
    }

    @Override
    public void setDamageAmount(double damage) {
        set("damageAmount", damage);
    }

    @Override
    public int getWarningTime() {
        return getWarningTimeTicks() / 20;
    }

    @Override
    public void setWarningTime(int seconds) {
        set("warningTime", seconds);
    }

    @Override
    public int getWarningTimeTicks() {
        return state().get("warningTime").getAsInt() * 20;
    }

    @Override
    public void setWarningTimeTicks(int ticks) {
        set("warningTime", ticks / 20);
    }

    @Override
    public int getWarningDistance() {
        return state().get("warningDistance").getAsInt();
    }

    @Override
    public void setWarningDistance(int distance) {
        set("warningDistance", distance);
    }

    @Override
    public boolean isInside(Location location) {
        JsonObject state = state();
        double half = state.get("size").getAsDouble() / 2;
        double centerX = state.get("centerX").getAsDouble();
        double centerZ = state.get("centerZ").getAsDouble();
        return Math.abs(location.getX() - centerX) <= half && Math.abs(location.getZ() - centerZ) <= half;
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
