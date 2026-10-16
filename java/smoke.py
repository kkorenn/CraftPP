#!/usr/bin/env python3
"""Drive prism-bridge.jar the way PrismMC does, without the C# server.

Usage: smoke.py <java> <prism-bridge.jar> <plugins-dir>
"""
import base64, json, os, subprocess, sys, threading, queue, time

java, jar, plugins = sys.argv[1:4]
# NMS_CP=<vanilla server jar + libraries, ':'-joined> runs the bridge with Mojang's classes like PrismMC does for NMS plugins.
nms_cp = os.environ.get("NMS_CP")
cmd = [java, "-cp", jar + os.pathsep + nms_cp, "-Dprismmc.nms=true", "prism.bridge.Main"] if nms_cp else [java, "-jar", jar]
proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr, text=True, bufsize=1)
packets = []
replies = {}
events = queue.Queue()
next_id = [1]
players = {"11111111-2222-3333-4444-555555555555": {"name": "Tester", "uuid": "11111111-2222-3333-4444-555555555555", "x": 1.5, "y": 70.0, "z": 2.5,
           "yaw": 0.0, "pitch": 0.0, "gamemode": 1, "op": True, "health": 20, "food": 20, "level": 3, "ip": "127.0.0.1", "port": 1234, "entityId": 7}}
logs = []

