#!/bin/sh
# Build CraftPP. `./build.sh test` also runs the ping + bot tests against a temporary server.
set -e
cd "$(dirname "$0")"
[ -f data/config.bin ] || python3 tools/gen_data.py
[ -f data/game.json ] || python3 tools/gen_game_data.py
[ -f data/mobs.json ] || python3 tools/gen_mob_data.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
if [ "$1" = "test" ]; then cmake --build build -j; else cmake --build build -j --target craftpp; fi

if [ "$1" = "test" ]; then
  # Free port per run so parallel test runs (other worktrees) never collide.
  PORT=${TEST_PORT:-$(python3 -c "import socket; s=socket.socket(); s.bind((\"\",0)); print(s.getsockname()[1])")}
  ./build/mobs_test
  rm -rf build/test-world
  printf 'server-port=%s\nlevel-name=build/test-world\nlevel-type=flat\n' "$PORT" > build/test.properties
  rm -f build/banned-players.json build/banned-ips.json build/whitelist.json
  printf '[{"uuid": "", "name": "Alice", "level": 4, "bypassesPlayerLimit": false}, {"uuid": "", "name": "Sam", "level": 4, "bypassesPlayerLimit": false}]\n' > build/ops.json  # bot.py and survival.py run op commands
  ./build/craftpp build/test.properties > build/test-server.log 2>&1 &
  PID=$!
  trap 'kill $PID 2>/dev/null' EXIT
  sleep 0.5
  python3 test/ping.py 127.0.0.1 $PORT
  python3 -u test/bot.py 127.0.0.1 $PORT
  python3 -u test/persist.py 127.0.0.1 $PORT
  python3 -u test/survival.py 127.0.0.1 $PORT
  python3 -u test/commands_test.py 127.0.0.1 $PORT
  kill $PID; wait $PID 2>/dev/null || true
  ./build/craftpp build/test.properties >> build/test-server.log 2>&1 &
  PID=$!
  sleep 0.5
  python3 -u test/persist.py 127.0.0.1 $PORT --after-restart
  python3 -u test/commands_test.py 127.0.0.1 $PORT --after-restart
  echo "all tests passed"
fi
