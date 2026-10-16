#!/usr/bin/env python3
"""Stress + fuzz: the server must survive garbage and load, and keep serving.

  python3 test/stress.py HOST PORT [--quick]            against a running server (build.sh test uses --quick)
  python3 test/stress.py --server build-asan/craftpp      spawns its own server (temp world, max-players 500)

Phases (--phases, comma list): fuzz (malformed bytes in every protocol state), cmdfuzz (random op commands from
vanilla's command tree with extreme arguments), bots (concurrent bots moving,
breaking/placing in one shared chunk, chatting, op commands), storm (reconnect storm, abandoned logins),
soak (bots in a loop for --minutes, sampling the server's RSS; needs --server or --pid).
After every phase the server must still answer a status ping and accept a fresh login."""
import argparse, math, os, random, shutil, socket, struct, subprocess, sys, threading, time, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bot import CFG_SB, PLAY_CB, PLAY_SB, PROTO, VERSION, login, string, text_of, varint, wait

T0 = time.time()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def frame(body): return varint(len(body)) + body
def pkt(pid, body=b""): return frame(varint(pid) + body)
def handshake(port, nxt, proto=PROTO): return pkt(0, varint(proto) + string("localhost") + struct.pack(">H", port) + varint(nxt))
def pos(x, y, z): return struct.pack(">q", ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF))

def play_prefix(port, name):
    """Handshake + login + configuration, pipelined: the server reads them in order, so no round trips."""
    return (handshake(port, 2) + pkt(0, string(name) + uuid.uuid4().bytes) + pkt(3) +
            pkt(CFG_SB["select_known_packs"], varint(1) + string("minecraft") + string("core") + string(VERSION)) +
            pkt(CFG_SB["finish_configuration"]))

def blast(host, port, data, linger=0.2):
    """Fresh connection: send `data`, drain replies until the server closes or `linger` s of silence."""
    try:
        s = socket.create_connection((host, port), timeout=5)
    except OSError as e:
        raise AssertionError(f"connect failed: {e}")
    try:
        s.sendall(data)
        s.settimeout(linger)
        while s.recv(1 << 16): pass
    except OSError:
        pass
    finally:
        s.close()

def close(c):
    """Really close a bot connection (c.f, the makefile, keeps the fd open after a plain close)."""
    try: c.s.shutdown(socket.SHUT_RDWR)
    except OSError: pass
    c.s.close()

def alive(host, port, what):
    """Status ping + a full login must still work."""
    for attempt in range(50):
        try:
            s = socket.create_connection((host, port), timeout=10)
            s.sendall(handshake(port, 1) + pkt(0))
            data = b""
            while b"version" not in data:
                d = s.recv(4096)
                assert d, "closed"
                data += d
            s.close()
            break
        except (OSError, AssertionError) as e:
            if attempt == 49: raise AssertionError(f"server dead after {what}: {e}")
            time.sleep(0.2)
    c, n = login("Probe%d" % random.randrange(10**6), host, port)
    assert n > 0
    # join() runs on the network thread, so also prove the tick thread is alive: it echoes chat.
    word = "probe%d" % random.randrange(10**9)
    c.send(PLAY_SB["chat"], string(word) + bytes(8 + 8 + 1) + varint(0) + bytes(3) + b"\x00")
    c.s.settimeout(30)
    wait(c, PLAY_CB["system_chat"], lambda b: word in text_of(b), limit=100000)
    close(c)
    print(f"{what} ok [{time.time() - T0:.1f}s]", flush=True)

# ---------- fuzz ----------
NAN, INF = float("nan"), float("inf")
VARINTS = [0, 1, -1, 2, 3, 5, 6, 7, 8, 9, 40, 45, 46, 127, 128, 255, 256, 32767, 65535, 2**31 - 1, -2**31, -2]
LONGS = [0, -1, 2**63 - 1, -2**63, (2**25) << 38, ((2**25 - 1) << 38) | ((2**25 - 1) << 12) | 0x7FF]
DOUBLES = [NAN, INF, -INF, 1e308, -1e308, 3.0e7, -3.0e7, 1e10, 2**31, -2**31 - 1.0, 0.5, -0.0, 5e-324]
FLOATS = [NAN, INF, -INF, 3.4e38, -3.4e38, 1e10, 0.5]

