#!/usr/bin/env python3
"""Compare /locate structure against vanilla.
  python3 test/locate_compare.py --seed S [--at X Z ...] [--structures id ...]
Runs a fresh vanilla server per seed (vanilla/compare_run), issues `execute positioned X 64 Z run locate structure ID`
from the console, parses the answers and compares them with build/structure_test (same origin). Exit 1 on mismatch."""
import argparse, os, re, shutil, subprocess, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JAVA = "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home/bin/java"
RUN = f"{ROOT}/vanilla/compare_run"
DEFAULT = ["village_plains", "village_desert", "village_savanna", "village_snowy", "village_taiga", "pillager_outpost",
           "stronghold", "desert_pyramid", "jungle_pyramid", "swamp_hut", "igloo", "monument", "mansion", "mineshaft",
           "mineshaft_mesa", "shipwreck", "shipwreck_beached", "ocean_ruin_cold", "ocean_ruin_warm", "ruined_portal",
           "ruined_portal_desert", "ruined_portal_jungle", "ruined_portal_mountain", "ruined_portal_ocean",
           "ruined_portal_swamp", "buried_treasure", "ancient_city", "trail_ruins", "trial_chambers"]
ap = argparse.ArgumentParser()
ap.add_argument("--seed", required=True)
ap.add_argument("--at", nargs="+", type=int, default=[0, 0])
ap.add_argument("--structures", nargs="+", default=DEFAULT)
ap.add_argument("--port", type=int, default=25612)
a = ap.parse_args()
origins = list(zip(a.at[0::2], a.at[1::2]))
ids = [s if ":" in s else "minecraft:" + s for s in a.structures]

def vanilla():
    shutil.rmtree(f"{RUN}/world", ignore_errors=True)
    os.makedirs(RUN, exist_ok=True)
    open(f"{RUN}/eula.txt", "w").write("eula=true\n")
    open(f"{RUN}/server.properties", "w").write(f"level-seed={a.seed}\nserver-port={a.port}\nonline-mode=false\n"
                                                 "view-distance=2\nsimulation-distance=2\nsync-chunk-writes=false\n")
    log = f"{RUN}/locate.log"
    p = subprocess.Popen([JAVA, "-Xmx2G", "-jar", f"{ROOT}/vanilla/server.jar", "nogui"], cwd=RUN, stdin=subprocess.PIPE,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT)
    while "Done (" not in open(log).read():
        assert p.poll() is None, "vanilla exited"
        time.sleep(0.5)
    out = {}
    for (x, z) in origins:
        for i in ids:
            start = len(open(log).read())
            p.stdin.write(f"execute positioned {x} 64 {z} run locate structure {i}\n".encode()); p.stdin.flush()
            while True:
                t = open(log).read()[start:]
                m = re.search(r"The nearest \S+ is at \[(-?\d+), ~, (-?\d+)\]|Could not find a structure", t)
                if m: break
                time.sleep(0.2)
            out[(x, z, i)] = (int(m.group(1)), int(m.group(2))) if m.group(1) else None
    p.stdin.write(b"stop\n"); p.stdin.flush(); p.wait(120)
    return out

def craftpp():
    out = {}
    for (x, z) in origins:
        r = subprocess.run([f"{ROOT}/build/structure_test", a.seed, str(x), str(z)] + ids, cwd=ROOT, capture_output=True, text=True)
        for line in r.stdout.splitlines():
            f = line.split()
            out[(x, z, f[0])] = None if f[1] == "none" else (int(f[1]), int(f[2]))
    return out

v, c = vanilla(), craftpp()
bad = 0
for k in v:
    ok = v[k] == c.get(k)
    bad += not ok
    print(("OK  " if ok else "BAD ") + f"{k[2]} from {k[0]},{k[1]}: vanilla {v[k]} craftpp {c.get(k)}")
print(f"{len(v) - bad}/{len(v)} match")
sys.exit(1 if bad else 0)
