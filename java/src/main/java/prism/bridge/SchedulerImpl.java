package prism.bridge;

import org.bukkit.plugin.IllegalPluginAccessException;
import org.bukkit.plugin.Plugin;
import org.bukkit.scheduler.BukkitRunnable;
import org.bukkit.scheduler.BukkitScheduler;
import org.bukkit.scheduler.BukkitTask;
import org.bukkit.scheduler.BukkitWorker;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Callable;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.Future;
import java.util.concurrent.FutureTask;
import java.util.concurrent.PriorityBlockingQueue;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

/**
 * Bukkit scheduler over the bridge's 20 TPS server thread. Sync tasks run on
 * the server thread in tick order; async tasks run on a cached pool.
 */
public final class SchedulerImpl implements BukkitScheduler {
    private final AtomicInteger ids = new AtomicInteger(1);
    private final PriorityBlockingQueue<Task> pending = new PriorityBlockingQueue<>();
    private final Map<Integer, Task> running = new ConcurrentHashMap<>();
    private volatile long tick;
    private volatile Task current;

    long currentTick() {
        return tick;
    }

    /** Called once per tick on the server thread. */
    void tick() {
        tick++;
        List<Task> due = new ArrayList<>();
        while (true) {
            Task next = pending.peek();
            if (next == null || next.nextRun > tick) {
                break;
            }
            due.add(pending.poll());
        }
        for (Task task : due) {
            if (task.cancelled) {
                continue;
            }
            if (task.async) {
                task.nextRun = tick + Math.max(1, task.period);
                Main.asyncPool.execute(() -> runTask(task, false));
                if (task.period > 0) {
                    pending.add(task);
                }
                continue;
            }
            runTask(task, true);
            if (task.period > 0 && !task.cancelled) {
                task.nextRun = tick + task.period;
                pending.add(task);
            }
        }
    }

    private void runTask(Task task, boolean sync) {
        if (sync) {
            current = task;
        }
        running.put(task.id, task);
        try {
            task.body.accept(task);
        } catch (Throwable t) {
            String name = task.plugin == null ? "?" : task.plugin.getDescription().getFullName();
            Main.logThrowable(task.plugin == null ? java.util.logging.Logger.getLogger("") : task.plugin.getLogger(),
                    "Task #" + task.id + " for " + name + " generated an exception", t);
        } finally {
            running.remove(task.id);
            if (sync) {
                current = null;
            }
        }
    }

    private Task schedule(Plugin plugin, Consumer<BukkitTask> body, long delay, long period, boolean async) {
        if (plugin == null) {
            throw new IllegalArgumentException("Plugin cannot be null");
        }
        if (!plugin.isEnabled()) {
            throw new IllegalPluginAccessException("Plugin attempted to register task while disabled");
        }
        Task task = new Task(ids.getAndIncrement(), plugin, body, async, period);
        task.nextRun = tick + Math.max(0, delay);
        if (delay <= 0 && Main.isServerThread() && !async) {
            task.nextRun = tick; // same tick, after the current work
        }
        pending.add(task);
        return task;
    }

    private static Consumer<BukkitTask> wrap(Runnable runnable) {
        return task -> runnable.run();
    }

    // ----- BukkitScheduler -----

    @Override
    public int scheduleSyncDelayedTask(Plugin plugin, Runnable task, long delay) {
        return schedule(plugin, wrap(task), delay, 0, false).id;
    }

    @Override
    @Deprecated
    public int scheduleSyncDelayedTask(Plugin plugin, BukkitRunnable task, long delay) {
        return runTaskLater(plugin, (Runnable) task, delay).getTaskId();
    }

    @Override
    public int scheduleSyncDelayedTask(Plugin plugin, Runnable task) {
        return scheduleSyncDelayedTask(plugin, task, 0);
    }

    @Override
    @Deprecated
    public int scheduleSyncDelayedTask(Plugin plugin, BukkitRunnable task) {
        return runTask(plugin, (Runnable) task).getTaskId();
    }

    @Override
    public int scheduleSyncRepeatingTask(Plugin plugin, Runnable task, long delay, long period) {
        return schedule(plugin, wrap(task), delay, Math.max(1, period), false).id;
    }

    @Override
    @Deprecated
    public int scheduleSyncRepeatingTask(Plugin plugin, BukkitRunnable task, long delay, long period) {
        return runTaskTimer(plugin, (Runnable) task, delay, period).getTaskId();
    }

    @Override
    @Deprecated
    public int scheduleAsyncDelayedTask(Plugin plugin, Runnable task, long delay) {
        return schedule(plugin, wrap(task), delay, 0, true).id;
    }

    @Override
    @Deprecated
    public int scheduleAsyncDelayedTask(Plugin plugin, Runnable task) {
        return scheduleAsyncDelayedTask(plugin, task, 0);
    }

    @Override
    @Deprecated
    public int scheduleAsyncRepeatingTask(Plugin plugin, Runnable task, long delay, long period) {
        return schedule(plugin, wrap(task), delay, Math.max(1, period), true).id;
    }

    @Override
    public <T> Future<T> callSyncMethod(Plugin plugin, Callable<T> task) {
        FutureTask<T> future = new FutureTask<>(task);
        schedule(plugin, wrap(future), 0, 0, false);
        return future;
    }

    @Override
    public void cancelTask(int taskId) {
        for (Task task : pending) {
            if (task.id == taskId) {
                task.cancel();
            }
        }
        Task active = running.get(taskId);
        if (active != null) {
            active.cancel();
        }
    }

    @Override
    public void cancelTasks(Plugin plugin) {
        for (Task task : pending) {
            if (task.plugin == plugin) {
                task.cancel();
            }
        }
        for (Task task : running.values()) {
            if (task.plugin == plugin) {
                task.cancel();
            }
        }
    }

