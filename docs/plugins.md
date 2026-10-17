# Bukkit / Paper plugins

CraftPP runs unmodified Bukkit, Spigot and Paper plugin jars. Drop them into
`plugins/` and start the server. The plugins run against the real
**paper-api 26.3** (`26.3.build.195-stable`, built for Minecraft 26.3, so no
version gap) on a JVM **embedded in the CraftPP process**. CraftPP's own
implementation of the server side of that API lives in `java/`.

When `plugins/` holds no `.jar`, nothing on this page happens: libjvm is never
loaded and the server needs no Java and no extra memory.

## Quick start

```sh
tools/build_bridge.sh                # once: build/java/libs/craftpp-bridge.jar (Gradle + JDK 25)
cp MyPlugin.jar plugins/
./build/craftpp                      # loads, enables and runs the plugins
```

- Java 25 or newer is required at runtime, because paper-api 26.3 is compiled
  for 25. CraftPP looks for it in this order: `java-home` in
  `plugins/jvm.properties`, then `$JAVA_HOME`, then
  `/Library/Java/JavaVirtualMachines/*` and `~/Library/...` (macOS), then
  `/usr/lib/jvm/*` (Linux, including Debian/Raspberry Pi OS
  `openjdk-25-jdk-headless` on aarch64), then Homebrew. It reads each
  candidate's `release` file and uses the first one with `JAVA_VERSION` of 25
  or more that has `lib/server/libjvm.{so,dylib}`.
- `jni.h` is needed at **build** time. CMake finds it in any installed JDK. If
  no JDK headers are present, the plugin code compiles to stubs that log
  "PLUGINS DISABLED" when jars are found.
- Operators come from the vanilla `ops.json` in the server directory.
  `Player.setOp` writes that file back.

## Architecture

```
craftpp (C++, one process)
 ├─ src/plugins/plugins.cpp ── dlopen(libjvm) → JNI_CreateJavaVM ─┐
 │    hooks: packet-handler wrappers, join/quit, console, on_tick  │ JNI
 │    natives: api / notify / log / playerPos / sendMessage /     │ (in-process,
 │             packet                                             │  no pipes)
 └─ JVM ──────────────────────────────────────────────────────────┘
      build/java/libs/craftpp-bridge.jar = paper-api 26.3 + deps + prism.bridge.*
      prism.bridge.Main      boot / request / move / tick (called from C++)
      prism.bridge.Native    natives registered by C++ (RegisterNatives)
      ServerImpl, PlayerImpl, WorldImpl, BlockImpl, SchedulerImpl, PluginManagerImpl, ...
```

The Java side is the plugin bridge from the user's earlier project PrismMC,
which supports a large part of the API, including NMS (see below). It was
moved to 26.3 (paper-api, registries, block states) and its transport was
changed. In PrismMC it was a sidecar JVM that spoke NDJSON over stdio. In
CraftPP every crossing is a direct JNI call on the calling thread:

| Direction | Mechanism | Used for |
|---|---|---|
| C++ → Java | cached `jmethodID`s on `prism.bridge.Main`: `request(byte[] type, byte[] json)`, typed `move(...)`, `tick()` | lifecycle (`init`/`enable`/`disable`), events, command dispatch/tab, ticks |
| Java → C++ | natives on `prism.bridge.Native`: `call` (generic API), `notify0`, `log`, `playerPos(long,long,double[])`, `sendMessage(long,long,byte[])`, `packet(long,long,byte[])` | every API call a plugin makes |

- Hot paths are typed and never serialize. These are player position
  (`getLocation`), messages, `PlayerMoveEvent`, the per-tick call, and NMS
  packets. The rest of the API (about 60 methods such as `world.setBlockData`
  or `player.title`) passes small JSON documents as UTF-8 byte arrays, using
  the shapes PrismMC already defined. Strings cross as UTF-8 bytes, not
  modified UTF-8, so emoji survive. Moving a method to a typed native means
  adding one `static native` in `Native.java`, one function in
  `plugins.cpp` and one entry in its `RegisterNatives` table.
