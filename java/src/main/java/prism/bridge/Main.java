package prism.bridge;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import org.bukkit.Bukkit;

import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogManager;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

/**
 * Bridge entry point. CraftPP embeds this JVM in-process (JNI) and calls
 * {@link #boot}, then {@link #request} for init/enable/events/commands.
 */
public final class Main {
    public static Rpc rpc;
    public static ServerImpl server;
    public static PluginManagerImpl pluginManager;
    public static SchedulerImpl scheduler;
    public static Thread serverThread;
    public static final ExecutorService asyncPool = Executors.newCachedThreadPool(runnable -> {
        Thread thread = new Thread(runnable, "Prism Async Thread");
        thread.setDaemon(true);
        return thread;
    });

    private static final LinkedBlockingQueue<Runnable> mainQueue = new LinkedBlockingQueue<>();
    private static volatile boolean running = true;
    /** PrismMC drives ticks once its clock is running; before that the bridge self-ticks. */
    private static volatile boolean externalTicks;

    /**
     * Called by CraftPP right after JNI_CreateJavaVM (natives already registered).
     * Everything a plugin prints becomes a log line on CraftPP's console.
     */
    public static void boot() {
        rpc = new Rpc();
        System.setOut(new PrintStream(new LineToLog("STDOUT", "INFO"), true, StandardCharsets.UTF_8));
        System.setErr(new PrintStream(new LineToLog("STDERR", "WARN"), true, StandardCharsets.UTF_8));
        installLogging();
        serverThread = new Thread(Main::runServerThread, "Server thread");
        serverThread.setDaemon(true); // CraftPP owns process lifetime
        serverThread.start();
    }

    /**
     * Entry point for every request from CraftPP (init, enable, event, dispatch, tab, ...).
     * Blocks the calling C++ thread until the work, usually queued onto the
     * server thread, has finished; errors surface as a Java exception CraftPP reports.
     */
    public static byte[] request(byte[] typeBytes, byte[] json) throws Throwable {
        String type = new String(typeBytes, StandardCharsets.UTF_8);
        JsonObject payload = json == null || json.length == 0 ? new JsonObject()
                : com.google.gson.JsonParser.parseString(new String(json, StandardCharsets.UTF_8)).getAsJsonObject();
        long id = requestIds.incrementAndGet();
        java.util.concurrent.CompletableFuture<JsonElement> future = new java.util.concurrent.CompletableFuture<>();
        rpc.waiting.put(id, future);
        payload.addProperty("__id", id);
        try {
            JsonElement result = handleRequest(type, payload);
            if (result == Rpc.DEFERRED) {
                boolean lifecycle = type.equals("init") || type.equals("enable") || type.equals("disable");
                result = await(future, lifecycle ? Long.MAX_VALUE : 60_000, type);
            }
            return result == null ? null : Rpc.utf8(Rpc.GSON.toJson(result));
        } finally {
            rpc.waiting.remove(id);
        }
    }

    private static final java.util.concurrent.atomic.AtomicLong requestIds = new java.util.concurrent.atomic.AtomicLong();

    /** Typed PlayerMoveEvent hook (hot path, no JSON): returns null to accept, else the x,y,z,yaw,pitch to send the player to. */
    public static double[] move(long hi, long lo, double fx, double fy, double fz, float fyaw, float fpitch,
                                double tx, double ty, double tz, float tyaw, float tpitch) throws Throwable {
        return onServerThread(() -> Events.move(new java.util.UUID(hi, lo), fx, fy, fz, fyaw, fpitch, tx, ty, tz, tyaw, tpitch));
    }

    /** Runs work on the server thread (inline when already there) and waits for it. */
    static <T> T onServerThread(Callable<T> work) throws Throwable {
        if (isServerThread()) {
            return work.call();
        }
        java.util.concurrent.CompletableFuture<T> future = new java.util.concurrent.CompletableFuture<>();
        mainQueue.offer(() -> {
            try {
                future.complete(work.call());
            } catch (Throwable t) {
                future.completeExceptionally(t);
            }
        });
        return await(future, 60_000, "move");
    }

