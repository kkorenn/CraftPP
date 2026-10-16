#!/usr/bin/env python3
"""End-to-end Bukkit plugin test: builds test/plugins/*, starts CraftPP with them in a temp dir, and checks
lifecycle, events, commands, permissions, scheduler and world/player API through two headless bots.
Prints SKIP (exit 0) when no Java 25+ JDK or no bridge jar can be built.
Usage: python3 test/plugin_test.py [port]"""
import glob, json, os, re, shutil, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "test"))
from bot import PLAY_CB, PLAY_SB, Buf, login, string, varint, wait  # noqa: E402

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 25598
BRIDGE = os.path.join(ROOT, "build/java/libs/craftpp-bridge.jar")
OUT = os.path.join(ROOT, "build/test-plugins")


def skip(why):
    print("SKIP plugin_test:", why)
    sys.exit(0)


def find_jdk():
    homes = [os.environ.get("JAVA_HOME", "")] + sorted(glob.glob("/Library/Java/JavaVirtualMachines/*/Contents/Home"), reverse=True) \
        + sorted(glob.glob("/usr/lib/jvm/*"), reverse=True)
    for h in homes:
        try:
            v = re.search(r'JAVA_VERSION="(\d+)', open(os.path.join(h, "release")).read())
        except OSError:
            continue
        if v and int(v.group(1)) >= 25 and os.path.exists(os.path.join(h, "bin/javac")):
            return h
    return None


def build_plugins(jdk):
    if not os.path.exists(BRIDGE):
        if not shutil.which("gradle"):
            skip("bridge jar missing and gradle not installed (tools/build_bridge.sh)")
        subprocess.run(["sh", os.path.join(ROOT, "tools/build_bridge.sh")], check=True)
    jars = []
    for src in sorted(glob.glob(os.path.join(ROOT, "test/plugins/*/"))):
        name = os.path.basename(src.rstrip("/"))
        classes = os.path.join(OUT, name)
        shutil.rmtree(classes, ignore_errors=True)
        os.makedirs(classes)
        javas = glob.glob(os.path.join(src, "src/**/*.java"), recursive=True)
        cp = BRIDGE + os.pathsep + os.pathsep.join(os.path.join(OUT, d) for d in os.listdir(OUT))
        subprocess.run([os.path.join(jdk, "bin/javac"), "-nowarn", "--release", "21", "-cp", cp, "-d", classes] + javas, check=True)
        for r in glob.glob(os.path.join(src, "resources/*")):
            shutil.copy(r, classes)
        jar = os.path.join(OUT, name + ".jar")
        subprocess.run([os.path.join(jdk, "bin/jar"), "cf", jar, "-C", classes, "."], check=True)
        jars.append(jar)
    return jars


def texts(b):
    """Every 'text' string of an NBT text component (legacy § messages become a root + extra list)."""
    d, out, i = b.d[b.i:], [], 0
    while (i := d.find(b"\x08\x00\x04text", i)) >= 0:
        n = struct.unpack(">H", d[i + 7:i + 9])[0]
        out.append(d[i + 9:i + 9 + n].decode())
        i += 9 + n
    return "".join(out)


def chat_with(c, needle, limit=3000):
    return texts(wait(c, PLAY_CB["system_chat"], lambda b: needle.encode() in b.d, limit))


def pos(x, y, z): return struct.pack(">q", ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF))


def rss_kib(pid):
    return int(subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip() or 0)


def start_server(d, port, plugins):
    os.makedirs(os.path.join(d, "plugins"), exist_ok=True)
    for name in ("data", "vanilla"):
        if not os.path.exists(os.path.join(d, name)):
            os.symlink(os.path.join(ROOT, name), os.path.join(d, name))
    for j in plugins:
        shutil.copy(j, os.path.join(d, "plugins"))
    if plugins and not os.path.exists(os.path.join(d, "plugins/jvm.properties")):
        open(os.path.join(d, "plugins/jvm.properties"), "w").write(f"bridge-jar={BRIDGE}\n")
    open(os.path.join(d, "test.properties"), "w").write(f"server-port={port}\nlevel-name=world\nlevel-type=flat\n")
    log = open(os.path.join(d, "server.log"), "w")
    proc = subprocess.Popen([os.path.join(ROOT, "build/craftpp"), "test.properties"], cwd=d, stdin=subprocess.PIPE,
                            stdout=log, stderr=subprocess.STDOUT, text=True)
    for _ in range(300):
        if "listening on port" in open(os.path.join(d, "server.log")).read():
            return proc
        time.sleep(0.05)
    raise AssertionError("server did not start: " + open(os.path.join(d, "server.log")).read()[-2000:])


