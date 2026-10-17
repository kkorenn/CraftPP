#!/usr/bin/env python3
"""Mob/entity data for CraftPP -> data/mobs.json (loaded by src/mobs/data.hpp) and src/gen/entity_data.hpp.

Sources:
  * tools/dump/MobDump.java run against vanilla/versions/26.3/server-26.3.jar (build time only): per-state collision
    boxes, block flags (full block, conductor, valid spawn...), path types, fluids, block friction/speed/jump/blast
    resistance, attribute registry, entity type dimensions/tracking/attributes, SynchedEntityData accessor ids.
    The dump is cached in data/mob_dump.json so regenerating without a JDK still works.
  * decompiled SpawnPlacements.java (placement type, heightmap, spawn rule name) and MobCategory.java.
  * vanilla/src/data: biome natural_mob_spawns attributes, entity loot tables (+ shearing), tags, smelting.
"""
import glob, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VAN = os.path.join(ROOT, "vanilla")
DATA = os.path.join(VAN, "src", "data", "minecraft")
SRC = os.path.join(VAN, "src", "net", "minecraft")
REPORTS = os.path.join(VAN, "generated", "reports")
JAR = os.path.join(VAN, "versions", "26.3", "server-26.3.jar")
DUMP = os.path.join(ROOT, "data", "mob_dump.json")


def short(n):
    return n.split(":")[-1]


def find_jdk():
    for c in [os.environ.get("JAVA_HOME", "")] + sorted(glob.glob("/Library/Java/JavaVirtualMachines/*/Contents/Home"), reverse=True) + \
            sorted(glob.glob("/usr/lib/jvm/*"), reverse=True):
        if c and os.path.exists(os.path.join(c, "bin", "javac")):
            ok = subprocess.run([os.path.join(c, "bin", "java"), "-version"], capture_output=True, text=True).stderr
            m = re.search(r'version "(\d+)', ok)
            if m and int(m.group(1)) >= 25:
                return c
    return None


def run_dump():
    jdk = find_jdk()
    if not jdk or not os.path.exists(JAR):
        if os.path.exists(DUMP):
            print("gen_mob_data: no JDK >= 25 / server jar, using cached", DUMP)
            return json.load(open(DUMP))
        sys.exit("gen_mob_data: need a JDK >= 25 and vanilla/versions/26.3/server-26.3.jar (or a cached data/mob_dump.json)")
    cp = ":".join([JAR] + sorted(glob.glob(os.path.join(VAN, "libraries", "**", "*.jar"), recursive=True)))
    out = os.path.join(ROOT, "build", "mobdump")
    os.makedirs(out, exist_ok=True)
    subprocess.run([os.path.join(jdk, "bin", "javac"), "-nowarn", "-d", out, "-cp", cp,
                    os.path.join(ROOT, "tools", "dump", "MobDump.java")], check=True, capture_output=True)
    r = subprocess.run([os.path.join(jdk, "bin", "java"), "-cp", out + ":" + cp, "MobDump", JAR], check=True,
                       capture_output=True, text=True, cwd=out)
    d = json.loads(r.stdout)
    with open(DUMP, "w") as f:
        json.dump(d, f, separators=(",", ":"))
    return d


dump = run_dump()
registries = json.load(open(os.path.join(REPORTS, "registries.json")))
blocks_rep = json.load(open(os.path.join(REPORTS, "blocks.json")))


def reg_ids(reg):
    return {short(k): v["protocol_id"] for k, v in registries[reg]["entries"].items()}


etype_id = reg_ids("minecraft:entity_type")
attr_id = reg_ids("minecraft:attribute")
item_id = reg_ids("minecraft:item")
block_names = [short(n) for _, n in sorted((min(s["id"] for s in b["states"]), n) for n, b in blocks_rep.items())]
block_index = {n: i for i, n in enumerate(block_names)}
biome_ids = sorted("minecraft:" + os.path.relpath(f, os.path.join(DATA, "worldgen", "biome"))[:-5]
                   for f in glob.glob(os.path.join(DATA, "worldgen", "biome", "**", "*.json"), recursive=True))


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


entity_tags = {short(t): sorted(etype_id[short(v)] for v in vs if short(v) in etype_id) for t, vs in load_tags("entity_type").items()}
damage_type_names = sorted(short(f)[:-5] for f in os.listdir(os.path.join(DATA, "damage_type")))
damage_type_index = {n: i for i, n in enumerate(damage_type_names)}
damage_scaling = {n: json.load(open(os.path.join(DATA, "damage_type", n + ".json"))).get("scaling", "never") for n in damage_type_names}
damage_tags = {short(t): sorted(damage_type_index[short(v)] for v in vs if short(v) in damage_type_index)
               for t, vs in load_tags("damage_type").items()}
