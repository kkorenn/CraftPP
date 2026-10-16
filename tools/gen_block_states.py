#!/usr/bin/env python3
"""Compact block-state table for the plugin bridge, from the vanilla blocks report.
One line per block: `name first_state default_state [prop=v1,v2;prop2=...]`. State ids are
first + mixed-radix index over the properties sorted by name (last property fastest); asserted here.
Usage: python3 tools/gen_block_states.py [vanilla/generated/reports/blocks.json] [out]"""
import json, sys

src = sys.argv[1] if len(sys.argv) > 1 else "vanilla/generated/reports/blocks.json"
out = sys.argv[2] if len(sys.argv) > 2 else "java/src/main/resources/block-states-26.3.txt"
blocks = json.load(open(src))
lines = []
for name, b in blocks.items():  # report order == registry order == block id
    props = sorted(b.get("properties", {}).items())  # vanilla StateDefinition sorts by name
    states = b["states"]
    first = min(s["id"] for s in states)
    default = next(s["id"] for s in states if s.get("default"))
    for s in states:
        idx = 0
        for p, vals in props:
            idx = idx * len(vals) + vals.index(s["properties"][p])
        assert s["id"] == first + idx, (name, s)
    lines.append(f"{name} {first} {default}" + (" " + ";".join(f"{p}={','.join(v)}" for p, v in props) if props else ""))
open(out, "w").write("\n".join(lines) + "\n")
print(f"{len(lines)} blocks -> {out}")