def stop(proc):
    proc.stdin.write("stop\n"); proc.stdin.flush()
    proc.wait(timeout=30)


def main():
    jdk = find_jdk()
    if not jdk:
        skip("no Java 25+ JDK")
    jars = build_plugins(jdk)
    d = tempfile.mkdtemp(prefix="craftpp-plugins-")
    alice_id = "11111111-2222-3333-4444-555555555555"
    open(os.path.join(d, "ops.json"), "w").write(json.dumps([{"uuid": alice_id, "name": "Alice", "level": 4}]))
    proc = start_server(d, PORT, jars)
    logpath = os.path.join(d, "server.log")
    log = lambda: open(logpath).read()
    try:
        assert "[CraftTestPlugin] onLoad on Paper" in log(), log()
        assert "enabled; greeting=Welcome primary=true" in log(), log()
        assert "[DependsOnTest] enabled after CraftTestPlugin=true" in log(), log()
        print("load/enable + config + depend order ok")
        time.sleep(0.6)
        assert "delayed task ran after" in log()
        print("scheduler runTaskLater ok")

        import uuid
        a, _ = login("Alice", "127.0.0.1", PORT, view=2, uid=uuid.UUID(alice_id))
        msg = chat_with(a, "[CraftTest] Alice")
        assert msg == "[CraftTest] Alice joined", msg
        print("PlayerJoinEvent message ok:", msg)
        b, _ = login("Bob", "127.0.0.1", PORT, view=2)
        a.s.settimeout(20)  # a missing reply fails the test instead of hanging it
        b.s.settimeout(20)
        chat_with(a, "[CraftTest] Bob joined")

        a.send(PLAY_SB["chat_command"], string("crafttest hello world"))
        msg = chat_with(a, "crafttest ok")
        assert msg.startswith("crafttest ok: hello world | Alice " + alice_id) and "op=true" in msg and "world=world" in msg \
            and "online=2" in msg, msg
        a.send(PLAY_SB["chat_command"], string("ct lookup"))
        msg = chat_with(a, "lookup t")
        assert msg == "lookup true true", msg
        print("plugin command + alias + Player/Server API ok:", msg)

        a.send(PLAY_SB["chat_command"], string("craftop"))
        assert chat_with(a, "craftop") == "craftop ok"
        b.send(PLAY_SB["chat_command"], string("craftop"))
        msg = chat_with(b, "permission")
        print("permissions ok (op allowed, non-op denied):", msg)

        a.send(PLAY_SB["chat"], string("hello") + struct.pack(">qq", 0, 0) + b"\x00" + varint(0) + bytes(3) + b"\x00")
        msg = chat_with(b, "hello")
        assert msg == "[Alice] hello (async=true)", msg
        a.send(PLAY_SB["chat"], string("forbidden word") + struct.pack(">qq", 0, 0) + b"\x00" + varint(0) + bytes(3) + b"\x00")
        a.send(PLAY_SB["chat"], string("after") + struct.pack(">qq", 0, 0) + b"\x00" + varint(0) + bytes(3) + b"\x00")
        msg = chat_with(b, "[Alice]")
        assert "after" in msg, msg  # the cancelled message never arrived
        print("AsyncPlayerChatEvent format + cancel ok")

        a.send(PLAY_SB["chat_command"], string("blocked thing"))
        chat_with(a, "blocked by preprocess")
        a.send(PLAY_SB["chat_command"], string("rewrite-me"))
        chat_with(a, "crafttest ok: rewritten")
        print("PlayerCommandPreprocessEvent cancel + rewrite ok")

        # Block API: put diamond under Alice, then try to break it (cancelled -> block resent).
        x, y, z = (int(v // 1) for v in a.pos)
        a.send(PLAY_SB["chat_command"], string("craftblock"))
        msg = chat_with(a, "craftblock")
        assert msg.endswith("-> DIAMOND_BLOCK"), msg
        diamond = int(re.search(r"diamond_block = (\d+)", open(os.path.join(ROOT, "src/gen/ids.hpp")).read()).group(1))
        a.send(PLAY_SB["player_action"], varint(0) + pos(x, y - 1, z) + b"\x01" + varint(7))
        chat_with(a, "break cancelled: DIAMOND_BLOCK")
        wait(a, PLAY_CB["block_update"], lambda q: Buf(q.d[q.i + 8:]).varint() == diamond)
        print("World.getBlockAt/setType + BlockBreakEvent cancel (block resent) ok")

        reg = json.load(open(os.path.join(ROOT, "java/src/main/resources/registries-26.3.json")))
        stone = reg["minecraft:item"]["entries"]["minecraft:stone"]["protocol_id"]
        a.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 36 + 0) + varint(1) + varint(stone) + varint(0) + varint(0))
        a.send(PLAY_SB["use_item_on"], varint(0) + pos(x, 150, z) + varint(1) + struct.pack(">fff", .5, .5, .5) + b"\x00\x00" + varint(8))
        chat_with(a, "place cancelled at y=150")
        print("BlockPlaceEvent cancel ok")

        a.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", 1001.5, a.pos[1], 0.5, 1))
        chat_with(a, "move cancelled")
        tp = wait(a, PLAY_CB["player_position"])
        tp.varint()
        assert struct.unpack(">d", tp.take(8))[0] < 1000
        print("PlayerMoveEvent cancel (teleported back) ok")

        a.send(PLAY_SB["chat_command"], string("crafttest tp"))
        tp = wait(a, PLAY_CB["player_position"])
        tp.varint()
        assert struct.unpack(">ddd", tp.take(24)) == (100.5, 80.0, 100.5)
        a.send(PLAY_SB["chat_command"], string("crafttest creative"))
        wait(a, PLAY_CB["game_event"], lambda q: q.d[q.i] == 3)
        print("Player.teleport + setGameMode ok")

        a.send(PLAY_SB["command_suggestion"], varint(42) + string("/crafttest cr"))
        s = wait(a, PLAY_CB["command_suggestions"])
        tid, start, length, n = s.varint(), s.varint(), s.varint(), s.varint()
        got = []
        for _ in range(n):
            got.append(s.str())
            assert s.u8() == 0  # no tooltip
        assert tid == 42 and start == 11 and "creative" in got, (tid, start, got)
        print("TabCompleter ok:", got)

        a.send(PLAY_SB["chat_command"], string("craftsched"))
        assert chat_with(a, "sched async") == "sched async=true back on main=true"
        chat_with(a, "sched repeating ran 3 times")
        print("runTaskAsynchronously / runTask / BukkitRunnable timer ok")

        a.send(PLAY_SB["chat_command"], string("crafttest broadcast"))
        chat_with(b, "broadcast from plugin")
        print("Bukkit.broadcastMessage ok")

        proc.stdin.write("crafttest from-console\n"); proc.stdin.flush()
        for _ in range(100):
            if "crafttest ok: from-console | console" in log(): break
            time.sleep(0.05)
        assert "crafttest ok: from-console | console" in log(), log()[-1500:]
        print("console command ok")

        b.send(PLAY_SB["chat_command"], string("crafttest kick"))
        wait(b, PLAY_CB["disconnect"])
        chat_with(a, "[CraftTest] Bob left")
        print("kickPlayer + PlayerQuitEvent message ok")
        rss_plugins = rss_kib(proc.pid)
        a.s.close()
    finally:
        stop(proc)
    assert "[CraftTestPlugin] disabled after" in log(), log()[-1500:]
    print("onDisable on stop ok")

    # Memory: the same session without plugins (no JVM at all).
    d2 = tempfile.mkdtemp(prefix="craftpp-noplugins-")
    proc = start_server(d2, PORT + 1, [])
    try:
        c, _ = login("Alice", "127.0.0.1", PORT + 1, view=2)
        time.sleep(0.5)
        rss_plain = rss_kib(proc.pid)
        assert not os.path.exists(os.path.join(d2, "plugins/jvm.properties"))
        c.s.close()
    finally:
        stop(proc)
    print(f"RSS with 2 test plugins: {rss_plugins // 1024} MiB, without plugins: {rss_plain // 1024} MiB")
    shutil.rmtree(d2, ignore_errors=True)
    shutil.rmtree(d, ignore_errors=True)
    print("plugin tests passed")


if __name__ == "__main__":
    main()