- **Listener flags.** The bridge tells C++ which hookable events have at least
  one listener (`notify0("listeners")`). A movement packet costs nothing unless
  some plugin listens for `PlayerMoveEvent`. Moves are throttled the way Paper
  throttles them: more than 1/16 block or 10° of rotation.
- **Proxies.** `Server`, `Player`, `World`, `Block` and the other API
  interfaces are `java.lang.reflect.Proxy` objects over plain backing
  classes. Interface default methods run as written. A method the backing
  class lacks throws `UnsupportedOperationException("CraftPP does not
  implement Player.getX yet")`. An API call that reaches C++ for a feature
  CraftPP lacks throws `IllegalStateException("CraftPP does not implement
  plugin API call entity.spawn yet")`. Either way the plugin gets a loud error
  with a stack trace instead of a wrong answer.
- **Block states.** The bridge jar carries `block-states-26.3.txt`, generated
  by `tools/gen_block_states.py` from the vanilla blocks report (name, first
  state, default state, properties). C++ loads it once at boot. It maps state
  id ↔ `Material`/`BlockData` names in both directions, including typed
  properties such as `oak_stairs[facing=east]`.

## Threading: the Bukkit main thread is the tick thread

- CraftPP's tick thread (`src/game/tick.hpp`) owns all game state. Packet
  handlers run on it.
- `plugins::start()` registers an `on_tick` hook. The first `Main.tick()` call
  makes the tick thread the bridge's **server thread**. From then on:
  - `Bukkit.isPrimaryThread()` is true there.
  - Events fired from packet handlers run inline in Java with no thread hop.
  - `BukkitScheduler` sync tasks and `runTaskTimer` ticks run once per game
    tick, so 20 TPS means world ticks.
  - The bridge's own 20 TPS thread stops.
- Before the first tick (plugin `onLoad`/`onEnable` during startup), the
  bridge's own "Server thread" is the main thread.
- **Async events.** `AsyncPlayerChatEvent`/`AsyncChatEvent` fire on a separate
  thread, as Paper fires them off the main thread, so the tick never waits for
  chat listeners. `event.isAsynchronous()` is true.
- **JVM threads calling the API.** When an async task or async listener calls
  a generic API method, the native notices it is not on the tick thread. It
  runs the work with `run_on_tick` and waits (10 s timeout). The typed
  `sendMessage` and `playerPos` natives only use thread-safe operations
  (outbox push, field read), so they run directly.
- **Shutdown.** `plugins::stop()` queues `disable`, and the next tick runs
  `onDisable` on the main thread. If nothing is pumping the queue, for example
  because the tick thread stopped, the waiting thread takes over the queue
  (`Main.await`).
- **Empty server.** With plugins loaded, CraftPP disables vanilla's
  empty-server tick pause unless `pause-when-empty-seconds` is set explicitly.
  Plugin timers expect the main thread to keep ticking.
- **Thread attach.** A C++ thread that calls Java is attached as a daemon on
  first use and detached by a `thread_local` destructor when it exits. JNI
  local references are scoped with `PushLocalFrame`/`PopLocalFrame`, so
  long-lived attached threads don't leak them.
- **Big stack.** The JVM is created on a dedicated pthread with an 8 MiB
  stack, because macOS secondary threads default to 512 KiB.

## Signals and other embedded-JVM pitfalls

- **`-Xrs`.** The JVM does not install SIGINT, SIGTERM, SIGHUP or SIGQUIT
  handlers, so CraftPP's own Ctrl+C and `stop` handling stays in charge. The
  JVM still uses SIGSEGV/SIGBUS internally (implicit null checks). CraftPP
  installs no handler for those, so libjsig is not needed. If C++ code ever
  installs one after the JVM starts, preload libjsig.
- **No `System.exit`.** CraftPP exits with `_exit` after saving. The JVM is
  never destroyed: plugin non-daemon threads could block `DestroyJavaVM`, and
  C++ static destructors must not run under live JVM threads.
- **Attach is off.** `jcmd`/`jstack` can't attach, because they use SIGQUIT.
  Add `-XX:+StartAttachListener` to `extra-args` for profiling sessions.
- **No in-process CDS dump.** An in-process dynamic CDS dump
  (`jcmd VM.cds dynamic_dump`) crashed HotSpot 26 in the lambda-proxy table
  afterwards. CraftPP uses the static-archive flow below instead.

