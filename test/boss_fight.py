#!/usr/bin/env python3
"""Ender dragon fight end to end against a dedicated server (debug-end-fight=true, flat, seed 12345):
spikes + crystals from the seed, dragon + boss bar, /kill crystals, phase cycle (holding pattern -> landing
approach -> landing -> sitting scanning -> takeoff), /kill dragon -> exit portal + egg + gateway, exit portal ->
credits (win_game) -> respawn.  Usage: python3 test/boss_fight.py [--keep]"""
import json, math, os, shutil, socket, struct, subprocess, sys, time, uuid
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bot import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get("BUILD_DIR", "build")
RUN = os.path.join(ROOT, BUILD, "boss-test")
ENT = ids("entity_types")
BLK = ids("blocks")
SEED = 12345
# EndSpikeFeature for seed 12345 (x, z, radius, height, guarded), from the java.util.Random reference in boss_test.cpp
SPIKES = [(42, 0, 4, 97, 0), (33, 24, 5, 103, 0), (12, 39, 3, 91, 0), (-13, 39, 2, 76, 0), (-34, 24, 4, 94, 0),
          (-42, -1, 4, 100, 0), (-34, -25, 2, 79, 1), (-13, -40, 3, 88, 0), (12, -40, 3, 85, 0), (33, -25, 2, 82, 1)]
LAST_GATEWAY = 11  # gateways_for_seed(12345).back()

PORT = free_port()
shutil.rmtree(RUN, ignore_errors=True)
os.makedirs(RUN)
open(f"{RUN}/server.properties", "w").write(f"server-port={PORT}\nlevel-name={BUILD}/boss-test/world\nlevel-type=flat\n"
                                            f"level-seed={SEED}\npause-when-empty-seconds=600\nspawn-protection=0\n")
