#!/usr/bin/env python3
"""Per-mob static data for CraftPP -> data/mobs.json (loaded once by src/game/mob_data.hpp).

Sources: EntityTypes.java (category, size, eye height, tracking), DefaultAttributes.java + each mob's
createAttributes() chain, Attributes.java (defaults/ranges), registries.json (protocol ids), item tags
(<mob>_food / _tempt_items), loot_table/entities, villager_trade + trade_set + tags/villager_trade,
PoiTypes.java and VillagerProfession.java (job sites, requested items, secondary POI).
"""
import json, os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VAN = os.path.join(ROOT, "vanilla")
DATA = os.path.join(VAN, "src", "data", "minecraft")
SRC = os.path.join(VAN, "src", "net", "minecraft")
registries = json.load(open(os.path.join(VAN, "generated", "reports", "registries.json")))
reg = lambda r: {k[10:]: v["protocol_id"] for k, v in registries["minecraft:" + r]["entries"].items()}
entity_ids, item_ids, attr_ids = reg("entity_type"), reg("item"), reg("attribute")
fails = []


def f32(s):  # Java float literal -> the double it widens to (attributes store doubles)
    return struct.unpack("f", struct.pack("f", float(s)))[0]


def num(s):
    s = s.strip()
    return f32(s[:-1]) if s[-1] in "Ff" else float(s.rstrip("Dd"))


# ---------- java index ----------
java_files = {}
for dp, _, fs in os.walk(SRC):
    for f in fs:
        if f.endswith(".java"):
            java_files.setdefault(f[:-5], []).append(os.path.join(dp, f))


def java(cls):
    paths = java_files.get(cls, [])
    paths.sort(key=lambda p: "world/entity" not in p)
    return open(paths[0]).read() if paths else ""


def upper_to_name(c):
    return c.lower()


# ---------- attributes ----------
attr_src = java("Attributes")
attributes = {}
for m in re.finditer(r'register\(\s*"(\w+)",\s*new RangedAttribute\("[^"]+",\s*([-\d.EeFfDd]+),\s*([-\d.EeFfDd]+),\s*([-\d.EeFfDd]+)\)', attr_src):
    attributes[m.group(1)] = [attr_ids[m.group(1)], num(m.group(2)), num(m.group(3)), num(m.group(4))]
field_attr = {f.lower(): n for f, n in re.findall(r'Holder<Attribute> (\w+) = register\(\s*"(\w+)"', attr_src)}
missing = set(attr_ids) - set(attributes)
if missing:
    fails.append(f"attributes without defaults: {sorted(missing)}")


def method_body(cls, name):
    """(owning class, body) of a static attribute method, searching superclasses (static methods inherit)."""
    while cls:
        src = java(cls)
        m = re.search(r"static AttributeSupplier\.Builder " + name + r"\(\)\s*\{(.*?)\n   \}", src, re.S)
        if m:
            return cls, m.group(1)
        e = re.search(r"class " + cls + r"\b[^{]*?extends (\w+)", src)
        cls = e.group(1) if e else None
    return None, None


def supplier(cls, name, seen=()):
    """Resolve Class.method() attribute chain -> {attr: value}."""
    cls, body = method_body(cls, name)
    if body is None:
        fails.append(f"no attribute method {cls}.{name}")
        return {}
    m = re.search(r"return\s+(.*?);", body, re.S)
    expr = re.sub(r"\s+", "", m.group(1))
    base = re.match(r"(?:(\w+)\.)?(\w+)\(\)", expr)
    out = {}
    if base.group(2) != "builder":
        bcls = base.group(1) or cls
        if bcls == "super":
            fails.append(f"super in {cls}.{name}")
        out = dict(supplier(bcls, base.group(2)))
    for a in re.finditer(r"\.add\(Attributes\.(\w+)(?:,([^)]+))?\)", expr):
        an = field_attr[a.group(1).lower()]
        out[an] = num(a.group(2)) if a.group(2) else attributes[an][1]
    return out


da = java("DefaultAttributes")
type_attrs = {}
for m in re.finditer(r"\.put\(EntityTypes\.(\w+),\s*(\w+)\.(\w+)\(\)\.build\(\)\)", da):
    type_attrs[upper_to_name(m.group(1))] = supplier(m.group(2), m.group(3))

