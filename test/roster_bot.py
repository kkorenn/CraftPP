#!/usr/bin/env python3
"""Mob roster: summon every roster type (spawn packets parse), breed two cows, tame a wolf, trade with a farmer.
Usage: python3 test/roster_bot.py host port  (flat test world, Sam is op)"""
import json, os, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENTITY = ids("entity_types")
ITEMS = json.load(open(os.path.join(ROOT, "vanilla/generated/reports/registries.json")))["minecraft:item"]["entries"]
def item(n): return ITEMS["minecraft:" + n]["protocol_id"]
CORE = {"cow", "pig", "sheep", "chicken", "zombie", "skeleton", "spider", "creeper", "player", "armor_stand", "mannequin"}
BOSSES = {"ender_dragon", "wither", "warden", "elder_guardian"}
ROSTER = [t["id"] for t in json.load(open(os.path.join(ROOT, "data/mobs.json")))["entity_types"]
          if t.get("attr") and t["id"] not in CORE | BOSSES]

def cmd(c, line): c.send(PLAY_SB["chat_command"], string(line))
def add_entity(b):
    eid = b.varint(); b.take(16); t = b.varint()
    x, y, z = struct.unpack(">ddd", b.take(24))
    return eid, t, x, y, z
def pump(c, seconds, on):
    end = time.time() + seconds
    while time.time() < end:
        pid, b = c.recv()
        if on(pid, b): return True
    return False
def interact(c, eid): c.send(PLAY_SB["interact"], varint(eid) + varint(0) + b"\0" + b"\0")
def hold(c, *stacks):
    cmd(c, "clear Sam")
    for name, n in stacks: cmd(c, f"give Sam minecraft:{name} {n}")
    c.send(PLAY_SB["set_carried_item"], struct.pack(">h", 0))
    time.sleep(0.3)
def spawned(c, kind, seconds=5, exclude=()):
    got = {}
    def on(pid, b):
        if pid == PLAY_CB["add_entity"]:
            e = add_entity(b)
            if e[1] == ENTITY[kind] and e[0] not in exclude: got["id"] = e[0]
        return "id" in got
    assert pump(c, seconds, on), f"{kind} not spawned"
    return got["id"]

