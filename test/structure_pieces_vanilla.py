#!/usr/bin/env python3
"""Dump the pieces of the vanilla structure start nearest to (x, z) in a dimension (reference only, test tool):
  python3 test/structure_pieces_vanilla.py --seed 12345 --dim the_nether --structure minecraft:fortress [--x 0 --z 0]
Runs the vanilla server in --run-dir, locates the structure, force-loads its chunk, saves and reads the start's
piece list (id, bounding box, orientation) from the region file. Compare with structure_test LEGACY=... output."""
import argparse, gzip, os, re, shutil, struct, subprocess, sys, time, zlib
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JAVA = "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home/bin/java"
ap = argparse.ArgumentParser()
ap.add_argument("--seed", default="12345")
ap.add_argument("--dim", default="the_nether")
ap.add_argument("--structure", default="minecraft:fortress")
ap.add_argument("--x", type=int, default=0)
ap.add_argument("--z", type=int, default=0)
ap.add_argument("--port", type=int, default=25751)
ap.add_argument("--run-dir", default=f"{ROOT}/build/vpieces")
ap.add_argument("--radius", type=int, default=0, help="also force-load this many chunks around the start and dump their blocks")
ap.add_argument("--reuse", action="store_true", help="skip the server: read the world of the previous run")
ap.add_argument("--blocks-out", help="write the force-loaded chunks' block states (raw uint16 per block) here")
a = ap.parse_args()

def nbt(b, i, t):  # returns (value, next index)
    if t == 1: return struct.unpack_from(">b", b, i)[0], i + 1
    if t == 2: return struct.unpack_from(">h", b, i)[0], i + 2
    if t == 3: return struct.unpack_from(">i", b, i)[0], i + 4
    if t == 4: return struct.unpack_from(">q", b, i)[0], i + 8
    if t == 5: return struct.unpack_from(">f", b, i)[0], i + 4
    if t == 6: return struct.unpack_from(">d", b, i)[0], i + 8
    if t == 7: n = struct.unpack_from(">i", b, i)[0]; return b[i + 4:i + 4 + n], i + 4 + n
    if t == 8: n = struct.unpack_from(">H", b, i)[0]; return b[i + 2:i + 2 + n].decode("utf-8", "replace"), i + 2 + n
    if t == 9:
        et, n = b[i], struct.unpack_from(">i", b, i + 1)[0]; i += 5; out = []
        for _ in range(n): v, i = nbt(b, i, et); out.append(v)
        return out, i
    if t == 10:
        d = {}
        while b[i] != 0:
            tt = b[i]; name, i = nbt(b, i + 1, 8); d[name], i = nbt(b, i, tt)
        return d, i + 1
    if t == 11: n = struct.unpack_from(">i", b, i)[0]; return list(struct.unpack_from(f">{n}i", b, i + 4)), i + 4 + 4 * n
    if t == 12: n = struct.unpack_from(">i", b, i)[0]; return list(struct.unpack_from(f">{n}q", b, i + 4)), i + 4 + 8 * n
    raise ValueError(t)

def read_chunk(path, cx, cz):
    f = open(path, "rb").read()
    off = struct.unpack_from(">I", f, 4 * ((cx & 31) + (cz & 31) * 32))[0]
    if not off: return None
    p = (off >> 8) * 4096
    n, comp = struct.unpack_from(">IB", f, p)
    data = f[p + 5:p + 4 + n]
    data = zlib.decompress(data) if comp == 2 else gzip.decompress(data) if comp == 1 else data
    return nbt(data, 3, 10)[0]

