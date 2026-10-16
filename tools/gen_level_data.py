#!/usr/bin/env python3
"""Block physics data for the level tick system -> data/level.json (loaded by src/level/data.hpp).

Sources:
  * tools/dump/BlockDump.java run against vanilla/versions/26.3/server-26.3.jar (build time only): per-state
    collision boxes, legacy solid/liquid flags, face sturdiness (FULL/CENTER/RIGID), random ticking, light,
    fluid state; per-block class chain and fire odds. Cached in data/block_dump.json, so regenerating without
    a JDK still works.
  * vanilla/src/data block tags used by block behaviours (resolved to block indices = blocks.json order).
"""
import glob, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VAN = os.path.join(ROOT, "vanilla")
DATA = os.path.join(VAN, "src", "data", "minecraft")
JAR = os.path.join(VAN, "versions", "26.3", "server-26.3.jar")
DUMP = os.path.join(ROOT, "data", "block_dump.json")


def find_jdk():
    for c in [os.environ.get("JAVA_HOME", "")] + sorted(glob.glob("/Library/Java/JavaVirtualMachines/*/Contents/Home"), reverse=True) + \
            sorted(glob.glob("/usr/lib/jvm/*"), reverse=True):
        if c and os.path.exists(os.path.join(c, "bin", "javac")):
            v = subprocess.run([os.path.join(c, "bin", "java"), "-version"], capture_output=True, text=True).stderr
            m = re.search(r'version "(\d+)', v)
            if m and int(m.group(1)) >= 25:
                return c
    return None


def run_dump():
    jdk = find_jdk()
    if not jdk or not os.path.exists(JAR):
        if os.path.exists(DUMP):
            print("gen_level_data: no JDK >= 25 / server jar, using cached", DUMP)
            return json.load(open(DUMP))
        sys.exit("gen_level_data: need a JDK >= 25 and vanilla/versions/26.3/server-26.3.jar (or data/block_dump.json)")
    cp = ":".join([JAR] + sorted(glob.glob(os.path.join(VAN, "libraries", "**", "*.jar"), recursive=True)))
    out = os.path.join(ROOT, "build", "blockdump")
    os.makedirs(out, exist_ok=True)
    subprocess.run([os.path.join(jdk, "bin", "javac"), "-nowarn", "-d", out, "-cp", cp,
                    os.path.join(ROOT, "tools", "dump", "BlockDump.java")], check=True)
    subprocess.run([os.path.join(jdk, "bin", "java"), "-cp", out + ":" + cp, "BlockDump", os.path.join(out, "dump.json")],
                   check=True, capture_output=True, cwd=out)
    d = json.load(open(os.path.join(out, "dump.json")))
    with open(DUMP, "w") as f:
        json.dump(d, f, separators=(",", ":"))
    return d


dump = run_dump()
blocks_rep = json.load(open(os.path.join(VAN, "generated", "reports", "blocks.json")))
names = [n.split(":")[1] for _, n in sorted((min(s["id"] for s in b["states"]), n) for n, b in blocks_rep.items())]
index = {n: i for i, n in enumerate(names)}
assert [b[0] for b in dump["blocks"]] == names, "block registry order differs from blocks.json"


def load_tags(kind):
    root = os.path.join(DATA, "tags", kind)
    raw = {}
    for dp, _, fs in os.walk(root):
        for f in fs:
            if f.endswith(".json"):
                rel = os.path.relpath(os.path.join(dp, f), root)[:-5].replace(os.sep, "/")
                raw[rel] = json.load(open(os.path.join(dp, f)))["values"]
    out = {}

    def res(t):
        if t in out:
            return out[t]
        out[t] = []
        r = []
        for v in raw.get(t, []):
            v = v["id"] if isinstance(v, dict) else v
            r += res(v[1:].split(":")[-1]) if v.startswith("#") else [v.split(":")[-1]]
        out[t] = list(dict.fromkeys(r))
        return out[t]

    for t in raw:
        res(t)
    return out


block_tags = load_tags("block")
tags = {t: sorted({index[v] for v in vs if v in index}) for t, vs in block_tags.items()}
fluid_tags = load_tags("fluid")
print(f"gen_level_data: {len(dump['states'])} states, {len(dump['shapes'])} shapes, {len(tags)} block tags")
defaults = {n.split(":")[1]: next(st["id"] for st in b["states"] if st.get("default")) for n, b in blocks_rep.items()}
out = {"shapes": dump["shapes"], "states": dump["states"], "blocks": [b[1:] + [defaults[b[0]]] for b in dump["blocks"]], "tags": tags,
       "fluid_tags": fluid_tags}
with open(os.path.join(ROOT, "data", "level.json"), "w") as f:
    json.dump(out, f, separators=(",", ":"))