    /**
     * Waits for work queued on the server thread. If nobody is pumping the queue
     * (CraftPP's game thread adopted it and has stopped or paused ticking, e.g.
     * during shutdown), the waiting thread becomes the server thread and runs it.
     */
    private static <T> T await(java.util.concurrent.CompletableFuture<T> future, long timeoutMillis, String what) throws Throwable {
        long deadline = timeoutMillis == Long.MAX_VALUE ? Long.MAX_VALUE : System.currentTimeMillis() + timeoutMillis;
        while (true) {
            try {
                return future.get(100, TimeUnit.MILLISECONDS);
            } catch (java.util.concurrent.ExecutionException e) {
                throw e.getCause();
            } catch (java.util.concurrent.TimeoutException e) {
                if (System.nanoTime() - lastPump > 1_000_000_000L) {
                    serverThread = Thread.currentThread();
                    drainQueue();
                }
                if (System.currentTimeMillis() > deadline) {
                    throw new IllegalStateException("plugins did not answer '" + what + "' within " + timeoutMillis / 1000 + "s (server thread busy?)");
                }
            }
        }
    }

    private static volatile long lastPump = System.nanoTime();

    private static void drainQueue() {
        lastPump = System.nanoTime();
        for (Runnable task; (task = mainQueue.poll()) != null; ) {
            try {
                task.run();
            } catch (Throwable t) {
                logThrowable(Logger.getLogger(""), "Server thread task failed", t);
            }
        }
    }

    /**
     * CraftPP's game loop calls this once per tick from its tick thread. The first
     * call makes that thread Bukkit's main thread (the bridge's own 20 TPS thread
     * stops); after that events fired from game code run inline, and queued tasks
     * and scheduler ticks run here.
     */
    public static void tick() {
        if (serverThread != Thread.currentThread()) {
            externalTicks = true;
            serverThread = Thread.currentThread();
        }
        drainQueue();
        if (scheduler != null) {
            scheduler.tick();
        }
        lastPump = System.nanoTime();
    }

    /** Names of a vanilla registry ordered by protocol id (from the bundled registry report). */
    public static String[] registryNames(String registry) throws Exception {
        try (java.io.Reader reader = new java.io.InputStreamReader(
                Main.class.getClassLoader().getResourceAsStream("registries-26.3.json"), StandardCharsets.UTF_8)) {
            JsonObject entries = com.google.gson.JsonParser.parseReader(reader).getAsJsonObject().getAsJsonObject(registry).getAsJsonObject("entries");
            String[] names = new String[entries.size()];
            for (var entry : entries.entrySet()) {
                names[entry.getValue().getAsJsonObject().get("protocol_id").getAsInt()] = entry.getKey();
            }
            return names;
        }
    }

    /** A bundled resource (e.g. block-states-26.3.txt for CraftPP's block state table). */
    public static byte[] resource(String name) throws Exception {
        try (java.io.InputStream in = Main.class.getClassLoader().getResourceAsStream(name)) {
            return in == null ? null : in.readAllBytes();
        }
    }

    /**
     * First run only: write an AppCDS archive of the classes loaded so far (JVM started
     * with -XX:+RecordDynamicDumpInfo), so later starts map them instead of parsing jars.
     */
    public static void dumpCds(String file) {
        try {
            javax.management.MBeanServer mbs = java.lang.management.ManagementFactory.getPlatformMBeanServer();
            Object out = mbs.invoke(new javax.management.ObjectName("com.sun.management:type=DiagnosticCommand"), "vmCds",
                    new Object[] {new String[] {"dynamic_dump", file}}, new String[] {String[].class.getName()});
            if (out != null && !out.toString().isBlank()) {
                Logger.getLogger("").fine(out.toString());
            }
        } catch (Exception e) {
            Logger.getLogger("").log(Level.WARNING, "Could not write the AppCDS archive " + file, e);
        }
    }

    /** Final stop: System.exit lets the JVM write the AppCDS archive (-XX:+AutoCreateSharedArchive) on the way out. */
    public static void exit(int status) {
        running = false;
        rpc.close();
        System.exit(status);
    }

    // ----- request routing -----