RUN = a.run_dir
def run_server():
  global x, z
  os.makedirs(RUN, exist_ok=True)
  shutil.rmtree(f"{RUN}/world", ignore_errors=True)
  open(f"{RUN}/eula.txt", "w").write("eula=true\n")
  open(f"{RUN}/server.properties", "w").write(f"online-mode=false\nlevel-seed={a.seed}\nserver-port={a.port}\nsync-chunk-writes=true\n")
  log = f"{RUN}/server.log"
  p = subprocess.Popen([JAVA, "-Xmx2G", "-jar", f"{ROOT}/vanilla/server.jar", "nogui"], cwd=RUN, stdin=subprocess.PIPE,
                       stdout=open(log, "w"), stderr=subprocess.STDOUT)
  def cmd(c): p.stdin.write((c + "\n").encode()); p.stdin.flush()
  def wait(rx, t=600):
      end = time.time() + t
      while time.time() < end:
          m = re.search(rx, open(log, errors="replace").read())
          if m: return m
          time.sleep(0.3)
      raise TimeoutError(rx)
  try:
      wait(r"Done \(")
      dim = f"minecraft:{a.dim}"
      cmd(f"execute in {dim} positioned {a.x} 64 {a.z} run locate structure {a.structure}")
      m = wait(r"nearest .* is at \[(-?\d+), [~\d-]+, (-?\d+)\]")
      x, z = int(m.group(1)), int(m.group(2))
      r = a.radius * 16
      cmd(f"execute in {dim} run forceload add {x - r} {z - r} {x + r} {z + r}")
      wait(r"(Marked|is already marked)")
      time.sleep(5 + a.radius)
      cmd("save-all flush")
      wait(r"Saved the game")
  finally:
      cmd("stop")
      try: p.wait(120)
      except subprocess.TimeoutExpired: p.kill()
  open(f"{RUN}/located.txt", "w").write(f"{x} {z}\n")
if a.reuse: x, z = map(int, open(f"{RUN}/located.txt").read().split())
else: run_server()
cx, cz = x >> 4, z >> 4
c = read_chunk(f"{RUN}/world/dimensions/minecraft/{a.dim}/region/r.{cx >> 5}.{cz >> 5}.mca", cx, cz)
starts = c["structures"]["starts"]
st = starts[a.structure]
print(f"start {a.structure} chunk {cx},{cz} pieces {len(st['Children'])}")
for ch in st["Children"]:
    print(" ", ch["id"], *ch["BB"], ch.get("O"), ch.get("Template", ""), *(ch[k] for k in ("TPX", "TPY", "TPZ") if k in ch), ch.get("Rot", ""))

if a.blocks_out:  # cx0 cz0 n miny height, then n*n chunks x height x 16 x 16 states (y, z, x order) as uint16
    import json
    ids = {}
    for name, blk in json.load(open(f"{ROOT}/data/reports/blocks.json")).items():
        for s_ in blk["states"]:
            ids[(name, tuple(sorted((s_.get("properties") or {}).items())))] = s_["id"]
            if s_.get("default"): ids[(name, ())] = s_["id"]  # bare name: default state
    n = 2 * a.radius + 1
    cx0, cz0 = cx - a.radius, cz - a.radius
    miny, height = (0, 256) if a.dim != "overworld" else (-64, 384)
    out = bytearray(struct.pack("<5i", cx0, cz0, n, miny, height))
    for dz in range(n):
        for dx in range(n):
            ch = read_chunk(f"{RUN}/world/dimensions/minecraft/{a.dim}/region/r.{(cx0 + dx) >> 5}.{(cz0 + dz) >> 5}.mca", cx0 + dx, cz0 + dz)
            col = [0] * (height * 256)
            for sec in (ch or {}).get("sections", []):
                bs = sec.get("block_states")
                if not bs: continue
                pal = [ids[(e, ())] if isinstance(e, str) else ids[(e.get("id") or e.get("Name") or e.get(""), tuple(sorted((e.get("properties") or e.get("Properties") or {}).items())))]
                       for e in bs["palette"]]  # 26.x: {"id", "properties"}; a bare name list becomes {"": name}
                base = (sec["Y"] * 16 - miny) * 256
                if base < 0 or base >= height * 256: continue
                if len(pal) == 1:
                    for i in range(4096): col[base + i] = pal[0]
                    continue
                bits = max(4, (len(pal) - 1).bit_length()); per = 64 // bits; mask = (1 << bits) - 1
                data = bs["data"]
                for i in range(4096):
                    v = (data[i // per] & 0xFFFFFFFFFFFFFFFF) >> ((i % per) * bits) & mask
                    col[base + i] = pal[v]
            out += struct.pack(f"<{len(col)}H", *col)
    open(a.blocks_out, "wb").write(out)
    print("blocks written", a.blocks_out)
