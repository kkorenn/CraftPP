#!/usr/bin/env python3
"""Light survives a restart: edit blocks in a generated world, snapshot light, restart (saved chunks load with
their light, the rest regenerate), snapshot again, diff the chunks within 2 of spawn. --no-edits: restart only.
  python3 test/light_reload.py [--no-edits]   (CRAFTPP_LIGHT_VERIFY=1 also checks the fixed point per send)"""
import os, shutil, subprocess, sys, time, socket
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT + "/test")
from bot import *
PORT = 25741
props = f"{ROOT}/build/reload.properties"
open(props, "w").write(f"server-port={PORT}\nlevel-seed=12345\nview-distance=4\nlevel-name=build/reload-world\nlevel-type=normal\nwhite-list=false\n")
shutil.rmtree(f"{ROOT}/build/reload-world", ignore_errors=True)
open(f"{ROOT}/build/eula.txt", "w").write("eula=true\n")
open(f"{ROOT}/build/ops.json", "w").write('[{"uuid": "", "name": "Alice", "level": 4, "bypassesPlayerLimit": false}]\n')
def start():
    p = subprocess.Popen([f"{ROOT}/build/craftpp", props], cwd=ROOT, stdin=subprocess.PIPE, stdout=open(f"{ROOT}/build/reload.log", "a"), stderr=subprocess.STDOUT)
    for _ in range(200):
        try: socket.create_connection(("127.0.0.1", PORT)).close(); return p
        except OSError: time.sleep(0.1)
def snap(name):
    c, _ = login(name, "127.0.0.1", PORT, view=4, decode=True)
    end = time.time() + 3
    while time.time() < end: play_step(c)
    c.s.close(); return c
p = start()
c = snap("Bob")
sx, sy, sz = c.spawn
cmds = [f"fill {sx-4} {sy-30} {sz-4} {sx+4} {sy-25} {sz+4} air", f"setblock {sx} {sy-30} {sz} torch", f"fill {sx+4} {sy-24} {sz+4} {sx+4} {sy+10} {sz+4} air",
        f"setblock {sx+16} {sy-20} {sz} glowstone", f"fill {sx-20} {sy} {sz-20} {sx-12} {sy+8} {sz-12} stone"]
for cmd in ([] if "--no-edits" in sys.argv else cmds): p.stdin.write(cmd.encode() + b"\n"); p.stdin.flush()
time.sleep(1)
before = snap("Ann")
p.stdin.write(b"stop\n"); p.stdin.flush(); p.wait(60)
p = start()
after = snap("Cid")
p.kill(); p.wait()
keys = [k for k in sorted(set(before.light) & set(after.light)) if max(abs(k[0] - (sx >> 4)), abs(k[1] - (sz >> 4))) <= 2]
bad = 0
for k in keys:
    for kind in ("sky", "block"):
        a, b = before.light[k][kind], after.light[k][kind]
        for si in set(a) | set(b):
            x, y = a.get(si, bytes(2048)), b.get(si, bytes(2048))
            if x != y:
                n = sum(((x[i>>1] >> 4*(i&1)) & 15) != ((y[i>>1] >> 4*(i&1)) & 15) for i in range(4096))
                bad += n; print("diff", k, kind, si, n)
print(len(keys), "chunks", bad, "light values differ after restart")
sys.exit(1 if bad else 0)
