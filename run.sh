#!/bin/sh
# Build, then start the server in the foreground (type `stop` to quit).
set -e
cd "$(dirname "$0")"
./build.sh
exec ./build/craftpp