    private static JsonElement handleRequest(String type, JsonObject payload) {
        switch (type) {
            case "init" -> defer(payload, () -> Bootstrap.init(payload));
            case "enable" -> defer(payload, () -> Bootstrap.enable());
            case "disable" -> defer(payload, () -> { pluginManager.disablePlugins(); return new JsonObject(); });
            case "inbound" -> deferAsync(payload, () -> {
                prism.bridge.nms.Nms nms = prism.bridge.nms.Nms.get();
                java.util.List<byte[]> out = nms == null ? null : nms.inbound(java.util.UUID.fromString(payload.get("uuid").getAsString()),
                        java.util.Base64.getDecoder().decode(payload.get("data").getAsString()));
                JsonObject result = new JsonObject();
                if (out != null) {
                    com.google.gson.JsonArray packets = new com.google.gson.JsonArray();
                    for (byte[] bytes : out) {
                        packets.add(java.util.Base64.getEncoder().encodeToString(bytes));
                    }
                    result.add("packets", packets);
                }
                return result;
            });
            case "packet" -> deferAsync(payload, () -> {
                prism.bridge.nms.Nms nms = prism.bridge.nms.Nms.get();
                java.util.List<byte[]> out = nms == null ? null : nms.filter(java.util.UUID.fromString(payload.get("uuid").getAsString()),
                        java.util.Base64.getDecoder().decode(payload.get("data").getAsString()));
                JsonObject result = new JsonObject();
                if (out != null) {
                    com.google.gson.JsonArray packets = new com.google.gson.JsonArray();
                    for (byte[] bytes : out) {
                        packets.add(java.util.Base64.getEncoder().encodeToString(bytes));
                    }
                    result.add("packets", packets);
                }
                return result;
            });
            case "event" -> {
                String name = payload.get("e").getAsString();
                if (Events.isAsync(name)) {
                    deferAsync(payload, () -> Events.fire(name, payload.getAsJsonObject("p")));
                } else {
                    defer(payload, () -> Events.fire(name, payload.getAsJsonObject("p")));
                }
            }
            case "dispatch" -> defer(payload, () -> Commands.dispatch(payload));
            case "tab" -> defer(payload, () -> Commands.tabComplete(payload));
            case "visible" -> defer(payload, () -> Commands.visibleCommands(payload));
            case "op" -> {
                PlayerImpl player = server.playerByUuid(payload.get("uuid").getAsString());
                if (player != null) {
                    player.setOpCached(payload.get("op").getAsBoolean());
                }
                return new JsonObject();
            }
            case "player" -> {
                // A player joined with no join listeners registered; still track them.
                server.ensurePlayer(payload);
                return new JsonObject();
            }
            case "playergone" -> {
                server.removePlayer(java.util.UUID.fromString(payload.get("uuid").getAsString()));
                return new JsonObject();
            }
            case "tick" -> {
                externalTicks = true;
                mainQueue.offer(() -> {
                    if (scheduler != null) {
                        scheduler.tick();
                    }
                });
                return new JsonObject();
            }
            case "invchange" -> {
                PlayerImpl player = server.playerByUuid(payload.get("uuid").getAsString());
                if (player != null) {
                    mainQueue.offer(() -> player.applyServerInventoryChange(payload.get("windowId").getAsInt(), payload.getAsJsonArray("items")));
                }
                return new JsonObject();
            }
            case "shutdown" -> {
                running = false;
                return new JsonObject();
            }
            default -> throw new IllegalArgumentException("Unknown request type " + type);
        }
        return Rpc.DEFERRED;
    }

    /** Runs the work on the server thread and replies when it finishes. */
    /** After any request: tell PrismMC which players gained or lost plugin Netty handlers (cheap compare per player). */
    static void publishInjection() {
        prism.bridge.nms.Nms nms = prism.bridge.nms.Nms.get();
        if (nms != null) {
            nms.publishInjection();
        }
    }

    private static void defer(JsonObject payload, Callable<JsonElement> work) {
        long id = payload.get("__id").getAsLong();
        // Already on the server thread (a plugin API call re-entered CraftPP, which fired an event): run inline.
        java.util.function.Consumer<Runnable> run = isServerThread() ? Runnable::run : mainQueue::offer;
        run.accept(() -> {
            try {
                JsonElement result = work.call();
                publishInjection();
                rpc.replyLater(id, result, null);
            } catch (Throwable t) {
                logThrowable(Logger.getLogger(""), "Bridge request failed", t);
                rpc.replyLater(id, null, t);
            }
        });
    }

