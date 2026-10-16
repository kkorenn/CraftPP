#!/usr/bin/env python3
"""Compare CraftPP world generation against the vanilla reference server, chunk by chunk.
  python3 test/compare.py --seed 12345 --radius 2 [--mode full|solid] [--vanilla-datapack DIR] [--against craftpp|vanilla]
Runs vanilla (vanilla/compare_run/, fresh world), records the chunks within --radius of the world spawn
chunk, stops it, then runs the other server (CraftPP by default; `--against vanilla` = self-check)
with the same seed, walks the bot to the same spawn and diffs block states + biomes.
Exit status 1 when anything differs."""
import argparse, collections, json, os, shutil, socket, struct, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bot import login, play_step, PLAY_SB

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JAVA = "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home/bin/java"
RUN = os.environ.get("COMPARE_RUN", f"{ROOT}/vanilla/compare_run")  # override when several worktrees compare at once
MIN_Y = -64

ap = argparse.ArgumentParser()
ap.add_argument("--seed", required=True)
ap.add_argument("--radius", type=int, default=2)
ap.add_argument("--mode", choices=["full", "solid"], default="full")
ap.add_argument("--vanilla-datapack", help="datapack dir copied into the vanilla world (also disables structures)")
ap.add_argument("--against", choices=["craftpp", "vanilla"], default="craftpp")
ap.add_argument("--port", type=int, default=25611)
ap.add_argument("--examples", type=int, default=10)
ap.add_argument("--timeout", type=float, default=300)
args = ap.parse_args()
view = min(32, int(args.radius * 1.42) + 2)  # vanilla sends a circle; cover the square + spawn fuzz

