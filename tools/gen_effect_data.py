#!/usr/bin/env python3
"""Tool-time extractor for src/effects: data/effects.json.

  potions      minecraft:potion registry order: [name, [[effect, duration, amplifier], ...]] (Potions.java)
  consume      item id -> on_consume_effects of its default consumable component (items report)
  enchantments minecraft:enchantment registry order (sorted ids, as synced): the datapack JSON (effects, slots, ...)
Sources: vanilla/src (Potions.java, PotionIds.java, data/minecraft/enchantment), vanilla/generated/reports.
"""
import json, os, re

VAN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "vanilla")
DATA = os.path.join(VAN, "src", "data", "minecraft")
REPORTS = os.path.join(VAN, "generated", "reports")
SRC = os.path.join(VAN, "src", "net", "minecraft")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "effects.json")

registries = json.load(open(os.path.join(REPORTS, "registries.json")))
def ids(reg):
    return {k: v["protocol_id"] for k, v in registries[reg]["entries"].items()}
item_ids = ids("minecraft:item")
potion_ids = ids("minecraft:potion")

# ---------- potions ----------
src = open(os.path.join(SRC, "world", "item", "alchemy", "Potions.java")).read()
ids_src = open(os.path.join(SRC, "world", "item", "alchemy", "PotionIds.java")).read()
const_to_key = {m.group(1): "minecraft:" + m.group(2) for m in re.finditer(r'(\w+) = register\("(\w+)"\)', ids_src)}
potions = [None] * len(potion_ids)
for m in re.finditer(r'register\(\s*PotionIds\.(\w+),\s*new Potion\("(\w+)"(.*?)\)\s*\);', src, re.S):
    effects = []
    for e in re.finditer(r'new MobEffectInstance\(MobEffects\.(\w+),\s*(\d+)(?:,\s*(\d+))?\)', m.group(3)):
        effects.append([e.group(1).lower(), int(e.group(2)), int(e.group(3) or 0)])
    potions[potion_ids[const_to_key[m.group(1)]]] = [m.group(2), effects]
assert all(potions), "unparsed potion"

# ---------- consumables ----------
comp_dir = os.path.join(REPORTS, "minecraft", "components", "item")
consume = {}
for name, iid in item_ids.items():
    p = os.path.join(comp_dir, name.split(":")[1] + ".json")
    if not os.path.exists(p):
        continue
    c = json.load(open(p))["components"].get("minecraft:consumable")
    if c and c.get("on_consume_effects"):
        consume[str(iid)] = c["on_consume_effects"]

# ---------- enchantments ----------
ench = []
for f in sorted(os.listdir(os.path.join(DATA, "enchantment"))):  # datapack registry: sorted ids
    name = "minecraft:" + f[:-5]
    j = json.load(open(os.path.join(DATA, "enchantment", f)))
    ench.append({"name": name, "max_level": j["max_level"], "slots": j["slots"], "effects": j.get("effects", {}),
                 "supported": j.get("supported_items"), "exclusive": j.get("exclusive_set")})

json.dump({"potions": potions, "consume": consume, "enchantments": ench}, open(OUT, "w"), separators=(",", ":"))
print(f"effects.json: {len(potions)} potions, {len(consume)} consumables, {len(ench)} enchantments")
