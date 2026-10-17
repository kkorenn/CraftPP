#!/usr/bin/env python3
"""On-disk format check of a world CraftPP saved (build.sh: after test/persist.py and a clean stop).
  python3 test/storage_test.py build/test-world
Reads the files the way vanilla 26.3 does (tools/nbt_dump.py): region chunk NBT, level.dat, SavedData files,
players/data/<uuid>.dat. Uses persist.py's edits: stone placed at (0, -60, 0), (2, -61, 2) broken, Steve at
(5.5, -60, 7.5)."""
import os, sys, uuid
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
from nbt_dump import chunk, root
import gzip

world = sys.argv[1]
dim = f"{world}/dimensions/minecraft/overworld"


def block_at(c, x, y, z):
    sec = next(s for s in c["sections"] if int(s["Y"][:-1]) == y >> 4)
    bs = sec["block_states"]
    pal = bs["palette"]
    if "data" not in bs: return pal[0]
    n = len(pal); bits = max(4, (n - 1).bit_length())
    per = 64 // bits; i = ((y & 15) * 16 + (z & 15)) * 16 + (x & 15)
    v = (bs["data"][1][i // per] & (2**64 - 1)) >> ((i % per) * bits) & ((1 << bits) - 1)
    return pal[v]


c = chunk(f"{dim}/region/r.0.0.mca", 0, 0)
assert c["Status"] == "minecraft:full", c["Status"]
assert c["DataVersion"] == "5023" and c["xPos"] == "0" and c["yPos"] == "-4", (c["DataVersion"], c["yPos"])
assert block_at(c, 0, -60, 0) == "minecraft:stone", block_at(c, 0, -60, 0)
assert block_at(c, 2, -61, 2) == "minecraft:air", "broken block saved"
assert block_at(c, 0, -64, 0) == "minecraft:bedrock"
assert c["isLightOn"] == "1b" and any("SkyLight" in s for s in c["sections"]), "light not saved"
assert set(c["Heightmaps"]) == {"WORLD_SURFACE", "OCEAN_FLOOR", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES"}
assert [s["Y"] for s in c["sections"] if "block_states" in s] == [f"{y}b" for y in range(-4, 20)], "24 block sections"
assert c["sections"][0]["biomes"]["palette"] == ["minecraft:plains"]

level = root(gzip.decompress(open(f"{world}/level.dat", "rb").read()))["Data"]
assert level["CraftPP"]["level-type"] == "flat" and level["Version"]["Name"] == "26.3", level["Version"]
gen = root(gzip.decompress(open(f"{world}/data/minecraft/world_gen_settings.dat", "rb").read()))["data"]
assert gen["dimensions"]["minecraft:overworld"]["generator"]["type"] == "minecraft:flat"
rules = root(gzip.decompress(open(f"{world}/data/minecraft/game_rules.dat", "rb").read()))["data"]
assert rules["minecraft:keep_inventory"] in ("0b", "1b") and rules["minecraft:random_tick_speed"] == "3"

steve = uuid.uuid3(uuid.NAMESPACE_DNS, "Steve")
p = root(gzip.decompress(open(f"{world}/players/data/{steve}.dat", "rb").read()))
assert [float(v[:-1]) for v in p["Pos"]] == [5.5, -60.0, 7.5], p["Pos"]
assert p["Dimension"] == "minecraft:overworld" and "Inventory" in p and "Health" in p
print("storage ok: region chunk, level.dat, game rules, world_gen_settings, player data")