def atom(r):
    k = r.randrange(9)
    if k == 0: return varint(r.choice(VARINTS))
    if k == 1: return struct.pack(">Q", r.choice(LONGS + [r.getrandbits(64)]) & (2**64 - 1))
    if k == 2: return struct.pack(">d", r.choice(DOUBLES))
    if k == 3: return struct.pack(">f", r.choice(FLOATS))
    if k == 4: return struct.pack(">h", r.choice([-1, -999, -32768, 32767, 45, 46, 100, 5]))
    if k == 5: return varint(r.choice([-1, 2**31 - 1, 40000, 300, 17])) + bytes(r.randrange(20))  # strings / arrays
    if k == 6: return bytes(r.randrange(256) for _ in range(r.randrange(1, 40)))
    if k == 7: return string(r.choice(["", "a" * 300, "\x00\xff", "minecraft:brand", "x" * 40000, "§k", "퟿"]))
    return bytes([r.randrange(3)])

def body(r): return b"".join(atom(r) for _ in range(r.randrange(0, 12)))

def fuzz(host, port, rounds, seed, pid=None, flood_mib=256, threads=8):
    r0 = random.Random(seed)
    # Framing / handshake / status / login garbage.
    cases = [
        b"\xff\xff\xff\xff\xff\x0f",                     # varint length too long
        b"\xff\xff\x7f",                                 # 2 MiB length, then nothing
        b"\x80\x80\x80\x80\x80\x80",                     # unterminated varint
        frame(b""),                                      # empty packet: no id
        b"\x00" * 64,                                    # zero-length frames
        frame(b"\x00"),                                  # handshake with no fields
        frame(b"\x00" + varint(PROTO) + varint(-1)),    # negative string length
        frame(b"\x00" + varint(PROTO) + varint(10**6)),  # string longer than the packet
        handshake(port, 1) + pkt(0) + pkt(0) + pkt(0) + pkt(1, b"\x01"),  # truncated ping long
        handshake(port, 1) + pkt(5) + pkt(-1 & 0xFFFFFFFF),
        handshake(port, 7), handshake(port, -1), handshake(port, 2**31 - 1), handshake(port, 2, proto=1),
        handshake(port, 3) + pkt(0, string("x" * 17)),   # transfer intent, over-long name
        handshake(port, 2) + pkt(0, string("")),         # empty name, no uuid
        handshake(port, 2) + pkt(0, string("ok") + b"\x00" * 4),   # truncated uuid
        handshake(port, 2) + pkt(0, string("../../etc") + uuid.uuid4().bytes),
        handshake(port, 2) + pkt(0, string("Fz%d" % r0.randrange(10**6)) + uuid.uuid4().bytes) + b"\xff\xff\x7f",
        handshake(port, 2) + pkt(0, string("Fz%d" % r0.randrange(10**6)) + uuid.uuid4().bytes) + pkt(3) + pkt(99) +
        pkt(CFG_SB["select_known_packs"], varint(2**31 - 1)) + pkt(CFG_SB["finish_configuration"], b"junk"),
        play_prefix(port, "Fz%d" % r0.randrange(10**6))[:-3],   # cut mid-configuration
    ]
    for d in cases: blast(host, port, d)
    for i in range(rounds * 20):                          # random bytes in each pre-play state
        r = random.Random(seed * 7919 + i)
        pre = r.choice([b"", handshake(port, 1), handshake(port, 2),
                        handshake(port, 2) + pkt(0, string("Fz%d" % i) + uuid.uuid4().bytes) + pkt(3)])
        blast(host, port, pre + b"".join(pkt(r.randrange(-1, 12) & 0xFFFFFFFF, body(r)) for _ in range(3)), 0.05)
    alive(host, port, "fuzz: framing/handshake/status/login/config")

    # Play: every serverbound id with structured garbage. A handler that throws kicks the player (expected),
    # so each connection sends a short burst and many connections run in parallel.
    ids = sorted(set(PLAY_SB.values()) | {max(PLAY_SB.values()) + 1, 0x7F, 1000})
    errs = []
    def worker(t):
        try:
            for i in range(t, rounds * len(ids), threads):
                r = random.Random(seed * 104729 + i)
                pid = ids[i % len(ids)]
                burst = b"".join(pkt(pid, body(r)) for _ in range(4))
                blast(host, port, play_prefix(port, "Fz%d_%d" % (t, i)) + pkt(PLAY_SB["player_loaded"]) + burst, 0.15)
        except Exception as e:
            errs.append(e)
    ts = [threading.Thread(target=worker, args=(t,)) for t in range(threads)]
    for t in ts: t.start()
    for t in ts: t.join()
    assert not errs, errs[0]
    alive(host, port, "fuzz: play packets")

    # Flood: 1 MiB packets as fast as loopback goes. The inbox must apply backpressure, not buffer it all.
    before = rss_kib(pid) if pid else 0
    s = socket.create_connection((host, port), timeout=30)
    s.sendall(play_prefix(port, "Flood") + pkt(PLAY_SB["player_loaded"]))
    big = pkt(1000, bytes(1 << 20))
    drained = [0]
    def drain():
        try:
            while (d := s.recv(1 << 16)): drained[0] += len(d)
        except OSError: pass
    threading.Thread(target=drain, daemon=True).start()
    try:
        for _ in range(flood_mib): s.sendall(big)
    except OSError:
        pass  # being disconnected is fine too
    grown = (rss_kib(pid) - before) // 1024 if pid else 0
    s.close()
    assert flood_mib < 256 or grown < 128, f"RSS grew {grown} MiB during a packet flood"
    alive(host, port, "fuzz: flood" + (f" (RSS +{grown} MiB)" if pid else ""))

