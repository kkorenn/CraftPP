#!/bin/sh
# Builds the server in Release mode and packs it into dist/craftpp-<name>.tar.gz:
#   craftpp, data/ (CraftPP's own tables, no Mojang files), setup_vanilla.sh, start.sh, README.md.
# start.sh downloads Mojang's server jar on first run (SHA-1 checked) and unpacks the game data next to it.
#   tools/package_release.sh <name>      e.g. linux-aarch64
set -e
cd "$(dirname "$0")/.."
NAME=${1:?usage: tools/package_release.sh <name>}
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-release -j --target craftpp
D=dist/craftpp-$NAME
rm -rf "$D" && mkdir -p "$D/tools"
cp build-release/craftpp "$D/" && strip "$D/craftpp"
mkdir "$D/data" && (cd data && tar --exclude=./vanilla --exclude=./reports -cf - .) | tar -xf - -C "$D/data"
cp tools/setup_vanilla.sh "$D/tools/" && cp README.md "$D/"
printf '#!/bin/sh\n# First run downloads the Minecraft server data from Mojang, then starts CraftPP.\nset -e\ncd "$(dirname "$0")"\ntools/setup_vanilla.sh\nexec ./craftpp "$@"\n' > "$D/start.sh"
chmod +x "$D/start.sh"
COPYFILE_DISABLE=1 tar -C dist -czf "dist/craftpp-$NAME.tar.gz" "craftpp-$NAME"
echo "dist/craftpp-$NAME.tar.gz"
