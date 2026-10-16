#!/usr/bin/env python3
"""Ender dragon fight end to end against a dedicated server (debug-end-fight=true, flat, seed 12345):
spikes + crystals from the seed, dragon + boss bar, /kill crystals, phase cycle (holding pattern -> landing
approach -> landing -> sitting scanning -> takeoff), /kill dragon -> exit portal + egg + gateway, exit portal ->
credits (win_game) -> respawn.  Usage: python3 test/boss_fight.py [--keep]"""
import math, os, shutil, socket, struct, subprocess, sys, time, uuid
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bot import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(ROOT, "build", "boss-test")
ENT = ids("entity_types")
BLK = ids("blocks")
SEED = 12345
# EndSpikeFeature for seed 12345 (x, z, radius, height, guarded), from the java.util.Random reference in boss_test.cpp
SPIKES = [(42, 0, 4, 97, 0), (33, 24, 5, 103, 0), (12, 39, 3, 91, 0), (-13, 39, 2, 76, 0), (-34, 24, 4, 94, 0),
          (-42, -1, 4, 100, 0), (-34, -25, 2, 79, 1), (-13, -40, 3, 88, 0), (12, -40, 3, 85, 0), (33, -25, 2, 82, 1)]
LAST_GATEWAY = 11  # gateways_for_seed(12345).back()

port = socket.socket(); port.bind(("", 0)); PORT = port.getsockname()[1]; port.close()
shutil.rmtree(RUN, ignore_errors=True)
os.makedirs(RUN)
open(f"{RUN}/server.properties", "w").write(f"server-port={PORT}\nlevel-name=build/boss-test/world\nlevel-type=flat\n"
                                            f"level-seed={SEED}\ndebug-end-fight=true\npause-when-empty-seconds=600\n")
open(f"{RUN}/eula.txt", "w").write("eula=true\n")
open(f"{RUN}/ops.json", "w").write('[{"uuid": "", "name": "Alice", "level": 4, "bypassesPlayerLimit": false}]\n')
log = open(f"{RUN}/server.log", "w")
srv = subprocess.Popen([f"{ROOT}/build/craftpp", f"{RUN}/server.properties"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)

def block_at(c, x, y, z):
    return c.chunks[x >> 4, z >> 4][(y + 64) >> 4][0][((y & 15) * 16 + (z & 15)) * 16 + (x & 15)]

try:
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", PORT)).close(); break
        except OSError: time.sleep(0.1)
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
            elif pid == PLAY_CB["level_chunk_with_light"]:
                cx, cz, secs = parse_chunk(b, True); c.chunks[cx, cz] = secs
            elif pid == PLAY_CB["player_position"]:
                tid, pos = b.varint(), b.take(24); b.take(24)
                c.pos = struct.unpack(">ddd", pos)
                c.send(PLAY_SB["accept_teleportation"], varint(tid) + pos + b.take(8))
            elif pid == PLAY_CB["game_event"]:
                c.events = getattr(c, "events", []) + [b.u8()]
            elif pid == PLAY_CB["respawn"]:
                c.respawned = True
            if until(): return True
        raise AssertionError("timed out")

    crystals = lambda: sorted(p for t, p in types.values() if t == ENT["end_crystal"])
    pump(lambda: dragon is not None and len(crystals()) == 10 and 0 in bar, 20)
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
    i = phases.index(2)
    assert phases[i:i + 4] == [2, 3, 6, 4], phases
    print(f"phases ok: {phases} in {time.time() - t0:.0f}s")

    c.send(PLAY_SB["chat_command"], string("kill @e[type=ender_dragon]"))
    gx = math.floor(96 * math.cos(2 * (-math.pi + math.pi / 20 * LAST_GATEWAY)))
    gz = math.floor(96 * math.sin(2 * (-math.pi + math.pi / 20 * LAST_GATEWAY)))
    pump(lambda: dragon not in types and 1 in bar and blocks_seen.get((gx, 75, gz)) == BLK["end_gateway"]
         and blocks_seen.get((0, 64, 0)) == BLK["dragon_egg"], 10)
    portal = [p for p, s in blocks_seen.items() if s == BLK["end_portal"]]
    assert len(portal) == 20 and {p[1] for p in portal} == {60}, portal
    assert block_at(c, 0, 63, 0) == BLK["bedrock"] and block_at(c, 3, 60, 0) == BLK["bedrock"]
    print(f"kill ok: exit portal (20 blocks at y 60), egg at 0 64 0, gateway at {gx} 75 {gz}")

    c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", 1.5, 60.0, 0.5, 1))
    pump(lambda: 4 in getattr(c, "events", []), 5)
    c.send(PLAY_SB["client_command"], varint(0))
    pump(lambda: getattr(c, "respawned", False), 5)
    print("credits ok: win_game, then respawn")
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
finally:
    if srv.poll() is None: srv.kill()
    if "--keep" not in sys.argv: shutil.rmtree(RUN, ignore_errors=True)