# ---------- bots ----------
def lock_send(c):
    lk, raw_send = threading.Lock(), c.send
    def send(pid, b=b""):
        with lk: raw_send(pid, b)
    c.send = send

def bot_session(host, port, name, seconds, r, errs, op=False):
    try:
        c, _ = login(name, host, port)
    except Exception as e:  # a full server is fine; anything else is not
        errs.append(f"{name} login: {e!r}")
        return
    lock_send(c)
    stop = threading.Event()
    def drain():
        try:
            while not stop.is_set(): c.recv()
        except Exception as e:
            if not stop.is_set(): errs.append(f"{name} read: {e!r}")
    rt = threading.Thread(target=drain, daemon=True)
    rt.start()
    x, z, seq = 0.5, 0.5, 1
    end = time.time() + seconds
    try:
        while time.time() < end:
            k = r.randrange(10)
            if k < 3:  # wander far enough to cross chunk borders (chunk streaming on the tick thread)
                x += r.uniform(-12, 12); z += r.uniform(-12, 12)
                x, z = max(-120, min(120, x)), max(-120, min(120, z))
                c.send(PLAY_SB["move_player_pos_rot"], struct.pack(">dddffB", x, -60, z, r.uniform(-180, 180), 0, 1))
            elif k < 6:  # everyone edits the same chunk (0, 0)
                bx, by, bz = r.randrange(16), r.randrange(-61, -50), r.randrange(16)
                if r.randrange(2):
                    c.send(PLAY_SB["player_action"], varint(0) + pos(bx, by, bz) + b"\x01" + varint(seq))
                else:
                    c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 36) + varint(1) + varint(r.randrange(1, 40)) + varint(0) + varint(0))
                    c.send(PLAY_SB["use_item_on"], varint(0) + pos(bx, by, bz) + varint(1) + struct.pack(">fff", .5, 1, .5) + b"\0\0" + varint(seq))
                seq += 1
            elif k == 6:
                c.send(PLAY_SB["chat"], string("hi from %s %d" % (name, seq)) + bytes(8 + 8 + 1) + varint(0) + bytes(3) + b"\x00")
            elif k == 7:
                c.send(PLAY_SB["set_carried_item"], struct.pack(">h", r.randrange(9)))
            elif k == 8 and op:
                c.send(PLAY_SB["chat_command"], string(r.choice([
                    "time add 1000", "weather rain", "weather clear", "gamerule doDaylightCycle false",
                    "gamerule doDaylightCycle true", "scoreboard objectives add s%d dummy" % seq,
                    "scoreboard players add @a s%d 1" % (seq - 1), "fill 0 -61 0 15 -55 15 air",
                    "setblock 3 -60 3 stone", "difficulty hard", "bossbar add b%d \"x\"" % seq, "list",
                    "tp @s 8 -60 8", "clear @s", "give @s stone 64", "worldborder set 1000", "defaultgamemode creative",
                    "whitelist off", "ban Nobody%d" % seq, "pardon Nobody%d" % (seq - 1), "op Bot1", "deop Bot1"])))
            elif k == 9:
                c.send(PLAY_SB["client_tick_end"])
            time.sleep(r.uniform(0.01, 0.08))
    except OSError as e:
        errs.append(f"{name}: {e!r}")
    stop.set()
    close(c)

