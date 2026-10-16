#!/bin/sh
# Pi-like benchmark (docs/performance.md): builds a Linux aarch64 Release binary in Docker (Debian bookworm =
# Raspberry Pi OS toolchain), then runs tools/bench.py against it in a container limited like a Pi 4.
#   [CMAKE_ARGS=-DCRAFTPP_CPU=cortex-a72] tools/bench.sh [--cc gcc|clang] [bench.py args...]   e.g. --cpus 1.5 --walk 30
set -e
cd "$(dirname "$0")/.."
CC=gcc
if [ "$1" = "--cc" ]; then CC=$2; shift 2; fi
[ "$CC" = clang ] && CXX=clang++ || CXX=g++
BUILD=build-linux-$CC
docker build -q -t craftpp-pi tools/docker >/dev/null
docker run --rm -v "$PWD:$PWD" -w "$PWD" craftpp-pi sh -c "
  cmake -S . -B $BUILD -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=$CXX $CMAKE_ARGS >/dev/null &&
  cmake --build $BUILD -j 2>&1 | grep -E 'warning|error|Error' || true
  test -x $BUILD/craftpp"
exec python3 tools/bench.py --bin "$BUILD/craftpp" "$@"