block_tags_all = load_tags("block")
item_tags_all = load_tags("item")
WANT_BLOCK_TAGS = ["prevent_mob_spawning_inside", "animals_spawnable_on", "fences", "walls", "leaves", "logs", "climbable",
                   "infiniburn_overworld", "fire", "campfires", "trapdoors", "doors", "wool", "sheep_food",
                   "enderman_holdable", "bamboo_plantable_on", "suppresses_bounce", "dirt", "sand", "blocks_fluid_flow", "ice", "speleothems", "beds", "edible_for_sheep", "fall_damage_resetting"]
block_tags = {t: sorted(block_index[short(v)] for v in block_tags_all.get("minecraft:" + t, []) if short(v) in block_index)
              for t in WANT_BLOCK_TAGS}
item_tags = {short(t): sorted(item_id[short(v)] for v in vs if short(v) in item_id) for t, vs in item_tags_all.items()
             if short(t).endswith("_food") or short(t) in ("arrows", "swords", "axes", "creeper_igniters", "skeleton_preferred_weapons")}

biome_index = {short(b): i for i, b in enumerate(biome_ids)}
biome_tags_all = load_tags("worldgen/biome")
biome_tags = {short(t): sorted(biome_index[short(v)] for v in vs if short(v) in biome_index) for t, vs in biome_tags_all.items()}

# ---------- spawn placements (decompiled SpawnPlacements.java) ----------
sp_java = open(os.path.join(SRC, "world", "entity", "SpawnPlacements.java")).read()
placements = {}
for m in re.finditer(r"register\(\s*EntityTypes\.(\w+),\s*SpawnPlacementTypes\.(\w+),\s*Heightmap\.Types\.(\w+),\s*([\w.]+::\w+)\s*\)", sp_java):
    placements[m.group(1).lower()] = [m.group(2), m.group(3), m.group(4)]

# ---------- mob categories (MobCategory.java) ----------
mc_java = open(os.path.join(SRC, "world", "entity", "MobCategory.java")).read()
categories = []
for m in re.finditer(r'(\w+)\("(\w+)", "\w+", (-?\d+), (true|false), (true|false), (\d+)\)', mc_java):
    categories.append({"name": m.group(2), "max": int(m.group(3)), "friendly": m.group(4) == "true",
                       "persistent": m.group(5) == "true", "despawn": int(m.group(6)), "no_despawn": 32})
cat_index = {c["name"]: i for i, c in enumerate(categories)}

# ---------- entity types ----------
attr_by_name = {a[0]: a for a in dump["attributes"]}
types = []
loot_dir = os.path.join(DATA, "loot_table", "entities")
for e in sorted(dump["entity_types"], key=lambda e: etype_id[e["id"]]):
    t = {"id": e["id"], "w": e["w"], "h": e["h"], "eye": e["eye"], "fixed": e["fixed"], "cat": cat_index[e["cat"]],
         "range": e["range"], "interval": e["interval"], "deltas": e["deltas"], "fire_immune": e["fire_immune"],
         "summon": e["summon"], "serialize": e["serialize"], "far": e["far"], "peaceful": e["peaceful"],
         "immune": block_tags_all.get("minecraft:" + e.get("immune", ""), [])}
    t["immune"] = sorted(block_index[short(v)] for v in t["immune"] if short(v) in block_index)
    if "attr" in e:
        t["attr"] = sorted([attr_id[k], v] for k, v in e["attr"].items())
    if e["id"] in placements:
        t["place"] = placements[e["id"]]
    if os.path.exists(os.path.join(loot_dir, e["id"] + ".json")):
        t["loot"] = "entities/" + e["id"]
    types.append(t)
assert [etype_id[t["id"]] for t in types] == list(range(len(types)))

attributes = [None] * len(attr_id)
for name, i in attr_id.items():
    a = attr_by_name[name]
    attributes[i] = {"id": name, "def": a[1], "min": a[2], "max": a[3], "sync": a[4]}

# ---------- biome spawns (environment attribute gameplay/natural_mob_spawns, overlay) ----------
def count_range(c):
    if isinstance(c, int):
        return [c, c]
    return [c["min_inclusive"], c["max_inclusive"]]


biome_spawns = []
for b in biome_ids:
    j = json.load(open(os.path.join(DATA, "worldgen", "biome", b.split(":")[1] + ".json")))
    attrs = j.get("attributes", {})
    ns = attrs.get("minecraft:gameplay/natural_mob_spawns", {})
    arg = ns.get("argument", ns) if isinstance(ns, dict) else {}
    cats = {}
    for cat, lst in arg.get("spawns_by_category", {}).items():
        cats[cat_index[cat]] = [[etype_id[short(s["type"])], s["weight"]] + count_range(s["count"]) for s in lst]
    costs = {etype_id[short(k)]: [v["energy_budget"], v["charge"]] for k, v in arg.get("spawn_costs", {}).items()}
    biome_spawns.append({"id": short(b), "cats": [[k, v] for k, v in sorted(cats.items())],
                         "costs": sorted([k] + v for k, v in costs.items()),
                         "creature_p": attrs.get("minecraft:gameplay/creature_world_gen_spawn_probability", 0.1),
                         "temp": j.get("temperature", 0.5)})


