#!/bin/sh
# Builds the server in Release mode and packs it with data/ into dist/craftpp-<name>.tar.gz.
#   tools/package_release.sh <name>      e.g. linux-aarch64
# The archive runs as-is: extract it, cd into the folder, ./craftpp (first run writes eula.txt).
set -e
cd "$(dirname "$0")/.."
NAME=${1:?usage: tools/package_release.sh <name>}
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-release -j --target craftpp
D=dist/craftpp-$NAME
rm -rf "$D" && mkdir -p "$D"
cp build-release/craftpp "$D/" && strip "$D/craftpp"
cp -R data README.md "$D/"
COPYFILE_DISABLE=1 tar -C dist -czf "dist/craftpp-$NAME.tar.gz" "craftpp-$NAME"
echo "dist/craftpp-$NAME.tar.gz"