def bots(host, port, n, seconds, seed, op_name):
    errs = []
    ts = [threading.Thread(target=bot_session, args=(host, port, "Bot%d" % i, seconds, random.Random(seed + i), errs))
          for i in range(n)]
    if op_name:  # first, so its commands (bans, default game mode, ...) run while the others log in
        ts.insert(0, threading.Thread(target=bot_session, args=(host, port, op_name, seconds, random.Random(seed), errs, True)))
    for i, t in enumerate(ts):
        t.start()
        time.sleep(0.05)  # joins overlap other bots' play
    for t in ts: t.join()
    assert not errs, errs[:5]
    alive(host, port, f"bots: {len(ts)} concurrent for {seconds}s")

def storm(host, port, n, seed, threads=8):
    """Connections opened and dropped at every point of the login, plus full joins that vanish at once."""
    full = play_prefix(port, "x")
    errs = []
    def worker(t):
        r = random.Random(seed + t)
        try:
            for i in range(t, n, threads):
                d = play_prefix(port, "St%d" % i)
                d = d[:r.randrange(len(d) + 1)] if r.randrange(3) else d + pkt(PLAY_SB["player_loaded"])
                blast(host, port, d, r.choice([0, 0.01, 0.1]))
        except Exception as e:
            errs.append(e)
    ts = [threading.Thread(target=worker, args=(t,)) for t in range(threads)]
    for t in ts: t.start()
    for t in ts: t.join()
    assert not errs, errs[0]
    del full
    alive(host, port, f"storm: {n} connections")