# ---------- structure spawn_overrides (StructureSpawnOverride): {id, cats: [[cat, piece?, spawners]]} ----------
structure_spawns = []
for f in sorted(glob.glob(os.path.join(DATA, "worldgen", "structure", "*.json"))):
    ov = json.load(open(f)).get("spawn_overrides", {})
    if ov:
        structure_spawns.append({"id": "minecraft:" + os.path.basename(f)[:-5], "cats": [
            [cat_index[c], o["bounding_box"] == "piece", [[etype_id[short(s["type"])], s["weight"]] + count_range(s["count"]) for s in o["spawns"]]]
            for c, o in sorted(ov.items())]})


# ---------- entity loot tables: entity predicates pre-compiled ----------
def type_set(spec):
    if isinstance(spec, list):
        out = []
        for s in spec:
            out += type_set(s)
        return sorted(set(out))
    if spec.startswith("#"):
        return entity_tags.get(short(spec[1:]), [])
    return [etype_id[short(spec)]] if short(spec) in etype_id else []


def entity_pred(p):
    """minecraft entity predicate -> list of checks; None when something unsupported is required."""
    checks = []
    for k, v in p.items():
        k = short(k)
        if k == "flags":
            for f, want in v.items():
                checks.append(["flag", f, want])
        elif k == "entity_type":
            checks.append(["type", type_set(v)])
        elif k == "vehicle":
            sub = entity_pred(v)
            if sub is None:
                return None
            checks.append(["vehicle", sub])
        elif k == "components":
            for ck, cv in v.items():
                checks.append(["prop", short(ck), cv if isinstance(cv, str) else json.dumps(cv)])
        elif k.startswith("type_specific/"):
            for ck, cv in v.items():
                checks.append(["prop", k[len("type_specific/"):] + "/" + ck, json.dumps(cv) if not isinstance(cv, str) else cv])
        else:
            return None  # equipment, nbt, location...: unsupported (vanilla: smelting-enchantment checks etc.)
    return checks


def compile_loot(o):
    if isinstance(o, list):
        return [compile_loot(v) for v in o]
    if not isinstance(o, dict):
        return o
    r = {k: compile_loot(v) for k, v in o.items()}
    t = short(r.get("type", ""))
    if t == "entity_properties":
        checks = entity_pred(o.get("predicate", {}))
        r = {"type": "minecraft:entity_properties", "entity": o["entity"], "checks": checks or [], "unsupported": checks is None}
    elif t == "damage_source_properties":
        p = o.get("predicate", {})
        r = {"type": "minecraft:damage_source_properties", "unsupported": False}
        for k, v in p.items():
            if k in ("source_entity", "direct_entity"):
                c = entity_pred(v)
                if c is None:
                    r["unsupported"] = True
                r[k] = c or []
            elif k == "tags":
                r["tags"] = [[damage_tags.get(short(x["id"][1:]), []), x["expected"]] for x in v]
            elif k == "is_direct":
                r["is_direct"] = v
            else:
                r["unsupported"] = True
    elif t == "match_tool":
        pr = o.get("predicate", {})
        items = pr.get("items")
        if isinstance(items, str):
            items = [items]
        ids = None
        if items:
            ids = []
            for n in items:
                ids += [item_id[short(v)] for v in (item_tags_all.get(n[1:], []) if n.startswith("#") else [n]) if short(v) in item_id]
            ids = sorted(set(ids))
        r = {"type": "minecraft:match_tool", "items": ids, "unsupported": any(k != "items" for k in pr)}
    elif t == "location_check":
        r = {"type": "minecraft:location_check", "off": [0, 0, 0], "blocks": None, "state": {}, "unsupported": True}
    elif t == "item" and "name" in r:
        r["item"] = item_id.get(short(r["name"]), -1)
    elif t == "tag" and "name" in r:
        r["items"] = sorted(item_id[short(v)] for v in item_tags_all.get(r["name"], []) if short(v) in item_id)
    return r


loot = {}
for sub in ("entities", "shearing", "gameplay"):
    base = os.path.join(DATA, "loot_table", sub)
    for dp, _, fs in os.walk(base):
        for f in fs:
            if f.endswith(".json"):
                name = os.path.relpath(os.path.join(dp, f), os.path.join(DATA, "loot_table"))[:-5].replace(os.sep, "/")
                loot[name] = compile_loot(json.load(open(os.path.join(dp, f))))