def main(host, port):
    c, _ = login("Sam", host, port)
    sx, _, sz = c.pos
    sy = -60.0
    cmd(c, f"tp Sam {sx} {sy} {sz}")
    cmd(c, "gamerule spawn_mobs false")
    cmd(c, "difficulty easy")
    cmd(c, "time set noon")
    cmd(c, "kill @e[type=!player]")
    time.sleep(0.5)

    # ---- summon every roster type: add_entity + entity data must parse ----
    want = {ENTITY[n]: n for n in ROSTER}
    seen = set()
    for i, n in enumerate(ROSTER):
        cmd(c, f"summon minecraft:{n} {sx + 6 + (i % 10) * 3} {sy} {sz + 6 + (i // 10) * 3} {{NoAI:1b}}")
    def on_add(pid, b):
        if pid == PLAY_CB["add_entity"]:
            t = add_entity(b)[1]
            if t in want: seen.add(t)
        elif pid == PLAY_CB["set_entity_data"]:  # walk every entry: unknown serializers would desync the parse
            b.varint()
            while (idx := b.u8()) != 0xFF:
                ser = b.varint()
                if ser in (0, 8): b.u8()
                elif ser == 3: b.take(4)
                else: return False  # other serializers: stop parsing this packet (sizes vary)
        return len(seen) == len(want)
    pump(c, 10, on_add)
    missing = sorted(want[t] for t in want if t not in seen)
    assert not missing, f"not spawned: {missing}"
    print(f"summon ok: {len(seen)} roster mob types")
    cmd(c, "kill @e[type=!player]")
    time.sleep(0.5)

    # ---- breed two cows ----
    hold(c, ("wheat", 64))
    cmd(c, f"summon cow {sx + 2} {sy} {sz}")
    a = spawned(c, "cow")
    cmd(c, f"summon cow {sx + 2} {sy} {sz + 1}")
    b2 = spawned(c, "cow", exclude=(a,))
    interact(c, a); interact(c, b2)
    def hearts(pid, b):  # entity_event 18 (in love) for both
        if pid == PLAY_CB["entity_event"]:
            e, ev = b.i32(), b.u8()
            if ev == 18: hearts.n.add(e)
        return hearts.n >= {a, b2}
    hearts.n = set()
    assert pump(c, 5, hearts), "cows didn't fall in love"
    calf = spawned(c, "cow", seconds=20, exclude=(a, b2))
    print(f"breeding ok: cows {a} + {b2} -> calf {calf}")
    cmd(c, "kill @e[type=!player]")

    # ---- tame a wolf (1/3 per bone) ----
    hold(c, ("bone", 64))
    cmd(c, f"summon wolf {sx + 2} {sy} {sz} {{NoAI:1b}}")
    w = spawned(c, "wolf")
    tamed = {"ok": False}
    for tries in range(1, 40):
        interact(c, w)
        def on(pid, b):
            if pid == PLAY_CB["entity_event"]:
                e, ev = b.i32(), b.u8()
                if e == w and ev == 7: tamed["ok"] = True
                if e == w and ev in (6, 7): return True
            return False
        pump(c, 2, on)
        if tamed["ok"]: break
    assert tamed["ok"], "wolf never tamed"
    print(f"taming ok: wolf tamed after {tries} bone(s)")
    cmd(c, "kill @e[type=!player]")

    # ---- villager claims a composter, becomes a farmer, trades ----
    vx, vz = int(sx) + 3, int(sz)
    cmd(c, f"setblock {vx + 1} {int(sy)} {vz} minecraft:composter")
    hold(c, ("emerald", 64), ("wheat", 64), ("potato", 64), ("carrot", 64), ("beetroot", 64))
    cmd(c, f"summon villager {vx + 0.5} {sy} {vz + 0.5}")
    v = spawned(c, "villager")
    screen = {}
    deadline = time.time() + 15
    while "offers" not in screen and time.time() < deadline:
        interact(c, v)
        def on(pid, b):
            if pid == PLAY_CB["open_screen"]: screen["id"] = b.varint()
            elif pid == PLAY_CB["merchant_offers"]:
                b.varint(); n = b.varint()
                a_item, a_count = b.varint(), b.varint(); b.varint()
                cnt, res = b.varint(), b.varint()
                screen["offers"] = (n, a_item, a_count, res)
                return True
            return False
        pump(c, 1, on)
    assert "offers" in screen, "villager never offered trades (no profession?)"
    n, a_item, a_count, res = screen["offers"]
    c.send(PLAY_SB["select_trade"], varint(0))  # moves the payment in from the inventory
    state = {}
    def on_state(pid, b):
        if pid in (PLAY_CB["container_set_content"], PLAY_CB["container_set_slot"]) and b.varint() == screen["id"]:
            state["id"] = b.varint()
            if pid == PLAY_CB["container_set_slot"] and b.i16() == 2: state["result"] = True
        return "result" in state and pid == -1
    pump(c, 2, on_state)
    assert "result" in state, "no trade result after select_trade"
    c.send(PLAY_SB["container_click"], varint(screen["id"]) + varint(state["id"]) + struct.pack(">hb", 2, 0) + varint(0) + varint(0) + b"\0")
    def on_cursor(pid, b):
        if pid == PLAY_CB["set_cursor_item"]:
            cnt = b.varint()
            return cnt > 0 and b.varint() == res
        return False
    assert pump(c, 5, on_cursor), "trade result not taken"
    print(f"trading ok: farmer offered {n} trade(s); paid {a_count}x item {a_item} for item {res}")
    c.send(PLAY_SB["container_close"], varint(screen["id"]))
    cmd(c, "kill @e[type=!player]")
    time.sleep(0.3)
    print("roster ok")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1", int(sys.argv[2]) if len(sys.argv) > 2 else 25565)
