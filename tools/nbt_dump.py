#!/usr/bin/env python3
"""Print NBT files as SNBT-ish text: level.dat / playerdata (gzip), or one chunk of a region file.
  tools/nbt_dump.py world/level.dat
  tools/nbt_dump.py world/region/r.0.0.mca 3 5      # chunk (3, 5) of the region (local or world coords)
  tools/nbt_dump.py world/region/r.0.0.mca          # list chunks present
Long arrays are shortened unless --full."""
import gzip, struct, sys, zlib

FULL = "--full" in sys.argv
args = [a for a in sys.argv[1:] if a != "--full"]


class Num(str):
    def __repr__(self): return str(self)


def read(b, i, t):
    if 1 <= t <= 6:  # numbers print with their SNBT suffix
        f, n, suf = {1: ("b", 1, "b"), 2: ("h", 2, "s"), 3: ("i", 4, ""), 4: ("q", 8, "L"), 5: ("f", 4, "f"), 6: ("d", 8, "d")}[t]
        return Num(f"{struct.unpack_from('>' + f, b, i)[0]}{suf}"), i + n
    if t in (7, 11, 12):
        n = struct.unpack_from(">i", b, i)[0]; i += 4
        f, s = {7: ("b", 1), 11: ("i", 4), 12: ("q", 8)}[t]
        return (t, list(struct.unpack_from(f">{n}{f}", b, i))), i + n * s
    if t == 8:
        n = struct.unpack_from(">H", b, i)[0]; i += 2
        return b[i:i + n].decode("utf-8", "replace"), i + n
    if t == 9:
        et, n = b[i], struct.unpack_from(">i", b, i + 1)[0]; i += 5
        out = []
        for _ in range(n):
            v, i = read(b, i, et); out.append(v)
        return out, i
    if t == 10:
        out = {}
        while b[i]:
            et = b[i]; nl = struct.unpack_from(">H", b, i + 1)[0]
            k = b[i + 3:i + 3 + nl].decode(); i += 3 + nl
            out[k], i = read(b, i, et)
        return out, i + 1
    raise ValueError(f"tag {t}")


def show(v, ind=0):
    p = "  " * ind
    if isinstance(v, dict):
        if not v: return "{}"
        return "{\n" + "".join(f"{p}  {k}: {show(x, ind + 1)}\n" for k, x in v.items()) + p + "}"
    if isinstance(v, tuple):
        t, a = v
        s = a if FULL or len(a) <= 8 else a[:4] + ["..."]
        return f"[{'BIL'[(7, 11, 12).index(t)]}; {len(a)}] " + str(s)
    if isinstance(v, list):
        if not v: return "[]"
        if all(not isinstance(x, (dict, list, tuple)) for x in v) and len(v) < 40: return str(v)
        return "[\n" + "".join(f"{p}  {show(x, ind + 1)}\n" for x in v) + p + "]"
    return repr(v)


def root(b):
    return read(b, 3 + struct.unpack_from(">H", b, 1)[0], 10)[0]


def chunk(path, x, z):
    d = open(path, "rb").read()
    off = struct.unpack_from(">I", d, 4 * ((x & 31) + (z & 31) * 32))[0]
    if not off: return None
    at = (off >> 8) * 4096
    n, comp = struct.unpack_from(">IB", d, at)
    data = d[at + 5:at + 4 + n]
    return root(zlib.decompress(data) if comp == 2 else gzip.decompress(data) if comp == 1 else data)


if __name__ == "__main__":
    path = args[0]
    if path.endswith(".mca"):
        if len(args) == 1:
            d = open(path, "rb").read()
            print([(i & 31, i >> 5) for i in range(1024) if struct.unpack_from(">I", d, 4 * i)[0]])
        else:
            print(show(chunk(path, int(args[1]), int(args[2]))))
    else:
        b = open(path, "rb").read()
        if b[:2] == b"\x1f\x8b": b = gzip.decompress(b)
        print(show(root(b)))