    @Override
    public boolean isCurrentlyRunning(int taskId) {
        return running.containsKey(taskId);
    }

    @Override
    public boolean isQueued(int taskId) {
        for (Task task : pending) {
            if (task.id == taskId && !task.cancelled) {
                return true;
            }
        }
        return false;
    }

    @Override
    public List<BukkitWorker> getActiveWorkers() {
        List<BukkitWorker> workers = new ArrayList<>();
        for (Task task : running.values()) {
            if (task.async) {
                workers.add(task);
            }
        }
        return workers;
    }

    @Override
    public List<BukkitTask> getPendingTasks() {
        List<BukkitTask> tasks = new ArrayList<>();
        Iterator<Task> iterator = pending.iterator();
        while (iterator.hasNext()) {
            Task task = iterator.next();
            if (!task.cancelled) {
                tasks.add(task);
            }
        }
        return tasks;
    }

    @Override
    public BukkitTask runTask(Plugin plugin, Runnable task) {
        return schedule(plugin, wrap(task), 0, 0, false);
    }

    @Override
    public void runTask(Plugin plugin, Consumer<? super BukkitTask> task) {
        schedule(plugin, task::accept, 0, 0, false);
    }

    @Override
    @Deprecated
    public BukkitTask runTask(Plugin plugin, BukkitRunnable task) {
        return runTask(plugin, (Runnable) task);
    }

    @Override
    public BukkitTask runTaskAsynchronously(Plugin plugin, Runnable task) {
        return schedule(plugin, wrap(task), 0, 0, true);
    }

    @Override
    public void runTaskAsynchronously(Plugin plugin, Consumer<? super BukkitTask> task) {
        schedule(plugin, task::accept, 0, 0, true);
    }

    @Override
    @Deprecated
    public BukkitTask runTaskAsynchronously(Plugin plugin, BukkitRunnable task) {
        return runTaskAsynchronously(plugin, (Runnable) task);
    }

    @Override
    public BukkitTask runTaskLater(Plugin plugin, Runnable task, long delay) {
        return schedule(plugin, wrap(task), delay, 0, false);
    }

    @Override
    public void runTaskLater(Plugin plugin, Consumer<? super BukkitTask> task, long delay) {
        schedule(plugin, task::accept, delay, 0, false);
    }

    @Override
    @Deprecated
    public BukkitTask runTaskLater(Plugin plugin, BukkitRunnable task, long delay) {
        return runTaskLater(plugin, (Runnable) task, delay);
    }

    @Override
    public BukkitTask runTaskLaterAsynchronously(Plugin plugin, Runnable task, long delay) {
        return schedule(plugin, wrap(task), delay, 0, true);
    }

    @Override
    public void runTaskLaterAsynchronously(Plugin plugin, Consumer<? super BukkitTask> task, long delay) {
        schedule(plugin, task::accept, delay, 0, true);
    }

    @Override
    @Deprecated
    public BukkitTask runTaskLaterAsynchronously(Plugin plugin, BukkitRunnable task, long delay) {
        return runTaskLaterAsynchronously(plugin, (Runnable) task, delay);
    }

    @Override
    public BukkitTask runTaskTimer(Plugin plugin, Runnable task, long delay, long period) {
        return schedule(plugin, wrap(task), delay, Math.max(1, period), false);
    }

    @Override
    public void runTaskTimer(Plugin plugin, Consumer<? super BukkitTask> task, long delay, long period) {
        schedule(plugin, task::accept, delay, Math.max(1, period), false);
    }

    @Override
    @Deprecated
    public BukkitTask runTaskTimer(Plugin plugin, BukkitRunnable task, long delay, long period) {
        return runTaskTimer(plugin, (Runnable) task, delay, period);
    }

    @Override
    public BukkitTask runTaskTimerAsynchronously(Plugin plugin, Runnable task, long delay, long period) {
        return schedule(plugin, wrap(task), delay, Math.max(1, period), true);
    }

    @Override
    public void runTaskTimerAsynchronously(Plugin plugin, Consumer<? super BukkitTask> task, long delay, long period) {
        schedule(plugin, task::accept, delay, Math.max(1, period), true);
    }

    @Override
    @Deprecated
    public BukkitTask runTaskTimerAsynchronously(Plugin plugin, BukkitRunnable task, long delay, long period) {
        return runTaskTimerAsynchronously(plugin, (Runnable) task, delay, period);
    }

    @Override
    public Executor getMainThreadExecutor(Plugin plugin) {
        return runnable -> runTask(plugin, runnable);
    }

    /** A scheduled task; also its own BukkitWorker view for async tasks. */
    final class Task implements BukkitTask, BukkitWorker, Comparable<Task> {
        final int id;
        final Plugin plugin;
        final Consumer<BukkitTask> body;
        final boolean async;
        final long period;
        volatile long nextRun;
        volatile boolean cancelled;

        Task(int id, Plugin plugin, Consumer<BukkitTask> body, boolean async, long period) {
            this.id = id;
            this.plugin = plugin;
            this.body = body;
            this.async = async;
            this.period = period;
        }

        @Override
        public int getTaskId() {
            return id;
        }

        @Override
        public Plugin getOwner() {
            return plugin;
        }

        @Override
        public boolean isSync() {
            return !async;
        }

        @Override
        public boolean isCancelled() {
            return cancelled;
        }

        @Override
        public void cancel() {
            cancelled = true;
            pending.remove(this);
        }

        @Override
        public Thread getThread() {
            return Thread.currentThread();
        }

        @Override
        public int compareTo(Task other) {
            int byTick = Long.compare(nextRun, other.nextRun);
            return byTick != 0 ? byTick : Integer.compare(id, other.id);
        }
    }
}
