#!/usr/bin/env python3
"""Generates src/gen/command_data.hpp: the vanilla command tree + data the command system needs.

Inputs (all from the vanilla jar / reports, see README):
  vanilla/generated/reports/{commands,blocks,registries}.json, reports/minecraft/components/item/*.json,
  vanilla/src/assets/minecraft/lang/en_us.json, vanilla/src/data/minecraft/**, decompiled GameRules.java.
Everything is emitted as static constexpr tables (no JSON at runtime).
"""
import json, os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VAN = os.path.join(ROOT, "vanilla")
REPORTS = os.path.join(VAN, "generated", "reports")
DATA = os.path.join(VAN, "src", "data", "minecraft")
SRC = os.path.join(VAN, "src", "net", "minecraft")

tree = json.load(open(os.path.join(REPORTS, "commands.json")))
blocks = json.load(open(os.path.join(REPORTS, "blocks.json")))
registries = json.load(open(os.path.join(REPORTS, "registries.json")))
lang = json.load(open(os.path.join(VAN, "src", "assets", "minecraft", "lang", "en_us.json")))
arg_ids = {k: v["protocol_id"] for k, v in registries["minecraft:command_argument_type"]["entries"].items()}

LEVELS = {"moderators": 1, "gamemasters": 2, "admins": 3, "owners": 4}

# The report sorts keys; vanilla registration order matters where argument siblings compete.
ORDER = {("teleport",): ["location", "destination", "targets"], ("teleport", "targets"): ["location", "destination"]}

# Argument nodes with a custom SuggestionProvider (from `.suggests(` in server/commands/*.java):
# (root literal, argument name) -> provider. The report doesn't carry these, so they're listed by hand.
SUGGEST = {
    ("advancement", "criterion"): "ask_server", ("attribute", "id"): "ask_server", ("bossbar", "id"): "ask_server",
    ("deop", "targets"): "ask_server", ("op", "targets"): "ask_server", ("pardon", "targets"): "ask_server",
    ("pardon-ip", "target"): "ask_server", ("whitelist", "targets"): "ask_server",
    ("datapack", "name"): "ask_server", ("datapack", "existing"): "ask_server",
    ("function", "name"): "ask_server", ("debug", "name"): "ask_server", ("schedule", "function"): "ask_server",
    ("place", "template"): "ask_server", ("random", "sequence"): "ask_server",
    ("playsound", "sound"): "available_sounds", ("stopsound", "sound"): "available_sounds",
    ("summon", "entity"): "summonable_entities", ("posteffect", "posteffect"): "post_effects",
    ("tag", "name"): "ask_server", ("team", "members"): "ask_server", ("time", "timemarker"): "ask_server",
    ("tick", "rate"): "ask_server", ("tick", "time"): "ask_server", ("trigger", "objective"): "ask_server",
    ("stopwatch", "id"): "ask_server",
    ("scoreboard", "targets"): "ask_server", ("scoreboard", "target"): "ask_server", ("scoreboard", "source"): "ask_server",
    ("execute", "entity"): "summonable_entities", ("execute", "targets"): "ask_server", ("execute", "target"): "ask_server",
    ("execute", "source"): "ask_server", ("execute", "id"): "ask_server", ("execute", "name"): "ask_server",
}
SUGGEST_IDS = ["", "ask_server", "available_sounds", "summonable_entities", "post_effects"]


def suggest_for(path, name, node):
    p = node.get("parser")
    s = SUGGEST.get((path[0], name))
    if path[:3] == ["scoreboard", "players", "enable"] and name == "objective":
        return 1
    if path[0] in ("tag", "bossbar", "attribute", "stopwatch") and ("add" in path or "create" in path):
        return 0
    if not s:
        return 0
    # Narrow the shared names to the parser the vanilla node uses.
    if name in ("targets", "target", "source") and path[0] in ("scoreboard", "execute") and p != "minecraft:score_holder":
        return 0
    if path[0] == "execute":
        if name == "entity" and p != "minecraft:resource": return 0
        if name == "id" and p != "minecraft:resource_location": return 0
        if name == "name" and p != "minecraft:function": return 0
    if path[0] == "attribute" and p != "minecraft:resource_location": return 0
    if path[0] == "team" and p != "minecraft:score_holder": return 0
    if (path[0], name) in (("tick", "rate"), ("tick", "time")) and "query" in path: return 0
    return SUGGEST_IDS.index(s)


