# CraftPP

Minecraft Java server in C++, targeting **26.3** (protocol 777).

## Setup (once)
```
mkdir -p vanilla && curl -o vanilla/server.jar https://piston-data.mojang.com/v1/objects/33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c/server.jar
cd vanilla && java -DbundlerMainClass=net.minecraft.data.Main -jar server.jar --reports --server --output generated && cd ..
java -jar tools/vineflower.jar --only=net/minecraft vanilla/versions/26.3/server-26.3.jar vanilla/src   # also extracts data/
python3 tools/gen_data.py
```
Needs Java 25 for the vanilla jar.

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
