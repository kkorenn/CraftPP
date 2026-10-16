#!/bin/sh
# Build CraftPP. `./build.sh test` also runs the ping + bot tests against a temporary server.
# `./build.sh [test] asan|tsan` does the same with ASan+UBSan / TSan in build-asan/ or build-tsan/.
set -e
cd "$(dirname "$0")"
[ -f data/config.bin ] || python3 tools/gen_data.py
[ data/game.json -nt tools/gen_game_data.py ] || python3 tools/gen_game_data.py  # regenerate when the generator changes
TEST=; SAN=; B=build
for a in "$@"; do
  case $a in test) TEST=1;; asan) SAN=address; B=build-asan;; tsan) SAN=thread; B=build-tsan;; esac
done
export BUILD_DIR=$B
cmake -S . -B $B -DCMAKE_BUILD_TYPE=Release -DCRAFTPP_SANITIZE=$SAN >/dev/null
if [ -n "$TEST" ]; then cmake --build $B -j; else cmake --build $B -j --target craftpp; fi
export ASAN_OPTIONS=${ASAN_OPTIONS:-abort_on_error=1:detect_leaks=0} UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
export TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1}

if [ -n "$TEST" ]; then
  ./$B/boss_test
  # Free port per run so parallel test runs (other worktrees) never collide.
  PORT=${TEST_PORT:-$(python3 -c "import socket; s=socket.socket(); s.bind((\"\",0)); print(s.getsockname()[1])")}
  LOG=$B/test-server.log
  fail() { echo "FAIL: $1"; tail -20 $LOG; exit 1; }
  wait_log() {  # wait_log <text> [count]: until the server log has <text> at least <count> times
    for _ in $(seq 300); do
      [ "$(grep -cF -- "$1" $LOG)" -ge "${2:-1}" ] && return 0
      sleep 0.1
    done
    fail "no '$1' in $LOG"
  }

  # EULA gate: a fresh directory gets eula.txt (eula=false) and a full server.properties, then refuses to start.
  rm -rf $B/eula-test && mkdir -p $B/eula-test
  printf 'server-port=%s\nlevel-name=%s/eula-test/world\n' "$PORT" $B > $B/eula-test/server.properties
  ./$B/craftpp $B/eula-test/server.properties > $B/eula-test/out.log 2>&1 || fail "eula refusal exit code"
  grep -q "You need to agree to the EULA in order to run the server. Go to eula.txt for more info." $B/eula-test/out.log || fail "eula message"
  grep -q "^eula=false" $B/eula-test/eula.txt || fail "eula.txt not written"
  grep -q "^white-list=true" $B/eula-test/server.properties || fail "full vanilla server.properties not written"
  [ ! -d $B/eula-test/world ] || fail "world created before eula was accepted"

  rm -rf $B/test-world $B/logs
  printf 'server-port=%s\nlevel-name=%s/test-world\nlevel-type=flat\ngamemode=creative\nview-distance=8\nwhite-list=false\nspawn-protection=0\n' "$PORT" $B > $B/test.properties
  echo "eula=true" > $B/eula.txt
  rm -f $B/banned-players.json $B/banned-ips.json $B/whitelist.json
  printf '[{"uuid": "", "name": "Alice", "level": 4, "bypassesPlayerLimit": false}, {"uuid": "", "name": "Sam", "level": 4, "bypassesPlayerLimit": false}]\n' > $B/ops.json  # bot.py and survival.py run op commands
  # The console is a FIFO kept open on fd 3, so the tests type commands into it like an admin would.
  rm -f $B/console.fifo && mkfifo $B/console.fifo
  ./$B/craftpp $B/test.properties < $B/console.fifo > $LOG 2>&1 &
  PID=$!
  trap 'kill $PID 2>/dev/null || true; rm -f $B/console.fifo' EXIT
  exec 3> $B/console.fifo
  wait_log 'Done ('
  grep -qE '^\[[0-9:]{8} INFO\]: Done \([0-9]+\.[0-9]{3}s\)! For help, type "help"$' $LOG || fail "Done line format"
  python3 test/ping.py 127.0.0.1 $PORT
  python3 -u test/bot.py 127.0.0.1 $PORT
  python3 -u test/persist.py 127.0.0.1 $PORT
  python3 -u test/survival.py 127.0.0.1 $PORT
  python3 -u test/parity_test.py 127.0.0.1 $PORT
  python3 -u test/light_test.py 127.0.0.1 $PORT
  python3 -u test/commands_test.py 127.0.0.1 $PORT
  python3 -u test/mobs_test.py 127.0.0.1 $PORT
  grep -qE 'Alice\[/127\.0\.0\.1:[0-9]+\] logged in with entity id [0-9]+ at \(\['$B'/test-world\]' $LOG || fail "login line"
  grep -q 'Alice joined the game' $LOG || fail "join line"
  grep -q 'Alice lost connection: Disconnected' $LOG || fail "lost connection line"
  grep -q 'Alice left the game' $LOG || fail "leave line"
  grep -q 'issued server command: /' $LOG || fail "command line"
  # Console commands from piped stdin (with and without the slash, like Paper).
  echo "op Carol" >&3
  echo "/tps" >&3
  wait_log 'Made Carol a server operator'
  wait_log 'TPS from last 1m, 5m, 15m: '
  grep -q '"name": "Carol"' $B/ops.json || fail "op from console not saved"
  echo "deop Carol" >&3
  wait_log 'Made Carol no longer a server operator'
  echo "stop" >&3
  wait $PID || fail "server exit code after stop"
  for l in 'Stopping the server' 'Stopping server' 'Saving players' 'Saving worlds' \
           "Saving chunks for level 'ServerLevel[$B/test-world]'/minecraft:overworld" 'All dimensions are saved'; do
    grep -qF "$l" $LOG || fail "shutdown line: $l"
  done
  grep -q '\[Server thread/INFO\]: Done (' $B/logs/latest.log || fail "logs/latest.log format"
  exec 3>&-

  # Restart; this time stdin closing (EOF) stops the server.
  rm -f $B/console.fifo && mkfifo $B/console.fifo
  ./$B/craftpp $B/test.properties < $B/console.fifo >> $LOG 2>&1 &
  PID=$!
  exec 3> $B/console.fifo
  wait_log 'Done (' 2
  ls $B/logs/*-1.log* > /dev/null 2>&1 || fail "previous latest.log was not rolled over"
  python3 -u test/persist.py 127.0.0.1 $PORT --after-restart
  python3 -u test/commands_test.py 127.0.0.1 $PORT --after-restart
  python3 -u test/mobs_test.py 127.0.0.1 $PORT --after-restart
  exec 3>&-
  wait $PID || fail "server exit code after EOF"
  [ "$(grep -c 'All dimensions are saved' $LOG)" -eq 2 ] || fail "no clean save on EOF"
  python3 -u test/stress.py --server ./$B/craftpp --quick ${SAN:+--no-rss-check} || fail "stress (log: $B/stress/server.log)"  # own server
  PLUGIN_PORT=$(python3 -c "import socket; s=socket.socket(); s.bind((\"\",0)); print(s.getsockname()[1])")
  [ -n "$SAN" ] || python3 -u test/plugin_test.py $PLUGIN_PORT  # Bukkit plugins; prints SKIP without a Java 25+ JDK
  python3 -u test/boss_fight.py  # own server: debug-end-fight
  echo "all tests passed"
fi