nodes = []  # dicts: type, name, parser, props, exec, perm, redirect(path), children(list of idx), suggest


def add(node, name, path):
    idx = len(nodes)
    nodes.append(None)
    kids = node.get("children", {})
    order = list(kids)
    if tuple(path) in ORDER:
        want = ORDER[tuple(path)]
        order = [k for k in order if kids[k]["type"] == "literal"] + want + \
                [k for k in order if kids[k]["type"] != "literal" and k not in want]
    child_idx = [add(kids[k], k, path + [k]) for k in order]
    redirect = node.get("redirect")
    if redirect is None and path in (["execute", "run"], ["return", "run"]):
        redirect = []  # redirect to root (the report omits empty paths)
    perm = node.get("permissions", {}).get("permission", {}).get("level")
    nodes[idx] = dict(type=node["type"], name=name, parser=node.get("parser"), props=node.get("properties") or {},
                      exec=bool(node.get("executable")), perm=LEVELS.get(perm, 0), redirect=redirect,
                      children=child_idx, suggest=suggest_for(path, name, node) if node["type"] == "argument" else 0)
    return idx


add(tree, "", [])


def resolve_path(p):
    i = 0
    for piece in p:
        i = next(c for c in nodes[i]["children"] if nodes[c]["name"] == piece)
    return i


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def num(v):
    return repr(float(v)) if isinstance(v, float) else str(v)


h = ["// Generated by tools/gen_commands.py from the vanilla 26.3 reports. Do not edit.", "#pragma once",
     "#include <cstdint>", "", "namespace gen {", "",
     "// Vanilla command tree. type: 0 root 1 literal 2 argument. Children are kids[first..first+count).",
     "// props: string kind (0 word 1 phrase 2 greedy) / entity flags (1 single, 2 players) / score_holder (1 multiple),",
     "// min/max for numbers and time, registry for resource*. suggest: index into SUGGESTION_IDS.",
     "struct CmdNode { uint8_t type, perm, exec, suggest; int16_t parser; int8_t has_min, has_max; double min, max;",
     "                 int32_t flags, redirect, first, count; const char* name; const char* registry; };"]
kids = []
rows = []
for n in nodes:
    first = len(kids)
    kids += n["children"]
    pr = n["props"]
    parser = arg_ids[n["parser"]] if n["parser"] else -1
    flags = 0
    if n["parser"] == "brigadier:string":
        flags = {"word": 0, "phrase": 1, "greedy": 2}[pr["type"]]
    elif n["parser"] == "minecraft:entity":
        flags = (1 if pr["amount"] == "single" else 0) | (2 if pr["type"] == "players" else 0)
    elif n["parser"] == "minecraft:score_holder":
        flags = 1 if pr["amount"] == "multiple" else 0
    has_min, has_max = int("min" in pr), int("max" in pr)
    redirect = -1 if n["redirect"] is None else resolve_path(n["redirect"])
    t = {"root": 0, "literal": 1, "argument": 2}[n["type"]]
    rows.append(f'{{{t},{n["perm"]},{int(n["exec"])},{n["suggest"]},{parser},{has_min},{has_max},'
                f'{num(pr.get("min", 0))},{num(pr.get("max", 0))},{flags},{redirect},{first},{len(n["children"])},'
                f'{cstr(n["name"])},{cstr(pr.get("registry", ""))}}}')
