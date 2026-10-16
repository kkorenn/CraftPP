#!/bin/sh
# Build CraftPP. `./build.sh test` also runs the ping + bot tests against a temporary server.
set -e
cd "$(dirname "$0")"
[ -f data/config.bin ] || python3 tools/gen_data.py
[ -f data/game.json ] || python3 tools/gen_game_data.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
if [ "$1" = "test" ]; then cmake --build build -j; else cmake --build build -j --target craftpp; fi

if [ "$1" = "test" ]; then
  PORT=${TEST_PORT:-25599}
  rm -rf build/test-world
  printf 'server-port=%s\nlevel-name=build/test-world\nlevel-type=flat\n' "$PORT" > build/test.properties
  ./build/craftpp build/test.properties > build/test-server.log 2>&1 &
  PID=$!
  trap 'kill $PID 2>/dev/null' EXIT
  sleep 0.5
  python3 test/ping.py 127.0.0.1 $PORT
  python3 -u test/bot.py 127.0.0.1 $PORT
  python3 -u test/persist.py 127.0.0.1 $PORT
  kill $PID; wait $PID 2>/dev/null || true
  ./build/craftpp build/test.properties >> build/test-server.log 2>&1 &
  PID=$!
  sleep 0.5
  python3 -u test/persist.py 127.0.0.1 $PORT --after-restart
  echo "all tests passed"
fi