# ---------- entity types ----------
et = java("EntityTypes")
cats = {"MONSTER": "monster", "CREATURE": "creature", "AMBIENT": "ambient", "AXOLOTLS": "axolotls",
        "UNDERGROUND_WATER_CREATURE": "underground_water_creature", "WATER_CREATURE": "water_creature",
        "WATER_AMBIENT": "water_ambient", "MISC": "misc"}
types = {}
for m in re.finditer(r"public static final EntityType<[^>]*> (\w+) = register\((.*?)\n   \);", et, re.S):
    body = re.sub(r"\s+", " ", m.group(2))
    name = upper_to_name(m.group(1))
    if name not in entity_ids:
        fails.append(f"unknown entity {name}")
        continue
    t = {"id": entity_ids[name]}
    c = re.search(r"MobCategory\.(\w+)", body)
    t["cat"] = cats[c.group(1)] if c else "misc"
    s = re.search(r"\.sized\(([^,]+),([^)]+)\)", body)
    w, h = (f32(s.group(1).strip().rstrip("Ff")), f32(s.group(2).strip().rstrip("Ff"))) if s else (f32(0.6), f32(1.8))
    t["w"], t["h"] = w, h
    e = re.search(r"\.eyeHeight\(([^)]+)\)", body)
    t["eye"] = f32(e.group(1).strip().rstrip("Ff")) if e else f32(struct.unpack("f", struct.pack("f", h))[0] * f32(0.85))
    r = re.search(r"\.clientTrackingRange\((\d+)\)", body)
    t["track"] = int(r.group(1)) if r else 5
    u = re.search(r"\.updateInterval\((\d+)\)", body)
    t["interval"] = int(u.group(1)) if u else 3
    if ".fireImmune()" in body:
        t["fire_immune"] = True
    if ".notInPeaceful()" in body:
        t["no_peaceful"] = True
    if ".canSpawnFarFromPlayer()" in body:
        t["far_spawn"] = True
    types[name] = t
for name, a in type_attrs.items():
    if name in types:
        types[name]["attrs"] = a
    else:
        fails.append(f"attributes for unknown type {name}")
for name in entity_ids:
    if name not in types:
        fails.append(f"entity {name} missing from EntityTypes.java")


# ---------- tags, eggs, loot, food ----------
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


item_tags, trade_tags = load_tags("item"), load_tags("villager_trade")
entity_tags = load_tags("entity_type")
items = lambda names: sorted(item_ids[n] for n in names if n in item_ids)
for name, t in types.items():
    if name + "_spawn_egg" in item_ids:
        t["egg"] = item_ids[name + "_spawn_egg"]
    if os.path.exists(os.path.join(DATA, "loot_table", "entities", name + ".json")):
        t["loot"] = "entities/" + name
    if name + "_food" in item_tags:
        t["food"] = items(item_tags[name + "_food"])
    if name + "_tempt_items" in item_tags:
        t["tempt"] = items(item_tags[name + "_tempt_items"])
for name, t in types.items():
    tags = sorted(tag for tag, vals in entity_tags.items() if name in vals)
    if tags:
        t["tags"] = tags
# Taming items not covered by tags (Wolf.isFood vs tryToTame, Parrot, Ocelot trust, horses by temper).
extra = {"wolf": {"tame": ["bone"]}, "cat": {"tame": item_tags.get("cat_food", [])},
         "parrot": {"tame": item_tags.get("parrot_food", []), "poison": item_tags.get("parrot_poisonous_food", [])},
         "ocelot": {"tame": item_tags.get("ocelot_food", [])}}
for name, kv in extra.items():
    for k, v in kv.items():
        types[name][k] = items(v)