# ---------- op command fuzz ----------
# Random walks over vanilla's command tree (generated/reports/commands.json) with extreme argument values.
ARGS = {
    "brigadier:integer": ["-2147483648", "2147483647", "-1", "0", "1", "1000000", "30000000", "64"],
    "brigadier:double": ["1e308", "-1e308", "999999999999999999999", "30000001", "-0.0", ".5", "1"],
    "brigadier:bool": ["true", "false"],
    "minecraft:entity": ["@s", "@a", "@e", "@p", "@r", "@e[limit=2147483647]", "@e[distance=..-1]", "Nobody", "@e[type=item]"],
    "minecraft:block_pos": ["~ ~ ~", "30000000 0 30000000", "~2147483647 ~ ~", "-2147483648 -2147483648 -2147483648",
                            "^ ^ ^1000000", "0 -64 0", "0 319 0", "~-1 ~-1 ~-1", "29999999 320 -29999999"],
    "minecraft:vec3": ["~ ~ ~", "1e308 0 0", "30000001 0 0", "~1000000 ~ ~", "^ ^ ^-1e9", "0.5 -2147483649 0.5"],
    "minecraft:column_pos": ["30000000 30000000", "-2147483648 2147483647", "~ ~"],
    "minecraft:rotation": ["~ ~", "1e30 -1e30", "180 90"], "minecraft:vec2": ["~ ~", "1e30 -1e30"],
    "minecraft:int_range": ["..-1", "2147483647..", "5..1", "-2147483648..2147483647"],
    "minecraft:float_range": ["..-1", "1e308..", "5..1"],
    "minecraft:time": ["2147483647d", "-1", "0t", "1000000000s", "1"],
    "minecraft:item_stack": ["stone", "stone[damage=2147483647]", "diamond_sword[max_damage=-1]", "air", "nosuch"],
    "minecraft:block_state": ["stone", "oak_stairs[facing=up]", "air", "nosuch", "water[level=99]"],
    "minecraft:block_predicate": ["stone", "#minecraft:logs", "*"],
    "minecraft:nbt_compound_tag": ["{}", "{a:" * 3000 + "1" + "}" * 3000, "{a:[I;1,2]}", "{"],
    "minecraft:nbt_tag": ["1b", "[" * 3000, "{}", '"x"'], "minecraft:nbt_path": ["a.b[0]", "a" + "[0]" * 2000, "{}"],
    "minecraft:component": ['"x"', "[" * 3000, '{"text":"a"}', "{}"], "minecraft:message": ["hi", "@e " * 100],
    "minecraft:objective": ["obj", "x" * 300], "minecraft:score_holder": ["@s", "*", "Nobody"],
    "brigadier:string": ['"x"', "a" * 2000, '""', "word"],
}
GENERIC = ["0", "-1", "2147483647", "~", "@s", "x", "stone", "minecraft:stone", "{}", "a:b", "1.5", "true"]
SKIP = {"stop", "kick", "op", "deop", "ban", "ban-ip", "whitelist", "transfer", "save-off", "publish", "jfr", "perf",
        "debug", "reload", "setidletimeout", "unpublish"}  # would end the run or lock the fuzzer out

def random_command(tree, r):
    words, node = [], tree
    for _ in range(40):
        kids = {k: v for k, v in node.get("children", {}).items() if node is not tree or k not in SKIP}
        if not kids or (node.get("executable") and r.random() < 0.3): break
        name, node = r.choice(list(kids.items()))
        words.append(name if node["type"] == "literal" else r.choice(ARGS.get(node.get("parser"), GENERIC)))
        if "redirect" in node:
            target = tree
            for k in node["redirect"]: target = target["children"][k]
            node = target
        elif node.get("executable") and not node.get("children") and r.random() < 0.8: break
    return " ".join(words)

def cmdfuzz(host, port, n, seed, op):
    import json
    tree = json.load(open(os.path.join(ROOT, "vanilla/generated/reports/commands.json")))
    r = random.Random(seed)
    sent = 0
    while sent < n:
        c, _ = login(op, host, port)
        lock_send(c)
        def drain():
            try:
                while True: c.recv()
            except Exception:
                pass
        threading.Thread(target=drain, daemon=True).start()
        try:
            while sent < n:
                c.send(PLAY_SB["chat_command"], string(random_command(tree, r)[:30000]))
                sent += 1
                time.sleep(0.005)
        except OSError:
            pass  # kicked (e.g. bad characters) or killed: log in again
        close(c)
    time.sleep(1)
    alive(host, port, f"cmdfuzz: {n} op commands")

