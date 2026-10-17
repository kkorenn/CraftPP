#!/bin/sh
# Mojang's game data is not redistributed with CraftPP. This fetches the official 26.3 server jar (SHA-1 checked)
# into vanilla/ and unpacks what the server reads into data/vanilla (the datapack: worldgen, loot, recipes, tags,
# structure templates). Idempotent; run from the CraftPP folder (repo root or an extracted release).
#   tools/setup_vanilla.sh            server data only (needs curl + unzip, no Java)
#   tools/setup_vanilla.sh --reports  also data/reports from Mojang's data generator (needs Java 25; tests use it)
set -e
URL=https://piston-data.mojang.com/v1/objects/33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c/server.jar
SHA1=33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c
REPORTS=; [ "$1" = "--reports" ] && REPORTS=1
sha() { (sha1sum "$1" 2>/dev/null || shasum "$1") | cut -d' ' -f1; }
mkdir -p vanilla data
if [ ! -f vanilla/server.jar ] || [ "$(sha vanilla/server.jar)" != "$SHA1" ]; then
  echo "Downloading the Minecraft 26.3 server jar from Mojang (by using it you accept https://aka.ms/MinecraftEULA)..."
  curl -fsSL -o vanilla/server.jar.part "$URL"
  [ "$(sha vanilla/server.jar.part)" = "$SHA1" ] || { echo "server.jar checksum mismatch"; rm -f vanilla/server.jar.part; exit 1; }
  mv vanilla/server.jar.part vanilla/server.jar
fi
if [ ! -f vanilla/versions/26.3/server-26.3.jar ]; then
  mkdir -p vanilla/versions/26.3
  unzip -p vanilla/server.jar META-INF/versions/26.3/server-26.3.jar > vanilla/versions/26.3/server-26.3.jar
fi
if [ ! -d data/vanilla/minecraft ]; then
  rm -rf data/vanilla.tmp && mkdir -p data/vanilla.tmp
  unzip -qo vanilla/versions/26.3/server-26.3.jar 'data/*' -d data/vanilla.tmp
  rm -rf data/vanilla && mv data/vanilla.tmp/data data/vanilla && rm -rf data/vanilla.tmp
fi
if [ -n "$REPORTS" ] && [ ! -f data/reports/registries.json ]; then
  rm -rf vanilla/generated.tmp
  (cd vanilla && "${JAVA:-java}" -DbundlerMainClass=net.minecraft.data.Main -jar server.jar --reports --server --output generated.tmp > /dev/null)
  rm -rf data/reports && mv vanilla/generated.tmp/reports data/reports && rm -rf vanilla/generated.tmp
fi