h.append("inline constexpr CmdNode CMD_NODES[] = {\n" + ",\n".join(rows) + "};")
h.append("inline constexpr int32_t CMD_KIDS[] = {" + ",".join(map(str, kids)) + "};")
h.append("inline constexpr const char* SUGGESTION_IDS[] = {" + ",".join(cstr("minecraft:" + s if s else "") for s in SUGGEST_IDS) + "};")

# ---- language subset (console output; players get translatable components) ----
PREFIXES = ("commands.", "command.", "argument.", "arguments.", "parsing.", "permissions.", "multiplayer.disconnect.",
            "chat.", "gameMode.", "snbt.", "item.minecraft.", "block.minecraft.", "entity.minecraft.",
            "effect.minecraft.", "enchantment.minecraft.", "options.difficulty.", "disconnect.", "gamerule.",
            "mirror.", "rotation.", "filled_map.", "biome.minecraft.", "attribute.name.", "commands", "death.",
            "multiplayer.player.")
sub = sorted((k, v) for k, v in lang.items() if k.startswith(PREFIXES))
h.append("struct LangEntry { const char* key; const char* value; };")
h.append("inline constexpr LangEntry LANG[] = {\n" + ",\n".join(f"{{{cstr(k)},{cstr(v)}}}" for k, v in sub) + "};")

# ---- items ----
items = sorted(registries["minecraft:item"]["entries"].items(), key=lambda kv: kv[1]["protocol_id"])
comp_dir = os.path.join(REPORTS, "minecraft", "components", "item")
stack, name_keys = [], []
for name, _ in items:
    short = name.split(":")[1]
    c = json.load(open(os.path.join(comp_dir, short + ".json")))["components"] if os.path.exists(
        os.path.join(comp_dir, short + ".json")) else {}
    stack.append(str(c.get("minecraft:max_stack_size", 64)))
    nm = c.get("minecraft:item_name", {})
    name_keys.append(nm.get("translate", "item.minecraft." + short) if isinstance(nm, dict) else "item.minecraft." + short)
h.append("inline constexpr const char* ITEM_NAMES[] = {" + ",".join(cstr(n.split(":")[1]) for n, _ in items) + "};")
h.append("inline constexpr uint8_t ITEM_MAX_STACK[] = {" + ",".join(stack) + "};")
h.append("inline constexpr const char* ITEM_NAME_KEY[] = {" + ",".join(cstr(k) for k in name_keys) + "};")

# ---- blocks: name, first state, default state, properties (state index: first property is most significant) ----
block_reg = sorted(registries["minecraft:block"]["entries"].items(), key=lambda kv: kv[1]["protocol_id"])
props_rows, brows = [], []
for name, _ in block_reg:
    b = blocks[name]
    first = min(s["id"] for s in b["states"])
    default = next(s["id"] for s in b["states"] if s.get("default"))
    po = len(props_rows)
    for pn, vals in b.get("properties", {}).items():
        props_rows.append(f'{{{cstr(pn)},{cstr(",".join(vals))},{len(vals)}}}')
    brows.append(f'{{{cstr(name.split(":")[1])},{first},{default},{po},{len(b.get("properties", {}))}}}')
h.append("struct BlockProp { const char* name; const char* values; uint8_t count; };")
h.append("struct BlockInfo { const char* name; int32_t first, def; uint16_t prop_off; uint8_t prop_count; };")
h.append("inline constexpr BlockProp BLOCK_PROPS[] = {" + ",".join(props_rows) + "};")
h.append("inline constexpr BlockInfo BLOCKS[] = {\n" + ",\n".join(brows) + "};")


# ---- tags (block + item), resolved to flat id lists ----
def resolve_tags(folder, ids):
    base = os.path.join(DATA, "tags", folder)
    raw = {}
    for dp, _, fs in os.walk(base):
        for f in fs:
            if f.endswith(".json"):
                rel = os.path.relpath(os.path.join(dp, f), base)[:-5].replace(os.sep, "/")
                raw["minecraft:" + rel] = json.load(open(os.path.join(dp, f)))["values"]
    out = {}

    def res(t):
        if t in out: return out[t]
        out[t] = []
        r = []
        for v in raw.get(t, []):
            v = v["id"] if isinstance(v, dict) else v
            r += res(v[1:]) if v.startswith("#") else ([ids[v]] if v in ids else [])
        out[t] = list(dict.fromkeys(r))
        return out[t]

    for t in raw: res(t)
    return out


