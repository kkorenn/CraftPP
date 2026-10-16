#!/usr/bin/env python3
"""Compare CraftPP world generation against the vanilla reference server, chunk by chunk.
  python3 test/compare.py --seed 12345 --radius 2 [--mode full|solid] [--vanilla-datapack DIR] [--against craftpp|vanilla] [--light]
Runs vanilla (vanilla/compare_run/, fresh world), records the chunks within --radius of the world spawn
chunk, stops it, then runs the other server (CraftPP by default; `--against vanilla` = self-check)
with the same seed, walks the bot to the same spawn and diffs block states + biomes (--light: also sky/block
light per section, after applying light_update packets for --settle seconds).
Exit status 1 when anything differs."""
import argparse, collections, json, os, shutil, socket, struct, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bot import login, play_step, PLAY_SB

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JAVA = "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home/bin/java"
MIN_Y = -64
RUN = f"{ROOT}/vanilla/compare_run"

ap = argparse.ArgumentParser()
ap.add_argument("--seed", required=True)
ap.add_argument("--radius", type=int, default=2)
ap.add_argument("--mode", choices=["full", "solid"], default="full")
ap.add_argument("--vanilla-datapack", help="datapack dir copied into the vanilla world (also disables structures)")
ap.add_argument("--against", choices=["craftpp", "vanilla"], default="craftpp")
ap.add_argument("--no-structures", action="store_true", help="generate-structures=false without a datapack")
ap.add_argument("--port", type=int, default=25611)
ap.add_argument("--run-dir", default=RUN, help="vanilla server directory (one per parallel compare run)")
ap.add_argument("--examples", type=int, default=10)
ap.add_argument("--timeout", type=float, default=300)
ap.add_argument("--light", action="store_true", help="also compare sky + block light")
ap.add_argument("--edits", help="';'-separated console commands run after the chunks arrived ({x} {y} {z} = spawn), "
                "e.g. \"fill {x} {y-30} {z} {x+6} {y-24} {z+6} air;setblock {x+3} {y-27} {z+3} torch\" (tests incremental light); 'wait' settles in between")
ap.add_argument("--settle", type=float, default=5, help="seconds to keep reading light_update after the last chunk")
ap.add_argument("--fluids", action="store_true", help="also settle without --light: apply the block updates of fluids "
                "that flow after post-processing (vanilla sends chunks right after it)")
args = ap.parse_args()
RUN = os.path.abspath(args.run_dir)
view = min(32, int(args.radius * 1.42) + 2)  # vanilla sends a circle; cover the square + spawn fuzz

