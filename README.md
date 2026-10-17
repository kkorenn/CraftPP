# CraftPP

Minecraft Java server in C++, targeting **26.3** (protocol 777).

## Run a release
Download `craftpp-<version>-<platform>.tar.gz` from GitHub Releases (linux-aarch64 for a Raspberry Pi with a
64-bit OS), extract it, `cd` into the folder and run `./craftpp`. The first start (after accepting the EULA) downloads the official
Minecraft 26.3 server jar from Mojang (about 60 MB, SHA-1 checked) and unpacks the game data CraftPP needs into
`data/vanilla`; Mojang's files are never shipped with CraftPP. Needs `curl` and `unzip`, no Java.

## Build from source
`./build.sh` (or `./run.sh` to build and start) fetches the same data via `tools/setup_vanilla.sh`.
`./build.sh test` also generates `data/reports` with Mojang's data generator, which needs Java 25.
CI (`.github/workflows/ci.yml`) builds and tests every push; publishing a GitHub release runs
`.github/workflows/release.yml`, which attaches Linux x86_64/aarch64 and macOS arm64 builds.

## Developer setup (porting from vanilla, regenerating data)
```
mkdir -p vanilla && curl -o vanilla/server.jar https://piston-data.mojang.com/v1/objects/33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c/server.jar
cd vanilla && java -DbundlerMainClass=net.minecraft.data.Main -jar server.jar --reports --server --output generated && cd ..
java -jar tools/vineflower.jar --only=net/minecraft vanilla/versions/26.3/server-26.3.jar vanilla/src   # also extracts data/
python3 tools/gen_data.py
```
Needs Java 25 for the vanilla jar. `vanilla/`, `data/vanilla` and `data/reports` are local only and never committed.

## Build + run
```
cmake -S . -B build && cmake --build build
./build/craftpp          # run from repo root (reads data/config.bin, server.properties)
```

Like a Paper server jar: the first run writes `eula.txt` and stops until it says `eula=true`. Files live beside
`server.properties` (pass another path as the first argument): a full vanilla `server.properties` (missing keys
are added, unknown ones kept), `bukkit.yml`, `spigot.yml`, `commands.yml`, `config/paper-*.yml` (Paper defaults,
not read), the JSON lists, `usercache.json`, `plugins/`, and `logs/latest.log` (rolled to
`logs/YYYY-MM-DD-N.log.gz` on the next start; plain `.log` if built without zlib). On a terminal the console has a
`> ` prompt with line editing, history (`.console_history`) and tab completion; commands work with or without `/`.
`stop`, Ctrl+C, SIGTERM/SIGHUP and the end of piped stdin all save and exit. Paper's `version`, `plugins`, `tps` and
`mspt` are there too.

## Test
- `python3 test/ping.py` - server list ping
- `python3 test/bot.py` - two headless bots: join, chunk streaming, visibility, chat, commands
- `python3 tools/run_client.py` - real vanilla client, offline, auto-joins localhost

## Plugins
Bukkit/Spigot/Paper plugin jars in `plugins/` run on an embedded JVM (paper-api 26.3). Build the bridge once with
`tools/build_bridge.sh` (Gradle + JDK 25). At runtime you need Java 25+. Without plugin jars no Java is loaded.
See [docs/plugins.md](docs/plugins.md).