# ---------- villager trades ----------
trades, trade_index = [], {}
tdir = os.path.join(DATA, "villager_trade")
for dp, _, fs in sorted(os.walk(tdir)):
    for f in sorted(fs):
        rel = os.path.relpath(os.path.join(dp, f), tdir)[:-5].replace(os.sep, "/")
        d = json.load(open(os.path.join(dp, f)))

        def cost(c):
            return None if c is None else [item_ids[c["id"][10:]], c.get("count", 1)] + ([c["components"]] if "components" in c else [])

        tr = {"wants": cost(d["wants"]), "gives": [item_ids[d["gives"]["id"][10:]], d["gives"].get("count", 1)],
              "max_uses": d.get("max_uses", 4), "xp": d.get("xp", 1), "disc": d.get("reputation_discount", 0.0)}
        if "additional_wants" in d:
            tr["b"] = cost(d["additional_wants"])
        if "merchant_predicate" in d:
            v = d["merchant_predicate"]["predicate"]["minecraft:predicates"]["minecraft:villager/variant"]
            tr["variants"] = [x[10:] for x in (v if isinstance(v, list) else [v])]
        if "given_item_modifier" in d:
            tr["mod"] = d["given_item_modifier"]  # kept raw; trading.hpp skips what it can't apply
        if "double_trade_price_enchantments" in d:
            tr["double_price"] = d["double_trade_price_enchantments"]
        if "components" in d["gives"]:
            tr["gives_components"] = d["gives"]["components"]
        trade_index[rel] = len(trades)
        trades.append(tr)

trade_sets = {}
sdir = os.path.join(DATA, "trade_set")
for dp, _, fs in os.walk(sdir):
    for f in fs:
        rel = os.path.relpath(os.path.join(dp, f), sdir)[:-5].replace(os.sep, "/")
        d = json.load(open(os.path.join(dp, f)))
        ref = d["trades"]
        names = trade_tags[ref[11:]] if ref.startswith("#") else ([ref[10:]] if isinstance(ref, str) else [x[10:] for x in ref])
        if not isinstance(d["amount"], int):
            fails.append(f"trade set {rel}: non-constant amount")
        trade_sets[rel] = {"trades": [trade_index[n] for n in names], "amount": d["amount"],
                           "dup": d.get("allow_duplicates", False)}

# ---------- POI + professions ----------
poi_src = java("PoiTypes")
block_names = {k[10:] for k in registries["minecraft:block"]["entries"]}
poi = {}
for m in re.finditer(r"register\(registry, (\w+), (.*?), (\d+), (\d+)\);", poi_src):
    spec = m.group(2)
    b = re.match(r"getBlockStates\(Blocks\.(\w+)\)", spec)
    if b:
        blocks = [b.group(1).lower()]
    else:  # a static set (BEDS, CAULDRONS, LIGHTNING_RODS)
        d = re.search(r"Set<BlockState> " + spec + r" = (.*?);", poi_src, re.S)
        blocks = [x.lower() for x in re.findall(r"Blocks\.(\w+)", d.group(1))] if d else []
        if spec == "BEDS":
            blocks = [x[10:] for x in json.load(open(os.path.join(DATA, "tags", "block", "beds.json")))["values"]]
    if not blocks or any(x not in block_names for x in blocks):
        fails.append(f"poi {m.group(1)}: blocks {blocks}")
    poi[m.group(1).lower()] = {"blocks": blocks, "tickets": int(m.group(3)), "range": int(m.group(4))}

prof_src = java("VillagerProfession")
professions = {}
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
professions["none"] = {"poi": None, "trades": {}}
professions["nitwit"] = {"poi": None, "trades": {}}
for n in reg("villager_profession"):
    if n not in professions:
        fails.append(f"profession {n} not parsed")

out = {"attributes": attributes, "types": types, "trades": trades, "trade_sets": trade_sets, "poi": poi,
       "professions": professions, "villager_types": sorted(reg("villager_type"), key=reg("villager_type").get)}
os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)
json.dump(out, open(os.path.join(ROOT, "data", "mobs.json"), "w"), separators=(",", ":"), sort_keys=True)
for f in fails:
    print("gen_mob_data: " + f, file=sys.stderr)
print(f"mobs.json: {len(types)} entity types ({len(type_attrs)} with attributes), {len(trades)} trades, "
      f"{len(trade_sets)} trade sets, {len(poi)} poi, {len(professions)} professions")
sys.exit(1 if fails else 0)
