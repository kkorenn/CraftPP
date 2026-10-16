#!/usr/bin/env python3
"""Write a vanilla datapack that strips worldgen down to a stage, for bit-exact comparisons.
  noise:   no features, no carvers, empty material rule -> pure density terrain + aquifers
           (no bedrock / ore veins / surface blocks either: those are material rules in 26.x)
  surface: no features, no carvers; material rules (surface, bedrock, ore veins) kept
  carvers: no features; carvers and material rules kept
  full:    no overrides (pack is a no-op)
  structures: like surface (no features, no carvers) but structures stay on
--keep a,b (with --stage full): only those placed features remain (carvers kept).
Structures are off via generate-structures=false, which test/compare.py sets for every pack but structures_*.
Usage: python3 tools/make_noise_only_datapack.py [--stage noise|surface|carvers|structures|full] [--out DIR]"""
import argparse, glob, json, os, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = f"{ROOT}/vanilla/src/data/minecraft/worldgen"

ap = argparse.ArgumentParser()
ap.add_argument("--stage", choices=["noise", "surface", "carvers", "structures", "full"], default="noise")
ap.add_argument("--keep", help="comma-separated placed features to keep (stage full only): everything else is removed")
ap.add_argument("--out", help="pack directory (default vanilla/compare_run/datapacks/<stage>_only)")
a = ap.parse_args()
out = a.out or f"{ROOT}/vanilla/compare_run/datapacks/{a.stage}_only"
fmt = json.load(open(f"{ROOT}/vanilla/src/version.json"))["pack_version"]["data_major"]

shutil.rmtree(out, ignore_errors=True)
def put(rel, obj):
    p = f"{out}/{rel}"
    os.makedirs(os.path.dirname(p), exist_ok=True)
    json.dump(obj, open(p, "w"), indent=2)

put("pack.mcmeta", {"pack": {"description": f"CraftPP compare: {a.stage} stage", "min_format": fmt, "max_format": fmt}})
if a.stage != "full":
    for f in glob.glob(f"{SRC}/biome/*.json"):
        b = json.load(open(f))
        b["features"] = [[] for _ in b["features"]]
        if a.stage != "carvers": b["carvers"] = []
        put(f"data/minecraft/worldgen/biome/{os.path.basename(f)}", b)
if a.keep:
    keep = {k if ":" in k else "minecraft:" + k for k in a.keep.split(",")}
    for f in glob.glob(f"{SRC}/biome/*.json"):
        b = json.load(open(f))
        b["features"] = [[x for x in step if x in keep] for step in b["features"]]
        put(f"data/minecraft/worldgen/biome/{os.path.basename(f)}", b)
if a.stage == "noise":
    # SequenceRule's codec accepts an empty list; it compiles to "no match" -> default block stays.
    put("data/minecraft/worldgen/material_rule/overworld.json", {"type": "minecraft:sequence", "sequence": []})
print(out)
