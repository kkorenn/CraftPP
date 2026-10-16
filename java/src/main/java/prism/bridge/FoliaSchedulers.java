package prism.bridge;

import io.papermc.paper.threadedregions.scheduler.AsyncScheduler;
import io.papermc.paper.threadedregions.scheduler.GlobalRegionScheduler;
import io.papermc.paper.threadedregions.scheduler.RegionScheduler;
import io.papermc.paper.threadedregions.scheduler.ScheduledTask;
import org.bukkit.Location;
import org.bukkit.World;
import org.bukkit.plugin.Plugin;
import org.bukkit.scheduler.BukkitTask;

import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

/** Paper's region-aware scheduler APIs, mapped onto the single server thread. */
final class FoliaSchedulers {
    private FoliaSchedulers() {
    }

    private static ScheduledTask wrap(Plugin plugin, BukkitTask task, boolean repeating) {
        return new Wrapped(plugin, task, repeating);
    }

    private static Consumer<BukkitTask> adapt(Plugin plugin, Consumer<ScheduledTask> body, boolean repeating) {
        return task -> body.accept(wrap(plugin, task, repeating));
    }

    static final class Global implements GlobalRegionScheduler {
        @Override
        public void execute(Plugin plugin, Runnable run) {
            Main.scheduler.runTask(plugin, run);
        }

        @Override
        public ScheduledTask run(Plugin plugin, Consumer<ScheduledTask> task) {
            BukkitTask[] holder = new BukkitTask[1];
            holder[0] = Main.scheduler.runTask(plugin, () -> task.accept(wrap(plugin, holder[0], false)));
            return wrap(plugin, holder[0], false);
        }

        @Override
        public ScheduledTask runDelayed(Plugin plugin, Consumer<ScheduledTask> task, long delayTicks) {
            BukkitTask[] holder = new BukkitTask[1];
            holder[0] = Main.scheduler.runTaskLater(plugin, () -> task.accept(wrap(plugin, holder[0], false)), delayTicks);
            return wrap(plugin, holder[0], false);
        }

        @Override
        public ScheduledTask runAtFixedRate(Plugin plugin, Consumer<ScheduledTask> task, long initialDelayTicks, long periodTicks) {
            BukkitTask[] holder = new BukkitTask[1];
            holder[0] = Main.scheduler.runTaskTimer(plugin, () -> task.accept(wrap(plugin, holder[0], true)), initialDelayTicks, periodTicks);
            return wrap(plugin, holder[0], true);
        }

        @Override
        public void cancelTasks(Plugin plugin) {
            Main.scheduler.cancelTasks(plugin);
        }
    }

    static final class Async implements AsyncScheduler {
        @Override
        public ScheduledTask runNow(Plugin plugin, Consumer<ScheduledTask> task) {
            BukkitTask[] holder = new BukkitTask[1];
            holder[0] = Main.scheduler.runTaskAsynchronously(plugin, () -> task.accept(wrap(plugin, holder[0], false)));
            return wrap(plugin, holder[0], false);
        }

        @Override
        public ScheduledTask runDelayed(Plugin plugin, Consumer<ScheduledTask> task, long delay, TimeUnit unit) {
            long ticks = Math.max(1, unit.toMillis(delay) / 50);
            BukkitTask[] holder = new BukkitTask[1];
            holder[0] = Main.scheduler.runTaskLaterAsynchronously(plugin, () -> task.accept(wrap(plugin, holder[0], false)), ticks);
            return wrap(plugin, holder[0], false);
        }

        @Override
        public ScheduledTask runAtFixedRate(Plugin plugin, Consumer<ScheduledTask> task, long initialDelay, long period, TimeUnit unit) {
            long delayTicks = Math.max(0, unit.toMillis(initialDelay) / 50);
            long periodTicks = Math.max(1, unit.toMillis(period) / 50);
            BukkitTask[] holder = new BukkitTask[1];
            holder[0] = Main.scheduler.runTaskTimerAsynchronously(plugin, () -> task.accept(wrap(plugin, holder[0], true)), delayTicks, periodTicks);
            return wrap(plugin, holder[0], true);
        }

        @Override
        public void cancelTasks(Plugin plugin) {
            Main.scheduler.cancelTasks(plugin);
        }
    }

    static final class Region implements RegionScheduler {
        private final Global global = new Global();

        @Override
        public void execute(Plugin plugin, World world, int chunkX, int chunkZ, Runnable run) {
            global.execute(plugin, run);
        }

        @Override
        public ScheduledTask run(Plugin plugin, World world, int chunkX, int chunkZ, Consumer<ScheduledTask> task) {
            return global.run(plugin, task);
        }

        @Override
        public ScheduledTask runDelayed(Plugin plugin, World world, int chunkX, int chunkZ, Consumer<ScheduledTask> task, long delayTicks) {
            return global.runDelayed(plugin, task, delayTicks);
        }

        @Override
        public ScheduledTask runAtFixedRate(Plugin plugin, World world, int chunkX, int chunkZ, Consumer<ScheduledTask> task, long initialDelayTicks, long periodTicks) {
            return global.runAtFixedRate(plugin, task, initialDelayTicks, periodTicks);
        }

        @Override
        public void execute(Plugin plugin, Location location, Runnable run) {
            global.execute(plugin, run);
        }

        @Override
        public ScheduledTask run(Plugin plugin, Location location, Consumer<ScheduledTask> task) {
            return global.run(plugin, task);
        }

        @Override
        public ScheduledTask runDelayed(Plugin plugin, Location location, Consumer<ScheduledTask> task, long delayTicks) {
            return global.runDelayed(plugin, task, delayTicks);
        }

        @Override
        public ScheduledTask runAtFixedRate(Plugin plugin, Location location, Consumer<ScheduledTask> task, long initialDelayTicks, long periodTicks) {
            return global.runAtFixedRate(plugin, task, initialDelayTicks, periodTicks);
        }
    }

    private static final class Wrapped implements ScheduledTask {
        private final Plugin plugin;
        private final BukkitTask task;
        private final boolean repeating;

        Wrapped(Plugin plugin, BukkitTask task, boolean repeating) {
            this.plugin = plugin;
            this.task = task;
            this.repeating = repeating;
        }

        @Override
        public Plugin getOwningPlugin() {
            return plugin;
        }

        @Override
        public boolean isRepeatingTask() {
            return repeating;
        }

        @Override
        public CancelledState cancel() {
            if (task == null) {
                return CancelledState.ALREADY_EXECUTED;
            }
            if (task.isCancelled()) {
                return CancelledState.CANCELLED_ALREADY;
            }
            task.cancel();
            return CancelledState.CANCELLED_BY_CALLER;
        }

        @Override
        public ExecutionState getExecutionState() {
            if (task == null) {
                return ExecutionState.FINISHED;
            }
            if (task.isCancelled()) {
                return ExecutionState.CANCELLED;
            }
            if (Main.scheduler.isCurrentlyRunning(task.getTaskId())) {
                return ExecutionState.RUNNING;
            }
            return Main.scheduler.isQueued(task.getTaskId()) ? ExecutionState.IDLE : ExecutionState.FINISHED;
        }
    }
}
