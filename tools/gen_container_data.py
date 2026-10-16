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
recipe_files = sorted(os.path.relpath(os.path.join(dp, f), os.path.join(DATA, "recipe")).replace(os.sep, "/")
                      for dp, _, fs in os.walk(os.path.join(DATA, "recipe")) for f in fs if f.endswith(".json"))
potion_ids = {k: v["protocol_id"] for k, v in registries["minecraft:potion"]["entries"].items()}
brewing = []


def potion_ingredient(o):
    pc = o.get("potion_contents")
    pots = None
    if pc is not None:
        v = pc["potions"]
        pots = [potion_ids[x] for x in (v if isinstance(v, list) else [v])]
    return {"i": items(o["item"]), "p": pots}
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
    elif t == "brewing":
        out_ = r["output"]
        pc = out_.get("components", {}).get("minecraft:potion_contents")
        brewing.append({"in": potion_ingredient(r["input"]), "re": potion_ingredient(r["reagent"]),
                        "r": [item_ids[out_["id"]], out_.get("count", 1)], "p": potion_ids[pc["potion"]] if pc else None})
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

# ---------- enchantments (registry order = sorted ids, like the synced registry) ----------
ench_tags = load_tags("enchantment")
ench_ids = json_ids("enchantment")
ench_index = {n: i for i, n in enumerate(ench_ids)}


def ench_set(spec):
    names = []
    for v in spec if isinstance(spec, list) else [spec]:
        names += ench_tags.get(v[1:], []) if v.startswith("#") else [v]
    return [ench_index[n] for n in names if n in ench_index]


enchantments = []
for n in ench_ids:
    e = load("enchantment", n.replace("minecraft:", ""))
    enchantments.append({"n": n, "max": e["max_level"], "w": e["weight"], "ac": e["anvil_cost"],
                         "min": [e["min_cost"]["base"], e["min_cost"]["per_level_above_first"]],
                         "maxc": [e["max_cost"]["base"], e["max_cost"]["per_level_above_first"]],
                         "sup": items(e["supported_items"]), "pri": items(e["primary_items"]) if "primary_items" in e else None,
                         "ex": ench_set(e["exclusive_set"]) if "exclusive_set" in e else []})
ench_tag_out = {t: ench_set("#minecraft:" + t) for t in ("in_enchanting_table", "curse", "treasure", "non_treasure")}

# ---------- per-item extras ----------
DYES = ["white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray", "light_gray", "cyan", "purple",
        "blue", "brown", "green", "red", "black"]
item_extra = {}
for name, iid in item_ids.items():
    p = os.path.join(comp_dir, name.split(":")[-1] + ".json")
    if not os.path.exists(p):
        continue
    c = json.load(open(p))["components"]
    x = {}
    if "minecraft:enchantable" in c: x["ench"] = c["minecraft:enchantable"]["value"]
    if "minecraft:enchantments" in c: x["has_ench"] = 1
    if "minecraft:repairable" in c: x["rep"] = items(c["minecraft:repairable"]["items"])
    if "minecraft:provides_trim_material" in c: x["trim"] = c["minecraft:provides_trim_material"]
    if "minecraft:provides_banner_patterns" in c: x["banner"] = c["minecraft:provides_banner_patterns"]
    if "minecraft:brewing_fuel" in c: x["bfuel"] = 1
    if "minecraft:unbreakable" in c: x["unbreakable"] = 1
    if "minecraft:dye" in c: x["dye"] = DYES.index(c["minecraft:dye"])
    if x: item_extra[iid] = x

banner_tags = load_tags("banner_pattern")
banner_ids = json_ids("banner_pattern")
banner = {"ids": banner_ids, "no_item": [banner_ids.index(n) for n in banner_tags.get("minecraft:no_item_required", [])],
          "tags": {t: [banner_ids.index(n) for n in v] for t, v in banner_tags.items() if t.startswith("minecraft:pattern_item/")}}

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
    "enchantable/melee_weapon", "enchantable/lunge", "loom_dyes", "loom_patterns", "banners", "wool", "planks", "saplings", "dyes")
    if "minecraft:" + t in item_tags}
reg_ids = lambda r: {k.split(":")[-1]: v["protocol_id"] for k, v in registries["minecraft:" + r]["entries"].items()}

out = {"cooking": cooking, "fuel": fuel, "stonecutting": stonecutting, "smithing_transform": smithing_transform,
       "smithing_trim": smithing_trim, "sounds": {s: sounds[s] for s in want_sounds}, "item_tags": tags,
       "block_entity_types": reg_ids("block_entity_type"), "menus": reg_ids("menu"),
       "recipe_display": reg_ids("slot_display"), "trim_pattern": json_ids("trim_pattern"),
       "trim_material": json_ids("trim_material"), "brewing": brewing, "enchantments": enchantments,
       "enchantment_tags": ench_tag_out, "items": item_extra, "banner": banner, "potions": potion_ids}
os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)
with open(os.path.join(ROOT, "data", "containers.json"), "w") as fp:
    json.dump(out, fp, separators=(",", ":"))
print("containers.json: %s cooking, %d fuels, %d stonecutting, %d smithing" % (
    {k: len(v) for k, v in cooking.items()}, len(fuel), len(stonecutting), len(smithing_transform) + len(smithing_trim)))
