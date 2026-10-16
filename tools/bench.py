#!/usr/bin/env python3
"""Pi-like benchmark: runs a Linux build of CraftPP in a CPU/memory-limited container and drives it with a bot.
Usually started through tools/bench.sh (which builds the binary first).
  python3 tools/bench.py [--bin build-linux/craftpp] [--cpus 1.5] [--memory 1g] [--walk 30] [--label NAME]
Prints one markdown table row per metric (idle RSS, join time, gen ms/chunk, CPU idle, walking)."""
import argparse, math, os, shutil, socket, struct, subprocess, sys, threading, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "test"))
from bot import login, varint, PLAY_CB, PLAY_SB

ap = argparse.ArgumentParser()
ap.add_argument("--bin", default="build-linux/craftpp")
ap.add_argument("--image", default="craftpp-pi")
ap.add_argument("--cpus", default="1.5")
ap.add_argument("--memory", default="1g")
ap.add_argument("--port", type=int, default=25700)
ap.add_argument("--seed", default="12345")
ap.add_argument("--view", type=int, default=8)
ap.add_argument("--walk", type=float, default=30, help="seconds of walking east")
ap.add_argument("--speeds", default="10,40", help="walk phases, blocks/s (creative flight ~10.9, elytra ~30+)")
ap.add_argument("--extra-props", default="", help="extra server.properties lines, ';'-separated")
args = ap.parse_args()

NAME = "craftpp-bench"
work = os.path.join(ROOT, os.path.dirname(args.bin), "bench")
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
open(f"{work}/server.properties", "w").write(
    f"server-port=25565\nlevel-name={work}/world\nlevel-type=normal\nlevel-seed={args.seed}\nview-distance={args.view}\n"
    "pause-when-empty-seconds=60\n" + "".join(l + "\n" for l in args.extra_props.split(";") if l))
vanilla = os.path.realpath(os.path.join(ROOT, "vanilla"))
# vanilla/ is a symlink to an absolute host path: mount it elsewhere and recreate that path inside the container
# (bind-mounting at the host path itself is unreliable on Docker Desktop).
MOUNTS = ["-v", f"{ROOT}:{ROOT}", "-v", f"{vanilla}:/vanilla:ro", "-w", ROOT]
LINK = f"mkdir -p {os.path.dirname(vanilla)} && ln -sfn /vanilla {vanilla} && "
subprocess.run(["docker", "rm", "-f", NAME], capture_output=True)
docker = ["docker", "run", "-d", "--name", NAME, f"--cpus={args.cpus}", f"--memory={args.memory}", "--memory-swap", args.memory,
          "-p", f"{args.port}:25565"] + MOUNTS + [args.image]
t_start = time.time()
subprocess.run(docker + ["sh", "-c", LINK + f"exec {ROOT}/{args.bin} {work}/server.properties > {work}/server.log 2>&1"], check=True,
               capture_output=True)

def proc(field_file):
    return subprocess.run(["docker", "exec", NAME, "cat", f"/proc/1/{field_file}"], capture_output=True, text=True).stdout
def rss():  # MiB
    st = dict(l.split(":", 1) for l in proc("status").splitlines() if ":" in l)
    return int(st["VmRSS"].split()[0]) / 1024, int(st["VmHWM"].split()[0]) / 1024
def cpu_ticks():
    f = proc("stat").rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])  # utime + stime (USER_HZ = 100)
def cpu_pct(seconds):
    t0, c0 = time.time(), cpu_ticks()
    time.sleep(seconds)
    return (cpu_ticks() - c0) / (time.time() - t0)  # ticks/s at 100 Hz == percent of one core

