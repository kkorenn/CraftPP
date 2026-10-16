#!/usr/bin/env python3
"""Container / block entity data for CraftPP -> data/containers.json (src/containers/data.hpp).

Sources: vanilla/src/data (recipes, tags, context providers, enchantments), vanilla/generated/reports
(registries, item components). Item ids are protocol ids; ingredient lists are sorted item id lists.
"""
import json, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VAN = os.path.join(ROOT, "vanilla")
DATA = os.path.join(VAN, "src", "data", "minecraft")
REPORTS = os.path.join(VAN, "generated", "reports")
registries = json.load(open(os.path.join(REPORTS, "registries.json")))
item_ids = {k: v["protocol_id"] for k, v in registries["minecraft:item"]["entries"].items()}


def load_tags(kind):
    root = os.path.join(DATA, "tags", kind)
    raw = {}
    for dp, _, fs in os.walk(root):
        for f in fs:
            if f.endswith(".json"):
                rel = os.path.relpath(os.path.join(dp, f), root)[:-5].replace(os.sep, "/")
                raw["minecraft:" + rel] = json.load(open(os.path.join(dp, f)))["values"]
    out = {}

    def res(t):
        if t in out:
            return out[t]
        out[t] = []
        r = []
        for v in raw.get(t, []):
            v = v["id"] if isinstance(v, dict) else v
            r += res(v[1:]) if v.startswith("#") else [v]
        out[t] = list(dict.fromkeys(r))
        return out[t]

    for t in raw:
        res(t)
    return out


item_tags = load_tags("item")


def items(spec):
    names = []
    for s in spec if isinstance(spec, list) else [spec]:
        if isinstance(s, dict):
            s = s.get("item") or "#" + s["tag"]
        names += item_tags.get(s[1:], []) if s.startswith("#") else [s]
    return sorted({item_ids[n] for n in names if n in item_ids})


def json_ids(folder):
    base = os.path.join(DATA, folder)
    return sorted("minecraft:" + os.path.relpath(os.path.join(dp, f), base)[:-5].replace(os.sep, "/")
                  for dp, _, fs in os.walk(base) for f in fs if f.endswith(".json"))


def load(folder, name):
    return json.load(open(os.path.join(DATA, folder, name.split(":")[-1] + ".json")))


# ---------- recipes ----------
cooking = {"smelting": [], "blasting": [], "smoking": [], "campfire_cooking": []}
stonecutting, smithing_transform, smithing_trim = [], [], []
recipe_files = sorted(f for f in os.listdir(os.path.join(DATA, "recipe")) if f.endswith(".json"))
for f in recipe_files:
    r = json.load(open(os.path.join(DATA, "recipe", f)))
    t = r["type"].split(":")[-1]
    rid = "minecraft:" + f[:-5]
    if t in cooking:
        res = r["result"]
        cooking[t].append({"id": rid, "i": items(r["ingredient"]), "r": [item_ids[res["id"]], res.get("count", 1)],
                           "xp": r.get("experience", 0.0), "t": r.get("cookingtime", 200)})
    elif t == "stonecutting":
        res = r["result"]
        stonecutting.append({"i": items(r["ingredient"]), "r": [item_ids[res["id"]], res.get("count", 1)]})
    elif t == "smithing_transform":
        res = r["result"]
        smithing_transform.append({"t": items(r["template"]), "b": items(r["base"]), "a": items(r["addition"]),
                                   "r": [item_ids[res["id"]], res.get("count", 1)]})
    elif t == "smithing_trim":
        smithing_trim.append({"t": items(r["template"]), "b": items(r["base"]), "a": items(r["addition"]),
                              "p": r["pattern"]})