def wait_log(path, needle, proc, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if needle in open(path, errors="replace").read(): return
        assert proc.poll() is None, f"server exited, see {path}"
        time.sleep(0.5)
    raise TimeoutError(f"no {needle!r} in {path}")

def settle(c):  # both servers send time / keep-alive packets, so this returns
    end = time.time() + args.settle
    while time.time() < end: play_step(c)

def collect(port, center, proc, edited=False):
    """Join, walk to center (block pos; None = stay at spawn) and gather the chunk square around it."""
    c, _ = login("Cmp2" if edited else "Cmp", "127.0.0.1", port, view=view, decode=True)
    c.s.settimeout(args.timeout)
    while not (center or c.spawn): play_step(c)
    center = center or c.spawn
    ccx, ccz = center[0] >> 4, center[2] >> 4
    need = {(ccx + dx, ccz + dz) for dx in range(-args.radius, args.radius + 1) for dz in range(-args.radius, args.radius + 1)}
    c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", center[0] + .5, center[1], center[2] + .5, 1))
    while not need <= c.chunks.keys(): play_step(c)
    if args.light or args.fluids: settle(c)  # neighbours may still be lighting: take their light_update packets too
    if args.edits and not edited:
        x, y, z = center
        for cmd in args.edits.split(";"):
            cmd = eval("f" + repr(cmd.strip()), {}, dict(x=x, y=y, z=z))  # f-string: {y-30} works
            if cmd == "wait": settle(c); continue  # let light run between edits (tests decreases)
            proc.stdin.write(cmd.encode() + b"\n"); proc.stdin.flush()
        settle(c)
        c.s.close()
        # Vanilla sends light_update only for chunks on the view border (clients relight edits themselves),
        # so rejoin to read the server's light from fresh chunk packets.
        return collect(port, center, proc, True)
    c.s.close()
    if args.light: LIGHT.append({k: c.light[k] for k in need})
    return {k: c.chunks[k] for k in need}, c.biomes, center

LIGHT = []  # per run: {(cx, cz): {"sky"|"block": {section index: 2048 bytes}}}

def run_vanilla(n, center):
    os.makedirs(RUN, exist_ok=True)
    shutil.rmtree(f"{RUN}/world", ignore_errors=True)
    if args.vanilla_datapack:
        shutil.copytree(args.vanilla_datapack, f"{RUN}/world/datapacks/{os.path.basename(os.path.normpath(args.vanilla_datapack))}")
    open(f"{RUN}/eula.txt", "w").write("eula=true\n")
    props = dict(online_mode="false", white_list="false", level_seed=args.seed, server_port=args.port, spawn_protection=0,
                 view_distance=view, simulation_distance=view, network_compression_threshold=-1,
                 generate_structures="false" if args.vanilla_datapack or args.no_structures else "true", sync_chunk_writes="false")
    open(f"{RUN}/server.properties", "w").write("".join(f"{k.replace('_', '-')}={v}\n" for k, v in props.items()))
    log = f"{RUN}/server_{n}.log"
    p = subprocess.Popen([JAVA, "-Xmx2G", "-jar", f"{ROOT}/vanilla/server.jar", "nogui"], cwd=RUN,
                         stdin=subprocess.PIPE, stdout=open(log, "w"), stderr=subprocess.STDOUT)
    try:
        wait_log(log, "Done (", p, 600)
        return collect(args.port, center, p)
    finally:
        p.stdin.write(b"stop\n"); p.stdin.flush()
        try: p.wait(120)
        except subprocess.TimeoutExpired: p.kill()

def run_craftpp(center):
    pack = os.path.basename(os.path.normpath(args.vanilla_datapack)) if args.vanilla_datapack else ""
    stage = next((s for s in ("noise", "surface", "carvers") if pack.startswith(s)), "features")
    props = f"{ROOT}/build/compare.properties"
    shutil.rmtree(f"{ROOT}/build/compare-world", ignore_errors=True)  # throwaway world: no saved chunks
    open(props, "w").write(f"server-port={args.port}\nlevel-seed={args.seed}\nview-distance={args.radius + 1}\n"
                           f"level-name=build/compare-world\nlevel-type=normal\nwhite-list=false\n"
                           f"worldgen-stages={stage}\n")
    open(f"{ROOT}/build/eula.txt", "w").write("eula=true\n")  # server dir = build/ (beside the properties file)
    p = subprocess.Popen([f"{ROOT}/build/craftpp", props], cwd=ROOT, stdin=subprocess.PIPE, stdout=open(f"{ROOT}/build/compare-server.log", "w"), stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try: socket.create_connection(("127.0.0.1", args.port)).close(); break
            except OSError: time.sleep(0.1)
        return collect(args.port, center, p)
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
light_bad = 0
if args.light:
    LA, LB = LIGHT
    for kind in ("sky", "block"):
        diff, presence, per_y, ex, pairs, partial = 0, 0, collections.Counter(), [], collections.Counter(), 0
        for (cx, cz) in sorted(A):
            la, lb = LA[cx, cz][kind], LB[cx, cz][kind]
            for si in range(26):
                if (si in la) != (si in lb):
                    presence += 1
                    if len(ex) < args.examples: ex.append(f"  chunk {cx},{cz} section y {(si - 5) * 16}: present in {'vanilla' if si in la else args.against} only")
                a, b = la.get(si, bytes(2048)), lb.get(si, bytes(2048))
                partial += sum(0 < (v >> 4 * h & 15) < 15 for v in a for h in (0, 1))
                if a == b: continue
                for i in range(4096):
                    va, vb = a[i >> 1] >> 4 * (i & 1) & 15, b[i >> 1] >> 4 * (i & 1) & 15
                    if va == vb: continue
                    diff += 1; per_y[(si - 5) * 16] += 1; pairs[va, vb] += 1
                    if len(ex) < args.examples:
                        x, y, z = cx * 16 + (i & 15), (si - 5) * 16 + (i >> 8), cz * 16 + (i >> 4 & 15)
                        blk = NAMES[A[cx, cz][si - 1][0][i]] if 1 <= si <= 24 else "-"
                        ex.append(f"  {(x, y, z)} {blk}: vanilla {va}  {args.against} {vb}")
        light_bad += diff + presence
        print(f"{kind} light: {diff} mismatching values ({100 * diff / (len(A) * 26 * 4096):.3f}%), {presence} section presence mismatches; vanilla has {partial} values strictly between 0 and 15")
        if diff:
            print("  per section y:", ", ".join(f"{y}={n}" for y, n in sorted(per_y.items())))
            print("  top (vanilla, ours):", ", ".join(f"{p}={n}" for p, n in pairs.most_common(6)))
        if ex: print("\n".join(ex))
sys.exit(1 if bad or nb or light_bad else 0)
