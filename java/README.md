> **CraftPP:** this is PrismMC's bridge, moved to Minecraft 26.3 and embedded in-process through JNI
> (`Native`/`Rpc`/`Main`) instead of a sidecar over NDJSON. Build with `../tools/build_bridge.sh`
> (output: `../build/java/libs/craftpp-bridge.jar`). Design: `../docs/plugins.md`. The notes below
> describe the original PrismMC setup; `smoke.py` speaks the old stdio protocol and no longer applies.

# prism-bridge

The Java half of PrismMC's Bukkit/Paper plugin support: `paper-api` 26.2
(build 121) plus PrismMC's implementation of the server side of that API,
packaged as one self-contained jar that the C# server embeds and launches as a
sidecar JVM. See `../docs/plugins-26.2.md` for the design and the support
matrix.

## Build

Requires Gradle and a Java 25 toolchain (paper-api 26.2 is compiled for 25).

```sh
gradle jar testplugin:jar
cp build/libs/prism-bridge.jar ../src/PrismMC/Plugins/Data/prism-bridge.jar
```

`testplugin/` is a small plugin used by the smoke test and the C# end-to-end
test (join message rewrite, chat prefix, bedrock protection, `/prismtest`,
Brigadier `/prismbrig` (`/pb`), `/prismgui` inventory GUI with item meta,
`/prismspawn` entity/PDC/scoreboard/boss bar/block data/border/biome,
`PlayerInteractEvent`, scheduler tasks, `saveDefaultConfig`).

`gradle downloadVanilla` (run automatically by `compileJava`) fetches the
Mojang 26.2 server jar into `build/vanilla/`; the NMS sources compile against
it but it is never bundled. `gradle widenVanilla` writes
`build/vanilla/server-26.2-widened.jar` (all members public, as on Paper) for
`testplugin`, which uses NMS the way real plugins do.

## Smoke test without the C# server

```sh
python3 smoke.py "$JAVA_HOME/bin/java" build/libs/prism-bridge.jar /path/to/dir-with-PrismTestPlugin.jar
```

With `NMS_CP="build/vanilla/server-26.2.jar:$(find build/vanilla/libraries -name '*.jar' | tr '\n' ':')"`
the script also starts the JVM with the Mojang classes and checks `/prismnms`
(CraftPlayer.getHandle, a packet through `ServerPlayer.connection`, the filter
round trip). The script plays the PrismMC side of the NDJSON protocol and checks plugin
load/enable, commands (plugin.yml and Brigadier: dispatch, alias, tab
completion, tree export with parser ids), events (including inventory click
cancel and interact cancel), GUIs, entities, scoreboards, boss bars, block
data, permissions, scheduler, and disable.

## Layout

| File | Role |
|------|------|
| `Main` | stdio takeover, request routing, the 20 TPS `Server thread`, JUL → PrismMC logging |
| `Rpc` | NDJSON protocol, bidirectional requests with ids, deferred replies |
| `ProxyBase` | reflective `java.lang.reflect.Proxy` over plain backing classes; unimplemented methods throw |
| `ServerImpl`, `PlayerImpl`, `WorldImpl`, `BlockImpl`, `ConsoleImpl` | backings for the API proxies; state reads go to PrismMC |
| `PluginManagerImpl`, `PluginClassLoaderImpl`, `PaperPluginYaml` | plugin discovery, dependency ordering, class loading, events, permissions |
| `SchedulerImpl`, `FoliaSchedulers` | `BukkitScheduler` and Paper's region schedulers |
| `Events`, `Commands` | build Bukkit events from PrismMC hooks; dispatch/tab-complete/visibility of plugin commands |
| `ServicesImpl`, `BuildInfoImpl`, `PrismLifecycleEventTypeProvider` | the `ServiceLoader` providers paper-api needs from a server |
| `PrismItemStack`, `ItemMetaImpl`, `Inventories` | item stacks with meta (JSON ↔ data components on the C# side), player/container inventories, views, click mapping |
| `PdcImpl`, `Persistence` | persistent data containers; JSON form pushed to PrismMC and restored on load |
| `EntityImpl`, `ChunkImpl`, `BlockDataImpl`, `WorldBorderImpl` | entities (per-type proxies), chunks, typed block data over vanilla properties, world border |
| `Scoreboards`, `BossBars` | scoreboard manager/objectives/teams and Bukkit + Adventure boss bars, sent per viewer |
| `Brigadier`, `PrismVanillaArgumentProvider` | real `CommandDispatcher` behind `LifecycleEvents.COMMANDS`, `ArgumentTypes` resolvers, tree export |
| `Registries`, `MaterialProperties` | lazy registries from `registries-26.2.json` + synced registries; `Material` facts from `material-properties-26.2.json` |
| `nms/Nms`, `nms/NmsCore`, `nms/NmsPlayer` | server internals: vanilla registry bootstrap, play codecs both ways, per-player `ServerPlayer` + `Connection` in an `EmbeddedChannel` with a recording packet listener, clientbound filter and serverbound interception, entity handles via real constructors |
| `nms/PrismConnection` | vanilla `Connection` whose fault handling logs the cause and keeps the in-memory channel open (PrismMC owns the socket) |
| `nms/PrismServerLevel`, `nms/PrismServerPlayer` | allocated-without-constructor `ServerLevel`/`ServerPlayer` subclasses whose overrides answer from PrismMC (block states, entities, players, names) |
| `nms/Widen` | one-shot tool: makes every NMS class/field/method public (Paper access-transformer superset), run by PrismMC on the extracted Mojang jar |
| `CraftClasses`, `org.bukkit.craftbukkit.*.PrismAnchor` | `java.lang.classfile`-generated `CraftServer`/`CraftWorld`/`CraftPlayer` delegating to `ProxyBase` |
| `PluginManagerImpl.Libraries` | plugin.yml `libraries:` through paper-api's `MavenLibraryResolver` |
| `io.papermc.paper...PrismVanillaArgumentProvider`, `...PrismLifecycleEventTypeProvider` | providers for package-private paper-api interfaces, so they live in the API's own packages |