# ---------- own server + soak ----------
def rss_kib(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return int(out) if out else 0

def spawn(binary, world, view):
    d = os.path.join(os.path.dirname(os.path.abspath(binary)), "stress")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    s = socket.socket(); s.bind(("", 0)); port = s.getsockname()[1]; s.close()
    open(os.path.join(d, "server.properties"), "w").write(
        f"server-port={port}\nlevel-name={d}/world\nlevel-type={world}\ngamemode=creative\nview-distance={view}\n"
        "max-players=500\nwhite-list=false\npause-when-empty-seconds=3600\n")
    open(os.path.join(d, "eula.txt"), "w").write("eula=true\n")
    open(os.path.join(d, "ops.json"), "w").write('[{"uuid": "", "name": "Op", "level": 4, "bypassesPlayerLimit": false}]\n')
    log = open(os.path.join(d, "server.log"), "w")
    p = subprocess.Popen([os.path.abspath(binary), os.path.join(d, "server.properties")], cwd=ROOT,
                         stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
    path = os.path.join(d, "server.log")
    for _ in range(1200):
        if "Done (" in open(path).read(): break
        assert p.poll() is None, "server exited during startup:\n" + open(path).read()[-3000:]
        time.sleep(0.1)
    else:
        raise AssertionError("server did not start")
    return p, port, path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    ap.add_argument("port", nargs="?", type=int)
    ap.add_argument("--server", help="spawn this binary with a temp world (stress dir beside it)")
    ap.add_argument("--world", default="flat", help="flat | normal (with --server)")
    ap.add_argument("--view", type=int, default=4, help="view-distance (with --server)")
    ap.add_argument("--pid", type=int, help="server pid for RSS sampling")
    ap.add_argument("--quick", action="store_true", help="< 30 s: small fuzz + 12 bots + small storm")
    ap.add_argument("--phases", default="fuzz,cmdfuzz,bots,storm")
    ap.add_argument("--bots", type=int, default=50)
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--seed", type=int, default=int(os.environ.get("STRESS_SEED", "1")))
    ap.add_argument("--op", default="Op", help="op player name for command bots ('' = none)")
    a = ap.parse_args()
    proc = None
    if a.server:
        proc, a.port, log = spawn(a.server, a.world, a.view)
        a.pid = proc.pid
    if a.quick:
        a.rounds, a.bots, a.seconds = 2, 12, 4
    t0 = time.time()
    try:
        for ph in a.phases.split(","):
            if ph == "fuzz": fuzz(a.host, a.port, a.rounds, a.seed, a.pid, 24 if a.quick else 256)
            elif ph == "bots": bots(a.host, a.port, a.bots, a.seconds, a.seed, a.op)
            elif ph == "cmdfuzz": cmdfuzz(a.host, a.port, 100 if a.quick else a.rounds * 100, a.seed, a.op)
            elif ph == "storm": storm(a.host, a.port, 40 if a.quick else 400, a.seed)
            elif ph == "soak":
                assert a.pid, "soak needs --server or --pid"
                end, samples, i = time.time() + a.minutes * 60, [], 0
                while time.time() < end:
                    bots(a.host, a.port, a.bots, min(a.seconds, 60), a.seed + i, a.op)
                    storm(a.host, a.port, 100, a.seed + i)
                    i += 1
                    samples.append(rss_kib(a.pid))
                    print(f"soak {(time.time() - t0) / 60:.1f} min: RSS {samples[-1] // 1024} MiB", flush=True)
                # Warm-up fills chunk caches; after that RSS should be flat.
                if len(samples) >= 4:
                    base = samples[len(samples) // 3]
                    growth = (samples[-1] - base) / base
                    print(f"soak RSS growth after warm-up: {growth * 100:.1f}%")
                    assert growth < 0.25, f"RSS keeps growing: {samples}"
            else:
                raise SystemExit(f"unknown phase {ph}")
            if proc: assert proc.poll() is None, "server died"
        if a.pid: print(f"server RSS {rss_kib(a.pid) // 1024} MiB")
        print(f"stress ok ({time.time() - t0:.1f}s)")
    finally:
        if proc:
            dead = proc.poll() is not None
            proc.kill()
            proc.wait()
            if dead: print(open(log).read()[-5000:])

if __name__ == "__main__":
    main()