# smelting (furnace_smelt): input item -> result item
smelt = {}
for f in glob.glob(os.path.join(DATA, "recipe", "*.json")):
    r = json.load(open(f))
    if r.get("type") == "minecraft:smelting":
        ing = r["ingredient"]
        ings = ing if isinstance(ing, list) else [ing]
        for i in ings:
            for n in (item_tags_all.get(i[1:], []) if i.startswith("#") else [i]):
                if short(n) in item_id and short(r["result"]["id"]) in item_id:
                    smelt[item_id[short(n)]] = item_id[short(r["result"]["id"])]

# ---------- item attribute modifiers / weapon (combat) ----------
SLOTS = {"mainhand": 0, "offhand": 1, "feet": 2, "legs": 3, "chest": 4, "head": 5, "body": 6, "saddle": 7,
         "any": 8, "hand": 9, "armor": 10}
OPS = {"add_value": 0, "add_multiplied_base": 1, "add_multiplied_total": 2}
item_mods = {}
comp_dir = os.path.join(REPORTS, "minecraft", "components", "item")
for name, iid in item_id.items():
    p = os.path.join(comp_dir, name + ".json")
    if not os.path.exists(p):
        continue
    c = json.load(open(p))["components"]
    mods = [[attr_id[short(m["type"])], m["amount"], OPS[m["operation"]], SLOTS[m.get("slot", "any")]]
            for m in c.get("minecraft:attribute_modifiers", []) if short(m["type"]) in attr_id]
    w = c.get("minecraft:weapon")
    if mods or w is not None:
        item_mods[iid] = {"m": mods, "w": -1 if w is None else w.get("item_damage_per_attack", 1)}

# ---------- block physics by block index ----------
phys = {b[0]: b[1:] for b in dump["blocks"]}
block_phys = [phys[n] for n in block_names]

out = {
    "shapes": dump["shapes"], "state_shape": dump["state_shape"], "state_flags": dump["state_flags"],
    "state_path": dump["state_path"], "state_fluid": dump["state_fluid"], "path_types": dump["path_types"],
    "block_phys": block_phys, "attributes": attributes, "entity_types": types, "categories": categories,
    "biome_spawns": biome_spawns, "structure_spawns": structure_spawns, "entity_tags": entity_tags, "damage_tags": damage_tags, "damage_scaling": [damage_scaling[n] for n in damage_type_names], "block_tags": block_tags, "biome_tags": biome_tags,
    "registry_ids": {r.split(":")[1]: {short(k): v["protocol_id"] for k, v in registries[r]["entries"].items()}
                     for r in ("minecraft:particle_type", "minecraft:sound_event")},
    "item_tags": item_tags, "loot": loot, "item_mods": sorted([k, v["m"], v["w"]] for k, v in item_mods.items()), "smelt": sorted([k, v] for k, v in smelt.items()),
}
with open(os.path.join(ROOT, "data", "mobs.json"), "w") as fp:
    json.dump(out, fp, separators=(",", ":"))


# ---------- src/gen/entity_data.hpp ----------
def ident(s):
    s = re.sub(r"^DATA_", "", s)
    s = re.sub(r"_ID$", "", s)
    return s


h = ["// Generated by tools/gen_mob_data.py from the vanilla 26.3 jar (SynchedEntityData accessors). Do not edit.",
     "#pragma once", "", "// Serializer ids (EntityDataSerializers registration order).", "namespace edser {"]
for name, sid in sorted(dump["serializers"].items(), key=lambda kv: kv[1]):
    h.append(f"inline constexpr int {name} = {sid};")
h.append("}")
h.append("")
h.append("// Entity data slot ids per vanilla class: edata::<Class>::<FIELD> (absolute index, incl. superclasses).")
h.append("namespace edata {")
for cls, info in sorted(dump["entity_data"].items()):
    h.append(f"namespace {cls} {{  // extends {info['super']}")
    seen = set()
    for field, i, ser in info["f"]:
        n = ident(field)
        if n in seen:
            n = field
        seen.add(n)
        sname = next((k for k, v in dump["serializers"].items() if v == ser), str(ser))
        h.append(f"inline constexpr int {n} = {i};  // {sname}")
    h.append("}")
h.append("}")
with open(os.path.join(ROOT, "src", "gen", "entity_data.hpp"), "w") as fp:
    fp.write("\n".join(h) + "\n")
print(f"mobs.json: {len(dump['shapes'])} shapes, {len(types)} entity types, {len(attributes)} attributes, "
      f"{len(loot)} loot tables, {len(biome_spawns)} biomes; entity_data.hpp: {len(dump['entity_data'])} classes")