calls = []
def api(method, args):
    calls.append((method, args))
    if method == "server.registries":
        return {"minecraft:worldgen/biome": ["minecraft:plains", "minecraft:desert"], "minecraft:enchantment": ["minecraft:sharpness", "minecraft:unbreaking"], "minecraft:damage_type": ["minecraft:generic"]}
    if method == "server.tps":
        return {"tps": [20, 20, 20], "mspt": 0.1, "tick": 1}
    if method == "player.openInventory":
        return 5
    if method == "player.inventory":
        return {"heldSlot": 0, "items": [None] * 46, "window": None}
    if method == "entity.spawn":
        return {"id": 1000001, "uuid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "type": args["type"], "x": args["x"], "y": args["y"], "z": args["z"], "yaw": 0, "pitch": 0, "health": 20, "customName": None, "customNameVisible": False}
    if method == "entity.state":
        return {"id": 1000001, "uuid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "type": "minecraft:zombie", "x": 1, "y": 70, "z": 2, "yaw": 0, "pitch": 0, "health": 20, "customName": "§cBob", "customNameVisible": True}
    if method == "entity.list":
        return []
    if method == "world.blockDataOf":
        t = args["text"]
        if t.startswith("minecraft:oak_stairs"):
            return {"block": "minecraft:oak_stairs", "state": 100, "properties": {"facing": "north", "half": "bottom", "shape": "straight", "waterlogged": "false"}, "allowed": {"facing": ["north", "south", "west", "east"], "half": ["top", "bottom"], "shape": ["straight"], "waterlogged": ["true", "false"]}}
        return {"block": t.split("[")[0], "state": 1, "properties": {}, "allowed": {}}
    if method == "world.blockData":
        return {"block": "minecraft:stone", "state": 1, "properties": {}, "allowed": {}}
    if method == "world.border":
        return {"centerX": 0, "centerZ": 0, "size": 59999968, "damageAmount": 0.2, "damageBuffer": 5, "warningDistance": 5, "warningTime": 15}
    if method == "world.biome":
        return "minecraft:plains"
    if method == "world.spawn":
        return {"x": 0, "y": 64, "z": 0}
    if method == "world.chunkPluginData" or method == "world.pluginData":
        return None
    if method == "server.players":
        return list(players.values())
    if method == "player.state":
        return players[args["uuid"]]
    if method == "player.sendMessage":
        logs.append(("msg", args["uuid"], args["text"])); return None
    if method == "server.motd":
        return "smoke motd"
    if method == "world.block":
        return {"block": "minecraft:diamond_block" if args["y"] == 64 else "minecraft:bedrock" if args["y"] < -60 else "minecraft:stone", "state": 1}
    if method == "world.time":
        return 1000
    if method == "player.setOp":
        players[args["uuid"]]["op"] = args["op"]; return None
    return None

def reader():
    for line in proc.stdout:
        m = json.loads(line)
        t = m.get("t")
        if t == "reply":
            replies[m["id"]] = m
        elif t == "log":
            a = m["a"]; logs.append(("log", a["level"], a["logger"], a["msg"]))
            print(f"[{a['level']}] {'['+a['logger']+'] ' if a['logger'] else ''}{a['msg']}")
        elif t == "listeners":
            print("listeners:", json.dumps(m["a"]))
        elif t == "packet":
            packets.append(m["a"])
        elif t == "api":
            a = m["a"]
            try:
                r = api(a["m"], a["a"])
                send({"id": m["id"], "t": "reply", "r": r})
            except Exception as e:
                send({"id": m["id"], "t": "reply", "err": str(e)})
        else:
            print("??", line.strip())

def send(obj):
    proc.stdin.write(json.dumps(obj) + "\n"); proc.stdin.flush()

def call(t, a, timeout=20):
    i = next_id[0]; next_id[0] += 1
    send({"id": i, "t": t, "a": a})
    deadline = time.time() + timeout
    while i not in replies:
        if time.time() > deadline: raise SystemExit(f"timeout waiting for {t}")
        time.sleep(0.005)
    r = replies.pop(i)
    if "err" in r: raise SystemExit(f"{t} failed: {r['err']}")
    return r.get("r")

threading.Thread(target=reader, daemon=True).start()
ok = True
def check(cond, what):
    global ok
    print(("PASS " if cond else "FAIL ") + what)
    ok = ok and cond

r = call("init", {"pluginsDir": plugins, "serverDir": plugins, "mcVersion": "26.2", "version": "26.2-PrismMC", "bukkitVersion": "26.2-R0.1-SNAPSHOT", "maxPlayers": 20, "port": 30000, "onlineMode": False, "worldName": "world", "seed": 42})
check(any(p["name"] == "PrismTestPlugin" for p in r["plugins"]), "plugin loaded")
r = call("enable", {})
labels = {c["label"] for c in r["commands"]}
check("prismtest" in labels and "prismtestplugin:prismtest" in labels, f"commands registered {sorted(labels)}")
check(all(p["enabled"] for p in r["plugins"]), "plugin enabled")
p = players["11111111-2222-3333-4444-555555555555"]
r = call("event", {"e": "join", "p": p})
check(r.get("message", "").startswith("§b[PrismTest] Tester joined"), f"join message rewritten: {r}")
r = call("dispatch", {"sender": p["uuid"], "line": "prismtest hello world"})
check(r["handled"] and any(x[0] == "msg" and "prismtest ok: hello world" in x[2] for x in logs), f"plugin command ran: {r}")
r = call("visible", {"uuid": "99999999-2222-3333-4444-555555555555", "name": "Guest", "op": False})
check("prismtest" in r["commands"] and "prismop" not in r["commands"], f"visible commands respect permission: {r['commands']}")
r = call("dispatch", {"sender": p["uuid"], "line": "prismbrig"})
check(r["handled"] and any(x[0] == "msg" and x[2] == "brig ok" for x in logs), "brigadier command executed")
r = call("dispatch", {"sender": p["uuid"], "line": "pb Tester 3"})
check(any(x[0] == "msg" and x[2] == "brig target Tester x3" for x in logs), f"brigadier alias with player+integer args")
r = call("tab", {"sender": p["uuid"], "line": "prismbrig T"})
check("Tester" in r["completions"] and r.get("start") == 10, f"brigadier tab completion: {r}")
r = call("visible", {"uuid": p["uuid"], "name": "Tester", "op": True})
brig = [t["root"] for t in r["brigadier"]]
check("prismbrig" in brig and "pb" in brig and "prismbrig" in r["labels"], f"brigadier trees exported: {brig}")
node_types = {n["type"] for n in [t for t in r["brigadier"] if t["root"] == "prismbrig"][0]["nodes"]}
check(node_types == {"literal", "argument"} and any(n.get("parserId", -1) >= 0 for t in r["brigadier"] for n in t["nodes"]), "brigadier nodes carry parser ids")
r = call("dispatch", {"sender": p["uuid"], "line": "prismgui"})
opened = [c for c in calls if c[0] == "player.openInventory"]
check(len(opened) == 1 and opened[0][1]["type"] == "generic_9x3" and opened[0][1]["items"][13]["item"] == "minecraft:diamond" and opened[0][1]["items"][13]["meta"]["name"] == "§6Shiny" and opened[0][1]["items"][13]["meta"]["enchants"] == {"minecraft:sharpness": 2}, f"gui opened with item meta: {opened}")
check(any(x[0] == "msg" and "gui opened with §6Shiny" in x[2] for x in logs), "item meta round-trips through the inventory")
r = call("event", {"e": "invclick", "p": {"uuid": p["uuid"], "name": "Tester", "windowId": 5, "slot": 13, "button": 0, "mode": 0, "containerSize": 27, "current": {"item": "minecraft:diamond", "count": 3}, "cursor": None}})
check(r["cancelled"] and any(x[0] == "msg" and "clicked slot 13 LEFT item=DIAMOND" in x[2] for x in logs), f"inventory click event cancelled: {r}")
if nms_cp:
    r = call("dispatch", {"sender": p["uuid"], "line": "prismnms"})
    craft = [x[2] for x in logs if x[0] == "msg" and x[2].startswith("craft ")]
    check(craft and "org.bukkit.craftbukkit.CraftServer / org.bukkit.craftbukkit.entity.CraftPlayer id=7 name=Tester" in craft[0], f"CraftServer/CraftPlayer.getHandle: {craft}")
    check(craft and "level=minecraft:overworld below=Block{minecraft:stone} zombie=Zombie#1000001 players=1" in craft[0], f"ServerLevel/entity handles: {craft}")
    chat = base64.b64encode(bytes([0x09, 4]) + b"nope" + bytes(8 + 8 + 1 + 1 + 3 + 1)).decode()
    r = call("inbound", {"uuid": p["uuid"], "data": chat})
    check(r is not None and r.get("packets") == [], f"inbound chat 'nope' swallowed by the plugin's pipeline handler: {r}")
    hello = base64.b64encode(bytes([0x09, 5]) + b"hello" + bytes(8 + 8 + 1 + 1 + 3 + 1)).decode()
    r = call("inbound", {"uuid": p["uuid"], "data": hello})
    check(r is not None and r.get("packets") == [hello], f"inbound chat 'hello' passes through the pipeline unchanged: {r}")
    time.sleep(0.3)
    raw = [base64.b64decode(x["data"]) for x in packets if x["uuid"] == p["uuid"]]
    check(any(b[0] == 0x79 and b"nms ok" in b for b in raw), f"NMS packet reached the encoder: {[b[:8] for b in raw]}")
    info = bytes([0x45, 1]) + bytes.fromhex(p["uuid"].replace("-", ""))
    r = call("packet", {"uuid": p["uuid"], "data": base64.b64encode(info).decode()})
    check(r is not None and r.get("packets") == [base64.b64encode(info).decode()], f"clientbound filter passes the packet through the injected handler unchanged: {r}")
r = call("dispatch", {"sender": p["uuid"], "line": "prismspawn"})
spawn_msg = [x[2] for x in logs if x[0] == "msg" and x[2].startswith("spawned")]
check(spawn_msg and "ZOMBIE id=1000001 name=§cBob visits=1" in spawn_msg[0] and "stairs=minecraft:oak_stairs[facing=east,half=top,shape=straight,waterlogged=false]" in spawn_msg[0] and "border=59999968" in spawn_msg[0] and "biome=" in spawn_msg[0], f"entity/pdc/blockdata/border/biome: {spawn_msg}")
check(any(c[0] == "scoreboard.objective" and c[1]["op"] == "add" and c[1]["name"] == "prism" for c in calls) and any(c[0] == "scoreboard.score" and c[1]["value"] == 42 for c in calls) and any(c[0] == "scoreboard.objective" and c[1]["op"] == "display" for c in calls), "scoreboard packets sent")
check(any(c[0] == "bossbar.show" and c[1]["color"] == 1 and c[1]["style"] == 2 for c in calls), "adventure boss bar shown")
check(any(c[0] == "entity.setName" and c[1]["name"] == "§cBob" for c in calls) and any(c[0] == "entity.setPluginData" for c in calls) and any(c[0] == "player.setPluginData" for c in calls), "entity name + persistent data pushed")
r = call("event", {"e": "interact", "p": {"uuid": p["uuid"], "name": "Tester", "action": "RIGHT_CLICK_BLOCK", "x": 0, "y": 64, "z": 0, "face": 1, "block": "minecraft:diamond_block", "item": None}})
check(r["cancelled"], "PlayerInteractEvent cancelled")
r = call("event", {"e": "chat", "p": {"uuid": p["uuid"], "name": "Tester", "message": "hi there"}})
check(not r["cancelled"] and r.get("line") == "<Tester> [chat-hooked] hi there", f"chat rendered: {r}")
r = call("event", {"e": "blockbreak", "p": {"uuid": p["uuid"], "name": "Tester", "x": 0, "y": -64, "z": 0, "block": "minecraft:bedrock"}})
check(r["cancelled"], "bedrock break cancelled")
r = call("event", {"e": "blockbreak", "p": {"uuid": p["uuid"], "name": "Tester", "x": 0, "y": 64, "z": 0, "block": "minecraft:stone"}})
check(not r["cancelled"], "stone break allowed")
time.sleep(0.6)
check(any(x[0] == "log" and "delayed task ran after" in x[3] for x in logs), "scheduler ran delayed task")
r = call("event", {"e": "quit", "p": p})
check(r.get("message", "").startswith("§7[PrismTest] Tester quit"), f"quit message rewritten: {r}")
r = call("dispatch", {"sender": None, "line": "prismop"})
check(r["handled"], "console runs op-only command")
call("disable", {})
check(any(x[0] == "log" and "disabled after" in x[3] for x in logs), "onDisable ran")
proc.stdin.close(); proc.wait(timeout=10)
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