    private static void deferAsync(JsonObject payload, Callable<JsonElement> work) {
        long id = payload.get("__id").getAsLong();
        asyncPool.execute(() -> {
            try {
                JsonElement result = work.call();
                publishInjection();
                rpc.replyLater(id, result, null);
            } catch (Throwable t) {
                logThrowable(Logger.getLogger(""), "Bridge async request failed", t);
                rpc.replyLater(id, null, t);
            }
        });
    }

    /** Queue a task on the server thread (from any thread). */
    public static void runOnServerThread(Runnable task) {
        mainQueue.offer(task);
    }

    public static boolean isServerThread() {
        return Thread.currentThread() == serverThread;
    }

    // PrismMC's tick loop drives ticks (`tick` notifications); the local 50 ms
    // clock only fills in while no external tick has arrived (smoke.py, start-up).
    private static void runServerThread() {
        final long tickNanos = 50_000_000L;
        long nextTick = System.nanoTime() + tickNanos;
        while (running && !externalTicks) {
            lastPump = System.nanoTime();
            long wait = nextTick - System.nanoTime();
            Runnable task = null;
            try {
                task = wait > 0 ? mainQueue.poll(wait, TimeUnit.NANOSECONDS) : mainQueue.poll();
            } catch (InterruptedException ignored) {
            }
            if (task != null) {
                try {
                    task.run();
                } catch (Throwable t) {
                    logThrowable(Logger.getLogger(""), "Server thread task failed", t);
                }
            }
            if (System.nanoTime() >= nextTick) {
                nextTick += tickNanos;
                if (System.nanoTime() > nextTick + tickNanos * 20) {
                    nextTick = System.nanoTime() + tickNanos; // fell far behind: don't burst-catch-up
                }
                if (scheduler != null && !externalTicks) {
                    scheduler.tick();
                }
            }
        }
    }

    // ----- logging -----

    private static void installLogging() {
        LogManager.getLogManager().reset();
        Logger root = Logger.getLogger("");
        root.setLevel(Level.ALL);
        root.addHandler(new Handler() {
            @Override
            public void publish(LogRecord record) {
                String level = record.getLevel().intValue() >= Level.SEVERE.intValue() ? "ERROR"
                        : record.getLevel().intValue() >= Level.WARNING.intValue() ? "WARN"
                        : record.getLevel().intValue() >= Level.INFO.intValue() ? "INFO" : "DEBUG";
                if (level.equals("DEBUG")) {
                    return;
                }
                String message = record.getMessage();
                if (record.getParameters() != null && record.getParameters().length > 0) {
                    try {
                        message = java.text.MessageFormat.format(message, record.getParameters());
                    } catch (RuntimeException ignored) {
                    }
                }
                if (record.getThrown() != null) {
                    message = message + System.lineSeparator() + stackTrace(record.getThrown());
                }
                String logger = record.getLoggerName();
                if (logger == null || logger.equals("global") || logger.equals("Minecraft") || logger.equals("Server")) {
                    logger = "";
                }
                rpc.log(level, logger, message);
            }

            @Override
            public void flush() {
            }

            @Override
            public void close() {
            }
        });
    }

    public static String stackTrace(Throwable t) {
        java.io.StringWriter writer = new java.io.StringWriter();
        t.printStackTrace(new java.io.PrintWriter(writer));
        return writer.toString().stripTrailing();
    }

    public static void logThrowable(Logger logger, String message, Throwable t) {
        logger.log(Level.SEVERE, message, t);
    }

    /** Turns System.out/err writes into log messages, one per line. */
    private static final class LineToLog extends OutputStream {
        private final ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        private final String logger;
        private final String level;

        LineToLog(String logger, String level) {
            this.logger = logger;
            this.level = level;
        }

        @Override
        public synchronized void write(int b) {
            if (b == '\n') {
                flushLine();
            } else if (b != '\r') {
                buffer.write(b);
            }
        }

        @Override
        public synchronized void flush() {
        }

        private void flushLine() {
            String line = buffer.toString(StandardCharsets.UTF_8);
            buffer.reset();
            if (!line.isEmpty()) {
                rpc.log(level, logger, line);
            }
        }
    }
}
