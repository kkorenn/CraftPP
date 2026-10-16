#!/bin/sh
# Build CraftPP. `./build.sh test` also runs the ping + bot tests against a temporary server.
set -e
cd "$(dirname "$0")"
[ -f data/config.bin ] || python3 tools/gen_data.py
[ -f data/level.json ] || python3 tools/gen_level_data.py
[ data/game.json -nt tools/gen_game_data.py ] || python3 tools/gen_game_data.py  # regenerate when the generator changes
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
if [ "$1" = "test" ]; then cmake --build build -j; else cmake --build build -j --target craftpp; fi

if [ "$1" = "test" ]; then
  # Free port per run so parallel test runs (other worktrees) never collide.
  PORT=${TEST_PORT:-$(python3 -c "import socket; s=socket.socket(); s.bind((\"\",0)); print(s.getsockname()[1])")}
  LOG=build/test-server.log
  fail() { echo "FAIL: $1"; tail -20 $LOG; exit 1; }
  wait_log() {  # wait_log <text> [count]: until the server log has <text> at least <count> times
    for _ in $(seq 300); do
      [ "$(grep -cF -- "$1" $LOG)" -ge "${2:-1}" ] && return 0
      sleep 0.1
    done
    fail "no '$1' in $LOG"
  }

  # EULA gate: a fresh directory gets eula.txt (eula=false) and a full server.properties, then refuses to start.
  rm -rf build/eula-test && mkdir -p build/eula-test
  printf 'server-port=%s\nlevel-name=build/eula-test/world\n' "$PORT" > build/eula-test/server.properties
  ./build/craftpp build/eula-test/server.properties > build/eula-test/out.log 2>&1 || fail "eula refusal exit code"
  grep -q "You need to agree to the EULA in order to run the server. Go to eula.txt for more info." build/eula-test/out.log || fail "eula message"
  grep -q "^eula=false" build/eula-test/eula.txt || fail "eula.txt not written"
  grep -q "^white-list=true" build/eula-test/server.properties || fail "full vanilla server.properties not written"
  [ ! -d build/eula-test/world ] || fail "world created before eula was accepted"

  rm -rf build/test-world build/logs
  printf 'server-port=%s\nlevel-name=build/test-world\nlevel-type=flat\ngamemode=creative\nview-distance=8\nwhite-list=false\nspawn-protection=0\n' "$PORT" > build/test.properties
  echo "eula=true" > build/eula.txt
  rm -f build/banned-players.json build/banned-ips.json build/whitelist.json
  printf '[{"uuid": "", "name": "Alice", "level": 4, "bypassesPlayerLimit": false}, {"uuid": "", "name": "Sam", "level": 4, "bypassesPlayerLimit": false}]\n' > build/ops.json  # bot.py and survival.py run op commands
  # The console is a FIFO kept open on fd 3, so the tests type commands into it like an admin would.
  rm -f build/console.fifo && mkfifo build/console.fifo
  ./build/craftpp build/test.properties < build/console.fifo > $LOG 2>&1 &
  PID=$!
  trap 'kill $PID 2>/dev/null || true; rm -f build/console.fifo' EXIT
  exec 3> build/console.fifo
  wait_log 'Done ('
  grep -qE '^\[[0-9:]{8} INFO\]: Done \([0-9]+\.[0-9]{3}s\)! For help, type "help"$' $LOG || fail "Done line format"
  python3 test/ping.py 127.0.0.1 $PORT
  python3 -u test/bot.py 127.0.0.1 $PORT
  python3 -u test/persist.py 127.0.0.1 $PORT
  python3 -u test/survival.py 127.0.0.1 $PORT
  python3 -u test/level_test.py 127.0.0.1 $PORT
  python3 -u test/placement_test.py 127.0.0.1 $PORT
  python3 -u test/parity_test.py 127.0.0.1 $PORT
  python3 -u test/light_test.py 127.0.0.1 $PORT
  python3 -u test/commands_test.py 127.0.0.1 $PORT
  grep -qE 'Alice\[/127\.0\.0\.1:[0-9]+\] logged in with entity id [0-9]+ at \(\[build/test-world\]' $LOG || fail "login line"
  grep -q 'Alice joined the game' $LOG || fail "join line"
  grep -q 'Alice lost connection: Disconnected' $LOG || fail "lost connection line"
  grep -q 'Alice left the game' $LOG || fail "leave line"
  grep -q 'issued server command: /' $LOG || fail "command line"
  # Console commands from piped stdin (with and without the slash, like Paper).
  echo "op Carol" >&3
  echo "/tps" >&3
  wait_log 'Made Carol a server operator'
  wait_log 'TPS from last 1m, 5m, 15m: '
  grep -q '"name": "Carol"' build/ops.json || fail "op from console not saved"
  echo "deop Carol" >&3
  wait_log 'Made Carol no longer a server operator'
  echo "stop" >&3
  wait $PID || fail "server exit code after stop"
  for l in 'Stopping the server' 'Stopping server' 'Saving players' 'Saving worlds' \
           "Saving chunks for level 'ServerLevel[build/test-world]'/minecraft:overworld" 'All dimensions are saved'; do
    grep -qF "$l" $LOG || fail "shutdown line: $l"
  done
  grep -q '\[Server thread/INFO\]: Done (' build/logs/latest.log || fail "logs/latest.log format"
  exec 3>&-

  # Restart; this time stdin closing (EOF) stops the server.
  rm -f build/console.fifo && mkfifo build/console.fifo
  ./build/craftpp build/test.properties < build/console.fifo >> $LOG 2>&1 &
  PID=$!
  exec 3> build/console.fifo
  wait_log 'Done (' 2
  ls build/logs/*-1.log* > /dev/null 2>&1 || fail "previous latest.log was not rolled over"
  python3 -u test/persist.py 127.0.0.1 $PORT --after-restart
  python3 -u test/commands_test.py 127.0.0.1 $PORT --after-restart
  exec 3>&-
  wait $PID || fail "server exit code after EOF"
  [ "$(grep -c 'All dimensions are saved' $LOG)" -eq 2 ] || fail "no clean save on EOF"
  PLUGIN_PORT=$(python3 -c "import socket; s=socket.socket(); s.bind((\"\",0)); print(s.getsockname()[1])")
  python3 -u test/plugin_test.py $PLUGIN_PORT  # Bukkit plugins; prints SKIP without a Java 25+ JDK
  echo "all tests passed"
fi
