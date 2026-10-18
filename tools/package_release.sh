#!/bin/sh
# Builds the server in Release mode and packs it into dist/craftpp-<name>.tar.gz:
#   craftpp, data/ (CraftPP's own tables, no Mojang files), tools/setup_vanilla.sh, README.md.
# On first start craftpp runs setup_vanilla.sh: Mojang's server jar is downloaded (SHA-1 checked) and unpacked.
#   tools/package_release.sh <name>      e.g. linux-aarch64
set -e
cd "$(dirname "$0")/.."
NAME=${1:?usage: tools/package_release.sh <name>}
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-release -j 2 --target craftpp
D=dist/craftpp-$NAME
rm -rf "$D" && mkdir -p "$D/tools"
cp build-release/craftpp "$D/" && strip "$D/craftpp"
mkdir "$D/data" && (cd data && tar --exclude=./vanilla --exclude=./reports -cf - .) | tar -xf - -C "$D/data"
cp tools/setup_vanilla.sh "$D/tools/" && cp README.md "$D/"
COPYFILE_DISABLE=1 tar -C dist -czf "dist/craftpp-$NAME.tar.gz" "craftpp-$NAME"
echo "dist/craftpp-$NAME.tar.gz"