## JVM tuning for small machines (`plugins/jvm.properties`)

The file is created with these defaults on the first run with plugins. The
defaults suit a Raspberry Pi with 1–4 GB of RAM.

| key | default | effect |
|---|---|---|
| `max-heap` | `256m` | `-Xmx` |
| `initial-heap` | `16m` | `-Xms` (the heap grows on demand; about 24 MB is committed with two small plugins) |
| `c1-only` | `true` | `-XX:TieredStopAtLevel=1`: no C2, less JIT memory and CPU |
| `metaspace` | `128m` | `-XX:MaxMetaspaceSize` |
| `code-cache` | `32m` | `-XX:ReservedCodeCacheSize` |
| `thread-stack` | `512k` | `-Xss` |
| `cds` | `true` | AppCDS (below) |
| `java-home` | auto | explicit JDK |
| `bridge-jar` | `build/java/libs/craftpp-bridge.jar` | |
| `extra-args` | | extra JVM options, separated by spaces |

The JVM always uses `-XX:+UseSerialGC`. GC threads stay at zero, and on a
small heap it has the lowest footprint.

**AppCDS.** The first start runs with `-XX:DumpLoadedClassList`. Ten seconds
after enable, a background child JVM dumps a static archive into
`cache/craftpp-bridge-<hash>.jsa`. The hash covers the bridge jar, the NMS
jars, the heap size and the Java version. Later starts map that archive
read-only. Startup with two plugins dropped from **632–719 ms to 250 ms**.
Class metadata then comes from the shared mapping instead of being parsed into
metaspace.

## Measurements

These were taken on an Apple-silicon Mac with Temurin 26, a flat world, one
bot online at view distance 2 and an idle server. "Footprint" is macOS
`phys_footprint`, comparable to Linux RSS minus shared clean pages.

| | footprint | ps RSS | idle CPU |
|---|---|---|---|
| no plugins (no JVM loaded) | **77 MB** | 102 MB | 0.1 % |
| 2 test plugins, first start (no CDS yet) | 179 MB | 202 MB | 0.3 % |
| 2 test plugins, CDS archive | 178 MB | 204 MB | 0.25 % |
| same with `-Xint` (no JIT) | 169 MB | – | – |
| NMS plugin (Mojang classes loaded) | – | 432 MB peak | – |

After merging survival (more game state per player), `test/plugin_test.py` reports ps RSS **260 MiB with the two test plugins vs 118 MiB without**, so the JVM delta stays at about 140 MiB RSS (about 100 MB footprint).

Inside the JVM (NMT, two plugins): heap 24 MB committed (about 10 MB live
after a full GC), metaspace 21 MB for 3,470 classes, code cache 5 MB, 13
threads, and about 21 MB of JVM malloc. libjvm text and the CDS mapping add
about 10–20 MB of clean, shared pages. Flags such as `CICompilerCount=1`,
`-XX:-UsePerfData`, a smaller code cache, `UseCompactObjectHeaders` and heap
free ratios each changed the footprint by under 2 MB. So the fixed cost of
paper-api on a JVM is about **100 MB**. That fits a 1 GB Pi but is not small.
The next lever is loading fewer API classes eagerly (registries and
`Material` tables).

## What works (first slice, covered by `test/plugin_test.py`)

- **Loading.** `plugins/*.jar` with `plugin.yml` or `paper-plugin.yml`.
  Dependency order honours `depend`, `softdepend` and `loadbefore`.
  `onLoad`, `onEnable`, and `onDisable` on stop all run. `libraries:` is
  resolved from Maven Central. Bootstrappers and loaders work (PrismMC code).