open(f"{RUN}/eula.txt", "w").write("eula=true\n")
open(f"{RUN}/ops.json", "w").write('[{"uuid": "", "name": "Alice", "level": 4, "bypassesPlayerLimit": false}]\n')
log = open(f"{RUN}/server.log", "w")
srv = subprocess.Popen([f"{ROOT}/{BUILD}/craftpp", f"{RUN}/server.properties"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)

def block_at(c, x, y, z):
    secs = c.chunks[x >> 4, z >> 4]
    min_y = -64 if len(secs) == 24 else 0  # overworld -64..319, end 0..255
    return secs[(y - min_y) >> 4][0][((y & 15) * 16 + (z & 15)) * 16 + (x & 15)]

try:
    log_path = f"{RUN}/server.log"
    for _ in range(600):
        if os.path.exists(log_path) and "Done (" in open(log_path).read(): break
        assert srv.poll() is None, "server exited during startup:\n" + (open(log_path).read()[-2000:] if os.path.exists(log_path) else "")
        time.sleep(0.05)
    else:
        raise AssertionError("server did not start:\n" + (open(log_path).read()[-2000:] if os.path.exists(log_path) else ""))
    c, _ = login("Alice", "127.0.0.1", PORT, decode=True, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Alice"))
    c.send(PLAY_SB["chat_command"], string("gamemode creative"))
    c.s.settimeout(30)
    types, dragon, phases, bar = {}, None, [], []
    blocks_seen = {}

    def pump(until, limit=60.0):
        """Read packets (tracking entities, dragon phase, boss bar, block updates) until until() or timeout."""
        global dragon
        end = time.time() + limit
        while time.time() < end:
            pid, b = c.recv()
            if pid == PLAY_CB["add_entity"]:
                eid = b.varint(); b.take(16); t = b.varint()
                types[eid] = (t, struct.unpack(">ddd", b.take(24)))
                if t == ENT["ender_dragon"]: dragon = eid
            elif pid == PLAY_CB["remove_entities"]:
                for _ in range(b.varint()): types.pop(b.varint(), None)
            elif pid == PLAY_CB["boss_event"]:
                b.take(16); bar.append(b.varint())
            elif pid == PLAY_CB["set_entity_data"] and b.varint() == dragon:
                while (i := b.u8()) != 0xFF:
                    t = b.varint()
                    v = b.varint() if t == 1 else struct.unpack(">f", b.take(4))[0] if t == 3 else None
                    if v is None: break
                    if i == 16 and (not phases or phases[-1] != v): phases.append(v)
            elif pid == PLAY_CB["block_update"]:
                p = block_pos(b.i64()); s = b.varint(); blocks_seen[p] = s; set_block(c, *p, s)
            elif pid == PLAY_CB["section_blocks_update"]:  # the level batches a tick's changes per section
                v = b.i64(); sx, sy, sz = v >> 42, sext(v, 20), sext(v >> 20, 22)
                for _ in range(b.varint()):
                    e = b.varint()
                    p = (sx * 16 + (e >> 8 & 15), sy * 16 + (e & 15), sz * 16 + (e >> 4 & 15))
                    blocks_seen[p] = e >> 12; set_block(c, *p, e >> 12)
            elif pid == PLAY_CB["level_chunk_with_light"]:
                cx, cz, secs = parse_chunk(b, True)[:3]; c.chunks[cx, cz] = secs
            elif pid == PLAY_CB["player_position"]:
                c.teleports = getattr(c, "teleports", 0) + 1  # bot.Conn.recv already accepted it
            elif pid == PLAY_CB["game_event"]:
                c.events = getattr(c, "events", []) + [b.u8()]
            elif pid == PLAY_CB["disconnect"]:
                raise AssertionError(f"disconnected: {b.d[b.i:]!r}")
            elif pid == PLAY_CB["respawn"]:  # changed level: the client drops chunks and entities
                c.respawned = True
                c.chunks.clear(); types.clear()
            if until(): return True
        raise AssertionError(f"timed out; phases {phases} bar {bar}")

    crystals = lambda: sorted(p for t, p in types.values() if t == ENT["end_crystal"])
    c.send(PLAY_SB["chat_command"], string("execute in minecraft:the_end run tp @s 0.5 100 0.5"))
    pump(lambda: dragon is not None and len(crystals()) == 10 and 0 in bar
         and all((x >> 4, z >> 4) in c.chunks for x, z, r, h, g in SPIKES), 60)
    want = sorted((x + .5, h + 1.0, z + .5) for x, z, r, h, g in SPIKES)
    assert crystals() == want, (crystals(), want)
    for x, z, r, h, g in SPIKES:
        assert block_at(c, x, h, z) == BLK["bedrock"] and block_at(c, x, h - 1, z) == BLK["obsidian"], (x, z)
        assert block_at(c, x + r, h - 1, z) == BLK["obsidian"] and block_at(c, x + r + 1, h - 1, z) == 0, (x, z, r)
    print(f"spawn ok: dragon {dragon} at {types[dragon][1]}, 10 crystals on the seed's spikes, boss bar")

    c.send(PLAY_SB["chat_command"], string("kill @e[type=end_crystal]"))
    pump(lambda: not crystals(), 10)
    t0 = time.time()
    # HOLDING_PATTERN 0 -> LANDING_APPROACH 2 -> LANDING 3 -> SITTING_SCANNING 6 -> TAKEOFF 4 (no survival target)
    pump(lambda: phases[-2:] == [6, 4] or phases[-1:] == [0] and 4 in phases, 150)
    # phase data goes out at the dragon's tracker interval (3 ticks), so a phase that lasts < 3 ticks can be skipped
    i = phases.index(6)
    assert phases[i + 1] == 4 and (2 in phases[:i] or 3 in phases[:i]), phases
    print(f"phases ok: {phases} in {time.time() - t0:.0f}s")

    c.send(PLAY_SB["chat_command"], string("kill @e[type=ender_dragon]"))
    gx = math.floor(96 * math.cos(2 * (-math.pi + math.pi / 20 * LAST_GATEWAY)))
    gz = math.floor(96 * math.sin(2 * (-math.pi + math.pi / 20 * LAST_GATEWAY)))
    pump(lambda: dragon not in types and 1 in bar and blocks_seen.get((gx, 75, gz)) == BLK["end_gateway"]
         and any(k[0] == 0 and k[2] == 0 and v == BLK["dragon_egg"] for k, v in blocks_seen.items()), 10)
    ey = next(k[1] for k, v in blocks_seen.items() if k[0] == 0 and k[2] == 0 and v == BLK["dragon_egg"])
    py = ey - 4  # egg on the pillar (3 bedrock above the portal layer)
    portals = lambda: [(x, y, z) for x in range(-3, 4) for z in range(-3, 4) for y in range(py - 2, py + 3) if block_at(c, x, y, z) == BLK["end_portal"]]
    try: pump(lambda: len(portals()) == 20, 10)  # big edits reach the client as section updates or chunk resends
    except AssertionError: pass
    portal = portals()
    assert len(portal) == 20 and {p[1] for p in portal} == {py}, portal
    assert block_at(c, 0, py + 3, 0) == BLK["bedrock"] and block_at(c, 3, py, 0) == BLK["bedrock"]
    print(f"kill ok: exit portal (20 blocks at y {py}), egg at 0 {ey} 0, gateway at {gx} 75 {gz}")

    c.respawned = False
    walk(c, 1.5, py + 1.0, 0.5)  # <= 4 blocks a tick, like a client
    walk(c, 1.5, float(py), 0.5)
    pump(lambda: 4 in getattr(c, "events", []), 5)
    c.send(PLAY_SB["client_command"], varint(0))
    pump(lambda: getattr(c, "respawned", False), 5)
    n0 = getattr(c, "teleports", 0)
    pump(lambda: getattr(c, "teleports", 0) > n0, 20)  # the respawn's chunks come first: read them before going on
    print("credits ok: win_game, then respawn")

    # Wither: soul sand T + three skulls (the last one placed by the player) -> invulnerable charge-up -> blast
    for cmd in ["difficulty normal", "setblock 6 -60 6 soul_sand", "setblock 5 -59 6 soul_sand", "setblock 6 -59 6 soul_sand",
                "setblock 7 -59 6 soul_sand", "setblock 5 -58 6 wither_skeleton_skull", "setblock 6 -58 6 wither_skeleton_skull",
                "give @s wither_skeleton_skull 1", "tp @s 6.5 -60 3.5"]:
        c.send(PLAY_SB["chat_command"], string(cmd))
    pump(lambda: abs(c.pos[2] - 3.5) < 1e-6, 10)
    c.send(PLAY_SB["set_carried_item"], struct.pack(">h", 0))
    c.send(PLAY_SB["use_item_on"], varint(0) + struct.pack(">Q", (7 & 0x3FFFFFF) << 38 | (6 & 0x3FFFFFF) << 12 | (-59 & 0xFFF))
           + varint(1) + struct.pack(">fff", .5, 1, .5) + b"\0\0" + varint(99))
    withers = lambda: [e for e, (t, p) in types.items() if t == ENT["wither"]]
    pump(lambda: withers() and bar.count(0) >= 2, 10)
    w = withers()[0]
    assert abs(types[w][1][0] - 6.5) < 1e-6 and abs(types[w][1][1] - (-59.45)) < 1e-6, types[w]
    exploded = []
    end = time.time() + 20
    while time.time() < end and not exploded:
        pid, b = c.recv()
        if pid == PLAY_CB["explode"]: exploded.append(struct.unpack(">dddf", b.take(28)))
    assert exploded and exploded[0][3] == 7.0, exploded
    c.send(PLAY_SB["chat_command"], string("kill @e[type=wither]"))
    pump(lambda: w not in types and any(t == ENT["item"] for t, _ in types.values()), 10)
    print(f"wither ok: summoned from the T, 7.0 blast after the charge-up, /kill drops the nether star")

    if "--respawn" in sys.argv:  # 4 crystals on the exit portal rim -> respawn sequence -> new dragon
        c.send(PLAY_SB["chat_command"], string("clear @s"))
        c.send(PLAY_SB["chat_command"], string("give @s end_crystal 4"))
        old_dragon = dragon
        # stand on the rim bedrock next to each spot: block interaction range
        for i, (x, z, sx, sz) in enumerate([(3, 0, 3.5, 1.5), (-3, 0, -2.5, 1.5), (0, 3, 1.5, 3.5), (0, -3, 1.5, -2.5)]):
            c.send(PLAY_SB["chat_command"], string(f"execute in minecraft:the_end run tp @s {sx} {py + 1} {sz}"))
            pump(lambda: abs(c.pos[0] - sx) < 0.01 and abs(c.pos[2] - sz) < 0.01, 10)
            c.send(PLAY_SB["set_carried_item"], struct.pack(">h", 0))
            c.send(PLAY_SB["use_item_on"], varint(0) + struct.pack(">Q", (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | py)
                   + varint(1) + struct.pack(">fff", .5, 1, .5) + b"\0\0" + varint(i + 1))
        pump(lambda: len(crystals()) == 4, 10)
        t0 = time.time()
        pump(lambda: dragon != old_dragon and dragon in types, 60)
        pump(lambda: len(crystals()) == 10, 5)  # the 4 summoning crystals exploded, the pillars came back with 10
        print(f"respawn ok: new dragon {dragon} after {time.time() - t0:.0f}s, 10 crystals rebuilt")
    c.send(PLAY_SB["chat_command"], string("stop"))
    import threading  # keep reading so the server's outbox (respawn chunks) drains while it stops
    def drain():
        try:
            while True: c.recv()
        except Exception: pass
    threading.Thread(target=drain, daemon=True).start()
    try: srv.wait(10)
    except subprocess.TimeoutExpired: os.system(f"sample {srv.pid} 2 -file {RUN}/sample.txt >/dev/null 2>&1"); raise
    stats = [l for l in open(f"{RUN}/server.log") if "Tick stats" in l]
    print("boss fight ok;", stats[-1].strip() if stats else "")
except Exception:
    if os.path.exists(f"{RUN}/server.log"):
        print("=== boss fight server.log ===", file=sys.stderr)
        print(open(f"{RUN}/server.log").read()[-4000:], file=sys.stderr)
    raise
finally:
    if srv.poll() is None: srv.kill()
    if "--keep" not in sys.argv: shutil.rmtree(RUN, ignore_errors=True)
