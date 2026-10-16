#!/usr/bin/env python3
"""Survival game data for CraftPP -> data/game.json (parsed once at startup by src/game/data.hpp).

Sources: vanilla/generated/reports (blocks, registries, item components), vanilla/src/data (loot tables,
recipes, tags, predicates) and the decompiled Blocks.java / Items.java for block properties that no report
contains (destroy time, requiresCorrectToolForDrops, collision, replaceable) and crafting remainders.
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VAN = os.path.join(ROOT, "vanilla")
DATA = os.path.join(VAN, "src", "data", "minecraft")
REPORTS = os.path.join(VAN, "generated", "reports")
SRC = os.path.join(VAN, "src", "net", "minecraft")

registries = json.load(open(os.path.join(REPORTS, "registries.json")))
blocks_rep = json.load(open(os.path.join(REPORTS, "blocks.json")))
item_ids = {k: v["protocol_id"] for k, v in registries["minecraft:item"]["entries"].items()}
item_names = sorted(item_ids, key=item_ids.get)

# Blocks in state order; "block index" everywhere in game.json = position in this list.
blocks = sorted(((min(s["id"] for s in b["states"]), name, b) for name, b in blocks_rep.items()))
block_index = {name: i for i, (_, name, _) in enumerate(blocks)}


def short(n):
    return n.split(":")[-1]


# ---------- tags ----------
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


block_tags, item_tags = load_tags("block"), load_tags("item")


def block_set(spec):
    """'#tag' | 'name' | [names] -> sorted block indices."""
    names = []
    for s in spec if isinstance(spec, list) else [spec]:
        names += block_tags.get(s[1:], []) if s.startswith("#") else [s]
    return sorted({block_index[n] for n in names if n in block_index})


def item_set(spec):
    names = []
    for s in spec if isinstance(spec, list) else [spec]:
        names += item_tags.get(s[1:], []) if s.startswith("#") else [s]
    return sorted({item_ids[n] for n in names if n in item_ids})


# ---------- block properties from Blocks.java ----------
java = open(os.path.join(SRC, "world", "level", "block", "Blocks.java")).read()
DEFAULT = {"h": 0.0, "er": 0.0, "t": False, "col": True, "r": False, "air": False}
helpers = {}
for m in re.finditer(r"private static BlockBehaviour\.Properties (\w+)\(.*?\{(.*?)\n   \}", java, re.S):
    helpers[m.group(1)] = m.group(2)
consts = {}  # java constant -> props
F = r"(-?[0-9.]+)F"


def copy_of(arg):
    ident = re.search(r"[A-Z][A-Z0-9_]+", arg)
    return dict(consts.get(ident.group(0), DEFAULT)) if ident else dict(DEFAULT)


def eval_props(text):
    """Find the Properties base in `text`, then apply the setters that follow it."""
    pats = [(r"Properties\.of\(\)", "of"), (r"Properties\.of(?:Full|Legacy)Copy\(([^)]*)\)", "copy"),
            (r"register(?:Legacy)?Stair\(\s*[\w.]+,\s*([^)]*)\)", "copy"),
            (r"registerWall\(\s*[\w.]+,\s*([^)]*)\)", "copy"),
            (r"registerSlab\(\s*[\w.]+,\s*([A-Z_][\w.()]*)(?:,\s*" + F + r")?(?:,\s*" + F + r")?", "slab"),
            (r"\b(\w+Properties)\(", "helper"), (r"\bwallVariant\(", "of")]
    best = None
    for p, kind in pats:
        m = re.search(p, text)
        if m and (kind != "helper" or m.group(1) in helpers) and (best is None or m.start() < best[0].start()):
            best = (m, kind)
    if not best:
        return None
    m, kind = best
    if kind == "of":
        pr = dict(DEFAULT)
    elif kind == "copy":
        pr = copy_of(m.group(1))
    elif kind == "slab":
        pr = copy_of(m.group(1))
        if m.group(2):
            pr["h"] = float(m.group(2))
        if m.group(3):
            pr["er"] = float(m.group(3))
    else:
        pr = eval_props(helpers[m.group(1)]) or dict(DEFAULT)
    rest = text[m.end():]
    for s in re.finditer(r"\.(strength|destroyTime|explosionResistance)\(" + F + r"(?:,\s*" + F + r")?\)|\.(instabreak|requiresCorrectToolForDrops|noCollision|replaceable|air)\(\)", rest):
        if s.group(1) == "explosionResistance":
            pr["er"] = max(0.0, float(s.group(2)))
        elif s.group(1):
            pr["h"] = float(s.group(2))
            if s.group(1) == "strength":  # strength(t) = strength(t, t)
                pr["er"] = max(0.0, float(s.group(3) or s.group(2)))
        elif s.group(4) == "instabreak":
            pr["h"] = pr["er"] = 0.0
        elif s.group(4) == "requiresCorrectToolForDrops":
            pr["t"] = True
        elif s.group(4) == "noCollision":
            pr["col"] = False
        elif s.group(4) == "replaceable":
            pr["r"] = True
        elif s.group(4) == "air":
            pr["air"] = True
    return pr


by_name, collections = {}, {}
for m in re.finditer(r"\n   public static final ([\w<>]+) (\w+) = (.*?;)(?=\n   (?:public|private|static|@|\}))", java, re.S):
    typ, const, body = m.groups()
    pr = eval_props(body)
    if pr is None:
        continue
    consts[const] = pr
    idm = re.search(r"Block(?:Item)?Ids\.(\w+)", body)
    for n in {const.lower(), idm.group(1).lower() if idm else const.lower()}:
        (by_name if typ == "Block" else collections).setdefault("minecraft:" + n, pr)

PREFIXES = ["white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray", "light_gray", "cyan",
            "purple", "blue", "brown", "green", "red", "black", "waxed", "exposed", "weathered", "oxidized"]


def block_props(name):
    if name in by_name:
        return by_name[name]
    rest = short(name)
    for _ in range(3):
        for p in PREFIXES:
            if rest.startswith(p + "_"):
                rest = rest[len(p) + 1:]
                break
        for cand in (rest, rest + "_block", "dyed_" + rest):
            if "minecraft:" + cand in collections:
                return collections["minecraft:" + cand]
    return None


missing = []
out_blocks = []
for first, name, b in blocks:
    pr = block_props(name)
    if pr is None:
        missing.append(name)
        pr = DEFAULT
    out_blocks.append({"n": short(name), "f": first, "c": len(b["states"]), "h": pr["h"], "er": pr["er"], "t": pr["t"],
                       "col": pr["col"] and not pr["air"], "r": pr["r"],
                       "p": [[k, v] for k, v in b.get("properties", {}).items()]})

# ---------- items ----------
items_java = open(os.path.join(SRC, "world", "item", "Items.java")).read()
craft_rem = {}
for m in re.finditer(r"ItemIds\.(\w+),[^;]*?craftRemainder\((\w+)\)", items_java, re.S):
    craft_rem["minecraft:" + m.group(1).lower()] = "minecraft:" + m.group(2).lower()

comp_dir = os.path.join(REPORTS, "minecraft", "components", "item")
out_items = []
for name in item_names:
    p = os.path.join(comp_dir, short(name) + ".json")
    c = json.load(open(p))["components"] if os.path.exists(p) else {}
    it = {"n": short(name), "s": c.get("minecraft:max_stack_size", 64)}
    if "minecraft:max_damage" in c:
        it["d"] = c["minecraft:max_damage"]
    tool = c.get("minecraft:tool")
    if tool:
        it["tool"] = {"ds": tool.get("default_mining_speed", 1.0), "dpb": tool.get("damage_per_block", 1),
                      "r": [[block_set(r["blocks"]), r.get("speed"), r.get("correct_for_drops")] for r in tool["rules"]]}
        if not tool.get("can_destroy_blocks_in_creative", True):
            it["tool"]["nc"] = 1
    food = c.get("minecraft:food")
    if food:
        it["food"] = [food["nutrition"], food["saturation"], bool(food.get("can_always_eat", False))]
    cons = c.get("minecraft:consumable")
    if cons is not None:
        it["cs"] = cons.get("consume_seconds", 1.6)
    ur = c.get("minecraft:use_remainder")
    if ur:
        it["ur"] = item_ids[ur["id"]]
    if name in craft_rem:
        it["cr"] = item_ids[craft_rem[name]]
    for m in c.get("minecraft:attribute_modifiers", []):  # mainhand attack damage / speed (add_value)
        if m.get("slot") == "mainhand" and m.get("operation") == "add_value":
            if m["type"] == "minecraft:attack_damage":
                it["ad"] = m["amount"]
            elif m["type"] == "minecraft:attack_speed":
                it["as"] = m["amount"]
    if "minecraft:equippable" in c:
        eq = c["minecraft:equippable"]
        it["eq"] = eq["slot"]
        if not eq.get("damage_on_hurt", True):
            it["nd"] = 1
        # armor / armor_toughness add_value modifiers that apply in the item's own slot
        ar = [sum(m["amount"] for m in c.get("minecraft:attribute_modifiers", [])
                  if m["type"] == "minecraft:" + t and m.get("operation") == "add_value"
                  and m.get("slot", "any") in (eq["slot"], "armor", "any")) for t in ("armor", "armor_toughness")]
        if any(ar):
            it["ar"] = ar
    out_items.append(it)

# ---------- loot tables (blocks), predicate references inlined ----------
predicates = {}
for dp, _, fs in os.walk(os.path.join(DATA, "predicate")):
    for f in fs:
        rel = os.path.relpath(os.path.join(dp, f), os.path.join(DATA, "predicate"))[:-5].replace(os.sep, "/")
        predicates["minecraft:" + rel] = json.load(open(os.path.join(dp, f)))


def inline(o):
    if isinstance(o, list):
        return [inline(v) for v in o]
    if not isinstance(o, dict):
        return o
    r = {}
    for k, v in o.items():
        if k in ("condition", "term") and isinstance(v, str):
            v = predicates[v]
        if k == "terms":
            v = [predicates[t] if isinstance(t, str) else t for t in v]
        r[k] = inline(v)
    if r.get("type") == "minecraft:match_tool":  # pre-resolve the item predicate
        pr = r.get("predicate", {})
        r = {"type": "minecraft:match_tool", "items": item_set(pr["items"]) if "items" in pr else None,
             "unsupported": any(k != "items" for k in pr)}
    if r.get("type") == "minecraft:location_check":  # only the block part of the location predicate is used
        bp = r.get("predicate", {}).get("block")
        r = {"type": "minecraft:location_check", "off": [r.get("offsetX", 0), r.get("offsetY", 0), r.get("offsetZ", 0)],
             "blocks": block_set(bp["blocks"]) if bp and "blocks" in bp else None,
             "state": bp.get("state", {}) if bp else {}, "unsupported": not bp or any(k not in ("blocks", "state") for k in bp)}
    if r.get("type") == "minecraft:match_block" and "blocks" in r:
        r["blocks"] = block_set(r["blocks"])
    return r


loot = {}
ldir = os.path.join(DATA, "loot_table", "blocks")
for f in sorted(os.listdir(ldir)):
    loot[f[:-5]] = inline(json.load(open(os.path.join(ldir, f))))

# ---------- crafting recipes ----------
def ingredient(v):
    return item_set(v)


recipes = []
for f in sorted(f for f in os.listdir(os.path.join(DATA, "recipe")) if f.endswith(".json")):
    r = json.load(open(os.path.join(DATA, "recipe", f)))
    res = r.get("result")
    if r["type"] == "minecraft:crafting_shaped":
        pat = r["pattern"]
        w, h = max(len(row) for row in pat), len(pat)
        cells = [[] if row.ljust(w)[x] == " " else ingredient(r["key"][row.ljust(w)[x]]) for row in pat for x in range(w)]
        recipes.append({"id": f[:-5], "w": w, "h": h, "k": cells, "r": [item_ids[res["id"]], res.get("count", 1)]})
    elif r["type"] == "minecraft:crafting_shapeless":
        recipes.append({"id": f[:-5], "i": [ingredient(i) for i in r["ingredients"]],
                        "r": [item_ids[res["id"]], res.get("count", 1)]})

# ---------- registry ids the server writes ----------
def json_ids(folder):
    base = os.path.join(DATA, folder)
    return sorted("minecraft:" + os.path.relpath(os.path.join(dp, f), base)[:-5].replace(os.sep, "/")
                  for dp, _, fs in os.walk(base) for f in fs if f.endswith(".json"))


def damage_tag(t):
    vals = set()
    for v in json.load(open(os.path.join(DATA, "tags", "damage_type", t + ".json")))["values"]:
        vals |= damage_tag(short(v[1:])) if v.startswith("#") else {v}
    return vals


bypasses_armor = damage_tag("bypasses_armor")
reg = {
    "damage_type": {short(n): [i, d["message_id"], d.get("exhaustion", 0.0), d.get("death_message_type", "default"),
                               n in bypasses_armor]
                    for i, n in enumerate(json_ids("damage_type"))
                    for d in [json.load(open(os.path.join(DATA, "damage_type", short(n) + ".json")))]},
    "menu": {short(k): v["protocol_id"] for k, v in registries["minecraft:menu"]["entries"].items()},
    "data_component_type": {short(k): v["protocol_id"] for k, v in registries["minecraft:data_component_type"]["entries"].items()},
}

block_tag_out = {t: block_set("#minecraft:" + t) for t in ("climbable", "fall_damage_resetting", "dragon_immune",
                                                               "dragon_transparent", "wither_immune", "wither_summon_base_blocks")}
out = {"block_tags": block_tag_out, "blocks": out_blocks, "items": out_items, "loot": loot, "recipes": recipes, "registries": reg}
os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)
with open(os.path.join(ROOT, "data", "game.json"), "w") as fp:
    json.dump(out, fp, separators=(",", ":"))
print(f"game.json: {len(out_blocks)} blocks ({len(missing)} without parsed properties), {len(out_items)} items, "
      f"{len(loot)} loot tables, {len(recipes)} recipes")
if missing:
    print("  defaults used for:", " ".join(short(n) for n in missing), "..." if len(missing) > 40 else "")