- **Server.** `Bukkit.getServer`, `getLogger`, `getOnlinePlayers`,
  `getPlayer(name|UUID)`, `broadcastMessage`, `getVersion`, `getBukkitVersion`,
  `getName` (returns "Paper", so Paper-gated plugins run),
  `dispatchCommand` (plugin commands first, then CraftPP's), ops, MOTD, and
  `shutdown`.
- **Player.**
  - Identity and lookup: `getName`, `getUniqueId`, `isOp`/`setOp`, and
    `hasPermission` through a real `PermissibleBase` (plugin.yml permission
    defaults, attachments).
  - Messaging: `sendMessage` (legacy `§` and `§x` hex become a text
    component), action bar, titles.
  - Position and state: `getLocation`/`teleport`, `getGameMode`/`setGameMode`,
    health, food and xp (wired to the survival state).
  - Other: `kickPlayer`, inventory read and slot writes (item and count),
    held slot, `sendBlockChange`, `chat`/`performCommand`.
- **World and Block.** `getName`, `getBlockAt(x,y,z).getType()`/`setType()`,
  `getBlockData`/`setBlockData` with typed `BlockData`, `createBlockData`,
  `getHighestBlockYAt`, spawn, light (always 15, matching what CraftPP sends),
  and border (the vanilla default).
  Overworld time (`getTime`, `setTime`, `getFullTime`, `setFullTime`) uses the actual
  day clock; `getGameTime` reads independent elapsed game ticks. `setTime` normalizes
  to 0–23999 and moves forward to the requested time. Full time preserves Java long
  precision, including negative values. Difficulty reads/writes and spawn setters
  (including yaw) update clients and use existing world persistence. Spawn writes
  outside build height or the horizontal world bounds return false. These setters
  do not yet emit `TimeSkipEvent` or `SpawnChangeEvent`; the bridge still exposes
  only the overworld.
- **Events.**
  - `PlayerJoinEvent` and `PlayerQuitEvent`: message editable or
    suppressible.
  - `AsyncPlayerChatEvent` and `AsyncChatEvent`: cancel, format, renderer.
  - `PlayerCommandPreprocessEvent`: cancel or rewrite.
  - `BlockBreakEvent` and `BlockPlaceEvent`: cancel resends the real block to
    the client.
  - `PlayerMoveEvent`: cancel or redirect teleports, with Paper's throttle.
- **Commands.** `commands:` from plugin.yml go through Bukkit's
  `SimpleCommandMap` (aliases, `plugin:name`), and `CommandExecutor` and
  `TabCompleter` work. Permission checks use Paper's message. Commands run
  from player chat and from the console. Plugin roots join the client command
  tree as literal + greedy `args` with `ask_server` suggestions. Paper
  Brigadier commands (`LifecycleEvents.COMMANDS`) dispatch too. `/plugins`
  and `/version` are answered by CraftPP.
- **Scheduler.** The full `BukkitScheduler` works: sync, async, delayed,
  repeating, `callSyncMethod`, `BukkitRunnable`. So do Paper's region, global
  and async schedulers (mapped onto the main thread).
- **Config.** `getConfig`, `saveDefaultConfig`, `saveResource` and data
  folders work, through the API's own `YamlConfiguration`.

## Coverage matrix

| Area | Status |
|---|---|
| Lifecycle, plugin.yml, deps, config, logging | implemented |
| Commands, tab completion, permissions, ops | implemented (every player sees every plugin root in the tree; execution still checks permissions) |
| Scheduler (Bukkit + Paper) | implemented |
| Join/quit/chat/command/break/place/move events | implemented |
| Interact, inventory click/close, held item, drop, attack, entity death, chunk load, login/pre-login, server list ping | bridge-side ready; CraftPP doesn't fire them yet (each needs a wrapper in `install_handlers` or a hook) |
| Player vitals, xp, game mode, teleport, kick, messages, titles | implemented |
| Player inventory | read and write of item + count; **item meta (name, lore, enchants) is dropped** on writes |
| Custom inventories / GUIs (`openInventory`) | throws (`player.openInventory` not implemented) |
| Entities (`spawnEntity`, `getEntities`) | `getEntities` returns players only; spawning throws |
| Overworld time, elapsed game ticks, difficulty, spawn location | implemented; client updates and restart persistence covered by plugin tests |
| Scoreboards, boss bars, sounds, particles, biomes, game rules | throw `IllegalStateException("CraftPP does not implement plugin API call ...")` |
| Persistent data containers | in memory for the session only |
| Registries, `Material` facts | bundled 26.3 registry report. `material-properties-26.3.json` still holds the 26.2 values (regenerate with PrismMC's `tools/jar-truth/MaterialTruth.java` against the 26.3 jar) |
| NMS (`CraftPlayer.getHandle`, packets, Netty pipeline) | boots (see below). Plugin-sent packets reach the client. Clientbound filtering and serverbound interception through plugin pipelines are not wired yet |

Anything not listed throws with the method name. Run the plugin and read the
console to see what it needs next.

## Server internals (NMS)

At startup C++ hashes the plugin jars and the bridge jar. If the cache entry
`cache/craftpp-nms.txt` is stale, it runs one short-lived child JVM,
`prism.bridge.nms.Prepare`. That process does the following:

1. Scans the plugin classes for `net/minecraft/` or `org/bukkit/craftbukkit/`.
2. If any plugin needs NMS, takes Mojang's 26.3 server jar
   (`vanilla/server.jar` when present, otherwise downloaded, SHA-1 verified)
   into `cache/`.
3. Unpacks the jar and its libraries into `versions/` and `libraries/`.
4. Access-widens the jar into `versions/26.3/server-26.3-craftpp.jar`. This
   took about 4 s for 7,762 classes.
5. Writes the class path.

The embedded JVM then starts with those jars and `-Dprismmc.nms=true`.
`CraftServer`, `CraftWorld` and `CraftPlayer` are generated with
`java.lang.classfile`. `CraftPlayer.getHandle()` returns a `ServerPlayer`
whose `Connection` lives in an `EmbeddedChannel`. Its encoder hands encoded
packets to `Native.packet`, and C++ writes them to the client.

PrismMC's `java/testplugin` (`gradle testplugin:jar`) boots this way.
Prepare, widening and server internals took 2.4 s on the first run. The probe
command then stops at `entity.spawn`, which CraftPP doesn't implement yet. Without NMS
references nothing is downloaded, and the cached scan result costs nothing on
later starts.

## Extending coverage

1. Find the failing call in the console. It reads `CraftPP does not implement
   plugin API call X` or `... implement Player.getX`.
2. For `plugin API call X`, add a branch to `api()` in
   `src/plugins/plugins.cpp`. PrismMC's C# side
   (`src/PrismMC/Networking/MinecraftServer.Plugins*.cs`) documents the
   argument and result shape of every call the bridge makes.
3. For `implement Player.getX`, add the method to the backing class
   (`PlayerImpl`, `WorldImpl`, ...) in `java/src/main/java/prism/bridge/`. The
   proxy picks it up by name and signature.
4. For a new event, add a wrapper in `install_handlers()` that peeks the packet
   (copying the `Reader` leaves the original unread), fires
   `event("<hook>", json)`, and on cancel resends the server's truth to the
   client. The Java side (`Events.fire`) already maps most hook names to Bukkit
   events.
5. If the call is hot, give it a typed native.
6. Add a check to `test/plugins/CraftTestPlugin` and `test/plugin_test.py`.

## Files

- `src/plugins/plugins.hpp`, `src/plugins/plugins.cpp`: JVM discovery and
  boot, natives, the API, event hooks, commands, and text components.
- `java/`: the bridge (Gradle). Built by `tools/build_bridge.sh` into
  `build/java/libs/craftpp-bridge.jar`.
- `java/src/main/java/prism/bridge/{Native,Rpc,Main}.java`: the JNI transport
  and the main-thread model.
- `java/src/main/java/prism/bridge/nms/Prepare.java`: NMS detection and
  preparation.
- `tools/gen_block_states.py`: writes `java/src/main/resources/block-states-26.3.txt`.
- `test/plugins/CraftTestPlugin`, `test/plugins/DependsOnTest`: test plugins
  compiled by the test with `javac` against the bridge jar.
- `test/plugin_test.py`: the end-to-end test. Wired into `./build.sh test`; it
  prints SKIP without Java 25+ (or without Gradle when the bridge jar is
  missing).
- Hooks in shared files: `game/game.hpp` (`plugins::start` in `start_game`,
  `announce_join`/`announce_quit`) and `main.cpp` (console command,
  `plugins::stop` before saving). Plugin command roots go into the tree
  through `commands/dispatcher.hpp`'s `add_command_root`.