for folder, reg, sym in [("block", "minecraft:block", "BLOCK_TAGS"), ("item", "minecraft:item", "ITEM_TAGS")]:
    ids = {k: v["protocol_id"] for k, v in registries[reg]["entries"].items()}
    tags = resolve_tags(folder, ids)
    flat, rows = [], []
    for t in sorted(tags):
        rows.append(f'{{{cstr(t.split(":")[1])},{len(flat)},{len(tags[t])}}}')
        flat += tags[t]
    h.append(f"inline constexpr int32_t {sym}_IDS[] = {{" + ",".join(map(str, flat)) + "};")
    h.append(f"inline constexpr struct {{ const char* name; int32_t off, count; }} {sym}[] = {{" + ",".join(rows) + "};")


# ---- registries used by resource arguments / validation ----
def json_ids(folder):
    base = os.path.join(DATA, folder)
    out = []
    for dp, _, fs in os.walk(base):
        for f in fs:
            if f.endswith(".json"):
                out.append(os.path.relpath(os.path.join(dp, f), base)[:-5].replace(os.sep, "/"))
    return sorted(out)


REGS = ["attribute", "damage_type", "enchantment", "entity_type", "mob_effect", "timeline", "world_clock",
        "worldgen/biome", "particle_type", "dimension_type", "point_of_interest_type"]
h.append("struct Registry { const char* name; const char* const* entries; int count; };")
reg_rows = []
for r in REGS:
    key = "minecraft:" + r
    if key in registries:
        ents = [e.split(":")[1] for e, _ in sorted(registries[key]["entries"].items(), key=lambda kv: kv[1]["protocol_id"])]
    else:
        ents = json_ids(r)
    sym = "REG_" + re.sub(r"\W", "_", r).upper()
    h.append(f"inline constexpr const char* {sym}[] = {{" + ",".join(cstr(e) for e in ents) + "};")
    reg_rows.append(f'{{{cstr(key)},{sym},{len(ents)}}}')
h.append("inline constexpr Registry REGISTRIES[] = {" + ",".join(reg_rows) + "};")

# ---- game rules: id, is_int, default, min, max (from GameRules.java) ----
src = open(os.path.join(SRC, "world", "level", "gamerules", "GameRules.java")).read()
rules = []
for m in re.finditer(r'register(Boolean|Integer)\(\s*"(\w+)",\s*GameRuleCategory\.\w+,\s*([^;]*?)\)\s*;', src):
    kind, rid, rest = m.groups()
    parts = [p.strip() for p in rest.split(",")]
    if kind == "Boolean":
        d = parts[0]
        rules.append((rid, 0, 0 if d == "false" else 1, 0, 0))
    else:
        def iv(s, dflt):
            try: return int(s)
            except: return dflt
        rules.append((rid, 1, iv(parts[0], 0), iv(parts[1], -2**31) if len(parts) > 1 else -2**31,
                      iv(parts[2], 2**31 - 1) if len(parts) > 2 else 2**31 - 1))
h.append("struct GameRuleInfo { const char* id; uint8_t is_int; int32_t def, min, max; };")
h.append("inline constexpr GameRuleInfo GAME_RULES[] = {" + ",".join(
    f'{{{cstr(r[0])},{r[1]},{r[2]},{r[3]},{r[4]}}}' for r in rules) + "};")

h.append("}  // namespace gen")
open(os.path.join(ROOT, "src", "gen", "command_data.hpp"), "w").write("\n".join(h) + "\n")
print(f"command_data.hpp: {len(nodes)} nodes, {len(sub)} lang keys, {len(items)} items, {len(block_reg)} blocks, "
      f"{len(rules)} game rules")
