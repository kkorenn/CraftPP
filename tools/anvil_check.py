#!/usr/bin/env python3
"""World-format compatibility with the vanilla 26.3 server (logs + a bot, no GUI).
  python3 tools/anvil_check.py craftpp-to-vanilla [--seed S] [--radius R]
      CraftPP generates a world (and edits it from the console), saves; vanilla opens a copy; the bot compares
      the chunks (and the player position) both servers send; then CraftPP reopens the world vanilla saved.
  python3 tools/anvil_check.py vanilla-to-craftpp [--seed S] [--radius R]
      vanilla generates a world, saves; CraftPP opens a copy; the bot compares the chunks.
Chunks are compared block state for block state and biome for biome. Exit 1 on any difference or a chunk
error in either server's log. Work dir: build/anvil_check/."""
import argparse, math, os, shutil, socket, subprocess, sys, time, uuid
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "test"))
from bot import login, play_step

JAVA = os.environ.get("JAVA", "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home/bin/java")
ap = argparse.ArgumentParser()
ap.add_argument("direction", choices=["craftpp-to-vanilla", "vanilla-to-craftpp"])
ap.add_argument("--seed", default="12345")
ap.add_argument("--radius", type=int, default=3)
ap.add_argument("--type", default="normal", help="level-type for the generating server (normal|flat)")
args = ap.parse_args()
WORK = os.path.join(ROOT, "build", "anvil_check")
def offline_uuid(name):  # UUID.nameUUIDFromBytes("OfflinePlayer:" + name): what vanilla offline mode uses
    import hashlib
    b = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode()).digest())
    b[6] = b[6] & 0x0F | 0x30; b[8] = b[8] & 0x3F | 0x80
    return uuid.UUID(bytes=bytes(b))
UID = offline_uuid("AnvilCheck")
ERRORS = ("Couldn't load chunk", "Exception", "Failed to", "corrupt", "Recoverable errors", "wrong location", "ERROR")


def free_port():
    s = socket.socket(); s.bind(("", 0)); p = s.getsockname()[1]; s.close(); return p


def start(kind, dir, port):
    os.makedirs(dir, exist_ok=True)
    open(f"{dir}/eula.txt", "w").write("eula=true\n")
    props = dict(online_mode="false", white_list="false", level_seed=args.seed, server_port=port, spawn_protection=0,
                 view_distance=args.radius + 3, simulation_distance=args.radius + 3, level_type=args.type,
                 gamemode="creative", network_compression_threshold=-1,
                 level_name="world" if kind == "vanilla" else f"{dir}/world")
    open(f"{dir}/server.properties", "w").write("".join(f"{k.replace('_', '-')}={v}\n" for k, v in props.items()))
    log = f"{dir}/{kind}.log"
    if kind == "vanilla":
        cmd, cwd = [JAVA, "-Xmx2G", "-jar", f"{ROOT}/vanilla/server.jar", "nogui"], dir
    else:  # craftpp reads data/ relative to the cwd: run from the repo root
        cmd, cwd = [f"{ROOT}/build/craftpp", f"{dir}/server.properties"], ROOT
    p = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.PIPE, stdout=open(log, "w"), stderr=subprocess.STDOUT)
    end = time.time() + 300
    while "Done (" not in open(log, errors="replace").read():
        assert p.poll() is None and time.time() < end, f"{kind} did not start, see {log}"
        time.sleep(0.3)
    if kind == "vanilla":  # no block/fluid/random ticks: the saved world is what the bot saw
        p.stdin.write(b"tick freeze\n"); p.stdin.flush()
        time.sleep(1)
    return p


def stop(p, dir, kind):
    p.stdin.write(b"stop\n"); p.stdin.flush()
    p.wait(120)
    bad = [l for l in open(f"{dir}/{kind}.log", errors="replace") if any(e in l for e in ERRORS) and "Unpacking" not in l]
    return bad


def command(p, cmd):
    p.stdin.write(cmd.encode() + b"\n"); p.stdin.flush()


def collect(port):
    c, _ = login("AnvilCheck", "127.0.0.1", port, view=args.radius + 3, decode=True, uid=UID)
    c.s.settimeout(60)
    cx, cz = math.floor(c.pos[0]) >> 4, math.floor(c.pos[2]) >> 4
    need = {(cx + dx, cz + dz) for dx in range(-args.radius, args.radius + 1) for dz in range(-args.radius, args.radius + 1)}
    end = time.time() + 120
    while not need <= c.chunks.keys() and time.time() < end: play_step(c)
    pos = tuple(c.pos)
    c.s.close()
    return {k: c.chunks[k] for k in need if k in c.chunks}, pos


def diff(a, b, what):
    n = 0
    for k in sorted(a.keys() & b.keys()):
        for si, (sa, sb) in enumerate(zip(a[k], b[k])):
            for i, (x, y) in enumerate(zip(sa[0], sb[0])):
                if x != y:
                    if n < 10: print(f"  {what}: chunk {k} section {si} index {i}: state {x} vs {y}")
                    n += 1
            if sa[1] != sb[1]:
                if n < 10: print(f"  {what}: chunk {k} section {si}: biomes differ")
                n += 1
    print(f"{what}: {len(a.keys() & b.keys())} chunks compared, {n} differences")
    return n


shutil.rmtree(WORK, ignore_errors=True)
first, second = ("craftpp", "vanilla") if args.direction == "craftpp-to-vanilla" else ("vanilla", "craftpp")
A, B = f"{WORK}/{first}", f"{WORK}/{second}"
fails = 0
p = start(first, A, port := free_port())
chunks_a, pos_a = collect(port)
if first == "craftpp":  # edits: they must survive the trip (block entity included)
    x, y, z = (math.floor(v) for v in pos_a)
    for cmd in ["gamerule random_tick_speed 0",  # game rules travel too; and vanilla's random ticks stay out of the diff
                f"setblock {x + 2} {y - 1} {z} minecraft:oak_stairs[facing=east,half=top]",
                f"setblock {x + 3} {y - 1} {z} minecraft:chest[facing=north]", f"fill {x - 3} {y - 4} {z - 3} {x - 1} {y - 2} {z - 1} glass"]:
        command(p, cmd)
    time.sleep(1)
    chunks_a, pos_a = collect(port)
bad = stop(p, A, first)
fails += len(bad)
for l in bad[:10]: print(f"  {first} log: {l.rstrip()}")
shutil.copytree(f"{A}/world", f"{B}/world")
p = start(second, B, port := free_port())
chunks_b, pos_b = collect(port)
bad = stop(p, B, second)
fails += len(bad)
for l in bad[:10]: print(f"  {second} log: {l.rstrip()}")
print(f"player position: {first} {pos_a}, {second} {pos_b}")
fails += diff(chunks_a, chunks_b, f"{first} -> {second}")
if first == "craftpp":  # and back: the world as vanilla saved it (level.dat without CraftPP's keys)
    C = f"{WORK}/craftpp-again"
    shutil.copytree(f"{B}/world", f"{C}/world")
    p = start("craftpp", C, port := free_port())
    chunks_c, pos_c = collect(port)
    bad = stop(p, C, "craftpp")
    fails += len(bad)
    for l in bad[:10]: print(f"  craftpp (again) log: {l.rstrip()}")
    print(f"player position after vanilla: {pos_c}")
    fails += diff(chunks_b, chunks_c, "vanilla -> craftpp")
print("OK" if not fails else f"FAILED ({fails})")
sys.exit(1 if fails else 0)
