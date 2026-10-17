#!/usr/bin/env python3
"""Mob framework: /summon + selectors, movement packets, killing a cow (drops + xp), creeper explosion, night spawning.
Usage: python3 test/mobs_test.py host port  (flat test world, Sam is op)"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

ENTITY = ids("entity_types")
REG_ITEMS = __import__("json").load(open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data/reports/registries.json")))["minecraft:item"]["entries"]
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
    c.send(PLAY_SB["client_command"], varint(0))  # respawn in case an earlier test left Sam dead
    sx, sz = -40.5, 40.5  # away from blocks other tests place near the origin
    sy = -60.0  # flat world ground
    cmd(c, f"tp Sam {sx} {sy} {sz}")
    cmd(c, "gamerule spawn_mobs false")  # keep the area quiet until the spawning check
    cmd(c, "time set noon")
    cmd(c, "kill @e[type=!player]")
    pump(c, 1.5, lambda pid, b: False)  # let the respawn / teleport settle (entity views are rebuilt)

    # ---- /summon, selectors, gravity + movement packets ----
    cmd(c, f"summon minecraft:cow {sx + 3} {sy + 8} {sz}")
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
        return "id" in cow and (moved["dy"] < -6 or moved.get("sync_y", 1e9) <= sy + 0.01)
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
    def find_target(pid, b):
        if pid != PLAY_CB["add_entity"]: return False
        e = add_entity(b)
        # Other cow spawn packets can still be queued from the rebuilt entity view.
        if e[1] != ENTITY["cow"] or any(abs(a - v) > .01 for a, v in zip(e[2:], (sx + 2, sy, sz))): return False
        target["id"] = e[0]
        return True
    pump(c, 5, find_target)
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

    # ---- behaviour: sunlight burns zombies, zombies hit survival players, skeletons shoot, sheep shear ----
    cmd(c, "time set noon")
    cmd(c, f"summon zombie {sx - 6} {sy} {sz} {{Tags:[\"sun\"]}}")
    zid = {}
    def on_burn(pid, b):
        if pid == PLAY_CB["add_entity"]:
            e = add_entity(b)
            if e[1] == ENTITY["zombie"]: zid["id"] = e[0]
        elif pid == PLAY_CB["set_entity_data"] and "id" in zid and b.varint() == zid["id"]:
            while True:
                idx = b.u8()
                if idx == 0xFF: break
                ser = b.varint()
                if idx == 0 and ser == 0 and b.u8() & 1: return True
                return False
        return False
    assert pump(c, 15, on_burn), "zombie didn't catch fire in daylight"
    print("sunlight ok: zombie on fire")
    cmd(c, "kill @e[type=zombie]")
    cmd(c, "time set midnight")
    cmd(c, "gamemode survival")
    cmd(c, "difficulty normal")
    cmd(c, f"summon zombie {sx + 1.5} {sy} {sz}")
    cmd(c, f"summon skeleton {sx} {sy} {sz + 8}")
    hits = {"health": 20.0, "arrow": False}
    def on_attack(pid, b):
        if pid == PLAY_CB["set_health"]: hits["health"] = struct.unpack(">f", b.take(4))[0]
        elif pid == PLAY_CB["add_entity"] and add_entity(b)[1] == ENTITY["arrow"]: hits["arrow"] = True
        return hits["health"] < 20.0 and hits["arrow"]
    assert pump(c, 20, on_attack), f"mobs did not attack: {hits}"
    print(f"hostile AI ok: health {hits['health']:.1f}, skeleton arrow seen")
    cmd(c, "gamemode creative")
    cmd(c, "kill @e[type=!player]")
    cmd(c, "time set noon")
    cmd(c, "clear Sam")
    cmd(c, "give Sam minecraft:shears")
    cmd(c, f"summon sheep {sx + 1.5} {sy} {sz} {{NoAI:1b,Color:14b}}")
    sheep = {}
    pump(c, 5, lambda pid, b: pid == PLAY_CB["add_entity"] and (lambda e: e[1] == ENTITY["sheep"] and sheep.update(id=e[0]) is None)(add_entity(b)))
    assert "id" in sheep
    time.sleep(0.2)
    c.send(PLAY_SB["interact"], varint(sheep["id"]) + varint(0) + b"\0" + b"\0")
    wool = {"n": 0}
    pump(c, 3, lambda pid, b: pid == PLAY_CB["add_entity"] and add_entity(b)[1] == ENTITY["item"] and wool.__setitem__("n", wool["n"] + 1) is None and wool["n"] >= 1)
    assert wool["n"] >= 1, "shearing dropped no wool"
    print(f"shearing ok: {wool['n']} wool item(s)")
    cmd(c, "kill @e[type=!player]")

    # ---- PvP: Sam kills Alice; the death message names the killer; pvp=false stops damage ----
    a, _ = login("Alice", host, port)
    a.send(PLAY_SB["client_command"], varint(0))
    cmd(a, "gamemode survival")
    cmd(a, f"tp Alice {sx + 1.5} {sy} {sz}")
    alice = {}
    pump(c, 3, lambda pid, b: pid == PLAY_CB["add_entity"] and (lambda e: e[1] == ENTITY["player"] and alice.update(id=e[0]) is None)(add_entity(b)))
    assert "id" in alice, "Sam doesn't see Alice"
    cmd(c, "clear Sam")
    cmd(c, "give Sam minecraft:diamond_sword")
    cmd(c, "gamerule pvp false")
    pump(c, 0.8, lambda pid, b: False)
    c.send(PLAY_SB["attack"], varint(alice["id"]))
    assert not pump(c, 1.0, lambda pid, b: pid == PLAY_CB["damage_event"] and b.varint() == alice["id"]), "pvp=false still hurt"
    cmd(c, "gamerule pvp true")
    msg = {}
    for _ in range(12):
        c.send(PLAY_SB["attack"], varint(alice["id"]))
        if pump(c, 0.6, lambda pid, b: pid == PLAY_CB["system_chat"] and b"death.attack.player" in b.d and msg.update(d=b.d) is None): break
    assert "d" in msg and b"Sam" in msg["d"], f"no killer death message: {msg}"
    print("pvp ok: pvp=false blocks damage, death.attack.player names Sam")
    a.send(PLAY_SB["client_command"], varint(0))
    a.s.close()

    # ---- pick block on an entity (creative): spawn egg ----
    cmd(c, f"summon pig {sx + 1.5} {sy} {sz} {{NoAI:1b}}")
    pig = {}
    pump(c, 3, lambda pid, b: pid == PLAY_CB["add_entity"] and (lambda e: e[1] == ENTITY["pig"] and pig.update(id=e[0]) is None)(add_entity(b)))
    c.send(PLAY_SB["pick_item_from_entity"], varint(pig["id"]) + b"\0")
    egg = REG_ITEMS["minecraft:pig_spawn_egg"]["protocol_id"]
    def on_slot(pid, b):
        if pid != PLAY_CB["set_player_inventory"]: return False
        b.varint()
        return b.varint() > 0 and b.varint() == egg
    assert pump(c, 3, on_slot), "pick block on pig gave no spawn egg"
    print("pick entity ok: pig spawn egg")
    cmd(c, "kill @e[type=pig]")

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
    pump(c, 1.5, lambda pid, b: False)
    cmd(c, f'summon cow {sx} {sy} {sz} {{CustomName:"keeper",NoAI:1b,Tags:["persist"]}}')
    pump(c, 1, lambda pid, b: False)
    print("mobs ok")

def after_restart(host, port):
    c, _ = login("Sam", host, port)
    cmd(c, "execute if entity @e[type=cow,name=keeper,tag=persist]")
    wait(c, PLAY_CB["system_chat"], lambda b: b"commands.execute.conditional.pass" in b.d or b"conditional.fail" in b.d and (_ for _ in ()).throw(AssertionError("keeper cow lost")))
    cmd(c, "kill @e[type=cow,name=keeper]")
    print("mob persistence ok: named cow survived restart")

if __name__ == "__main__":
    if "--after-restart" in sys.argv:
        after_restart(sys.argv[1], int(sys.argv[2]))
        sys.exit(0)
    main(sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1", int(sys.argv[2]) if len(sys.argv) > 2 else 25565)