# ---------- fuel (cooking_fuel component -> context providers, evaluated per furnace kind) ----------
def provider(kind, ref, fast):
    v = ref
    if isinstance(v, str):
        v = load("context_" + kind + "_provider", v.replace("minecraft:", ""))
    if isinstance(v, (int, float)):
        return v
    t = v["type"].split(":")[-1]
    if t == "div":
        a, b = provider(kind, v["left"], fast), provider(kind, v["right"], fast)
        return int(a / b) if kind == "int" else a / b
    if t == "conditional":
        assert v["condition"] == "minecraft:block/fast_cooking", v
        return provider(kind, v["on_true"] if fast else v["on_false"], fast)
    raise SystemExit("unknown provider " + json.dumps(v))


fuel = {}
comp_dir = os.path.join(REPORTS, "minecraft", "components", "item")
for name, iid in item_ids.items():
    p = os.path.join(comp_dir, name.split(":")[-1] + ".json")
    if not os.path.exists(p):
        continue
    cf = json.load(open(p))["components"].get("minecraft:cooking_fuel")
    if cf:
        fuel[iid] = [provider("int", cf["burn_time"], False), provider("int", cf["burn_time"], True),
                     provider("float", cf["speed_multiplier"], False), provider("float", cf["speed_multiplier"], True)]

sounds = {k.split(":")[-1]: v["protocol_id"] for k, v in registries["minecraft:sound_event"]["entries"].items()}
want_sounds = [s for s in sounds if s.startswith(("block.chest.", "block.barrel.", "block.ender_chest.", "block.shulker_box.",
                                                 "block.furnace.", "block.blastfurnace.", "block.smoker.", "ui.stonecutter.",
                                                 "block.anvil.", "block.grindstone.", "ui.loom.", "ui.cartography_table.",
                                                 "block.smithing_table.", "block.brewing_stand.", "block.enchantment_table.",
                                                 "block.beacon.", "block.sign.", "item.dye.", "item.glow_ink_sac.",
                                                 "item.ink_sac.", "item.honeycomb.", "block.lectern.", "item.book.",
                                                 "block.copper_chest.", "entity.experience_orb."))]

tags = {t: items("#minecraft:" + t) for t in (
    "shulker_boxes", "lectern_books", "beacon_payment_items", "bookshelf_books", "furnace_fuel_bottom_takeable",
    "trim_materials", "trimmable_armor", "banner_patterns" if "minecraft:banner_patterns" in item_tags else "logs",
    "enchantable/foot_armor", "enchantable/leg_armor", "enchantable/chest_armor", "enchantable/head_armor",
    "enchantable/armor", "enchantable/sword", "enchantable/sharp_weapon", "enchantable/mace", "enchantable/fire_aspect",
    "enchantable/weapon", "enchantable/mining", "enchantable/mining_loot", "enchantable/fishing", "enchantable/trident",
    "enchantable/durability", "enchantable/bow", "enchantable/equippable", "enchantable/crossbow", "enchantable/vanishing",
    "enchantable/melee_weapon", "enchantable/lunge", "wool", "planks", "saplings", "dyes")
    if "minecraft:" + t in item_tags}
reg_ids = lambda r: {k.split(":")[-1]: v["protocol_id"] for k, v in registries["minecraft:" + r]["entries"].items()}

out = {"cooking": cooking, "fuel": fuel, "stonecutting": stonecutting, "smithing_transform": smithing_transform,
       "smithing_trim": smithing_trim, "sounds": {s: sounds[s] for s in want_sounds}, "item_tags": tags,
       "block_entity_types": reg_ids("block_entity_type"), "menus": reg_ids("menu"),
       "recipe_display": reg_ids("slot_display"), "trim_pattern": json_ids("trim_pattern"),
       "trim_material": json_ids("trim_material")}
os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)
with open(os.path.join(ROOT, "data", "containers.json"), "w") as fp:
    json.dump(out, fp, separators=(",", ":"))
print("containers.json: %s cooking, %d fuels, %d stonecutting, %d smithing" % (
    {k: len(v) for k, v in cooking.items()}, len(fuel), len(stonecutting), len(smithing_transform) + len(smithing_trim)))
