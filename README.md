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

## Test
- `python3 test/ping.py` - server list ping
- `python3 test/bot.py` - two headless bots: join, chunk streaming, visibility, chat, commands
- `python3 tools/run_client.py` - real vanilla client, offline, auto-joins localhost

## Plugins
Bukkit/Spigot/Paper plugin jars in `plugins/` run on an embedded JVM (paper-api 26.3). Build the bridge once with
`tools/build_bridge.sh` (Gradle + JDK 25). At runtime you need Java 25+. Without plugin jars no Java is loaded.
See [docs/plugins.md](docs/plugins.md).
