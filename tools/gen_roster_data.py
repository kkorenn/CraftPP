#!/usr/bin/env python3
"""Villager data for CraftPP -> data/roster.json (loaded once by src/mobs/trading.hpp).

Complements the mob framework's data/mobs.json with villager trades, trade sets, POI types and professions.
Sources: villager_trade + trade_set + tags/villager_trade, PoiTypes.java and VillagerProfession.java.
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VAN = os.path.join(ROOT, "vanilla")
DATA = os.path.join(VAN, "src", "data", "minecraft")
SRC = os.path.join(VAN, "src", "net", "minecraft")
registries = json.load(open(os.path.join(VAN, "generated", "reports", "registries.json")))
reg = lambda r: {k[10:]: v["protocol_id"] for k, v in registries["minecraft:" + r]["entries"].items()}
item_ids, block_names = reg("item"), set(reg("block"))
fails = []


def java(rel):
    return open(os.path.join(SRC, rel)).read()


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
        if t not in out:
            out[t] = []
            r = []
            for v in raw.get(t, []):
                v = v["id"] if isinstance(v, dict) else v
                r += res(v[11:]) if v.startswith("#") else [v[10:]]
            out[t] = list(dict.fromkeys(r))
        return out[t]

    for t in raw:
        res(t)
    return out


trade_tags, block_tags = load_tags("villager_trade"), load_tags("block")
items = lambda names: sorted(item_ids[n] for n in names if n in item_ids)

# ---------- villager trades ----------
trades, trade_index = [], {}
tdir = os.path.join(DATA, "villager_trade")
for dp, _, fs in sorted(os.walk(tdir)):
    for f in sorted(fs):
        rel = os.path.relpath(os.path.join(dp, f), tdir)[:-5].replace(os.sep, "/")
        d = json.load(open(os.path.join(dp, f)))
        cost = lambda c: [item_ids[c["id"][10:]], c.get("count", 1)]
        # Defaults from VillagerTrade.CODEC / TradeCost.CODEC.
        tr = {"wants": cost(d["wants"]), "gives": cost(d["gives"]), "max_uses": d.get("max_uses", 4), "xp": d.get("xp", 1),
              "disc": d.get("reputation_discount", 0.0)}
        if "additional_wants" in d:
            tr["b"] = cost(d["additional_wants"])
        if "merchant_predicate" in d:
            v = d["merchant_predicate"]["predicate"]["minecraft:predicates"]["minecraft:villager/variant"]
            tr["variants"] = [x[10:] for x in (v if isinstance(v, list) else [v])]
        # Item components we don't model yet (enchantments, dyes, potions, maps, payment components).
        if "given_item_modifier" in d or "components" in d["wants"] or "components" in d["gives"]:
            mods = d.get("given_item_modifier", [])
            mods = mods if isinstance(mods, list) else [mods]
            tr["unsupported"] = sorted({m["type"][10:] for m in mods} or {"components"})
        trade_index[rel] = len(trades)
        trades.append(tr)

trade_sets = {}
sdir = os.path.join(DATA, "trade_set")
for dp, _, fs in os.walk(sdir):
    for f in fs:
        rel = os.path.relpath(os.path.join(dp, f), sdir)[:-5].replace(os.sep, "/")
        d = json.load(open(os.path.join(dp, f)))
        ref = d["trades"]
        names = trade_tags[ref[11:]] if ref.startswith("#") else [ref[10:]]
        if not isinstance(d["amount"], int):
            fails.append(f"trade set {rel}: non-constant amount")
        trade_sets[rel] = {"trades": [trade_index[n] for n in names], "amount": d["amount"],
                           "dup": d.get("allow_duplicates", False)}

# ---------- POI + professions ----------
poi_src = java("world/entity/ai/village/poi/PoiTypes.java")
poi = {}
for m in re.finditer(r"register\(registry, (\w+), (.*?), (\d+), (\d+)\);", poi_src):
    spec = m.group(2)
    b = re.match(r"getBlockStates\(Blocks\.(\w+)\)", spec)
    if b:
        blocks = [b.group(1).lower()]
    elif spec == "BEDS":
        blocks = block_tags["beds"]
    else:  # a static set built from Blocks.X (CAULDRONS, LIGHTNING_RODS)
        d = re.search(r"Set<BlockState> " + spec + r" = (.*?);", poi_src, re.S)
        blocks = [x.lower() for x in re.findall(r"Blocks\.(\w+)", d.group(1))] if d else []
    if not blocks or any(x not in block_names for x in blocks):
        fails.append(f"poi {m.group(1)}: blocks {blocks}")
    poi[m.group(1).lower()] = {"blocks": blocks, "tickets": int(m.group(3)), "range": int(m.group(4))}

prof_src = java("world/entity/npc/villager/VillagerProfession.java")
professions = {"none": {"poi": None, "trades": {}}, "nitwit": {"poi": None, "trades": {}}}
for m in re.finditer(r"register\(\s*registry,\s*(\w+),\s*PoiTypes\.(\w+),(.*?)\n      \);", prof_src, re.S):
    name, body = m.group(1).lower(), m.group(3)
    p = {"poi": m.group(2).lower(), "trades": {}}
    sets = re.findall(r"ImmutableSet\.of\((.*?)\)", body, re.S)
    if sets:
        p["requested"] = items([x.lower() for x in re.findall(r"Items\.(\w+)", sets[0])])
        p["secondary_poi"] = [x.lower() for x in re.findall(r"Blocks\.(\w+)", sets[1])] if len(sets) > 1 else []
    for lvl, ts in re.findall(r"entry\((\d), TradeSets\.(\w+)\)", body):
        key = re.sub(r"_level_(\d)$", r"/level_\1", ts.lower())
        if key not in trade_sets:
            fails.append(f"profession {name}: trade set {key}")
        p["trades"][lvl] = key
    professions[name] = p
for n, i in reg("villager_profession").items():
    if n not in professions:
        fails.append(f"profession {n} not parsed")
    else:
        professions[n]["id"] = i

vt = reg("villager_type")
out = {"trades": trades, "trade_sets": trade_sets, "poi": poi, "professions": professions,
       "villager_types": sorted(vt, key=vt.get)}
os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)
json.dump(out, open(os.path.join(ROOT, "data", "roster.json"), "w"), separators=(",", ":"), sort_keys=True)
for f in fails:
    print("gen_roster_data: " + f, file=sys.stderr)
print(f"roster.json: {len(trades)} trades "
      f"({sum('unsupported' in t for t in trades)} need item components), {len(trade_sets)} trade sets, "
      f"{len(poi)} poi, {len(professions)} professions")
sys.exit(1 if fails else 0)