rows = []
def row(metric, value): rows.append((metric, value)); print(f"  {metric}: {value}", flush=True)
try:
    t0 = t_start
    while "listening" not in open(f"{work}/server.log").read():
        if time.time() - t0 > 600: raise TimeoutError(open(f"{work}/server.log").read())
        time.sleep(0.1)
    row("startup (spawn search + generator init)", f"{time.time() - t0:.1f} s")
    time.sleep(6)  # past the first autosave (5 s), which trims the heap
    row("idle RSS, 0 players", "%.1f MiB" % rss()[0])
    row("CPU idle, 0 players", f"{cpu_pct(10):.1f} %")

    t0 = time.time()
    c, n = login("Bench", "127.0.0.1", args.port, view=args.view)
    join = time.time() - t0
    side = 2 * args.view + 1
    assert n == side * side, n
    row(f"join, {n} chunks (view {args.view})", f"{join:.2f} s ({1000 * join / n:.1f} ms/chunk)")
    row("RSS after join", "%.1f MiB" % rss()[0])

    # Reader thread: drain packets, track the client's chunk set.
    pings, lat = [], []  # /list round trips: tick-thread responsiveness
    lock, have, stop, last_chunk = threading.Lock(), set(), threading.Event(), [time.time()]
    send = c.send
    def locked_send(pid, body=b""):
        with lock: send(pid, body)
    c.send = locked_send
    c.s.settimeout(None)
    def reader():
        try:
            while not stop.is_set():
                pid, b = c.recv()
                if pid == PLAY_CB["level_chunk_with_light"]:
                    have.add((b.i32(), b.i32())); last_chunk[0] = time.time()
                elif pid == PLAY_CB["forget_level_chunk"]:
                    v = b.i64(); have.discard((struct.unpack(">i", struct.pack(">I", v & 0xFFFFFFFF))[0], v >> 32))
                elif pid == PLAY_CB["system_chat"] and b"players online" in b.d and pings:
                    lat.append(time.time() - pings.pop(0))
                elif pid == PLAY_CB["player_position"]:
                    tid, pos = b.varint(), b.take(24); b.take(24)
                    c.send(PLAY_SB["accept_teleportation"], varint(tid) + pos + b.take(8))
        except Exception:
            pass
    have.update(c.chunks)
    threading.Thread(target=reader, daemon=True).start()
    time.sleep(2)
    row("CPU, 1 idle player", f"{cpu_pct(10):.1f} %")
    row("RSS, 1 idle player", "%.1f MiB" % rss()[0])

    x, y, z = c.pos
    for speed in [float(v) for v in args.speeds.split(",")]:
        x0, lat[:] = x, []
        t0, c0, peak, sampled = time.time(), cpu_ticks(), 0, time.time()
        while time.time() - t0 < args.walk:
            x = x0 + speed * (time.time() - t0)
            c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", x, y, z, 0))
            time.sleep(0.05)
            if time.time() - sampled > 1:
                peak, sampled = max(peak, rss()[0]), time.time()
                pings.append(time.time()); c.send(PLAY_SB["chat_command"], varint(4) + b"list")
        walk_cpu = (cpu_ticks() - c0) / (time.time() - t0)
        ccx, ccz = math.floor(x) >> 4, math.floor(z) >> 4
        want = {(ccx + dx, ccz + dz) for dx in range(-args.view, args.view + 1) for dz in range(-args.view, args.view + 1)}
        behind = len(want - have)
        t1 = time.time()
        while not want <= have and time.time() - t1 < 120: time.sleep(0.05)
        catchup = time.time() - t1
        row(f"walk {args.walk:.0f} s at {speed:g} b/s: CPU", f"{walk_cpu:.0f} %")
        row(f"walk {speed:g}: RSS at end / peak sampled", "%.1f / %.1f MiB" % (rss()[0], max(peak, rss()[0])))
        row(f"walk {speed:g}: chunks missing when stopping / catch-up", f"{behind} / {catchup:.1f} s")
        if lat: row(f"walk {speed:g}: /list round trip avg / max", f"{1000 * sum(lat) / len(lat):.0f} / {1000 * max(lat):.0f} ms")
    row("peak RSS (VmHWM)", "%.1f MiB" % rss()[1])
    stop.set()
    c.s.close()
finally:
    subprocess.run(["docker", "rm", "-f", NAME], capture_output=True)

tt = os.path.join(ROOT, os.path.dirname(args.bin), "terrain_test")
if os.path.exists(tt):  # generator alone, single thread, same limits
    out = subprocess.run(["docker", "run", "--rm", f"--cpus={args.cpus}", f"--memory={args.memory}"] + MOUNTS +
                         [args.image, "sh", "-c", LINK + tt], capture_output=True, text=True).stdout
    for l in out.splitlines():
        if l.startswith("fingerprint"): row("worldgen fingerprint (must match macOS)", l.split()[1])
        if "ms/chunk" in l: row("generator " + l.split(":")[0].split(";")[0], l.split(":")[1].split(";")[0].strip())
print("\n| metric | value |\n|---|---|")
for m, v in rows: print(f"| {m} | {v} |")