def wait_log(path, needle, proc, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if needle in open(path, errors="replace").read(): return
        assert proc.poll() is None, f"server exited, see {path}"
        time.sleep(0.5)
    raise TimeoutError(f"no {needle!r} in {path}")

def collect(port, center):
    """Join, walk to center (block pos; None = stay at spawn) and gather the chunk square around it."""
    c, _ = login("Cmp", "127.0.0.1", port, view=view, decode=True)
    c.s.settimeout(args.timeout)
    while not (center or c.spawn): play_step(c)
    center = center or c.spawn
    ccx, ccz = center[0] >> 4, center[2] >> 4
    need = {(ccx + dx, ccz + dz) for dx in range(-args.radius, args.radius + 1) for dz in range(-args.radius, args.radius + 1)}
    c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", center[0] + .5, center[1], center[2] + .5, 1))
    while not need <= c.chunks.keys(): play_step(c)
    c.s.close()
    return {k: c.chunks[k] for k in need}, c.biomes, center

def run_vanilla(n, center):
    os.makedirs(RUN, exist_ok=True)
    shutil.rmtree(f"{RUN}/world", ignore_errors=True)
    if args.vanilla_datapack:
        shutil.copytree(args.vanilla_datapack, f"{RUN}/world/datapacks/{os.path.basename(os.path.normpath(args.vanilla_datapack))}")
    open(f"{RUN}/eula.txt", "w").write("eula=true\n")
    props = dict(online_mode="false", white_list="false", level_seed=args.seed, server_port=args.port, spawn_protection=0,
                 view_distance=view, simulation_distance=view, network_compression_threshold=-1,
                 generate_structures="false" if args.vanilla_datapack else "true", sync_chunk_writes="false")
    open(f"{RUN}/server.properties", "w").write("".join(f"{k.replace('_', '-')}={v}\n" for k, v in props.items()))
    log = f"{RUN}/server_{n}.log"
    p = subprocess.Popen([JAVA, "-Xmx2G", "-jar", f"{ROOT}/vanilla/server.jar", "nogui"], cwd=RUN,
                         stdin=subprocess.PIPE, stdout=open(log, "w"), stderr=subprocess.STDOUT)
    try:
        wait_log(log, "Done (", p, 600)
        return collect(args.port, center)
    finally:
        p.stdin.write(b"stop\n"); p.stdin.flush()
        try: p.wait(120)
        except subprocess.TimeoutExpired: p.kill()

def run_craftpp(center):
    stage = "noise" if args.vanilla_datapack and os.path.basename(os.path.normpath(args.vanilla_datapack)).startswith("noise") else "surface"
    props = f"{ROOT}/build/compare.properties"
    shutil.rmtree(f"{ROOT}/build/compare-world", ignore_errors=True)  # throwaway world: no saved chunks
    open(props, "w").write(f"server-port={args.port}\nlevel-seed={args.seed}\nview-distance={args.radius + 1}\n"
                           f"level-name=build/compare-world\nlevel-type=normal\nwhite-list=false\n"
                           f"worldgen-stages={stage}\n")
    open(f"{ROOT}/build/eula.txt", "w").write("eula=true\n")  # server dir = build/ (beside the properties file)
    p = subprocess.Popen([f"{ROOT}/build/craftpp", props], cwd=ROOT, stdout=open(f"{ROOT}/build/compare-server.log", "w"), stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try: socket.create_connection(("127.0.0.1", args.port)).close(); break
            except OSError: time.sleep(0.1)
        return collect(args.port, center)
    finally:
        p.kill(); p.wait()

# Block state id -> "name[props]" from the vanilla report (ids are global and shared by both servers).
NAMES = {}
for name, blk in json.load(open(f"{ROOT}/vanilla/generated/reports/blocks.json")).items():
    for s in blk["states"]:
        pr = s.get("properties")
        NAMES[s["id"]] = name + ("[" + ",".join(f"{k}={v}" for k, v in pr.items()) + "]" if pr else "")
AIR = {"minecraft:air", "minecraft:cave_air", "minecraft:void_air"}
FLUID = {"minecraft:water", "minecraft:lava", "minecraft:bubble_column"}
CLASS = {i: 0 if n.split("[")[0] in AIR else 1 if n.split("[")[0] in FLUID else 2 for i, n in NAMES.items()}
CLASS_NAME = ["air", "fluid", "solid"]

print(f"vanilla run (seed {args.seed}, radius {args.radius}, view {view}) ...", flush=True)
A, bioA, center = run_vanilla(1, None)
print(f"spawn {center}, chunk {center[0] >> 4},{center[2] >> 4}; {args.against} run ...", flush=True)
B, bioB, _ = run_vanilla(2, center) if args.against == "vanilla" else run_craftpp(center)

per_section, per_chunk, examples, block_pairs = collections.Counter(), collections.Counter(), [], collections.Counter()
bio_pairs, bio_examples, total = collections.Counter(), [], 0
for (cx, cz) in sorted(A):
    for si, ((ba, ga), (bb, gb)) in enumerate(zip(A[cx, cz], B[cx, cz])):
        if args.mode == "solid": ba, bb = [CLASS[x] for x in ba], [CLASS[x] for x in bb]
        total += 4096
        if ba != bb:
            for i, (x, y) in enumerate(zip(ba, bb)):
                if x == y: continue
                per_section[si] += 1; per_chunk[cx, cz] += 1
                block_pairs[(CLASS_NAME if args.mode == 'solid' else NAMES)[x], (CLASS_NAME if args.mode == 'solid' else NAMES)[y]] += 1
                if len(examples) < args.examples:
                    pos = (cx * 16 + (i & 15), MIN_Y + si * 16 + (i >> 8), cz * 16 + (i >> 4 & 15))
                    nm = CLASS_NAME if args.mode == "solid" else NAMES
                    examples.append(f"  {pos}: vanilla {nm[x]}  {args.against} {nm[y]}")
        for i, (x, y) in enumerate(zip(ga, gb)):
            na, nb = bioA[x], bioB[y]
            if na == nb: continue
            bio_pairs[na, nb] += 1
            if len(bio_examples) < args.examples:
                pos = (cx * 16 + (i & 3) * 4, MIN_Y + si * 16 + (i >> 4) * 4, cz * 16 + (i >> 2 & 3) * 4)
                bio_examples.append(f"  {pos}: vanilla {na}  {args.against} {nb}")

bad = sum(per_section.values())
print(f"\n{len(A)} chunks, {total} blocks compared ({args.mode} mode): {bad} block mismatches ({100 * bad / total:.2f}%)")
if bad:
    print("per section (y range: mismatches):")
    for si in sorted(per_section): print(f"  y {MIN_Y + si * 16:4}..{MIN_Y + si * 16 + 15:4}: {per_section[si]}")
    print("per chunk:", ", ".join(f"{k[0]},{k[1]}={v}" for k, v in sorted(per_chunk.items())))
    print("top pairs:", *(f"  vanilla {a} -> {args.against} {b}: {n}" for (a, b), n in block_pairs.most_common(8)), sep="\n")
    print("first mismatches:", *examples, sep="\n")
nb = sum(bio_pairs.values())
print(f"{nb} biome mismatches of {len(A) * 24 * 64} cells")
if nb:
    print("top pairs:", *(f"  vanilla {a} -> {args.against} {b}: {n}" for (a, b), n in bio_pairs.most_common(5)), sep="\n")
    print("first mismatches:", *bio_examples, sep="\n")
sys.exit(1 if bad or nb else 0)
