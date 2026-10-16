#!/usr/bin/env python3
"""Mob framework: /summon + selectors, movement packets, killing a cow (drops + xp), creeper explosion, night spawning.
Usage: python3 test/mobs_test.py host port  (flat test world, Sam is op)"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

ENTITY = ids("entity_types")
HOSTILE = {ENTITY[n] for n in ("zombie", "skeleton", "creeper", "spider")}

def cmd(c, line): c.send(PLAY_SB["chat_command"], string(line))

def add_entity(b):  # -> (eid, type, x, y, z)
    eid = b.varint(); b.take(16); t = b.varint()
    x, y, z = struct.unpack(">ddd", b.take(24))
    return eid, t, x, y, z

def pump(c, seconds, on):
    """Read packets for up to `seconds`; on(pid, buf) returning True stops early."""
    end = time.time() + seconds  # blocking reads: the server sends set_time every second
    while time.time() < end:
        pid, b = c.recv()
        if on(pid, b): return True
    return False

def main(host, port):
    c, _ = login("Sam", host, port)
    sx, _, sz = c.pos
    sy = -60.0  # flat world ground
    cmd(c, f"tp Sam {sx} {sy} {sz}")
    cmd(c, "gamerule spawn_mobs false")  # keep the area quiet until the spawning check
    cmd(c, "time set noon")
    cmd(c, "kill @e[type=!player]")

    # ---- /summon, selectors, gravity + movement packets ----
    cmd(c, f"summon minecraft:cow {sx + 3} {sy + 4} {sz}")
    cow = {}
    moved = {"dy": 0.0}
    def watch_cow(pid, b):
        if pid == PLAY_CB["add_entity"]:
            eid, t, x, y, z = add_entity(b)
            if t == ENTITY["cow"]: cow["id"], cow["y"] = eid, y
        elif pid in (PLAY_CB["move_entity_pos"], PLAY_CB["move_entity_pos_rot"]) and "id" in cow:
            if b.varint() == cow["id"]:
                b.varint(); dx, dy, dz = struct.unpack(">hhh", b.take(6))
                moved["dy"] += dy / 4096.0
        elif pid == PLAY_CB["entity_position_sync"] and "id" in cow:
            if b.varint() == cow["id"]:
                b.varint(); moved["sync_y"] = struct.unpack(">ddd", b.take(24))[1]
        # (with chunk light gating, the first pairing can arrive after the cow already landed)
        return "id" in cow and (moved["dy"] < -3.5 or moved.get("sync_y", 1e9) <= sy + 0.01 or cow["y"] <= sy + 0.01)
    assert pump(c, 10, watch_cow), f"cow fall not seen: {cow} {moved}"
    print(f"summon ok: cow {cow['id']} fell (deltas {moved['dy']:.2f}, sync y {moved.get('sync_y')})")
    # mob AI produces delta packets: wait for a pos/rot/head update of any mob
    kinds = set()
    def on_move(pid, b):
        for k in ("move_entity_pos", "move_entity_pos_rot", "move_entity_rot", "rotate_head"):
            if pid == PLAY_CB[k]: kinds.add(k)
        return len(kinds & {"move_entity_pos", "move_entity_pos_rot"}) > 0 and "rotate_head" in kinds
    assert pump(c, 20, on_move), f"no mob movement packets: {kinds}"
    print(f"movement packets ok: {sorted(kinds)}")

    cmd(c, "execute if entity @e[type=minecraft:cow,distance=..10]")
    wait(c, PLAY_CB["system_chat"], lambda b: b"commands.execute.conditional.pass" in b.d)
    cmd(c, "kill @e[type=cow]")
    wait(c, PLAY_CB["system_chat"], lambda b: b"commands.kill.success" in b.d)
    print("selectors ok: @e[type=cow] found and killed")

    # ---- a player kills a cow: death animation, loot, xp ----
    cmd(c, "give Sam minecraft:diamond_sword")
    cmd(c, f"summon cow {sx + 2} {sy} {sz} {{NoAI:1b}}")
    target = {}
    pump(c, 5, lambda pid, b: pid == PLAY_CB["add_entity"] and (lambda e: e[1] == ENTITY["cow"] and target.update(id=e[0]) is None)(add_entity(b)))
    assert "id" in target, "NoAI cow not spawned"
    time.sleep(0.4)  # full attack strength
    seen = {"death": False, "items": 0, "orbs": 0}
    hits = 0
    deadline = time.time() + 10
    while not seen["death"] and time.time() < deadline:
        c.send(PLAY_SB["attack"], varint(target["id"])); hits += 1
        def on(pid, b):
            if pid == PLAY_CB["entity_event"]:
                eid = b.i32()
                if eid == target["id"] and b.u8() == 3: seen["death"] = True
            elif pid == PLAY_CB["add_entity"]:
                e = add_entity(b)
                if e[1] == ENTITY["item"]: seen["items"] += 1
                if e[1] == ENTITY["experience_orb"]: seen["orbs"] += 1
            return seen["death"]
        pump(c, 0.7, on)
    assert seen["death"], "cow did not die"
    pump(c, 2, lambda pid, b: (pid == PLAY_CB["add_entity"] and (lambda e: seen.__setitem__("items", seen["items"] + (e[1] == ENTITY["item"]))
                                                                  or seen.__setitem__("orbs", seen["orbs"] + (e[1] == ENTITY["experience_orb"])))(add_entity(b))) and False)
    assert seen["items"] >= 1, f"no beef dropped: {seen}"
    assert seen["orbs"] >= 1, f"no xp orbs: {seen}"
    print(f"kill cow ok: {hits} hit(s), {seen['items']} item stack(s), {seen['orbs']} xp orb(s)")

    # ---- creeper explosion removes blocks ----
    ex, ez = int(sx) + 12, int(sz)
    gy = int(sy) - 1  # grass layer of the flat world
    cmd(c, f"summon creeper {ex + 0.5} {sy} {ez + 0.5} {{ignited:1b,NoAI:1b}}")
    boom = {"explode": False, "air": 0}
    def on_boom(pid, b):
        if pid == PLAY_CB["explode"]: boom["explode"] = True
        elif pid == PLAY_CB["block_update"]:
            x, y, z = block_pos(b.i64())
            if b.varint() == 0 and abs(x - ex) <= 4 and abs(z - ez) <= 4: boom["air"] += 1
        elif pid == PLAY_CB["section_blocks_update"]:
            b.i64()
            for _ in range(b.varint()):
                if b.varint() >> 12 == 0: boom["air"] += 1
        return boom["explode"] and boom["air"] > 0
    assert pump(c, 8, on_boom), f"creeper explosion not seen: {boom}"
    print(f"creeper ok: explosion removed {boom['air']} block(s)")

    # ---- natural spawning at night ----
    cmd(c, "difficulty normal")
    cmd(c, "time set midnight")
    cmd(c, "gamerule spawn_mobs true")
    spawned = {}
    def on_spawn(pid, b):
        if pid == PLAY_CB["add_entity"]:
            e = add_entity(b)
            if e[1] in HOSTILE: spawned[e[0]] = e[1]
        return len(spawned) >= 1
    assert pump(c, 30, on_spawn), "no monster spawned at night"
    name = {v: k for k, v in ENTITY.items()}
    print(f"night spawning ok: {', '.join(name[t] for t in spawned.values())}")
    cmd(c, "gamerule spawn_mobs false")
    cmd(c, "time set noon")
    cmd(c, "kill @e[type=!player]")
    time.sleep(0.3)
    print("mobs ok")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1", int(sys.argv[2]) if len(sys.argv) > 2 else 25565)
